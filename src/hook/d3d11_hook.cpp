#include "d3d11_hook.h"

#include "capture_context.h"
#include "hook_manager.h"
#include "texture_dump.h"

#include "ghost/attribute_guess.h"
#include "ghost/capture_writer.h"
#include "ghost/log.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

namespace ghost {
namespace d3d11 {

using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------------------
// Hook targets (vtable indices verified against d3d11.h interface layout)
// ---------------------------------------------------------------------------

constexpr int kIdxPresent               = 8;   // IDXGISwapChain::Present
constexpr int kIdxDrawIndexed           = 12;  // ID3D11DeviceContext::DrawIndexed
constexpr int kIdxDraw                  = 13;  // ID3D11DeviceContext::Draw
constexpr int kIdxDrawIndexedInstanced  = 20;  // ...::DrawIndexedInstanced
constexpr int kIdxDrawInstanced         = 21;  // ...::DrawInstanced

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT,
                                                        UINT, UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);

PresentFn              g_orig_present = nullptr;
DrawIndexedFn          g_orig_draw_indexed = nullptr;
DrawFn                 g_orig_draw = nullptr;
DrawIndexedInstancedFn g_orig_draw_indexed_instanced = nullptr;
DrawInstancedFn        g_orig_draw_instanced = nullptr;

bool g_installed = false;

// ---------------------------------------------------------------------------
// GPU -> CPU readback helpers
// ---------------------------------------------------------------------------

bool read_buffer(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Buffer* source,
                 std::vector<uint8_t>& out) {
    D3D11_BUFFER_DESC desc{};
    source->GetDesc(&desc);

    D3D11_BUFFER_DESC staging_desc{};
    staging_desc.ByteWidth      = desc.ByteWidth;
    staging_desc.Usage          = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Buffer> staging;
    if (FAILED(device->CreateBuffer(&staging_desc, nullptr, &staging)))
        return false;

    context->CopyResource(staging.Get(), source);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return false;
    const auto* bytes = static_cast<const uint8_t*>(mapped.pData);
    out.assign(bytes, bytes + desc.ByteWidth);
    context->Unmap(staging.Get(), 0);
    return true;
}

fmt::PrimitiveTopology map_topology(D3D11_PRIMITIVE_TOPOLOGY topo, bool& ok) {
    ok = true;
    switch (topo) {
        case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:  return fmt::PrimitiveTopology::TriangleList;
        case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return fmt::PrimitiveTopology::TriangleStrip;
        case D3D11_PRIMITIVE_TOPOLOGY_LINELIST:      return fmt::PrimitiveTopology::LineList;
        case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP:     return fmt::PrimitiveTopology::LineStrip;
        case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST:     return fmt::PrimitiveTopology::PointList;
        default:
            ok = false;  // patches, control points, adjacency, ... not captured yet
            return fmt::PrimitiveTopology::TriangleList;
    }
}

} // namespace
namespace {

// ---------------------------------------------------------------------------
// Texture capture: dump bound PS shader resources as DDS files
// ---------------------------------------------------------------------------

void capture_textures(ID3D11Device* device, ID3D11DeviceContext* context,
                      const std::string& frame_dir, std::vector<fmt::TextureRef>& out) {
    ID3D11ShaderResourceView* views[8]{};
    context->PSGetShaderResources(0, 8, views);

    for (uint32_t slot = 0; slot < 8; ++slot) {
        if (!views[slot])
            continue;

        ComPtr<ID3D11Resource> resource;
        views[slot]->GetResource(&resource);
        views[slot]->Release();

        ComPtr<ID3D11Texture2D> texture;
        if (!resource || FAILED(resource->QueryInterface(texture.GetAddressOf())))
            continue;

        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.SampleDesc.Count > 1 || desc.ArraySize != 1)
            continue;  // v1: plain 2D textures only
        if (!dds_format_supported(desc.Format))
            continue;

        D3D11_TEXTURE2D_DESC staging_desc = desc;
        staging_desc.Usage          = D3D11_USAGE_STAGING;
        staging_desc.BindFlags      = 0;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_desc.MiscFlags      = 0;

        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&staging_desc, nullptr, &staging)))
            continue;
        context->CopyResource(staging.Get(), texture.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            continue;

        // Hash the tightly packed pixel data so identical textures dedupe.
        const uint32_t tight = dds_tight_row_pitch(desc.Format, desc.Width);
        const uint32_t rows  = dds_is_compressed(desc.Format)
                                   ? (desc.Height + 3) / 4 : desc.Height;
        uint64_t hash = 1469598103934665603ull;
        const auto* base = static_cast<const uint8_t*>(mapped.pData);
        for (uint32_t y = 0; y < rows && tight; ++y) {
            const uint64_t row_hash = fnv1a64(base + static_cast<size_t>(y) * mapped.RowPitch,
                                              tight);
            hash ^= row_hash;
            hash *= 1099511628211ull;
        }

        char file_name[96];
        std::snprintf(file_name, sizeof(file_name), "textures/%016" PRIx64 ".dds", hash);
        const std::string full_path = frame_dir + "\\" + file_name;

        // Write only the first occurrence of this texture within the frame.
        if (GetFileAttributesA(full_path.c_str()) == INVALID_FILE_ATTRIBUTES)
            write_dds(full_path, desc.Width, desc.Height, desc.Format, mapped.pData,
                      mapped.RowPitch);
        context->Unmap(staging.Get(), 0);

        fmt::TextureRef ref{};
        ref.content_hash = hash;
        ref.slot   = slot;
        ref.usage  = static_cast<uint32_t>(slot == 0 ? fmt::TextureUsage::Diffuse
                                                     : fmt::TextureUsage::Unknown);
        std::strncpy(ref.file, file_name, sizeof(ref.file) - 1);
        out.push_back(ref);
    }
}

} // namespace

namespace {

// ---------------------------------------------------------------------------
// Draw capture
// ---------------------------------------------------------------------------

void capture_draw(ID3D11DeviceContext* context,
                  uint32_t index_count, uint32_t start_index, int32_t base_vertex,
                  bool indexed, uint32_t start_vertex) {
    CaptureContext& cap = CaptureContext::instance();
    if (!cap.capturing())
        return;

    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device)
        return;

    D3D11_PRIMITIVE_TOPOLOGY d3d_topo{};
    context->IAGetPrimitiveTopology(&d3d_topo);
    bool topo_ok = false;
    const fmt::PrimitiveTopology topology = map_topology(d3d_topo, topo_ok);
    if (!topo_ok) {
        log::warn("skipping draw: unsupported topology %d", static_cast<int>(d3d_topo));
        return;
    }

    // Vertex buffer, stream 0 (multi-stream meshes are partially captured; the
    // base geometry lives in stream 0 for the vast majority of engines).
    ID3D11Buffer* vb = nullptr;
    UINT stride = 0, vb_offset = 0;
    context->IAGetVertexBuffers(0, 1, &vb, &stride, &vb_offset);
    if (!vb || stride == 0) {
        log::warn("skipping draw: no vertex buffer bound (procedural draw?)");
        return;
    }

    std::vector<uint8_t> vb_bytes;
    if (!read_buffer(device.Get(), context, vb, vb_bytes)) {
        log::warn("skipping draw: vertex buffer readback failed");
        vb->Release();
        return;
    }
    vb->Release();

    // Drop the IASetVertexBuffers offset from the captured blob.
    if (vb_offset > 0 && vb_offset < vb_bytes.size())
        vb_bytes.erase(vb_bytes.begin(), vb_bytes.begin() + vb_offset);

    MeshData mesh;
    mesh.vertex_stride = stride;
    mesh.attributes    = guess_vertex_layout(stride);
    mesh.topology      = topology;

    std::vector<uint8_t> ib_slice;
    if (indexed) {
        ID3D11Buffer* ib = nullptr;
        DXGI_FORMAT ib_format = DXGI_FORMAT_UNKNOWN;
        UINT ib_offset = 0;
        context->IAGetIndexBuffer(&ib, &ib_format, &ib_offset);
        if (!ib || (ib_format != DXGI_FORMAT_R16_UINT && ib_format != DXGI_FORMAT_R32_UINT)) {
            if (ib) ib->Release();
            log::warn("skipping indexed draw: unexpected index buffer state");
            return;
        }

        std::vector<uint8_t> ib_bytes;
        const bool ok = read_buffer(device.Get(), context, ib, ib_bytes);
        ib->Release();
        if (!ok) {
            log::warn("skipping draw: index buffer readback failed");
            return;
        }

        const uint32_t elem = (ib_format == DXGI_FORMAT_R16_UINT) ? 2u : 4u;
        const size_t begin = static_cast<size_t>(ib_offset) +
                             static_cast<size_t>(start_index) * elem;
        const size_t bytes = static_cast<size_t>(index_count) * elem;
        if (begin + bytes > ib_bytes.size()) {
            log::warn("skipping draw: index range out of bounds");
            return;
        }

        // Store only the indices this draw actually used. Index values still
        // reference the full vertex buffer; base_vertex is applied by viewers.
        ib_slice.assign(ib_bytes.begin() + begin, ib_bytes.begin() + begin + bytes);
        mesh.index_count  = index_count;
        mesh.index_format = (elem == 2) ? fmt::IndexFormat::UInt16 : fmt::IndexFormat::UInt32;
        mesh.start_index  = start_index;
        mesh.base_vertex  = base_vertex;
    } else {
        // Non-indexed draw: slice the used vertex range out of the buffer.
        const size_t begin = static_cast<size_t>(start_vertex) * stride;
        const size_t bytes = static_cast<size_t>(index_count /* = vertex count */) * stride;
        if (begin + bytes > vb_bytes.size()) {
            log::warn("skipping draw: vertex range out of bounds");
            return;
        }
        vb_bytes.erase(vb_bytes.begin() + bytes + begin, vb_bytes.end());
        vb_bytes.erase(vb_bytes.begin(), vb_bytes.begin() + begin);
    }

    mesh.vertex_data      = vb_bytes.data();
    mesh.vertex_data_size = static_cast<uint32_t>(vb_bytes.size());
    mesh.vertex_count     = static_cast<uint32_t>(vb_bytes.size() / stride);
    mesh.index_data       = ib_slice.empty() ? nullptr : ib_slice.data();
    mesh.index_data_size  = static_cast<uint32_t>(ib_slice.size());

    const uint32_t mesh_idx = cap.next_mesh_index();
    char name[64];
    std::snprintf(name, sizeof(name), "Mesh_%04u", mesh_idx);
    mesh.name = name;

    if (cap.config().capture_textures)
        capture_textures(device.Get(), context, cap.frame_dir(), mesh.textures);

    const std::string path = cap.frame_dir() + "\\meshes\\" + name + ".grm";
    std::string err;
    if (write_mesh_file(path, mesh, cap.config().source_app,
                        0 /* frame encoded in dir name */, &err))
        log::info("captured %s (%u verts, %u indices)", name, mesh.vertex_count,
                  mesh.index_count);
    else
        log::error("failed to write %s: %s", name, err.c_str());
}

} // namespace

namespace {

// ---------------------------------------------------------------------------
// Detours
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain* swapchain, UINT sync_interval,
                                     UINT flags) {
    CaptureContext::instance().on_frame_boundary();
    return g_orig_present(swapchain, sync_interval, flags);
}

void STDMETHODCALLTYPE hk_draw_indexed(ID3D11DeviceContext* context, UINT index_count,
                                       UINT start_index, INT base_vertex) {
    capture_draw(context, index_count, start_index, base_vertex, true, 0);
    g_orig_draw_indexed(context, index_count, start_index, base_vertex);
}

void STDMETHODCALLTYPE hk_draw(ID3D11DeviceContext* context, UINT vertex_count,
                               UINT start_vertex) {
    capture_draw(context, vertex_count, 0, 0, false, start_vertex);
    g_orig_draw(context, vertex_count, start_vertex);
}

void STDMETHODCALLTYPE hk_draw_indexed_instanced(ID3D11DeviceContext* context,
                                                 UINT index_count, UINT instance_count,
                                                 UINT start_index, INT base_vertex,
                                                 UINT start_instance) {
    // Base geometry only; per-instance transforms live in a second stream.
    capture_draw(context, index_count, start_index, base_vertex, true, 0);
    g_orig_draw_indexed_instanced(context, index_count, instance_count, start_index,
                                  base_vertex, start_instance);
}

void STDMETHODCALLTYPE hk_draw_instanced(ID3D11DeviceContext* context, UINT vertex_count,
                                         UINT instance_count, UINT start_vertex,
                                         UINT start_instance) {
    capture_draw(context, vertex_count, 0, 0, false, start_vertex);
    g_orig_draw_instanced(context, vertex_count, instance_count, start_vertex,
                          start_instance);
}

// ---------------------------------------------------------------------------
// Dummy device: lets us read the vtable addresses the game will use
// ---------------------------------------------------------------------------

struct DummyObjects {
    ComPtr<ID3D11Device>        device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain>      swapchain;
    HWND                        hwnd = nullptr;
};

bool create_dummy(DummyObjects& out) {
    WNDCLASSA wc{};
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.lpszClassName = "GhostRipperDummy";
    RegisterClassA(&wc);

    out.hwnd = CreateWindowA(wc.lpszClassName, "ghost", WS_OVERLAPPED, 0, 0, 2, 2,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (!out.hwnd)
        return false;

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount                        = 1;
    sd.BufferDesc.Width                   = 2;
    sd.BufferDesc.Height                  = 2;
    sd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                       = out.hwnd;
    sd.SampleDesc.Count                   = 1;
    sd.Windowed                           = TRUE;

    for (const D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(
                nullptr, type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd,
                out.swapchain.GetAddressOf(), out.device.GetAddressOf(), nullptr,
                out.context.GetAddressOf())))
            return true;
    }
    return false;
}

template <typename T>
void* vtable_entry(T* com_object, int index) {
    return (*reinterpret_cast<void***>(com_object))[index];
}

} // namespace

bool available() {
    return GetModuleHandleA("d3d11.dll") != nullptr;
}

bool install() {
    if (g_installed)
        return true;
    if (!available())
        return false;

    DummyObjects dummy;
    if (!create_dummy(dummy)) {
        log::error("d3d11: failed to create dummy device");
        return false;
    }

    HookManager& hooks = HookManager::instance();
    bool ok = true;
    ok &= hooks.add(vtable_entry(dummy.swapchain.Get(), kIdxPresent),
                    reinterpret_cast<void*>(&hk_present),
                    reinterpret_cast<void**>(&g_orig_present), "IDXGISwapChain::Present");
    ok &= hooks.add(vtable_entry(dummy.context.Get(), kIdxDrawIndexed),
                    reinterpret_cast<void*>(&hk_draw_indexed),
                    reinterpret_cast<void**>(&g_orig_draw_indexed),
                    "ID3D11DeviceContext::DrawIndexed");
    ok &= hooks.add(vtable_entry(dummy.context.Get(), kIdxDraw),
                    reinterpret_cast<void*>(&hk_draw),
                    reinterpret_cast<void**>(&g_orig_draw), "ID3D11DeviceContext::Draw");
    ok &= hooks.add(vtable_entry(dummy.context.Get(), kIdxDrawIndexedInstanced),
                    reinterpret_cast<void*>(&hk_draw_indexed_instanced),
                    reinterpret_cast<void**>(&g_orig_draw_indexed_instanced),
                    "ID3D11DeviceContext::DrawIndexedInstanced");
    ok &= hooks.add(vtable_entry(dummy.context.Get(), kIdxDrawInstanced),
                    reinterpret_cast<void*>(&hk_draw_instanced),
                    reinterpret_cast<void**>(&g_orig_draw_instanced),
                    "ID3D11DeviceContext::DrawInstanced");

    if (dummy.hwnd)
        DestroyWindow(dummy.hwnd);

    g_installed = ok;
    if (ok) {
        CaptureContext::instance().set_api_name("d3d11");
        log::info("d3d11 hooks installed");
    }
    return ok;
}

} // namespace d3d11
} // namespace ghost

