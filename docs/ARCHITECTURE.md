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
| D3D9 | ✅ EndScene | ✅ DrawIndexedPrimitive | 📋 TODO |
| D3D12 | ✅ Present | 📋 TODO (ExecuteCommandLists) | 📋 TODO |
| Vulkan | ✅ vkQueuePresentKHR | 📋 TODO | 📋 TODO |

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

- **Browser panel**: capture folder input + mesh list (name, vert/tri counts).
- **3D viewport**: FBO-backed render target displayed as an ImGui image.
  Orbit camera (drag rotate, right-drag pan, scroll zoom).
- **Inspector panel**: mesh stats, AABB, attribute flags, render options
  (wireframe, texture, auto-frame), OBJ export button.
- **OBJ exporter**: writes `.obj` + `.mtl` with positions, normals, UVs.

The viewer uses ImGui's bundled stripped GL loader for core functions and a
small supplemental loader (`gl_extra.h`) for FBO/mipmap/depth functions that
the stripped loader omits.

### `tools/`

- **sample_gen**: generates sample `.grm` files (cube, sphere, grid) for
  testing the viewer without a live capture.
- **grinfo**: CLI inspector that prints a human-readable summary of `.grm`
  files (headers, attributes, AABB, textures).

## Data flow

1. User runs `ghost-inject --exe game.exe` (or `--pid <pid>`).
2. The hook DLL loads in the game process, installs API hooks.
3. User presses F9 in-game; the next frame's draws are captured to `.grm`.
4. User opens `ghost-viewer`, points it at the capture folder.
5. The viewer scans the folder, lists meshes, and renders the selected one.
6. User exports to OBJ.

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
