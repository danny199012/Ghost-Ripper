# Ghost-Ripper

An open-source game asset ripping tool — an alternative to Ninja Ripper — that
captures 3D geometry and textures straight out of a running game's rendering
pipeline, plus a desktop viewer to inspect and export what you ripped.

**How it works:** a small hook DLL is injected into the game process. It hooks
the graphics API in use (Direct3D 9 / 11 / 12, Vulkan), and when you press the
capture hotkey it dumps every draw call of that frame — vertex buffers, index
buffers, textures and draw parameters — into compact `.grm` mesh files. The
**ghost-viewer** app then lets you browse, inspect and export the captured
models.

> ⚠️ **Legal note:** Ghost-Ripper is intended for personal archival, modding
> research, interoperability and education. Ripping assets from a game may
> violate its EULA and assets remain the property of their respective
> copyright holders. Do not redistribute ripped content. Do not use it to
> cheat in online games — injection may be detected by anti-cheat systems.

## Feature status

| Component                     | Status          |
|-------------------------------|-----------------|
| Process injector (CLI)        | ✅ Implemented  |
| Direct3D 11 hook + capture    | ✅ Implemented  |
| Direct3D 9 hook + capture     | ✅ Implemented (draws + texture dump) |
| Direct3D 12 hook + capture    | ✅ Implemented (draws; texture dump TODO) |
| Vulkan hook + capture         | ✅ Implemented (draws; texture dump TODO) |
| Texture dump (DDS)            | ✅ D3D11 + D3D9 (2D textures, BC + 32bpp) |
| Capture format (`.grm`)       | ✅ v1 spec, reader + writer |
| Viewer GUI (browse / 3D view) | ✅ Implemented  |
| Viewer texture preview        | ✅ DDS incl. BC1-BC7 CPU decode |
| Frame browser (multi-frame)   | ✅ Implemented  |
| Vertex layout editing         | ✅ Implemented (with `.layout` sidecar) |
| OBJ export                    | ✅ Implemented  |
| glTF 2.0 export               | ✅ Single mesh + whole frame |
| Batch export                  | ✅ All meshes of a folder |
| Multi-stream vertex capture   | 📋 Needs GRMF v2 |
| D3D12/Vulkan texture dump     | 📋 Planned      |


## Repository layout

```
src/
  common/      Shared code: .grm capture format (reader/writer), math, logging
  injector/    ghost-inject: CLI that launches/attaches to a game and injects the hook DLL
  hook/        ghost_hook.dll: API hooking + frame capture (D3D9/11/12, Vulkan)
  viewer/      ghost-viewer: Dear ImGui + OpenGL browser/viewer with OBJ + glTF
               export (ghost_viewer_core holds the headless capture/texture/
               export logic shared with the unit tests)
tools/
  sample_gen/  Generates sample .grm captures (cube, sphere, ...) to test the viewer
  grinfo/      Dumps information about .grm capture files
tests/         Headless unit tests (capture format, BC decoders, glTF export)
ci/            Ready-to-use GitHub Actions workflow (copy to .github/workflows/)
scripts/       Development helpers (mingw / headless viewer compile checks)
docs/
  ARCHITECTURE.md, CAPTURE_FORMAT.md, ROADMAP.md
```

## Building

Requirements: **CMake ≥ 3.20** and a C++17 compiler.

* **Windows (full build — injector, hook, viewer, tools):** Visual Studio 2022
  (MSVC). Dependencies (GLFW, Dear ImGui, MinHook) are fetched automatically
  via CMake FetchContent.

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

* **Linux / macOS (viewer + tools + tests; hook components are Windows-only):**

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Build artifacts land in `build/bin/`. The headless unit tests (capture-format
round-trip, BC1-BC7 decoding, glTF export) need no GPU and also build with
`-DGHOST_BUILD_VIEWER=OFF` on machines without GL/X11.

## Quick start (Windows)

1. Start your game.
2. Inject the hook (from an elevated or same-integrity-level prompt):

```bat
:: attach to a running process
ghost-inject.exe --pid 12345 --dll ghost_hook.dll

:: or launch the game directly
ghost-inject.exe --exe "C:\Games\MyGame\game.exe" --dll ghost_hook.dll
```

3. In-game, press **F9** to capture a frame (configurable via `ghost_hook.ini`).
   Captures are written to `%USERPROFILE%\Documents\GhostRipper\<exe>_<pid>\frame_####\`.
4. Open `ghost-viewer.exe` and point it at the session folder (or a single
   `frame_*` folder) - point it at `%USERPROFILE%\Documents\GhostRipper\`
   to get the frame browser. Browse the ripped meshes, check the captured
   texture, fix the vertex layout if the heuristic guess looks wrong, and
   export to OBJ or glTF (single mesh, or the whole frame).

You can preview the viewer without a game by generating sample captures:

```sh
sample_gen captures/demo            # cube/sphere/grid + a textured cube, plus
                                    # captures/session/frame_00000{1,2} trees
ghost-viewer captures/session       # frame browser across the two frames
ghost-viewer captures/demo          # flat folder (texture preview, glTF export)
grinfo captures/demo/cube.grm       # inspect a capture file
```

The sample data includes a 32bpp DDS so the texture panel and the glTF
export's texture references work out of the box.

## Documentation

* [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — components, data flow, hooking strategy
* [docs/CAPTURE_FORMAT.md](docs/CAPTURE_FORMAT.md) — the `.grm` binary format specification
* [docs/ROADMAP.md](docs/ROADMAP.md) — planned work (D3D12/Vulkan texture dump, multi-stream capture, ...)
* [CHANGELOG.md](CHANGELOG.md) — what changed in v0.2

## License

MIT — see [LICENSE](LICENSE).
