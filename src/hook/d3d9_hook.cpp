#include "d3d9_hook.h"

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

#include <d3d9.h>
#include <dxgiformat.h>  // DXGI_FORMAT_* for the DDS writer
#include <wrl/client.h>

namespace ghost {
namespace d3d9 {

using Microsoft::WRL::ComPtr;

namespace {

// IDirect3DDevice9 vtable indices (verified against d3d9.h).
constexpr int kIdxEndScene             = 42;
constexpr int kIdxDrawIndexedPrimitive = 71;
constexpr int kIdxDrawPrimitive        = 72;
constexpr int kIdxDrawIndexedUP        = 73;
constexpr int kIdxDrawPrimitiveUP      = 74;

using EndSceneFn             = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using DrawIndexedPrimitiveFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE,
                                                          INT, UINT, UINT, UINT, UINT);
using DrawPrimitiveFn        = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE,
                                                          UINT, UINT);

EndSceneFn             g_orig_end_scene = nullptr;
DrawIndexedPrimitiveFn g_orig_draw_indexed = nullptr;
DrawPrimitiveFn        g_orig_draw = nullptr;

bool g_installed = false;

fmt::PrimitiveTopology map_topology(D3DPRIMITIVETYPE t, bool& ok) {
    ok = true;
    switch (t) {
        case D3DPT_TRIANGLELIST:  return fmt::PrimitiveTopology::TriangleList;
        case D3DPT_TRIANGLESTRIP: return fmt::PrimitiveTopology::TriangleStrip;
        case D3DPT_LINELIST:      return fmt::PrimitiveTopology::LineList;
        case D3DPT_LINESTRIP:     return fmt::PrimitiveTopology::LineStrip;
        case D3DPT_POINTLIST:     return fmt::PrimitiveTopology::PointList;
        default:                  ok = false; return fmt::PrimitiveTopology::TriangleList;
    }
}

uint32_t primitive_count(D3DPRIMITIVETYPE t, uint32_t prim_count) {
    switch (t) {
        case D3DPT_TRIANGLELIST:  return prim_count * 3;
        case D3DPT_TRIANGLESTRIP: return prim_count + 2;
        case D3DPT_LINELIST:      return prim_count * 2;
        case D3DPT_LINESTRIP:     return prim_count + 1;
        case D3DPT_POINTLIST:     return prim_count;
        default:                  return prim_count;
    }
}

// D3DFMT -> DXGI for the formats write_dds() supports. DXT2/4 are the
// premultiplied variants of DXT3/5; stored as their base format.
bool d3d9_to_dxgi(D3DFORMAT f, uint32_t& dxgi) {
    switch (f) {
        case D3DFMT_A8R8G8B8: dxgi = DXGI_FORMAT_B8G8R8A8_UNORM; return true;
        case D3DFMT_X8R8G8B8: dxgi = DXGI_FORMAT_B8G8R8X8_UNORM; return true;
        case D3DFMT_A8B8G8R8: dxgi = DXGI_FORMAT_R8G8B8A8_UNORM; return true;
        case D3DFMT_DXT1:     dxgi = DXGI_FORMAT_BC1_UNORM;      return true;
        case D3DFMT_DXT2:
        case D3DFMT_DXT3:     dxgi = DXGI_FORMAT_BC2_UNORM;      return true;
        case D3DFMT_DXT4:
        case D3DFMT_DXT5:     dxgi = DXGI_FORMAT_BC3_UNORM;      return true;
        default:              return false;
    }
}

} // namespace
namespace {

// Dump the textures bound to stages 0..7 as DDS files. D3D9 keeps textures in
// D3DPOOL_MANAGED for nearly all engine usage, so LockRect is the readback.
void capture_textures(IDirect3DDevice9* device, const std::string& frame_dir,
                      std::vector<fmt::TextureRef>& out) {
    for (DWORD stage = 0; stage < 8; ++stage) {
        IDirect3DBaseTexture9* base = nullptr;
        if (FAILED(device->GetTexture(stage, &base)) || !base)
            continue;

        ComPtr<IDirect3DTexture9> tex;
        // v1: plain 2D textures only (skip cube maps and volume maps).
        if (FAILED(base->QueryInterface(IID_PPV_ARGS(tex.GetAddressOf())))) {
            base->Release();
            continue;
        }
        base->Release();
        if (!tex)
            continue;

        D3DSURFACE_DESC desc{};
        if (FAILED(tex->GetLevelDesc(0, &desc)))
            continue;

        uint32_t dxgi = 0;
        if (!d3d9_to_dxgi(desc.Format, dxgi))
            continue;

        D3DLOCKED_RECT lr{};
        if (FAILED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)))
            continue;

        // Hash tightly packed pixel bytes so identical textures dedupe
        // (same rolling scheme as the D3D11 capture).
        const uint32_t tight = dds_tight_row_pitch(dxgi, desc.Width);
        const uint32_t rows   = dds_is_compressed(dxgi) ? (desc.Height + 3) / 4
                                                        : desc.Height;
        uint64_t hash = 1469598103934665603ull;
        const auto* base_px = static_cast<const uint8_t*>(lr.pBits);
        for (uint32_t y = 0; y < rows && tight; ++y) {
            const uint64_t row_hash = fnv1a64(base_px + static_cast<size_t>(y) * lr.Pitch,
                                              tight);
            hash ^= row_hash;
            hash *= 1099511628211ull;
        }

        char file_name[96];
        std::snprintf(file_name, sizeof(file_name), "textures/%016" PRIX64 ".dds",
                      static_cast<uint64_t>(hash));
        const std::string full_path = frame_dir + "\\" + file_name;

        // Write only the first occurrence of this texture within the frame.
        if (GetFileAttributesA(full_path.c_str()) == INVALID_FILE_ATTRIBUTES)
            write_dds(full_path, desc.Width, desc.Height, dxgi, lr.pBits, lr.Pitch);
        tex->UnlockRect(0);

        fmt::TextureRef ref{};
        ref.content_hash = hash;
        ref.slot  = stage;
        ref.usage = static_cast<uint32_t>(stage == 0 ? fmt::TextureUsage::Diffuse
                                                     : fmt::TextureUsage::Unknown);
        std::strncpy(ref.file, file_name, sizeof(ref.file) - 1);
        out.push_back(ref);
    }
}

} // namespace
namespace {

void capture_indexed(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base_vertex_idx,
                     UINT min_index, UINT num_vertices, UINT start_index,
                     UINT prim_count) {
    CaptureContext& cap = CaptureContext::instance();
    if (!cap.capturing())
        return;

    bool topo_ok = false;
    const fmt::PrimitiveTopology topology = map_topology(type, topo_ok);
    if (!topo_ok) return;

    // Vertex stream 0: the draw declares it touches [min_index, min_index +
    // num_vertices) of the stream (post stream-offset), so lock exactly that
    // window and store indices relative to it via base_vertex.
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT stride = 0, stream_offset = 0;
    if (FAILED(device->GetStreamSource(0, &vb, &stream_offset, &stride)) || !vb || stride == 0)
        return;

    const size_t lock_off  = static_cast<size_t>(stream_offset) +
                             static_cast<size_t>(min_index) * stride;
    const size_t lock_size = static_cast<size_t>(num_vertices) * stride;
    uint8_t* vb_data = nullptr;
    if (FAILED(vb->Lock(lock_off, lock_size, reinterpret_cast<void**>(&vb_data),
                        D3DLOCK_READONLY))) {
        vb->Release();
        return;
    }
    std::vector<uint8_t> vb_bytes(vb_data, vb_data + lock_size);
    vb->Unlock();
    vb->Release();

    // Index buffer.
    IDirect3DIndexBuffer9* ib = nullptr;
    if (FAILED(device->GetIndices(&ib)) || !ib)
        return;
    D3DINDEXBUFFER_DESC ib_desc{};
    ib->GetDesc(&ib_desc);
    const uint32_t elem = (ib_desc.Format == D3DFMT_INDEX16) ? 2u : 4u;
    const uint32_t index_count = primitive_count(type, prim_count);
    uint8_t* ib_data = nullptr;
    if (FAILED(ib->Lock(start_index * elem, index_count * elem,
                        reinterpret_cast<void**>(&ib_data), D3DLOCK_READONLY))) {
        ib->Release();
        return;
    }
    std::vector<uint8_t> ib_bytes(ib_data, ib_data + index_count * elem);
    ib->Unlock();
    ib->Release();

    MeshData mesh;
    mesh.name           = "Mesh";
    mesh.vertex_stride  = stride;
    mesh.attributes     = guess_vertex_layout(stride);
    mesh.topology       = topology;
    mesh.vertex_data    = vb_bytes.data();
    mesh.vertex_data_size = static_cast<uint32_t>(vb_bytes.size());
    mesh.vertex_count   = num_vertices;
    mesh.index_data     = ib_bytes.data();
    mesh.index_data_size = static_cast<uint32_t>(ib_bytes.size());
    mesh.index_count    = index_count;
    mesh.index_format   = (elem == 2) ? fmt::IndexFormat::UInt16 : fmt::IndexFormat::UInt32;
    // D3D9 fetches vertex (index + baseVertexIndex) of the stream; the stored
    // blob starts at stream vertex min_index, so map indices into it.
    mesh.base_vertex    = base_vertex_idx - static_cast<int32_t>(min_index);

    const uint32_t idx = cap.next_mesh_index();
    char name[64];
    std::snprintf(name, sizeof(name), "Mesh_%04u", idx);
    mesh.name = name;

    const std::string path = cap.frame_dir() + "\\meshes\\" + name + ".grm";

    if (cap.config().capture_textures)
        capture_textures(device, cap.frame_dir(), mesh.textures);

    std::string err;
    if (write_mesh_file(path, mesh, cap.config().source_app, 0, &err))
        log::info("d3d9 captured %s", name);
    else
        log::error("d3d9 write %s failed: %s", name, err.c_str());
}

} // namespace

namespace {

HRESULT STDMETHODCALLTYPE hk_end_scene(IDirect3DDevice9* device) {
    CaptureContext::instance().on_frame_boundary();
    return g_orig_end_scene(device);
}

HRESULT STDMETHODCALLTYPE hk_draw_indexed_primitive(IDirect3DDevice9* device,
                                                    D3DPRIMITIVETYPE type, INT base_vertex_idx,
                                                    UINT min_vertex_idx, UINT num_vertices,
                                                    UINT start_index, UINT prim_count) {
    capture_indexed(device, type, base_vertex_idx, min_vertex_idx, num_vertices,
                    start_index, prim_count);
    return g_orig_draw_indexed(device, type, base_vertex_idx, min_vertex_idx, num_vertices,
                               start_index, prim_count);
}

HRESULT STDMETHODCALLTYPE hk_draw_primitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
                                            UINT start_vertex, UINT prim_count) {
    // Non-indexed: we still need the vertex buffer to know the stride, but
    // there's no index data. Mark this as a TODO for now (most real geometry
    // in D3D9 games is indexed).
    (void)device; (void)type; (void)start_vertex; (void)prim_count;
    return g_orig_draw(device, type, start_vertex, prim_count);
}

struct DummyDevice {
    ComPtr<IDirect3D9>         d3d;
    ComPtr<IDirect3DDevice9>   device;
    HWND                       hwnd = nullptr;
};

bool create_dummy(DummyDevice& out) {
    out.d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!out.d3d)
        return false;

    WNDCLASSA wc{};
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.lpszClassName = "GhostRipperDummy9";
    RegisterClassA(&wc);
    out.hwnd = CreateWindowA(wc.lpszClassName, "ghost", WS_OVERLAPPED, 0, 0, 2, 2,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (!out.hwnd)
        return false;

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed         = TRUE;
    pp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth  = 2;
    pp.BackBufferHeight = 2;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.hDeviceWindow    = out.hwnd;

    return SUCCEEDED(out.d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, out.hwnd,
                                           D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp,
                                           &out.device)) ||
           SUCCEEDED(out.d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, out.hwnd,
                                           D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp,
                                           &out.device));
}

template <typename T>
void* vtable_entry(T* com_object, int index) {
    return (*reinterpret_cast<void***>(com_object))[index];
}

} // namespace

bool available() {
    return GetModuleHandleA("d3d9.dll") != nullptr;
}

bool install() {
    if (g_installed)
        return true;
    if (!available())
        return false;

    DummyDevice dummy;
    if (!create_dummy(dummy)) {
        log::error("d3d9: failed to create dummy device");
        return false;
    }

    HookManager& hooks = HookManager::instance();
    bool ok = true;
    ok &= hooks.add(vtable_entry(dummy.device.Get(), kIdxEndScene),
                    reinterpret_cast<void*>(&hk_end_scene),
                    reinterpret_cast<void**>(&g_orig_end_scene),
                    "IDirect3DDevice9::EndScene");
    ok &= hooks.add(vtable_entry(dummy.device.Get(), kIdxDrawIndexedPrimitive),
                    reinterpret_cast<void*>(&hk_draw_indexed_primitive),
                    reinterpret_cast<void**>(&g_orig_draw_indexed),
                    "IDirect3DDevice9::DrawIndexedPrimitive");
    ok &= hooks.add(vtable_entry(dummy.device.Get(), kIdxDrawPrimitive),
                    reinterpret_cast<void*>(&hk_draw_primitive),
                    reinterpret_cast<void**>(&g_orig_draw),
                    "IDirect3DDevice9::DrawPrimitive");

    if (dummy.hwnd)
        DestroyWindow(dummy.hwnd);

    g_installed = ok;
    if (ok) {
        CaptureContext::instance().set_api_name("d3d9");
        log::info("d3d9 hooks installed");
    }
    return ok;
}

} // namespace d3d9
} // namespace ghost

