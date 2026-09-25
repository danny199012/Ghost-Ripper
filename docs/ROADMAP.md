# Roadmap

## Current state (v0.2)

### Capture
- ✅ GRMF v1 capture format (reader + writer)
- ✅ ghost-inject: process launcher + DLL injector (Windows)
- ✅ D3D11 hook: full draw capture (Draw/DrawIndexed/Instanced) + texture dump
- ✅ D3D9 hook: DrawIndexedPrimitive capture + texture dump (stages 0-7 → DDS)
- ✅ D3D12 hook: draw capture via command-list recording hooks, buffer
      GPU-VA tracking and readback-heap copies at Present
- ✅ Vulkan hook: draw capture via loader-level device discovery + MinHook on
      the resolved command pointers, staging-buffer readback at present
- ✅ sample_gen + grinfo tools

### Viewer
- ✅ BC-compressed DDS decode (BC1–BC7) on the CPU
- ✅ Texture preview panel (bound DDS, decoded)
- ✅ Vertex layout editor with `.layout` sidecar persistence
- ✅ glTF 2.0 export (single mesh and whole frame)
- ✅ Batch export of every mesh in a folder (OBJ)
- ✅ Frame browser: navigate `frame_*` folders of a capture session
- ✅ Capture statistics (meshes/vertices/triangles/textures/disk use)

### Tooling
- ✅ Headless unit tests (capture format, BC decode, glTF export) + CTest
- ✅ GitHub Actions build definition (windows + linux) — see `ci/`

## Near-term goals

### Capture completeness
- [ ] D3D12 texture dump: track descriptor heaps / `CreateShaderResourceView`
      so bound textures can be copied to DDS like D3D11/D3D9
- [ ] Vulkan texture dump: track image views bound through descriptor sets
- [ ] Multi-stream vertex capture (stream 1+ for instanced/skinned meshes).
      Needs a GRMF v2 layout: one vertex-data section per stream plus a
      per-stream stride/count, because v1 stores a single interleaved stream
- [ ] Capture non-indexed D3D9 draws (DrawPrimitive/DrawPrimitiveUP)
- [ ] Capture `vkCmdDrawIndirect` / `ExecuteIndirect` draws

### Viewer
- [ ] FBX export (note: ASCII FBX is deprecated by many importers; a binary
      writer would be needed for real interoperability)
- [ ] Animation capture viewer (multiple frames → playback)
- [ ] Mesh cleanup tools (degenerate triangle removal, vertex welding)

### UX
- [ ] Native file dialogs (Win32 IFileOpenDialog / GTK on Linux); the current
      folder picker is a text field
- [ ] Settings dialog (edit `ghost_hook.ini` hotkey/output dir from the viewer)
- [ ] Capture overlay: in-game OSD showing capture status
- [ ] Session diffing: compare meshes across frames

## Medium-term

- [ ] Instanced draw full capture (per-instance transforms from stream 1)
- [ ] Skeleton/binding capture (blend weights + bone matrices → rigging)
- [ ] Shader reflection: capture HLSL/SPIR-V bytecode for reference
- [ ] Animation capture: capture multiple frames → skeletal animation
- [ ] Scene reconstruction: group meshes by transform hierarchy

## Long-term / research

- [ ] Anti-cheat-aware injection (manual mapping, syscall-based injection)
- [ ] Linux/Wine capture support (Vulkan + DXVK interception)
- [ ] Plugin system for custom capture/export pipelines
- [ ] Texture format converter (DDS → PNG/BCn → ASTC/ETC)
