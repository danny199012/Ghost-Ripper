// Direct3D 9 hooking: creates a dummy device to read the IDirect3DDevice9
// vtable, then MinHooks EndScene (frame boundary) and the draw calls.
// Captured vertex/index buffers are read back via the default pool copy.
#pragma once

namespace ghost {
namespace d3d9 {

bool available();
bool install();

} // namespace d3d9
} // namespace ghost
