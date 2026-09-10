// Vulkan hooking (scaffolding).
//
// We intercept vkQueuePresentKHR as the frame boundary. Because Vulkan loads
// its commands via vkGetInstanceProcAddr / vkGetDeviceProcAddr at runtime,
// we replace vkGetInstanceProcAddr with a shim that returns our wrappers for
// present and the queue submit family, while still forwarding everything else
// to the real loader. A small amount of per-device trampoline bookkeeping is
// needed because each VkDevice can have its own function pointers.
//
// Only present-based frame-boundary detection + manifest writing is wired up
// now; geometry capture is TODO. See docs/ROADMAP.md.
#pragma once

namespace ghost {
namespace vulkan {

bool available();
bool install();

} // namespace vulkan
} // namespace ghost
