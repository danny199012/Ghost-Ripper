// Direct3D 11 hooking: creates a dummy device to locate the vtables of
// ID3D11DeviceContext and IDXGISwapChain, then MinHooks the draw calls and
// Present. While a frame capture is active, every indexed/non-indexed draw
// dumps its bound vertex/index buffers and pixel shader textures.
#pragma once

namespace ghost {
namespace d3d11 {

// Returns true when the D3D11 runtime is present in this process (i.e. the
// game is, or may soon be, rendering with D3D11).
bool available();

// Installs hooks. Idempotent; returns true if hooks are active afterwards.
bool install();

} // namespace d3d11
} // namespace ghost
