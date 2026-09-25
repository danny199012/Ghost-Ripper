# Architecture

## Overview

Ghost-Ripper is a game asset ripping tool — an alternative to Ninja Ripper.
It has three main components that communicate through a shared binary format:

```
 ┌──────────────┐      injects       ┌────────────────┐
 │ ghost-inject │ ─────────────────▶ │  ghost_hook    │
 │  (CLI, Win)  │   CreateRemote     │  (.dll, Win)   │
 └──────────────┘   Thread +         │                │
                    LoadLibraryW     │  D3D9 / D3D11  │
                                     │  D3D12 / Vulkan│
                                     │  hooks         │
                                     └───────┬────────┘
                                             │ writes
                                             ▼
                                   ┌──────────────────┐
                                   │  .grm files      │  ← GRMF binary format
                                   │  + .dds textures │
                                   │  + manifest.json │
                                   └────────┬─────────┘
                                            │ reads
                                            ▼
                                   ┌──────────────────┐
                                   │  ghost-viewer    │
                                   │  (ImGui + GL,    │
                                   │   cross-platform)│
                                   │                  │
                                   │  browse / 3D     │
                                   │  inspect / OBJ   │
                                   └──────────────────┘
```

## Components

### `src/common` — shared library (ghost-common)

Dependency-free static library linked by every other component:

| File | Responsibility |
|------|---------------|
| `capture_format.h` | GRMF binary layout (headers, enums, structs, static asserts) |
| `attribute.h/.cpp` | Vertex attribute sizes, component counts, format decoding (half-float, normalized, packed), name helpers |
| `attribute_guess.h/.cpp` | Heuristic vertex-layout guessing from stride alone |
| `capture_writer.h/.cpp` | Writes `.grm` files; computes AABB from positions |
| `capture_reader.h/.cpp` | Loads `.grm` files into memory; typed section views; decode helpers |
| `math.h` | Vec2/3/4, Mat4 (column-major), perspective/look_at/rotate |
| `log.h` | Thread-safe logging to stderr + optional file |

### `src/injector` — ghost-inject (Windows CLI)

Launches a game (optionally suspended) or attaches to a running process,
then injects `ghost_hook.dll` via the classic `CreateRemoteThread` +
`LoadLibraryW` technique. Requests `SeDebugPrivilege` for elevated targets.

### `src/hook` — ghost_hook.dll (Windows DLL)

The core capture engine. On `DLL_PROCESS_ATTACH` it:

1. Reads `ghost_hook.ini` (capture hotkey, output dir, texture toggle).
2. Creates a dummy device/swapchain for each available API to read vtable
   addresses.
3. Installs MinHook detours on draw calls and the frame-boundary function.

**Frame model:** the swapchain `Present` (D3D11/12), `EndScene` (D3D9), or
`vkQueuePresentKHR` (Vulkan) detour calls `CaptureContext::on_frame_boundary()`
each frame. When the hotkey (default F9) is pressed, capturing begins on the
*next* frame: every draw call in that frame dumps its geometry/textures, and
the frame ends at the next boundary. This captures exactly one frame.

**Per-API status:**

| API | Frame boundary | Draw capture | Texture dump |
|-----|---------------|-------------|-------------|
| D3D11 | ✅ Present | ✅ Draw/DrawIndexed/Instanced | ✅ PS SRVs → DDS |
| D3D9 | ✅ EndScene | ✅ DrawIndexedPrimitive | ✅ stages 0-7 → DDS |
| D3D12 | ✅ Present | ✅ DrawInstanced/DrawIndexedInstanced | 📋 TODO |
| Vulkan | ✅ vkQueuePresentKHR | ✅ vkCmdDraw/vkCmdDrawIndexed | 📋 TODO |

**D3D9 capture.** Draws are captured by locking the vertex window
`[min_index, min_index + num_vertices)` of stream 0 and slicing the exact
index range, exactly as the API declares it. `base_vertex` in the file is
`BaseVertexIndex - MinVertexIndex` so that `index + base_vertex` addresses the
stored vertex slice (the same rule applies to every API). Textures bound to
stages 0-7 are dumped through `LockRect` as DDS (32bpp A8R8G8B8/X8R8G8B8/
A8B8G8R8, DXT1/2/3/4/5).

**D3D12 capture.** D3D12 has no queryable "current state" at Present time, so
capture runs in two phases:

1. *Recording time.* Detours on the `ID3D12GraphicsCommandList` vtable (shared
   by every instance) observe `IASetVertexBuffers` / `IASetIndexBuffer` /
   `IASetPrimitiveTopology` to keep a small per-command-list state, and
   `DrawInstanced` / `DrawIndexedInstanced` snapshot the bound views while a
   capture frame is open. `ID3D12Device::CreateCommittedResource` /
   `CreatePlacedResource` maintain a GPU-VA → `ID3D12Resource` interval map so
   views (which may point into the middle of a buffer) resolve back to the
   resource, and `ID3D12CommandQueue::ExecuteCommandLists` hands over the
   DIRECT queue + device used for the readback.
2. *Present time.* The referenced vertex/index ranges are copied (deduplicated
   per range) into one READBACK heap with an internal command list on the
   game's own direct queue — buffers are implicitly state-promoted from
   COMMON, so no barriers are needed — the fence is waited on, and each draw is
   trimmed to the vertex range its indices actually reference before being
   written as a `.grm`.

**Vulkan capture.** Vulkan has no COM vtables and no import library in the
Windows SDK, so everything is resolved dynamically. Detours on the loader
exports of `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr` (plus
`vkGetDeviceQueue` / `vkAllocateCommandBuffers` / `vkQueuePresentKHR`) learn
about every `VkDevice`; the first time the app resolves one of the commands we
care about, that pointer is MinHooked once (the driver serves one pointer per
ICD). Command-buffer recording is then observed (`vkCmdBindVertexBuffers(2)`,
`vkCmdBindIndexBuffer`, `vkCmdBindPipeline` for topology/stride,
`vkCmdSetPrimitiveTopology`, `vkCmdDraw`, `vkCmdDrawIndexed`), with
`vkBeginCommandBuffer`/`vkResetCommandBuffer` clearing per-command-buffer
state. At `vkQueuePresentKHR` the ranges are copied through a host-visible
staging buffer on the presenting queue (submit + fence wait) and written out.
Because the loader hands out the same address for every device of one ICD,
multi-ICD setups capture the first ICD only; apps that dig raw pointers out of
the dispatch table bypass the loader and are not captured.

**Output layout:**
```
<Documents>\GhostRipper\<exe>_<pid>\
    ghost_hook.log
    frame_000123\
        manifest.json
        meshes\Mesh_0000.grm
        meshes\Mesh_0001.grm
        textures\<hash>.dds
```

### `src/viewer` — ghost-viewer (cross-platform GUI)

Dear ImGui + OpenGL 3.3 + GLFW desktop application:

- **Browser panel**: capture folder input, frame browser (prev/next + list of
  `frame_*` folders when a session folder is opened), mesh list (name,
  vert/tri counts) and a statistics block (vertices, triangles, texture refs,
  DDS disk usage).
- **3D viewport**: FBO-backed render target displayed as an ImGui image.
  Orbit camera (drag rotate, right-drag pan, scroll zoom).
- **Inspector panel**: mesh stats, AABB, attribute flags, render options
  (wireframe, texture, auto-frame), a texture preview of the bound DDS, the
  vertex-layout editor, and the export buttons.
- **Texture handling**: `bc_decode` (pure CPU, no GL) decodes BC1-BC7 DDS
  surfaces to RGBA8; `texture_loader` parses the DDS header (legacy + DX10),
  decodes/expands the first mip and uploads it as an RGBA8 GL texture which the
  renderer binds and the inspector previews.
- **Vertex layout editor**: shows the attributes the mesh was decoded with
  (the hook's heuristic guess). Editing semantic/format/offset and pressing
  Apply re-decodes the raw vertex bytes; Save writes a `<file>.grm.layout`
  sidecar which is applied automatically the next time the file is loaded.
- **Exporters**: OBJ (+ `.mtl`), glTF 2.0 (`.gltf` + `.bin`, DDS textures
  referenced through the MSFT_texture_dds extension), single mesh or every
  mesh of the folder, plus a batch OBJ export.

`ghost_viewer_core` (mesh_asset, obj_exporter, gltf_exporter, bc_decode) is a
static library with no GL/ImGui dependency so the unit tests and headless
tools can use the same code.

The viewer uses ImGui's bundled stripped GL loader for core functions and a
small supplemental loader (`gl_extra.h`) for FBO/mipmap/depth functions that
the stripped loader omits.

### `tools/`

- **sample_gen**: generates sample `.grm` files (cube, sphere, grid) for
  testing the viewer without a live capture.
- **grinfo**: CLI inspector that prints a human-readable summary of `.grm`
  files (headers, attributes, AABB, textures).

### `tests/`

Headless CTest targets that need no GPU or display:

- `test_capture_format`: writer/reader round-trip, section bounds checks,
  AABB computation, negative `base_vertex`, attribute decoding.
- `test_bc_decode`: BC1-BC7 block decoding (unit vectors plus round-trips).
- `test_gltf_export`: glTF JSON/BIN structure, accessor data, JSON validity.

## Data flow

1. User runs `ghost-inject --exe game.exe` (or `--pid <pid>`).
2. The hook DLL loads in the game process, installs API hooks.
3. User presses F9 in-game; the next frame's draws are captured to `.grm`.
4. User opens `ghost-viewer`, points it at the capture folder.
5. The viewer scans the folder, browses frames if a session folder was
   opened, lists the meshes and renders the selected one with its captured
   texture.
6. User fixes the vertex layout if the heuristic guess is wrong (saved as a
   `.layout` sidecar) and exports to OBJ or glTF - a single mesh or the whole
   frame.

## Key design decisions

- **One format, one language**: GRMF is the single interchange format; the
  whole project is C++17 with CMake, keeping the toolchain unified.
- **Heuristic layout guessing**: D3D11+ input layouts are opaque at runtime,
  so vertex attributes are guessed from stride (like Ninja Ripper). Guesses
  are stored in the file; the viewer can override them later.
- **Dependency-free hook DLL**: ghost-common uses only the STL so the hook
  DLL has no external linkage beyond MinHook (fetched at build time).
- **Cross-platform viewer**: the viewer builds on Windows, Linux, and macOS;
  only the hook and injector are Windows-only.
