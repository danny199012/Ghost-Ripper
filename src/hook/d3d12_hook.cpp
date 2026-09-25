#include "d3d12_hook.h"

#include "capture_context.h"
#include "hook_manager.h"

#include "ghost/attribute_guess.h"
#include "ghost/capture_writer.h"
#include "ghost/log.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

namespace ghost {
namespace d3d12 {

using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------------------
// Hook targets (vtable indices verified against the IDL; see d3d12_hook.h)
// ---------------------------------------------------------------------------

constexpr int kIdxPresent                 = 8;   // IDXGISwapChain::Present
constexpr int kIdxExecuteCommandLists     = 10;  // ID3D12CommandQueue
constexpr int kIdxCreateCommittedResource = 27;  // ID3D12Device
constexpr int kIdxCreatePlacedResource    = 29;  // ID3D12Device
constexpr int kIdxDrawInstanced           = 12;  // ID3D12GraphicsCommandList
constexpr int kIdxDrawIndexedInstanced    = 13;  // ID3D12GraphicsCommandList
constexpr int kIdxIASetPrimitiveTopology  = 20;  // ID3D12GraphicsCommandList
constexpr int kIdxIASetIndexBuffer        = 43;  // ID3D12GraphicsCommandList
constexpr int kIdxIASetVertexBuffers      = 44;  // ID3D12GraphicsCommandList
constexpr int kIdxReset                   = 10;  // ID3D12GraphicsCommandList::Reset

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT,
                                                       ID3D12CommandList* const*);
using CreateCommittedFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,
                                                      const D3D12_HEAP_PROPERTIES*,
                                                      D3D12_HEAP_FLAGS,
                                                      const D3D12_RESOURCE_DESC*,
                                                      D3D12_RESOURCE_STATES,
                                                      const D3D12_CLEAR_VALUE*,
                                                      REFIID, void**);
using CreatePlacedFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64,
                                                    const D3D12_RESOURCE_DESC*,
                                                    D3D12_RESOURCE_STATES,
                                                    const D3D12_CLEAR_VALUE*,
                                                    REFIID, void**);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT,
                                                  UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT,
                                                         UINT, UINT, INT, UINT);
using IASetPrimitiveTopologyFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
                                                          D3D12_PRIMITIVE_TOPOLOGY);
using IASetIndexBufferFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
                                                    const D3D12_INDEX_BUFFER_VIEW*);
using IASetVertexBuffersFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT,
                                                      const D3D12_VERTEX_BUFFER_VIEW*);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*,
                                            ID3D12PipelineState*);

PresentFn                g_orig_present = nullptr;
ExecuteCommandListsFn    g_orig_execute_command_lists = nullptr;
CreateCommittedFn        g_orig_create_committed = nullptr;
CreatePlacedFn           g_orig_create_placed = nullptr;
DrawInstancedFn          g_orig_draw_instanced = nullptr;
DrawIndexedInstancedFn   g_orig_draw_indexed_instanced = nullptr;
IASetPrimitiveTopologyFn  g_orig_ia_set_topology = nullptr;
IASetIndexBufferFn       g_orig_ia_set_index_buffer = nullptr;
IASetVertexBuffersFn     g_orig_ia_set_vertex_buffers = nullptr;
ResetFn                  g_orig_reset = nullptr;

bool g_installed = false;


// ---------------------------------------------------------------------------
// Capture state (one mutex; D3D12 recording is batched, so these paths run
// far less often than per-draw D3D11 hooks)
// ---------------------------------------------------------------------------

// Vertex/index bytes to copy out of one resource.
struct ResourceRange {
    ComPtr<ID3D12Resource> resource;  // holds the buffer alive until the flush
    uint64_t offset = 0;              // from the start of the resource
    uint64_t size   = 0;
};

// One draw recorded while a capture frame was open.
struct RecordedDraw {
    ResourceRange            vb;    // stream 0 view (sliced after readback)
    ResourceRange            ib;    // exactly the indices this draw uses (size 0 -> non-indexed)
    uint32_t                 vb_stride = 0;
    uint32_t                 ib_elem = 0;     // 2 or 4; 0 = non-indexed
    uint32_t                 vertex_count = 0;  // non-indexed draws
    uint32_t                 index_count  = 0;
    uint32_t                 start_index  = 0;
    int32_t                  base_vertex  = 0;
    uint32_t                 start_vertex = 0;  // non-indexed draws
    D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
};

// Per-command-list input-assembly state while the game records.
struct ListState {
    D3D12_VERTEX_BUFFER_VIEW vb{};                 // size 0 = unbound
    D3D12_INDEX_BUFFER_VIEW  ib{};                 // size 0 = unbound
    D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
};

std::mutex g_mutex;
std::unordered_map<ID3D12GraphicsCommandList*, ListState> g_list_state;
std::vector<RecordedDraw> g_pending;

// GPU-VA interval map of every buffer created while hooked.
struct VaEntry {
    uint64_t size = 0;
    ComPtr<ID3D12Resource> resource;
};
std::map<uint64_t, VaEntry> g_va_map;  // keyed by base GPU VA

ComPtr<ID3D12Device>      g_device;       // first seen device
ComPtr<ID3D12CommandQueue> g_queue;        // first seen DIRECT queue on it
ComPtr<ID3D12Fence>       g_fence;        // readback completion fence
uint64_t                  g_fence_value = 0;
bool                      g_va_warned = false;  // "pre-hook resource" spam guard

fmt::PrimitiveTopology map_topology(D3D12_PRIMITIVE_TOPOLOGY topo, bool& ok) {
    ok = true;
    switch (topo) {
        case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:  return fmt::PrimitiveTopology::TriangleList;
        case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return fmt::PrimitiveTopology::TriangleStrip;
        case D3D_PRIMITIVE_TOPOLOGY_LINELIST:      return fmt::PrimitiveTopology::LineList;
        case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:     return fmt::PrimitiveTopology::LineStrip;
        case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:     return fmt::PrimitiveTopology::PointList;
        default:
            ok = false;  // patches, control points, adjacency, ... not captured yet
            return fmt::PrimitiveTopology::TriangleList;
    }
}

// Resolve [va, va + size) to the resource that contains it. Games may bind the
// middle of one big buffer, so this is an interval lookup, not an exact match.
bool resolve_range(uint64_t va, uint64_t size, ResourceRange& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_va_map.upper_bound(va);
    if (it == g_va_map.begin())
        return false;
    --it;
    if (va < it->first + it->second.size && va + size <= it->first + it->second.size) {
        out.resource = it->second.resource;  // AddRef via ComPtr copy
        out.offset   = va - it->first;
        out.size     = size;
        return true;
    }
    return false;
}

void note_unresolved_va() {
    if (!g_va_warned) {
        g_va_warned = true;
        log::warn("d3d12: buffer GPU VA unknown to us (created before injection?); "
                   "draws using it are skipped");
    }
}


// Dummy objects to read vtable addresses from before the game creates its own.
// The D3D12 runtime classes share one vtable per interface, so detours
// installed here cover every instance the game later creates.
struct DummyObjects {
    ComPtr<IDXGISwapChain>           swapchain;
    ComPtr<ID3D12Device>             device;
    ComPtr<ID3D12CommandQueue>        queue;
    ComPtr<ID3D12GraphicsCommandList> cmd_list;
    HWND                             hwnd = nullptr;
};

bool create_dummy(DummyObjects& out) {
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance   = GetModuleHandleA(nullptr);
    wc.lpszClassName = "GhostRipperDummy12";
    RegisterClassA(&wc);
    out.hwnd = CreateWindowA(wc.lpszClassName, "ghost", WS_OVERLAPPED, 0, 0, 2, 2,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (!out.hwnd)
        return false;

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        DestroyWindow(out.hwnd);
        return false;
    }

    ComPtr<IDXGIAdapter> adapter;
    factory->EnumAdapters(0, &adapter);

    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&out.device)))) {
        DestroyWindow(out.hwnd);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(out.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&out.queue)))) {
        DestroyWindow(out.hwnd);
        return false;
    }

    // Command allocator + list for the GraphicsCommandList vtable.
    ComPtr<ID3D12CommandAllocator> allocator;
    if (FAILED(out.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(&allocator)))) {
        DestroyWindow(out.hwnd);
        return false;
    }
    if (FAILED(out.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&out.cmd_list)))) {
        DestroyWindow(out.hwnd);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount                       = 1;
    sd.BufferDesc.Width                  = 2;
    sd.BufferDesc.Height                 = 2;
    sd.BufferDesc.Format                 = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage                       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                      = out.hwnd;
    sd.SampleDesc.Count                  = 1;
    sd.Windowed                          = TRUE;
    sd.SwapEffect                        = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    factory->CreateSwapChain(out.queue.Get(), &sd, out.swapchain.GetAddressOf());
    if (!out.swapchain) {
        DestroyWindow(out.hwnd);
        return false;
    }
    DestroyWindow(out.hwnd);
    return true;
}

template <typename T>
void* vtable_entry(T* com_object, int index) {
    return (*reinterpret_cast<void***>(com_object))[index];
}

// ---------------------------------------------------------------------------
// Recording-time detours (ID3D12GraphicsCommandList vtable). These run on the
// game's recording threads; per-call work is a map lookup + possible push_back.
// ---------------------------------------------------------------------------

void STDMETHODCALLTYPE hk_ia_set_vertex_buffers(ID3D12GraphicsCommandList* list, UINT count,
                                                const D3D12_VERTEX_BUFFER_VIEW* views) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ListState& st = g_list_state[list];
        st.vb = (count > 0 && views) ? views[0] : D3D12_VERTEX_BUFFER_VIEW{};
    }
    g_orig_ia_set_vertex_buffers(list, count, views);
}

void STDMETHODCALLTYPE hk_ia_set_index_buffer(ID3D12GraphicsCommandList* list,
                                              const D3D12_INDEX_BUFFER_VIEW* view) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_list_state[list].ib = view ? *view : D3D12_INDEX_BUFFER_VIEW{};
    }
    g_orig_ia_set_index_buffer(list, view);
}

void STDMETHODCALLTYPE hk_ia_set_primitive_topology(ID3D12GraphicsCommandList* list,
                                                   D3D12_PRIMITIVE_TOPOLOGY topo) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_list_state[list].topology = topo;
    }
    g_orig_ia_set_topology(list, topo);
}

// Reset returns the list to a blank state; stale bindings must not leak into
// the next recording session (a recycled list pointer could reuse them).
HRESULT STDMETHODCALLTYPE hk_reset(ID3D12GraphicsCommandList* list,
                                   ID3D12CommandAllocator* allocator,
                                   ID3D12PipelineState* initial_state) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_list_state.erase(list);
    }
    return g_orig_reset(list, allocator, initial_state);
}

// Snapshot the draw + its bound views while a capture frame is open.
void record_draw(ID3D12GraphicsCommandList* list, uint32_t index_count, uint32_t start_index,
                 int32_t base_vertex, uint32_t vertex_count, uint32_t start_vertex) {
    CaptureContext& cap = CaptureContext::instance();
    if (!cap.capturing())
        return;
    if (index_count == 0 && vertex_count == 0)
        return;  // empty draw

    ListState st;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_list_state.find(list);
        if (it == g_list_state.end())
            return;  // no IA state was ever recorded on this list
        st = it->second;
    }

    if (st.vb.StrideInBytes == 0 || st.vb.SizeInBytes == 0)
        return;  // procedural / no bound vertices: nothing to capture
    bool topo_ok = false;
    map_topology(st.topology, topo_ok);
    if (!topo_ok)
        return;

    RecordedDraw draw;
    draw.topology     = st.topology;
    draw.index_count  = index_count;
    draw.start_index  = start_index;
    draw.base_vertex  = base_vertex;
    draw.vertex_count = vertex_count;
    draw.start_vertex = start_vertex;
    draw.vb_stride    = st.vb.StrideInBytes;

    if (index_count > 0) {
        if (st.ib.SizeInBytes == 0 ||
            (st.ib.Format != DXGI_FORMAT_R16_UINT && st.ib.Format != DXGI_FORMAT_R32_UINT))
            return;  // unsupported index state; skip the whole draw
        const uint32_t elem = (st.ib.Format == DXGI_FORMAT_R32_UINT) ? 4u : 2u;
        // Copy only the slice this draw reads: the view covers the whole
        // binding, StartIndexLocation slides inside it.
        const uint64_t ib_off = static_cast<uint64_t>(start_index) * elem;
        if (ib_off + static_cast<uint64_t>(index_count) * elem > st.ib.SizeInBytes)
            return;  // out-of-range draw parameters
        if (!resolve_range(st.ib.BufferLocation, st.ib.SizeInBytes, draw.ib)) {
            note_unresolved_va();
            return;
        }
        draw.ib.offset += ib_off;
        draw.ib.size    = static_cast<uint64_t>(index_count) * elem;
        draw.ib_elem    = elem;
    }

    // The vertex view is copied whole (deduped across draws of the same
    // buffer); indexed draws are trimmed to their referenced range at flush.
    if (!resolve_range(st.vb.BufferLocation, st.vb.SizeInBytes, draw.vb)) {
        note_unresolved_va();
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_pending.push_back(std::move(draw));
}

void STDMETHODCALLTYPE hk_draw_instanced(ID3D12GraphicsCommandList* list, UINT vertex_count,
                                         UINT instance_count, UINT start_vertex,
                                         UINT start_instance) {
    record_draw(list, 0, 0, 0, vertex_count, start_vertex);
    g_orig_draw_instanced(list, vertex_count, instance_count, start_vertex, start_instance);
}

void STDMETHODCALLTYPE hk_draw_indexed_instanced(ID3D12GraphicsCommandList* list,
                                                UINT index_count, UINT instance_count,
                                                UINT start_index, INT base_vertex,
                                                UINT start_instance) {
    // Base geometry only; per-instance transforms live in a second stream
    // (needs GRMF v2, see docs/ROADMAP.md).
    record_draw(list, index_count, start_index, base_vertex, 0, 0);
    g_orig_draw_indexed_instanced(list, index_count, instance_count, start_index,
                                  base_vertex, start_instance);
}

// ---------------------------------------------------------------------------
// Device / queue detours
// ---------------------------------------------------------------------------

void STDMETHODCALLTYPE hk_execute_command_lists(ID3D12CommandQueue* queue, UINT count,
                                                 ID3D12CommandList* const* lists) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_queue && queue) {
            // GetDesc() returns the struct by value on MSVC / non-Windows and
            // through an out-pointer on Wine/mingw - take both paths.
            D3D12_COMMAND_QUEUE_DESC desc{};
#ifdef _MSC_VER
            desc = queue->GetDesc();
#else
            queue->GetDesc(&desc);
#endif
            if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
                ComPtr<ID3D12Device> device;
                if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf())))) {
                    g_queue  = queue;   // holds a reference
                    g_device = std::move(device);
                    log::info("d3d12: capture queue/device acquired");
                }
            }
        }
    }
    g_orig_execute_command_lists(queue, count, lists);
}

// Register every created buffer in the GPU-VA interval map.
HRESULT STDMETHODCALLTYPE hk_create_committed(ID3D12Device* device,
                                           const D3D12_HEAP_PROPERTIES* heap_props,
                                           D3D12_HEAP_FLAGS heap_flags,
                                           const D3D12_RESOURCE_DESC* desc,
                                           D3D12_RESOURCE_STATES initial_state,
                                           const D3D12_CLEAR_VALUE* clear,
                                           REFIID riid, void** out) {
    const HRESULT hr = g_orig_create_committed(device, heap_props, heap_flags, desc,
                                               initial_state, clear, riid, out);
    if (SUCCEEDED(hr) && desc && out && *out &&
        desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        auto* resource = static_cast<ID3D12Resource*>(*out);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_va_map[resource->GetGPUVirtualAddress()] = VaEntry{desc->Width, resource};
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_create_placed(ID3D12Device* device, ID3D12Heap* heap, UINT64 offset,
                                        const D3D12_RESOURCE_DESC* desc,
                                        D3D12_RESOURCE_STATES initial_state,
                                        const D3D12_CLEAR_VALUE* clear,
                                        REFIID riid, void** out) {
    const HRESULT hr = g_orig_create_placed(device, heap, offset, desc, initial_state,
                                            clear, riid, out);
    if (SUCCEEDED(hr) && desc && out && *out &&
        desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        auto* resource = static_cast<ID3D12Resource*>(*out);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_va_map[resource->GetGPUVirtualAddress()] = VaEntry{desc->Width, resource};
    }
    return hr;
}

// ---------------------------------------------------------------------------
// Flush: readback + .grm write-out. Runs once per capture frame at Present
// (before CaptureContext closes the frame).
// ---------------------------------------------------------------------------

// A unique copied range; maps to its byte offset inside the readback buffer.
struct RegionKey {
    ID3D12Resource* resource;
    uint64_t offset, size;
    bool operator<(const RegionKey& o) const {
        return resource != o.resource ? resource < o.resource
                                     : offset != o.offset ? offset < o.offset
                                                          : size < o.size;
    }
};

// Turn the mapped readback into one .grm per recorded draw.
void write_readback_draws(const std::vector<RecordedDraw>& draws,
                          const std::map<RegionKey, uint64_t>& regions,
                          const uint8_t* mapped) {
    CaptureContext& cap = CaptureContext::instance();
    unsigned written = 0;

    for (const RecordedDraw& d : draws) {
        if (d.vb.size == 0)
            continue;
        const auto vb_it = regions.find(RegionKey{d.vb.resource.Get(), d.vb.offset, d.vb.size});
        if (vb_it == regions.end())
            continue;
        const uint8_t* vb_base  = mapped + vb_it->second;
        const uint64_t vb_size = d.vb.size;

        MeshData mesh;
        mesh.vertex_stride = d.vb_stride;
        mesh.attributes    = guess_vertex_layout(d.vb_stride);
        bool topo_ok = false;
        mesh.topology = map_topology(d.topology, topo_ok);
        if (!topo_ok)
            continue;

        if (d.index_count > 0) {
            const auto ib_it =
                regions.find(RegionKey{d.ib.resource.Get(), d.ib.offset, d.ib.size});
            if (d.ib.size == 0 || ib_it == regions.end())
                continue;
            const uint8_t* ib_base = mapped + ib_it->second;

            // Trim the vertex view to the range the indices actually use so a
            // big shared buffer doesn't bloat every mesh file.
            int64_t min_eff = INT64_MAX, max_eff = INT64_MIN;
            for (uint32_t i = 0; i < d.index_count; ++i) {
                uint64_t idx = 0;
                if (d.ib_elem == 2) {
                    uint16_t v16 = 0;
                    std::memcpy(&v16, ib_base + static_cast<size_t>(i) * 2, 2);
                    idx = v16;
                } else {
                    uint32_t v32 = 0;
                    std::memcpy(&v32, ib_base + static_cast<size_t>(i) * 4, 4);
                    idx = v32;
                }
                const int64_t eff = static_cast<int64_t>(idx) + d.base_vertex;
                if (eff < 0 || static_cast<uint64_t>(eff) * d.vb_stride >= vb_size)
                    continue;  // out-of-range index (mismatched views)
                min_eff = std::min(min_eff, eff);
                max_eff = std::max(max_eff, eff);
            }
            if (min_eff > max_eff)
                continue;  // no valid index referenced

            const uint64_t begin = static_cast<uint64_t>(min_eff) * d.vb_stride;
            uint64_t bytes = static_cast<uint64_t>(max_eff - min_eff + 1) * d.vb_stride;
            if (begin + bytes > vb_size)  // last vertex may end past the view
                bytes = vb_size - begin;
            mesh.vertex_data      = vb_base + begin;
            mesh.vertex_data_size = static_cast<uint32_t>(bytes);
            mesh.vertex_count     = static_cast<uint32_t>(bytes / d.vb_stride);
            mesh.index_data       = ib_base;
            mesh.index_data_size  = static_cast<uint32_t>(d.index_count) * d.ib_elem;
            mesh.index_count      = d.index_count;
            mesh.index_format     = (d.ib_elem == 2) ? fmt::IndexFormat::UInt16
                                                     : fmt::IndexFormat::UInt32;
            mesh.start_index      = d.start_index;
            // Indices reference buffer vertices; the stored vertex slice
            // begins at min_eff, so viewers get idx + base_vertex into it.
            mesh.base_vertex = d.base_vertex - static_cast<int32_t>(min_eff);
        } else {
            const uint64_t begin = static_cast<uint64_t>(d.start_vertex) * d.vb_stride;
            const uint64_t bytes = static_cast<uint64_t>(d.vertex_count) * d.vb_stride;
            if (begin + bytes > vb_size)
                continue;
            mesh.vertex_data      = vb_base + begin;
            mesh.vertex_data_size = static_cast<uint32_t>(bytes);
            mesh.vertex_count     = d.vertex_count;
        }

        const uint32_t idx = cap.next_mesh_index();
        char name[64];
        std::snprintf(name, sizeof(name), "Mesh_%04u", idx);
        mesh.name = name;

        const std::string path = cap.frame_dir() + "\\meshes\\" + name + ".grm";
        std::string err;
        if (write_mesh_file(path, mesh, cap.config().source_app, 0, &err)) {
            ++written;
            log::info("d3d12 captured %s (%u verts, %u indices)", name, mesh.vertex_count,
                      mesh.index_count);
        } else {
            log::error("d3d12 write %s failed: %s", name, err.c_str());
        }
    }
    log::info("d3d12 frame flushed: %u/%u draws written", written,
              static_cast<unsigned>(draws.size()));
}

bool flush_pending_draws() {
    std::vector<RecordedDraw> draws;
    ComPtr<ID3D12Device>      device;
    ComPtr<ID3D12CommandQueue> queue;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        draws = std::move(g_pending);
        g_pending.clear();
        device = g_device;
        queue  = g_queue;
    }

    if (draws.empty())
        return true;
    if (!device || !queue) {
        log::warn("d3d12: %u draws pending but no capture queue was seen; dropped",
                  static_cast<unsigned>(draws.size()));
        return false;
    }

    // Dedupe the copies: many draws share one bound vertex view.
    std::map<RegionKey, uint64_t> regions;  // key -> dst offset in readback
    uint64_t total_bytes = 0;
    for (RecordedDraw& d : draws) {
        for (ResourceRange* r : {&d.vb, &d.ib}) {
            if (r->size == 0)
                continue;
            RegionKey key{r->resource.Get(), r->offset, r->size};
            if (regions.count(key))
                continue;
            if (total_bytes + r->size > 0xFFFFFFFFull) {  // keep the readback
                log::warn("d3d12: readback overflow, skipping further buffers");
                r->size = 0;  // a >4GB frame capture is unrealistic; move on
                continue;
            }
            regions[key] = total_bytes;
            total_bytes += r->size;
        }
    }
    if (total_bytes == 0)
        return false;

    // One readback buffer with every region packed back to back.
    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rb_desc{};
    rb_desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb_desc.Width            = total_bytes;
    rb_desc.Height          = 1;
    rb_desc.DepthOrArraySize = 1;
    rb_desc.MipLevels        = 1;
    rb_desc.SampleDesc.Count = 1;
    rb_desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    if (FAILED(device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &rb_desc, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(readback.GetAddressOf())))) {
        log::error("d3d12: readback buffer creation failed (%" PRIu64 " bytes)", total_bytes);
        return false;
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(allocator.GetAddressOf()))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                         nullptr, IID_PPV_ARGS(list.GetAddressOf())))) {
        log::error("d3d12: readback command list creation failed");
        return false;
    }

    if (!g_fence && FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                               IID_PPV_ARGS(g_fence.GetAddressOf())))) {
        log::error("d3d12: fence creation failed");
        return false;
    }

    for (const auto& kv : regions) {
        // Buffers are implicitly promoted from COMMON to COPY_SOURCE, and
        // decay back after the game's ExecuteCommandLists completes, so no
        // barriers are needed for these copies.
        list->CopyBufferRegion(readback.Get(), kv.second, kv.first.resource,
                               kv.first.offset, kv.first.size);
    }
    if (FAILED(list->Close()))
        return false;

    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ++g_fence_value;
    if (FAILED(queue->Signal(g_fence.Get(), g_fence_value)))
        return false;
    while (g_fence->GetCompletedValue() < g_fence_value)
        Sleep(0);  // spin; the copies are small relative to a frame

    void* mapped = nullptr;
    if (FAILED(readback->Map(0, nullptr, &mapped)))
        return false;

    write_readback_draws(draws, regions, static_cast<const uint8_t*>(mapped));
    readback->Unmap(0, nullptr);
    return true;
}

// ---------------------------------------------------------------------------
// Present (frame boundary)
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain* swapchain, UINT sync, UINT flags) {
    // The frame's command lists were executed before Present; drain the
    // recorded draws while the capture frame is still open so the manifest
    // gets the final mesh count.
    if (CaptureContext::instance().capturing())
        flush_pending_draws();
    CaptureContext::instance().on_frame_boundary();
    return g_orig_present(swapchain, sync, flags);
}

} // namespace

bool available() {
    return GetModuleHandleA("d3d12.dll") != nullptr;
}

bool install() {
    if (g_installed)
        return true;
    if (!available())
        return false;

    DummyObjects dummy;
    if (!create_dummy(dummy)) {
        log::error("d3d12: failed to create dummy device/swapchain");
        return false;
    }

    HookManager& hooks = HookManager::instance();
    bool ok = true;

    // Frame boundary + readback queue discovery.
    ok &= hooks.add(vtable_entry(dummy.swapchain.Get(), kIdxPresent),
                    reinterpret_cast<void*>(&hk_present),
                    reinterpret_cast<void**>(&g_orig_present),
                    "IDXGISwapChain::Present (d3d12)");
    ok &= hooks.add(vtable_entry(dummy.queue.Get(), kIdxExecuteCommandLists),
                    reinterpret_cast<void*>(&hk_execute_command_lists),
                    reinterpret_cast<void**>(&g_orig_execute_command_lists),
                    "ID3D12CommandQueue::ExecuteCommandLists");

    // Buffer GPU-VA tracking.
    ok &= hooks.add(vtable_entry(dummy.device.Get(), kIdxCreateCommittedResource),
                    reinterpret_cast<void*>(&hk_create_committed),
                    reinterpret_cast<void**>(&g_orig_create_committed),
                    "ID3D12Device::CreateCommittedResource");
    ok &= hooks.add(vtable_entry(dummy.device.Get(), kIdxCreatePlacedResource),
                    reinterpret_cast<void*>(&hk_create_placed),
                    reinterpret_cast<void**>(&g_orig_create_placed),
                    "ID3D12Device::CreatePlacedResource");

    // Command-list recording observation.
    ok &= hooks.add(vtable_entry(dummy.cmd_list.Get(), kIdxDrawInstanced),
                    reinterpret_cast<void*>(&hk_draw_instanced),
                    reinterpret_cast<void**>(&g_orig_draw_instanced),
                    "ID3D12GraphicsCommandList::DrawInstanced");
    ok &= hooks.add(vtable_entry(dummy.cmd_list.Get(), kIdxDrawIndexedInstanced),
                    reinterpret_cast<void*>(&hk_draw_indexed_instanced),
                    reinterpret_cast<void**>(&g_orig_draw_indexed_instanced),
                    "ID3D12GraphicsCommandList::DrawIndexedInstanced");
    ok &= hooks.add(vtable_entry(dummy.cmd_list.Get(), kIdxIASetPrimitiveTopology),
                    reinterpret_cast<void*>(&hk_ia_set_primitive_topology),
                    reinterpret_cast<void**>(&g_orig_ia_set_topology),
                    "ID3D12GraphicsCommandList::IASetPrimitiveTopology");
    ok &= hooks.add(vtable_entry(dummy.cmd_list.Get(), kIdxIASetIndexBuffer),
                    reinterpret_cast<void*>(&hk_ia_set_index_buffer),
                    reinterpret_cast<void**>(&g_orig_ia_set_index_buffer),
                    "ID3D12GraphicsCommandList::IASetIndexBuffer");
    ok &= hooks.add(vtable_entry(dummy.cmd_list.Get(), kIdxIASetVertexBuffers),
                    reinterpret_cast<void*>(&hk_ia_set_vertex_buffers),
                    reinterpret_cast<void**>(&g_orig_ia_set_vertex_buffers),
                    "ID3D12GraphicsCommandList::IASetVertexBuffers");
    ok &= hooks.add(vtable_entry(dummy.cmd_list.Get(), kIdxReset),
                    reinterpret_cast<void*>(&hk_reset),
                    reinterpret_cast<void**>(&g_orig_reset),
                    "ID3D12GraphicsCommandList::Reset");

    if (dummy.hwnd)
        DestroyWindow(dummy.hwnd);

    g_installed = ok;
    if (ok) {
        CaptureContext::instance().set_api_name("d3d12");
        log::info("d3d12 hooks installed (draw capture active)");
    }
    return ok;
}

} // namespace d3d12
} // namespace ghost
