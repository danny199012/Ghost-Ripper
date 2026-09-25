# Changelog

## v0.2 — complete capture pipeline + viewer

### Capture (hook DLL)

- **D3D12 draw capture** implemented: command-list recording hooks
  (`DrawInstanced`, `DrawIndexedInstanced`, IA bindings, topology), buffer
  GPU-VA tracking via `CreateCommittedResource`/`CreatePlacedResource`, and a
  readback-heap copy at `Present` executed on the game's own direct queue
  (implicit state promotion — no barriers). Indexed draws are trimmed to the
  vertex range their indices reference.
- **Vulkan draw capture** implemented: loader-level device discovery
  (`vkGetInstanceProcAddr`/`vkGetDeviceProcAddr` +
  `vkGetDeviceQueue`/`vkAllocateCommandBuffers`/`vkQueuePresentKHR` exports),
  MinHook on the first resolved address of each command (draw/bind/pipeline/
  topology), per-command-buffer state tracking, and a staging-buffer readback
  on the presenting queue. No `vulkan-1` import library is needed; everything
  is resolved dynamically (this also fixes the unresolved
  `vkGetDeviceProcAddr` link error).
- **D3D9 texture dump** implemented: texture stages 0-7 → DDS (`LockRect`),
  covering 32bpp A8R8G8B8/X8R8G8B8/A8B8G8R8 and DXT1/2/3/4/5.
- **Fixed captured vertex windows**: D3D9 now locks `[MinVertexIndex,
  MinVertexIndex+NumVertices)` and stores `base_vertex = BaseVertexIndex −
  MinVertexIndex`; the viewer applies `base_vertex` (previously ignored) when
  expanding indices, so sliced captures render with correct indices.
- Fixed three latent compile errors found by a MinGW cross-check: the
  `DrawIndexedInstanced` hook signature in the D3D11 path, a missing
  `<windows.h>` include in the Vulkan path, and `SE_DEBUG_NAME` (narrow in
  non-UNICODE builds) in the injector.
- Empty (zero-count) draws are skipped; hook state and limitations are
  documented in the headers.

### Viewer

- **BC1–BC7 CPU decode** for DDS textures (unit-tested and fuzz-verified
  against the `bcdec` reference decoder); the renderer now binds the mesh's
  captured texture instead of a checkerboard placeholder.
- **Texture preview panel** in the inspector.
- **Frame browser**: navigate `frame_*` folders of a capture session
  (buttons, combo, and the File menu); opening a session folder auto-opens
  its first frame.
- **Vertex layout editor**: edit the guessed semantic/format/offset per
  attribute, Apply re-decodes the raw bytes, and Save persists a `.layout`
  sidecar that is auto-applied on the next load.
- **glTF 2.0 export** (single mesh and whole frame, `.gltf` + `.bin`, with
  DDS textures referenced through the `MSFT_texture_dds` extension) plus
  **batch export** of every mesh in a folder (OBJ).
- **Statistics block**: meshes/vertices/triangles, texture refs, DDS bytes
  on disk, frame count.
- Textures resolve against the capture root (frames keep meshes in `meshes/`
  and textures in `textures/`); glTF image URIs are computed as real relative
  paths from the export location.

### Tooling & build

- Headless unit tests (`tests/`): capture-format round-trip/corruption, BC
  decoders, glTF export, and a viewer-asset integration test, wired into
  CTest (4 suites).
- `ghost_viewer_core` static library (capture loading, texture decode,
  exporters) shared by the GUI, the tests and headless builds.
- Ready-to-use GitHub Actions workflow in `ci/` (Windows MSVC + Ubuntu).
- `sample_gen` now also emits a textured sample (with a 32bpp DDS) and a
  hook-style session tree (two frames) to demo the frame browser.
- Fixed the `ghost_imgui` target missing GLFW include dirs.
- Development helper scripts for MinGW cross-compiles and headless viewer
  checks in `scripts/`.
