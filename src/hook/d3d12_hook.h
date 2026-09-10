// Direct3D 12 hooking (scaffolding).
//
// D3D12 capture is harder than D3D11: command lists are recorded on the CPU
// and executed asynchronously, buffers live in upload/default heaps, and the
// same command list is reset and reused. The plan:
//   1. Hook IDXGISwapChain3::Present (frame boundary) as in D3D11.
//   2. Hook ID3D12CommandQueue::ExecuteCommandLists to observe the lists.
//   3. At capture time, snapshot the bound IA vertex/index buffers and
//      resources by copying their GPU descriptors into a readback heap, then
//      copy-mapping. (D3D12 has no live "current state" query, so we capture
//      descriptor tables seen by ExecuteCommandLists in that frame.)
//
// Only step 1 (frame-boundary detection) is implemented now so captures are
// opened/closed correctly and the manifest is written; actual buffer dump is
// TODO. See docs/ROADMAP.md.
#pragma once

namespace ghost {
namespace d3d12 {

bool available();
bool install();

} // namespace d3d12
} // namespace ghost
