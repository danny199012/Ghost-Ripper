#include "vulkan_hook.h"

#include "capture_context.h"
#include "hook_manager.h"

#include "ghost/log.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>

#include <windows.h>  // GetModuleHandleA / GetProcAddress (vulkan.h alone pulls nothing)

#include <vulkan/vulkan.h>

namespace ghost {
namespace vulkan {

namespace {

// Frame-boundary detection via vkQueuePresentKHR.
using PresentFn = VkResult(VKAPI_PTR*)(VkQueue, const VkPresentInfoKHR*);
PresentFn g_orig_present = nullptr;

// Track the per-queue present function pointer we replaced, keyed by the queue
// handle. This lets us forward correctly even though each device resolves its
// own command pointers.
std::unordered_map<VkQueue, PresentFn> g_queue_present;
bool g_installed = false;

VkResult VKAPI_PTR hk_present(VkQueue queue, const VkPresentInfoKHR* info) {
    CaptureContext::instance().on_frame_boundary();
    auto it = g_queue_present.find(queue);
    if (it != g_queue_present.end())
        return it->second(queue, info);
    // Should not happen: we only hook queues we recorded.
    return VK_ERROR_INITIALIZATION_FAILED;
}

// Hook vkGetDeviceQueue so we can resolve the per-queue present pointer for
// every queue the application creates, then MinHook that function.
using GetDeviceQueueFn = void (VKAPI_PTR*)(VkDevice, uint32_t, uint32_t, VkQueue*);
GetDeviceQueueFn g_orig_get_device_queue = nullptr;

void VKAPI_PTR hk_get_device_queue(VkDevice device, uint32_t family, uint32_t index,
                                   VkQueue* queue) {
    g_orig_get_device_queue(device, family, index, queue);
    if (!queue || !*queue)
        return;

    auto cmd = reinterpret_cast<PresentFn>(
        vkGetDeviceProcAddr(device, "vkQueuePresentKHR"));
    if (!cmd)
        return;

    if (g_queue_present.count(*queue) == 0) {
        void* orig = nullptr;
        if (HookManager::instance().add(reinterpret_cast<void*>(cmd),
                                        reinterpret_cast<void*>(&hk_present),
                                        &orig, "vkQueuePresentKHR")) {
            g_queue_present[*queue] = reinterpret_cast<PresentFn>(orig);
        }
    }
}

// Replace the exported vkGetInstanceProcAddr so the loader hands our wrappers
// to the application for the functions we care about.
using GetInstanceProcAddrFn = PFN_vkVoidFunction(VKAPI_PTR*)(VkInstance, const char*);
GetInstanceProcAddrFn g_orig_gipa = nullptr;

PFN_vkVoidFunction VKAPI_PTR hk_get_instance_proc_addr(VkInstance instance, const char* name) {
    if (name && std::strcmp(name, "vkGetInstanceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&hk_get_instance_proc_addr);
    return g_orig_gipa(instance, name);
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

    // The simplest reliable entry point for Vulkan interception is to hook the
    // loader's exported vkGetInstanceProcAddr. We then wrap vkGetDeviceQueue so
    // each created queue gets its present hooked.
    HMODULE vulkan = GetModuleHandleA("vulkan-1.dll");
    auto gipa = reinterpret_cast<void*>(GetProcAddress(vulkan, "vkGetInstanceProcAddr"));
    if (!gipa) {
        log::error("vulkan: vkGetInstanceProcAddr export not found");
        return false;
    }

    bool ok = HookManager::instance().add(
        gipa, reinterpret_cast<void*>(&hk_get_instance_proc_addr),
        reinterpret_cast<void**>(&g_orig_gipa), "vkGetInstanceProcAddr");

    // We also want to catch queues created before the loader returns to the
    // app; best-effort hook of vkGetDeviceQueue via the instance address.
    auto gdq = reinterpret_cast<void*>(GetProcAddress(vulkan, "vkGetDeviceQueue"));
    if (gdq)
        ok &= HookManager::instance().add(
            gdq, reinterpret_cast<void*>(&hk_get_device_queue),
            reinterpret_cast<void**>(&g_orig_get_device_queue), "vkGetDeviceQueue");

    g_installed = ok;
    if (ok) {
        CaptureContext::instance().set_api_name("vulkan");
        log::info("vulkan frame-boundary hook installed (capture TODO)");
    }
    return ok;
}

} // namespace vulkan
} // namespace ghost
