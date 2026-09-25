// Vulkan hooking + draw capture.
//
// Vulkan has no import library in the Windows SDK and no COM vtables; the
// loader hands out dispatchable function pointers via vkGetInstanceProcAddr /
// vkGetDeviceProcAddr. We MinHook the *loader exports* of those two functions
// plus vkGetDeviceQueue/vkAllocateCommandBuffers to learn every VkDevice early,
// then resolve the device-level commands through the real (saved) loader
// pointers and MinHook them once per unique address — the driver serves the
// same pointer for every device of one ICD, and any call path the app uses
// (export, gipa, gdpa, dispatch table) ends in that same code.
//
// Capture works like D3D12: while a frame is open we observe command-buffer
// recording (vkCmdBindVertexBuffers(2)/vkCmdBindIndexBuffer/vkCmdBindPipeline/
// vkCmdSetPrimitiveTopology/vkCmdDraw/vkCmdDrawIndexed, plus
// vkCreateGraphicsPipelines for pipeline topologies and vkBeginCommandBuffer
// to clear per-command-buffer state). At vkQueuePresentKHR the recorded
// vertex/index ranges are copied through a staging buffer on the presenting
// queue (submit + fence wait) and written out as .grm files.
//
// Limitations (see docs/ROADMAP.md): apps that dig raw ICD pointers out of
// the dispatch table bypass the loader (rare; they also break layers); draw
// indirect is not captured; stream 1+ and texture capture need GRMF v2.
#pragma once

namespace ghost {
namespace vulkan {

bool available();
bool install();

} // namespace vulkan
} // namespace ghost
