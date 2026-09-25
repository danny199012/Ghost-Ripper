#include "vulkan_hook.h"

#include "capture_context.h"
#include "hook_manager.h"

#include "ghost/attribute_guess.h"
#include "ghost/capture_writer.h"
#include "ghost/log.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <windows.h>  // GetModuleHandleA / GetProcAddress (vulkan.h alone pulls nothing)

#include <vulkan/vulkan.h>

namespace ghost {
namespace vulkan {

namespace {

// ---------------------------------------------------------------------------
// Loader: everything is resolved dynamically (no vulkan-1 import library
// exists in the Windows SDK, so nothing here links against Vulkan).
// ---------------------------------------------------------------------------

struct Loader {
    HMODULE                  module = nullptr;
    PFN_vkGetInstanceProcAddr gipa  = nullptr;   // real export (unhooked)
    PFN_vkGetDeviceProcAddr   gdpa  = nullptr;
};
Loader g_loader;

// One MinHook slot per intercepted device-level function. The driver hands
// out one pointer per ICD, so hooking the first resolved address covers every
// device of that ICD (multi-ICD setups capture the first one only).
struct FnHook {
    void* target = nullptr;   // resolved address that was detoured
    void* orig   = nullptr;   // MinHook trampoline to call through
    bool  failed = false;
};

FnHook g_fn_present_device;      // vkQueuePresentKHR via gdpa
FnHook g_fn_get_device_queue;    // vkGetDeviceQueue via gdpa (queue -> device)
FnHook g_fn_get_device_queue2;   // vkGetDeviceQueue2 via gdpa
FnHook g_fn_allocate_cmd_bufs;   // vkAllocateCommandBuffers via gdpa
FnHook g_fn_begin_cmd_buf;       // vkBeginCommandBuffer
FnHook g_fn_reset_cmd_buf;       // vkResetCommandBuffer
FnHook g_fn_bind_pipeline;       // vkCmdBindPipeline
FnHook g_fn_bind_vertex_buffers;  // vkCmdBindVertexBuffers
FnHook g_fn_bind_vertex_buffers2; // vkCmdBindVertexBuffers2
FnHook g_fn_bind_index_buffer;   // vkCmdBindIndexBuffer
FnHook g_fn_set_topology;        // vkCmdSetPrimitiveTopology (1.3 / ext)
FnHook g_fn_draw;                // vkCmdDraw
FnHook g_fn_draw_indexed;        // vkCmdDrawIndexed
FnHook g_fn_create_pipelines;    // vkCreateGraphicsPipelines

// Export-level hooks (stable loader addresses).
PFN_vkGetInstanceProcAddr g_orig_gipa = nullptr;
PFN_vkGetDeviceProcAddr   g_orig_gdpa = nullptr;
using GetDeviceQueueFn = void(VKAPI_PTR*)(VkDevice, uint32_t, uint32_t, VkQueue*);
GetDeviceQueueFn g_orig_get_device_queue_export = nullptr;
using AllocateCommandBuffersFn =
    VkResult(VKAPI_PTR*)(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*);
AllocateCommandBuffersFn g_orig_allocate_cmd_bufs_export = nullptr;
using PresentFn = VkResult(VKAPI_PTR*)(VkQueue, const VkPresentInfoKHR*);
PresentFn g_orig_present_export = nullptr;

bool g_installed = false;

// ---------------------------------------------------------------------------
// Capture state
// ---------------------------------------------------------------------------

struct RecordedDraw {
    VkDevice  device = VK_NULL_HANDLE;
    VkBuffer  vb = VK_NULL_HANDLE;   // binding 0
    uint64_t  vb_offset = 0;         // binding offset
    uint32_t  vb_stride = 0;
    uint64_t  vb_size = 0;           // full buffer size (from memory requirements)
    VkBuffer  ib = VK_NULL_HANDLE;   // VK_NULL_HANDLE -> non-indexed
    uint64_t  ib_offset = 0;         // draw's slice offset within the buffer
    uint64_t  ib_size = 0;           // draw's slice size
    uint32_t  ib_elem = 0;
    uint32_t  vertex_count = 0;      // non-indexed
    uint32_t  index_count  = 0;
    uint32_t  first_vertex = 0;      // non-indexed
    int32_t   vertex_offset = 0;    // indexed (Vulkan's base vertex)
    uint32_t  first_index  = 0;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
};

// Per-command-buffer state while the app records.
struct CmdState {
    VkBuffer  vb = VK_NULL_HANDLE;
    uint64_t  vb_offset = 0;
    uint32_t  vb_stride = 0;
    VkBuffer  ib = VK_NULL_HANDLE;
    uint64_t  ib_offset = 0;
    VkIndexType ib_type = VK_INDEX_TYPE_UINT16;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;  // unset
};

// Topology + binding-0 stride of a graphics pipeline (static state lives in
// the pipeline object for Vulkan < 1.3; dynamic topology/strides override).
struct PipelineInfo {
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    uint32_t           stride = 0;   // binding 0 vertex stride; 0 = dynamic
};

std::mutex g_mutex;
std::unordered_map<VkCommandBuffer, CmdState> g_cmd_state;
std::unordered_map<VkCommandBuffer, VkDevice> g_cmd_device;
std::unordered_map<VkPipeline, PipelineInfo> g_pipeline_info;
// Queues we have seen, with the device and family they belong to (the
// family index is needed to create the readback command pool).
struct QueueInfo {
    VkDevice device = VK_NULL_HANDLE;
    uint32_t family = 0;
};
std::unordered_map<VkQueue, QueueInfo> g_queue_info;
std::vector<RecordedDraw> g_pending;

bool g_fan_warned = false;

// ---------------------------------------------------------------------------
// Device-level hook installation. Resolves a command through the real loader
// and MinHooks the first address seen per function (see FnHook above).
// ---------------------------------------------------------------------------

// Detours (defined below).
VkResult VKAPI_PTR hk_present_device(VkQueue, const VkPresentInfoKHR*);
void VKAPI_PTR hk_get_device_queue_device(VkDevice, uint32_t, uint32_t, VkQueue*);
void VKAPI_PTR hk_get_device_queue2_device(VkDevice, const VkDeviceQueueInfo2*, VkQueue*);
VkResult VKAPI_PTR hk_allocate_command_buffers_device(VkDevice,
                                                       const VkCommandBufferAllocateInfo*,
                                                       VkCommandBuffer*);
VkResult VKAPI_PTR hk_begin_command_buffer(VkCommandBuffer, const VkCommandBufferBeginInfo*);
VkResult VKAPI_PTR hk_reset_command_buffer(VkCommandBuffer, VkCommandBufferResetFlags);
void VKAPI_PTR hk_cmd_bind_pipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
void VKAPI_PTR hk_cmd_bind_vertex_buffers(VkCommandBuffer, uint32_t, uint32_t,
                                          const VkBuffer*, const VkDeviceSize*);
void VKAPI_PTR hk_cmd_bind_vertex_buffers2(VkCommandBuffer, uint32_t, uint32_t,
                                           const VkBuffer*, const VkDeviceSize*,
                                           const VkDeviceSize*, const VkDeviceSize*);
void VKAPI_PTR hk_cmd_bind_index_buffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkIndexType);
void VKAPI_PTR hk_cmd_set_primitive_topology(VkCommandBuffer, VkPrimitiveTopology);
void VKAPI_PTR hk_cmd_draw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
void VKAPI_PTR hk_cmd_draw_indexed(VkCommandBuffer, uint32_t, uint32_t, uint32_t, int32_t,
                                   uint32_t);
VkResult VKAPI_PTR hk_create_graphics_pipelines(VkDevice, VkPipelineCache, uint32_t,
                                                const VkGraphicsPipelineCreateInfo*,
                                                const VkAllocationCallbacks*, VkPipeline*);

bool hook_device_function(VkDevice device, const char* name, void* detour, FnHook& slot) {
    if (slot.target || slot.failed || !device || !g_loader.gdpa)
        return slot.target != nullptr;
    PFN_vkVoidFunction fn = g_loader.gdpa(device, name);
    if (!fn) {
        // Missing function (e.g. vkCmdSetPrimitiveTopology on Vulkan < 1.3)
        // is not fatal; remember so we don't retry per device.
        slot.failed = true;
        return false;
    }
    void* orig = nullptr;
    if (!HookManager::instance().add(reinterpret_cast<void*>(fn), detour, &orig, name)) {
        slot.failed = true;
        return false;
    }
    slot.target = reinterpret_cast<void*>(fn);
    slot.orig   = orig;
    return true;
}

void on_new_device(VkDevice device) {
    if (!device)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    hook_device_function(device, "vkQueuePresentKHR",
                         reinterpret_cast<void*>(&hk_present_device), g_fn_present_device);
    hook_device_function(device, "vkGetDeviceQueue",
                         reinterpret_cast<void*>(&hk_get_device_queue_device),
                         g_fn_get_device_queue);
    hook_device_function(device, "vkGetDeviceQueue2",
                         reinterpret_cast<void*>(&hk_get_device_queue2_device),
                         g_fn_get_device_queue2);
    hook_device_function(device, "vkAllocateCommandBuffers",
                         reinterpret_cast<void*>(&hk_allocate_command_buffers_device),
                         g_fn_allocate_cmd_bufs);
    hook_device_function(device, "vkBeginCommandBuffer",
                         reinterpret_cast<void*>(&hk_begin_command_buffer), g_fn_begin_cmd_buf);
    hook_device_function(device, "vkResetCommandBuffer",
                         reinterpret_cast<void*>(&hk_reset_command_buffer), g_fn_reset_cmd_buf);
    hook_device_function(device, "vkCmdBindPipeline",
                         reinterpret_cast<void*>(&hk_cmd_bind_pipeline), g_fn_bind_pipeline);
    hook_device_function(device, "vkCmdBindVertexBuffers",
                         reinterpret_cast<void*>(&hk_cmd_bind_vertex_buffers),
                         g_fn_bind_vertex_buffers);
    hook_device_function(device, "vkCmdBindVertexBuffers2",
                         reinterpret_cast<void*>(&hk_cmd_bind_vertex_buffers2),
                         g_fn_bind_vertex_buffers2);
    hook_device_function(device, "vkCmdBindIndexBuffer",
                         reinterpret_cast<void*>(&hk_cmd_bind_index_buffer),
                         g_fn_bind_index_buffer);
    hook_device_function(device, "vkCmdSetPrimitiveTopology",
                         reinterpret_cast<void*>(&hk_cmd_set_primitive_topology),
                         g_fn_set_topology);
    hook_device_function(device, "vkCmdDraw",
                         reinterpret_cast<void*>(&hk_cmd_draw), g_fn_draw);
    hook_device_function(device, "vkCmdDrawIndexed",
                         reinterpret_cast<void*>(&hk_cmd_draw_indexed), g_fn_draw_indexed);
    hook_device_function(device, "vkCreateGraphicsPipelines",
                         reinterpret_cast<void*>(&hk_create_graphics_pipelines),
                         g_fn_create_pipelines);
}

void record_queue(VkDevice device, uint32_t family, VkQueue* queue) {
    if (queue && *queue) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_queue_info[*queue] = QueueInfo{device, family};
    }
}

// ---------------------------------------------------------------------------
// Recording-time detours. Orig pointers are the MinHook trampolines.
// ---------------------------------------------------------------------------

using BeginCommandBufferFn = VkResult(VKAPI_PTR*)(VkCommandBuffer,
                                                  const VkCommandBufferBeginInfo*);
using ResetCommandBufferFn = VkResult(VKAPI_PTR*)(VkCommandBuffer, VkCommandBufferResetFlags);
using AllocateCommandBuffersFn =
    VkResult(VKAPI_PTR*)(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*);
using BindPipelineFn = void(VKAPI_PTR*)(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
using BindVertexBuffersFn = void(VKAPI_PTR*)(VkCommandBuffer, uint32_t, uint32_t,
                                             const VkBuffer*, const VkDeviceSize*);
using BindVertexBuffers2Fn = void(VKAPI_PTR*)(VkCommandBuffer, uint32_t, uint32_t,
                                              const VkBuffer*, const VkDeviceSize*,
                                              const VkDeviceSize*, const VkDeviceSize*);
using BindIndexBufferFn = void(VKAPI_PTR*)(VkCommandBuffer, VkBuffer, VkDeviceSize, VkIndexType);
using SetPrimitiveTopologyFn = void(VKAPI_PTR*)(VkCommandBuffer, VkPrimitiveTopology);
using CmdDrawFn = void(VKAPI_PTR*)(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
using CmdDrawIndexedFn = void(VKAPI_PTR*)(VkCommandBuffer, uint32_t, uint32_t, uint32_t, int32_t,
                                          uint32_t);
using CreateGraphicsPipelinesFn =
    VkResult(VKAPI_PTR*)(VkDevice, VkPipelineCache, uint32_t,
                          const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*,
                          VkPipeline*);
using GetDeviceQueueFn = void(VKAPI_PTR*)(VkDevice, uint32_t, uint32_t, VkQueue*);
using GetDeviceQueue2Fn = void(VKAPI_PTR*)(VkDevice, const VkDeviceQueueInfo2*, VkQueue*);

VkResult VKAPI_PTR hk_begin_command_buffer(VkCommandBuffer cb,
                                           const VkCommandBufferBeginInfo* info) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_cmd_state.erase(cb);  // fresh recording session
    }
    return reinterpret_cast<BeginCommandBufferFn>(g_fn_begin_cmd_buf.orig)(cb, info);
}

VkResult VKAPI_PTR hk_reset_command_buffer(VkCommandBuffer cb, VkCommandBufferResetFlags flags) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_cmd_state.erase(cb);
    }
    return reinterpret_cast<ResetCommandBufferFn>(g_fn_reset_cmd_buf.orig)(cb, flags);
}

VkResult VKAPI_PTR hk_allocate_command_buffers_device(VkDevice device,
                                                      const VkCommandBufferAllocateInfo* info,
                                                      VkCommandBuffer* out) {
    const VkResult r =
        reinterpret_cast<AllocateCommandBuffersFn>(g_fn_allocate_cmd_bufs.orig)(device, info,
                                                                                out);
    if (r == VK_SUCCESS && info && out) {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (uint32_t i = 0; i < info->commandBufferCount; ++i)
            if (out[i])
                g_cmd_device[out[i]] = device;
    }
    return r;
}

void note_unsupported_topology() {
    if (!g_fan_warned) {
        g_fan_warned = true;
        log::warn("vulkan: TRIANGLE_FAN / patch draws are not captured (unsupported)");
    }
}

void VKAPI_PTR hk_cmd_bind_pipeline(VkCommandBuffer cb, VkPipelineBindPoint point,
                                    VkPipeline pipeline) {
    if (point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        std::lock_guard<std::mutex> lock(g_mutex);
        CmdState& st = g_cmd_state[cb];
        const auto it = g_pipeline_info.find(pipeline);
        if (it != g_pipeline_info.end()) {
            st.topology  = it->second.topology;
            st.vb_stride = it->second.stride;  // 0 = dynamic (BindVertexBuffers2)
        }
    }
    reinterpret_cast<BindPipelineFn>(g_fn_bind_pipeline.orig)(cb, point, pipeline);
}

void VKAPI_PTR hk_cmd_bind_vertex_buffers(VkCommandBuffer cb, uint32_t first, uint32_t count,
                                         const VkBuffer* buffers, const VkDeviceSize* offsets) {
    if (first == 0 && count > 0 && buffers) {
        std::lock_guard<std::mutex> lock(g_mutex);
        CmdState& st = g_cmd_state[cb];
        st.vb        = buffers[0];
        st.vb_offset = offsets ? offsets[0] : 0;
    }
    reinterpret_cast<BindVertexBuffersFn>(g_fn_bind_vertex_buffers.orig)(cb, first, count,
                                                                        buffers, offsets);
}

void VKAPI_PTR hk_cmd_bind_vertex_buffers2(VkCommandBuffer cb, uint32_t first, uint32_t count,
                                          const VkBuffer* buffers, const VkDeviceSize* offsets,
                                          const VkDeviceSize* sizes,
                                          const VkDeviceSize* strides) {
    if (first == 0 && count > 0 && buffers) {
        std::lock_guard<std::mutex> lock(g_mutex);
        CmdState& st = g_cmd_state[cb];
        st.vb        = buffers[0];
        st.vb_offset = offsets ? offsets[0] : 0;
        if (strides)
            st.vb_stride = static_cast<uint32_t>(strides[0]);  // dynamic stride
    }
    reinterpret_cast<BindVertexBuffers2Fn>(g_fn_bind_vertex_buffers2.orig)(
        cb, first, count, buffers, offsets, sizes, strides);
}

void VKAPI_PTR hk_cmd_bind_index_buffer(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset,
                                        VkIndexType type) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        CmdState& st = g_cmd_state[cb];
        st.ib        = buffer;
        st.ib_offset = offset;
        st.ib_type   = type;
    }
    reinterpret_cast<BindIndexBufferFn>(g_fn_bind_index_buffer.orig)(cb, buffer, offset, type);
}

void VKAPI_PTR hk_cmd_set_primitive_topology(VkCommandBuffer cb, VkPrimitiveTopology topo) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_cmd_state[cb].topology = topo;
    }
    reinterpret_cast<SetPrimitiveTopologyFn>(g_fn_set_topology.orig)(cb, topo);
}

bool topology_supported(VkPrimitiveTopology t) {
    switch (t) {
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
        case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
        case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
        case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
            return true;
        default:
            note_unsupported_topology();
            return false;
    }
}

fmt::PrimitiveTopology map_topology(VkPrimitiveTopology t) {
    switch (t) {
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return fmt::PrimitiveTopology::TriangleStrip;
        case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:      return fmt::PrimitiveTopology::LineList;
        case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:     return fmt::PrimitiveTopology::LineStrip;
        case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:      return fmt::PrimitiveTopology::PointList;
        default:                                    return fmt::PrimitiveTopology::TriangleList;
    }
}

// Snapshot the draw + bound state while a capture frame is open.
void record_draw(VkCommandBuffer cb, uint32_t index_count, uint32_t first_index,
                 int32_t vertex_offset, uint32_t vertex_count, uint32_t first_vertex) {
    CaptureContext& cap = CaptureContext::instance();
    if (!cap.capturing())
        return;
    if (index_count == 0 && vertex_count == 0)
        return;  // empty draw

    CmdState st;
    VkDevice device;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_cmd_state.find(cb);
        if (it == g_cmd_state.end())
            return;
        st = it->second;
        auto dit = g_cmd_device.find(cb);
        if (dit == g_cmd_device.end())
            return;
        device = dit->second;
    }

    if (st.vb == VK_NULL_HANDLE || st.vb_stride == 0)
        return;  // procedural / dynamic-stride draws: nothing to capture
    if (!topology_supported(st.topology))
        return;

    RecordedDraw d;
    d.device        = device;
    d.topology      = st.topology;
    d.vb            = st.vb;
    d.vb_offset     = st.vb_offset;
    d.vb_stride     = st.vb_stride;
    d.index_count   = index_count;
    d.first_index   = first_index;
    d.vertex_offset = vertex_offset;
    d.vertex_count  = vertex_count;
    d.first_vertex  = first_vertex;

    if (index_count > 0) {
        if (st.ib == VK_NULL_HANDLE ||
            (st.ib_type != VK_INDEX_TYPE_UINT16 && st.ib_type != VK_INDEX_TYPE_UINT32))
            return;
        d.ib      = st.ib;
        d.ib_elem = (st.ib_type == VK_INDEX_TYPE_UINT32) ? 4u : 2u;
        // The binding offset stays constant; firstIndex slides inside it.
        d.ib_offset = st.ib_offset + static_cast<uint64_t>(first_index) * d.ib_elem;
        d.ib_size   = static_cast<uint64_t>(index_count) * d.ib_elem;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_pending.push_back(d);
}

void VKAPI_PTR hk_cmd_draw(VkCommandBuffer cb, uint32_t vertex_count, uint32_t instance_count,
                           uint32_t first_vertex, uint32_t first_instance) {
    record_draw(cb, 0, 0, 0, vertex_count, first_vertex);
    reinterpret_cast<CmdDrawFn>(g_fn_draw.orig)(cb, vertex_count, instance_count, first_vertex,
                                               first_instance);
}

void VKAPI_PTR hk_cmd_draw_indexed(VkCommandBuffer cb, uint32_t index_count,
                                   uint32_t instance_count, uint32_t first_index,
                                   int32_t vertex_offset, uint32_t first_instance) {
    // Base geometry only; per-instance data needs GRMF v2 (docs/ROADMAP.md).
    record_draw(cb, index_count, first_index, vertex_offset, 0, 0);
    reinterpret_cast<CmdDrawIndexedFn>(g_fn_draw_indexed.orig)(cb, index_count, instance_count,
                                                              first_index, vertex_offset,
                                                              first_instance);
}

VkResult VKAPI_PTR hk_create_graphics_pipelines(VkDevice device, VkPipelineCache cache,
                                                uint32_t count,
                                                const VkGraphicsPipelineCreateInfo* infos,
                                                const VkAllocationCallbacks* alloc,
                                                VkPipeline* out) {
    const VkResult r = reinterpret_cast<CreateGraphicsPipelinesFn>(g_fn_create_pipelines.orig)(
        device, cache, count, infos, alloc, out);
    if (r == VK_SUCCESS && infos && out) {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (uint32_t i = 0; i < count; ++i) {
            if (!out[i])
                continue;
            PipelineInfo info{};
            if (infos[i].pInputAssemblyState)
                info.topology = infos[i].pInputAssemblyState->topology;
            // Binding 0's stride comes from the pipeline's static vertex
            // input state; dynamic vertex input leaves it 0 (skipped).
            if (infos[i].pVertexInputState) {
                const auto* vi = infos[i].pVertexInputState;
                for (uint32_t b = 0; b < vi->vertexBindingDescriptionCount; ++b) {
                    if (vi->pVertexBindingDescriptions[b].binding == 0) {
                        info.stride = vi->pVertexBindingDescriptions[b].stride;
                        break;
                    }
                }
            }
            g_pipeline_info[out[i]] = info;
        }
    }
    return r;
}

// Queue discovery detours (gdpa-resolved pointers; the export versions live in
// the install section and share record_queue).
void VKAPI_PTR hk_get_device_queue_device(VkDevice device, uint32_t family, uint32_t index,
                                         VkQueue* queue) {
    reinterpret_cast<GetDeviceQueueFn>(g_fn_get_device_queue.orig)(device, family, index, queue);
    record_queue(device, family, queue);
}

void VKAPI_PTR hk_get_device_queue2_device(VkDevice device, const VkDeviceQueueInfo2* info,
                                          VkQueue* queue) {
    reinterpret_cast<GetDeviceQueue2Fn>(g_fn_get_device_queue2.orig)(device, info, queue);
    record_queue(device, info ? info->queueFamilyIndex : 0, queue);
}

// ---------------------------------------------------------------------------
// Flush: staging copy on the presenting queue + .grm write-out.
// ---------------------------------------------------------------------------

// Unhooked device plumbing, resolved through the real loader. Our hooked
// commands are called through their saved trampolines so capture state stays
// untouched by our own bookkeeping calls.
struct DeviceVulkan {
    PFN_vkGetBufferMemoryRequirements get_buffer_mem_req   = nullptr;
    PFN_vkCreateBuffer               create_buffer          = nullptr;
    PFN_vkAllocateMemory            allocate_memory         = nullptr;
    PFN_vkBindBufferMemory          bind_buffer_memory      = nullptr;
    PFN_vkMapMemory                 map_memory              = nullptr;
    PFN_vkUnmapMemory               unmap_memory            = nullptr;
    PFN_vkFreeMemory                free_memory             = nullptr;
    PFN_vkDestroyBuffer             destroy_buffer          = nullptr;
    PFN_vkCreateCommandPool         create_command_pool     = nullptr;
    PFN_vkDestroyCommandPool       destroy_command_pool    = nullptr;
    PFN_vkFreeCommandBuffers       free_command_buffers    = nullptr;
    PFN_vkEndCommandBuffer         end_command_buffer      = nullptr;
    PFN_vkCmdCopyBuffer            cmd_copy_buffer         = nullptr;
    PFN_vkQueueSubmit              queue_submit            = nullptr;
    PFN_vkCreateFence              create_fence            = nullptr;
    PFN_vkWaitForFences             wait_for_fences         = nullptr;
    PFN_vkDestroyFence             destroy_fence           = nullptr;
};

#define VKDEV(device, name) \
    reinterpret_cast<PFN_vk##name>(g_loader.gdpa(device, "vk" #name))

bool resolve_device_plumbing(VkDevice device, DeviceVulkan& vk) {
    if (!device || !g_loader.gdpa)
        return false;
    vk.get_buffer_mem_req   = VKDEV(device, GetBufferMemoryRequirements);
    vk.create_buffer        = VKDEV(device, CreateBuffer);
    vk.allocate_memory      = VKDEV(device, AllocateMemory);
    vk.bind_buffer_memory   = VKDEV(device, BindBufferMemory);
    vk.map_memory           = VKDEV(device, MapMemory);
    vk.unmap_memory         = VKDEV(device, UnmapMemory);
    vk.free_memory          = VKDEV(device, FreeMemory);
    vk.destroy_buffer       = VKDEV(device, DestroyBuffer);
    vk.create_command_pool  = VKDEV(device, CreateCommandPool);
    vk.destroy_command_pool = VKDEV(device, DestroyCommandPool);
    vk.free_command_buffers = VKDEV(device, FreeCommandBuffers);
    vk.end_command_buffer   = VKDEV(device, EndCommandBuffer);
    vk.cmd_copy_buffer      = VKDEV(device, CmdCopyBuffer);
    vk.queue_submit         = VKDEV(device, QueueSubmit);
    vk.create_fence         = VKDEV(device, CreateFence);
    vk.wait_for_fences      = VKDEV(device, WaitForFences);
    vk.destroy_fence        = VKDEV(device, DestroyFence);
    return vk.get_buffer_mem_req && vk.create_buffer && vk.allocate_memory &&
           vk.bind_buffer_memory && vk.map_memory && vk.unmap_memory && vk.free_memory &&
           vk.destroy_buffer && vk.create_command_pool && vk.destroy_command_pool &&
           vk.free_command_buffers && vk.end_command_buffer && vk.cmd_copy_buffer &&
           vk.queue_submit && vk.create_fence && vk.wait_for_fences && vk.destroy_fence;
}

struct RegionKey {
    VkBuffer buffer;
    uint64_t offset, size;
    bool operator<(const RegionKey& o) const {
        return buffer != o.buffer ? buffer < o.buffer
                                  : offset != o.offset ? offset < o.offset
                                                       : size < o.size;
    }
};

void write_staging_draws(const std::vector<RecordedDraw>& draws,
                         const std::map<RegionKey, uint64_t>& regions,
                         const uint8_t* mapped);

// Present / loader wrappers (defined below; referenced by the hooks above).
PFN_vkVoidFunction VKAPI_PTR hk_get_device_proc_addr(VkDevice, const char*);

// Find a mappable (host-visible) memory type among `bits` by trial mapping —
// without a VkPhysicalDevice we can't read memory properties directly, and
// vkMapMemory only ever succeeds on host-visible types.
bool find_mappable_memory(VkDevice device, const DeviceVulkan& vk, uint32_t bits,
                          uint64_t alloc_size, VkDeviceMemory* out_mem, void** out_ptr,
                          uint32_t* out_type) {
    for (uint32_t t = 0; t < 32; ++t) {
        if ((bits & (1u << t)) == 0)
            continue;
        VkMemoryAllocateInfo ai{};
        ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize  = alloc_size;
        ai.memoryTypeIndex = t;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (vk.allocate_memory(device, &ai, nullptr, &mem) != VK_SUCCESS)
            continue;
        void* ptr = nullptr;
        if (vk.map_memory(device, mem, 0, VK_WHOLE_SIZE, 0, &ptr) == VK_SUCCESS) {
            *out_mem  = mem;
            *out_ptr  = ptr;
            *out_type = t;
            return true;
        }
        vk.free_memory(device, mem, nullptr);
    }
    return false;
}

bool flush_pending_draws(VkQueue queue) {
    std::vector<RecordedDraw> draws;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        draws = std::move(g_pending);
        g_pending.clear();
        auto qit = g_queue_info.find(queue);
        if (qit != g_queue_info.end()) {
            device = qit->second.device;
            queue_family = qit->second.family;
        }
        for (auto it = draws.begin(); it != draws.end();) {
            if (device && it->device != device)
                it = draws.erase(it);  // draws from other devices: not on this queue
            else
                ++it;
        }
    }

    if (draws.empty())
        return true;
    if (!device) {
        log::warn("vulkan: %u draws pending but the presenting queue's device is unknown",
                  static_cast<unsigned>(draws.size()));
        return false;
    }

    DeviceVulkan vk;
    if (!resolve_device_plumbing(device, vk)) {
        log::error("vulkan: device plumbing could not be resolved");
        return false;
    }

    // Dedupe regions: the whole VB per buffer, the exact IB slice per draw.
    std::map<RegionKey, uint64_t> regions;  // key -> dst offset in staging
    std::unordered_map<VkBuffer, uint64_t> vb_sizes;
    uint64_t total_bytes = 0;
    bool overflowed = false;
    for (RecordedDraw& d : draws) {
        if (!vb_sizes.count(d.vb)) {
            VkMemoryRequirements req{};
            vk.get_buffer_mem_req(device, d.vb, &req);
            if (req.size == 0)
                req.size = 1;  // avoid zero-sized copies
            vb_sizes[d.vb] = req.size;
        }
        d.vb_size = vb_sizes[d.vb];

        RegionKey vb_key{d.vb, 0, d.vb_size};
        if (!regions.count(vb_key)) {
            if (total_bytes + d.vb_size > 0xFFFFFFFFull) {
                overflowed = true;
                d.vb_size = 0;  // skip this draw at write time
            } else {
                regions[vb_key] = total_bytes;
                total_bytes += d.vb_size;
            }
        }
        if (d.index_count > 0) {
            RegionKey ib_key{d.ib, d.ib_offset, d.ib_size};
            if (!regions.count(ib_key)) {
                if (total_bytes + d.ib_size > 0xFFFFFFFFull) {
                    overflowed = true;
                    d.index_count = 0;
                    d.ib = VK_NULL_HANDLE;
                } else {
                    regions[ib_key] = total_bytes;
                    total_bytes += d.ib_size;
                }
            }
        }
    }
    if (overflowed)
        log::warn("vulkan: staging overflow (>4GB), some draws were skipped");
    if (total_bytes == 0)
        return false;

    // Staging buffer + mappable memory.
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size  = total_bytes;
    bci.usage  = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer staging = VK_NULL_HANDLE;
    if (vk.create_buffer(device, &bci, nullptr, &staging) != VK_SUCCESS) {
        log::error("vulkan: staging buffer creation failed");
        return false;
    }
    VkMemoryRequirements req{};
    vk.get_buffer_mem_req(device, staging, &req);

    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    void* mapped = nullptr;
    uint32_t type_index = 0;
    if (!find_mappable_memory(device, vk, req.memoryTypeBits, req.size, &staging_mem,
                              &mapped, &type_index) ||
        vk.bind_buffer_memory(device, staging, staging_mem, 0) != VK_SUCCESS) {
        log::error("vulkan: no mappable memory for the staging buffer");
        if (staging_mem)
            vk.free_memory(device, staging_mem, nullptr);
        vk.destroy_buffer(device, staging, nullptr);
        return false;
    }



    // One-time copy command buffer on the presenting queue (the frame's
    // submissions precede it, so the sources hold final data).
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    // The pool must belong to the family that owns the presenting queue, or
    // the copy commands would not be valid on it.
    pci.queueFamilyIndex = queue_family;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vk.create_command_pool(device, &pci, nullptr, &pool) != VK_SUCCESS) {
        log::error("vulkan: command pool creation failed");
        vk.unmap_memory(device, staging_mem);
        vk.free_memory(device, staging_mem, nullptr);
        vk.destroy_buffer(device, staging, nullptr);
        return false;
    }
    VkCommandBufferAllocateInfo cai{};
    cai.sType               = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool         = pool;
    cai.level               = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount  = 1;
    // Through the saved trampolines: our own recording must not disturb state.
    if (reinterpret_cast<AllocateCommandBuffersFn>(g_fn_allocate_cmd_bufs.orig)(device, &cai,
                                                                                &cb) !=
            VK_SUCCESS ||
        !cb) {
        log::error("vulkan: copy command buffer allocation failed");
        vk.destroy_command_pool(device, pool, nullptr);
        vk.unmap_memory(device, staging_mem);
        vk.free_memory(device, staging_mem, nullptr);
        vk.destroy_buffer(device, staging, nullptr);
        return false;
    }
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (reinterpret_cast<BeginCommandBufferFn>(g_fn_begin_cmd_buf.orig)(cb, &bi) != VK_SUCCESS) {
        log::error("vulkan: copy command buffer begin failed");
        vk.destroy_command_pool(device, pool, nullptr);
        vk.unmap_memory(device, staging_mem);
        vk.free_memory(device, staging_mem, nullptr);
        vk.destroy_buffer(device, staging, nullptr);
        return false;
    }
    for (const auto& kv : regions) {
        VkBufferCopy region{};
        region.srcOffset = kv.first.offset;
        region.dstOffset = kv.second;
        region.size      = kv.first.size;
        vk.cmd_copy_buffer(cb, kv.first.buffer, staging, 1, &region);
    }
    if (vk.end_command_buffer(cb) != VK_SUCCESS) {
        log::error("vulkan: copy command buffer end failed");
        vk.destroy_command_pool(device, pool, nullptr);
        vk.unmap_memory(device, staging_mem);
        vk.free_memory(device, staging_mem, nullptr);
        vk.destroy_buffer(device, staging, nullptr);
        return false;
    }

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vk.create_fence(device, &fi, nullptr, &fence);
    if (fence) {
        VkSubmitInfo si{};
        si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers    = &cb;
        vk.queue_submit(queue, 1, &si, fence);
        vk.wait_for_fences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        vk.destroy_fence(device, fence, nullptr);
    }

    write_staging_draws(draws, regions, static_cast<const uint8_t*>(mapped));

    vk.free_command_buffers(device, pool, 1, &cb);
    vk.destroy_command_pool(device, pool, nullptr);
    vk.unmap_memory(device, staging_mem);
    vk.free_memory(device, staging_mem, nullptr);
    vk.destroy_buffer(device, staging, nullptr);
    return true;
}

bool flush_pending_draws(VkQueue queue);

// Turn the mapped staging bytes into one .grm per recorded draw.
void write_staging_draws(const std::vector<RecordedDraw>& draws,
                         const std::map<RegionKey, uint64_t>& regions,
                         const uint8_t* mapped) {
    CaptureContext& cap = CaptureContext::instance();
    unsigned written = 0;

    for (const RecordedDraw& d : draws) {
        if (d.vb_size == 0)
            continue;  // overflowed region
        const auto vb_it = regions.find(RegionKey{d.vb, 0, d.vb_size});
        if (vb_it == regions.end())
            continue;
        const uint8_t* vb_base = mapped + vb_it->second;

        MeshData mesh;
        mesh.vertex_stride = d.vb_stride;
        mesh.attributes    = guess_vertex_layout(d.vb_stride);
        mesh.topology      = map_topology(d.topology);

        if (d.index_count > 0) {
            const auto ib_it = regions.find(RegionKey{d.ib, d.ib_offset, d.ib_size});
            if (d.ib == VK_NULL_HANDLE || ib_it == regions.end())
                continue;
            const uint8_t* ib_base = mapped + ib_it->second;

            // Vulkan fetches at bindingOffset + (idx + vertexOffset) * stride.
            // The staging copy holds the whole buffer, so effective vertex
            // indices are buffer-global; the slice is trimmed to the range
            // the indices actually reference, exactly like the D3D12 path.
            if (d.vb_offset % d.vb_stride != 0)
                continue;  // unaligned binding: vertex boundary undefined
            const int64_t binding_vertex = static_cast<int64_t>(d.vb_offset / d.vb_stride);

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
                const int64_t eff = binding_vertex + static_cast<int64_t>(idx) +
                                    d.vertex_offset;
                if (eff < 0 || static_cast<uint64_t>(eff) * d.vb_stride >= d.vb_size)
                    continue;  // out-of-range index
                min_eff = std::min(min_eff, eff);
                max_eff = std::max(max_eff, eff);
            }
            if (min_eff > max_eff)
                continue;

            const uint64_t begin = static_cast<uint64_t>(min_eff) * d.vb_stride;
            uint64_t bytes = static_cast<uint64_t>(max_eff - min_eff + 1) * d.vb_stride;
            if (begin + bytes > d.vb_size)
                bytes = d.vb_size - begin;
            mesh.vertex_data      = vb_base + begin;
            mesh.vertex_data_size = static_cast<uint32_t>(bytes);
            mesh.vertex_count     = static_cast<uint32_t>(bytes / d.vb_stride);
            mesh.index_data       = ib_base;
            mesh.index_data_size  = static_cast<uint32_t>(d.index_count) * d.ib_elem;
            mesh.index_count      = d.index_count;
            mesh.index_format     = (d.ib_elem == 2) ? fmt::IndexFormat::UInt16
                                                     : fmt::IndexFormat::UInt32;
            mesh.start_index      = d.first_index;
            mesh.base_vertex      = d.vertex_offset + static_cast<int32_t>(binding_vertex) -
                                   static_cast<int32_t>(min_eff);
        } else {
            // Non-indexed: the exact vertex range [firstVertex, +count).
            const uint64_t begin = d.vb_offset +
                                   static_cast<uint64_t>(d.first_vertex) * d.vb_stride;
            const uint64_t bytes = static_cast<uint64_t>(d.vertex_count) * d.vb_stride;
            if (begin + bytes > d.vb_size)
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
            log::info("vulkan captured %s (%u verts, %u indices)", name, mesh.vertex_count,
                      mesh.index_count);
        } else {
            log::error("vulkan write %s failed: %s", name, err.c_str());
        }
    }
    log::info("vulkan frame flushed: %u/%u draws written", written,
              static_cast<unsigned>(draws.size()));
}


// ---------------------------------------------------------------------------
// Present (frame boundary) + loader wrappers
// ---------------------------------------------------------------------------

void on_present(VkQueue queue) {
    // The frame's command buffers were submitted before the present; drain
    // the recorded draws while the capture frame is still open.
    if (CaptureContext::instance().capturing())
        flush_pending_draws(queue);
    CaptureContext::instance().on_frame_boundary();
}

VkResult VKAPI_PTR hk_present_device(VkQueue queue, const VkPresentInfoKHR* info) {
    on_present(queue);
    return reinterpret_cast<VkResult(VKAPI_PTR*)(VkQueue, const VkPresentInfoKHR*)>(
        g_fn_present_device.orig)(queue, info);
}

VkResult VKAPI_PTR hk_present_export(VkQueue queue, const VkPresentInfoKHR* info) {
    on_present(queue);
    return g_orig_present_export(queue, info);
}

// The loader wrappers: hand out ourselves for the proc-address functions and
// learn each device the moment the app resolves any command for it (the
// resolved pointer is already MinHooked by then, so no rewrapping is needed).
PFN_vkVoidFunction VKAPI_PTR hk_get_instance_proc_addr(VkInstance instance, const char* name) {
    if (name) {
        if (std::strcmp(name, "vkGetInstanceProcAddr") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(&hk_get_instance_proc_addr);
        if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(&hk_get_device_proc_addr);
    }
    return g_orig_gipa(instance, name);
}

PFN_vkVoidFunction VKAPI_PTR hk_get_device_proc_addr(VkDevice device, const char* name) {
    on_new_device(device);
    return g_orig_gdpa(device, name);
}

// Export-level queue/command-buffer discovery (apps that call exports).
void VKAPI_PTR hk_get_device_queue_export(VkDevice device, uint32_t family, uint32_t index,
                                          VkQueue* queue) {
    on_new_device(device);
    g_orig_get_device_queue_export(device, family, index, queue);
    record_queue(device, family, queue);
}

VkResult VKAPI_PTR hk_allocate_command_buffers_export(VkDevice device,
                                                      const VkCommandBufferAllocateInfo* info,
                                                      VkCommandBuffer* out) {
    on_new_device(device);
    const VkResult r = g_orig_allocate_cmd_bufs_export(device, info, out);
    if (r == VK_SUCCESS && info && out) {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (uint32_t i = 0; i < info->commandBufferCount; ++i)
            if (out[i])
                g_cmd_device[out[i]] = device;
    }
    return r;
}


} // namespace



bool available() {
    return GetModuleHandleA("vulkan-1.dll") != nullptr;
}

bool install() {
    if (g_installed)
        return true;
    if (!available())
        return false;

    g_loader.module = GetModuleHandleA("vulkan-1.dll");
    g_loader.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(g_loader.module, "vkGetInstanceProcAddr"));
    g_loader.gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        GetProcAddress(g_loader.module, "vkGetDeviceProcAddr"));
    if (!g_loader.gipa || !g_loader.gdpa) {
        log::error("vulkan: loader exports not found");
        return false;
    }

    HookManager& hooks = HookManager::instance();
    bool ok = true;

    // Loader entry points: every path an app can take to reach a device-level
    // function pointer funnels through one of these.
    ok &= hooks.add(reinterpret_cast<void*>(g_loader.gipa),
                    reinterpret_cast<void*>(&hk_get_instance_proc_addr),
                    reinterpret_cast<void**>(&g_orig_gipa), "vkGetInstanceProcAddr");
    ok &= hooks.add(reinterpret_cast<void*>(g_loader.gdpa),
                    reinterpret_cast<void*>(&hk_get_device_proc_addr),
                    reinterpret_cast<void**>(&g_orig_gdpa), "vkGetDeviceProcAddr");

    if (auto gdq = reinterpret_cast<void*>(GetProcAddress(g_loader.module, "vkGetDeviceQueue")))
        ok &= hooks.add(gdq, reinterpret_cast<void*>(&hk_get_device_queue_export),
                        reinterpret_cast<void**>(&g_orig_get_device_queue_export),
                        "vkGetDeviceQueue (export)");
    if (auto acb = reinterpret_cast<void*>(
            GetProcAddress(g_loader.module, "vkAllocateCommandBuffers")))
        ok &= hooks.add(acb, reinterpret_cast<void*>(&hk_allocate_command_buffers_export),
                        reinterpret_cast<void**>(&g_orig_allocate_cmd_bufs_export),
                        "vkAllocateCommandBuffers (export)");
    if (auto pres = reinterpret_cast<void*>(GetProcAddress(g_loader.module,
                                                           "vkQueuePresentKHR")))
        ok &= hooks.add(pres, reinterpret_cast<void*>(&hk_present_export),
                        reinterpret_cast<void**>(&g_orig_present_export),
                        "vkQueuePresentKHR (export)");

    g_installed = ok;
    if (ok) {
        CaptureContext::instance().set_api_name("vulkan");
        log::info("vulkan hooks installed (device commands hook on first use)");
    }
    return ok;
}

} // namespace vulkan
} // namespace ghost

