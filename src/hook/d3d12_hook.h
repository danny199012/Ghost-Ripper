// Direct3D 12 hooking + draw capture.
//
// D3D12 has no live "current state" to query at Present time: command lists
// are recorded on the CPU and executed asynchronously, buffers live in
// default heaps, and the same command list is reset and reused. The capture
// therefore works in two phases:
//
//   1. Recording time. We detour the ID3D12GraphicsCommandList vtable (shared
//      by every instance) to observe what the game records: IASetVertexBuffers
//      (44), IASetIndexBuffer (43), IASetPrimitiveTopology (20) maintain a
//      small per-command-list state, and DrawInstanced (12) /
//      DrawIndexedInstanced (13) snapshot the bound views into a pending-draw
//      list while a capture frame is open. ID3D12Device::CreateCommitted-
//      Resource (27) / CreatePlacedResource (29) keep a GPU-VA -> resource
//      map so the views can be resolved to the underlying ID3D12Resource.
//      ID3D12CommandQueue::ExecuteCommandLists (10) hands us the direct
//      queue + device used for the readback.
//
//   2. Present time (frame boundary). The recorded vertex/index ranges are
//      copied into one readback heap with an internal command list on the
//      game's direct queue (buffers are implicitly state-promoted from
//      COMMON, so no barriers are needed), a fence is waited on, and every
//      pending draw is written out as a .grm file. Indexed draws are trimmed
//      to the vertex range their indices actually reference.
//
// Limitations (see docs/ROADMAP.md): draws inside bundles are recorded when
// the bundle is recorded (once, not per execution); resources created before
// the hook DLL was injected are unknown to the VA map; stream 1+ vertex
// buffers and texture capture need GRMF v2 / descriptor tracking; the buffers
// of a draw that was submitted on a *different* direct queue may not be final
// when the readback copy on the capture queue runs; the VA map and per-list
// state are never pruned (they hold one entry per distinct buffer/list for the
// lifetime of the process, which is small for a capture session).
#pragma once

namespace ghost {
namespace d3d12 {

bool available();
bool install();

} // namespace d3d12
} // namespace ghost
