# Roadmap

## Current state (v0.1)

- ✅ GRMF v1 capture format (reader + writer)
- ✅ ghost-inject: process launcher + DLL injector (Windows)
- ✅ D3D11 hook: full draw capture (Draw/DrawIndexed/Instanced) + texture dump
- ✅ D3D9 hook: DrawIndexedPrimitive capture
- ✅ D3D12/Vulkan: frame-boundary detection (Present/EndScene)
- ✅ ghost-viewer: 3D viewport, mesh browser, inspector, OBJ export
- ✅ sample_gen + grinfo tools

## Near-term goals

### Capture completeness
- [ ] D3D9 texture dump (LockRect on bound texture stages → DDS)
- [ ] D3D12 draw capture: hook `ExecuteCommandLists`, snapshot descriptor
      tables, copy vertex/index buffers via readback heap
- [ ] Vulkan draw capture: hook `vkCmdDraw`/`vkCmdDrawIndexed` via layered
      device, copy buffer memory via barriers
- [ ] Multi-stream vertex capture (stream 1+ for instanced/skinned meshes)

### Viewer
- [ ] BC-compressed DDS decode in the viewer (BC1–BC7 → RGBA)
- [ ] Texture preview panel (show bound DDS textures)
- [ ] Vertex layout editor: override guessed attributes and re-decode
- [ ] glTF 2.0 export (with textures + materials)
- [ ] FBX export
- [ ] Batch export (all meshes in a frame)
- [ ] Frame browser: navigate between captured frames

### UX
- [ ] Native file dialogs (Win32 IFileOpenDialog / GTK on Linux)
- [ ] Settings dialog (hotkey, output dir, capture options)
- [ ] Capture overlay: in-game OSD showing capture status (via ImGui)
- [ ] Statistics: total meshes, textures, disk usage per capture session

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
- [ ] Mesh decimation & cleanup tools (degenerate tri removal, welding)
- [ ] Texture format converter (DDS → PNG/BCn → ASTC/ETC)
