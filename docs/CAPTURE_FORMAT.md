# GRMF Capture Format Specification

**Version 1** — the `.grm` file format used by Ghost-Ripper.

One `.grm` file describes a single captured draw call: its vertex buffer,
optional index buffer, vertex attribute layout, bound textures, and metadata.

## File layout

```
┌──────────────────┐  offset 0
│  FileHeader      │  88 bytes (fixed)
├──────────────────┤
│  MeshHeader      │  152 bytes (fixed)
├──────────────────┤
│  MeshAttribute[] │  attribute_count × 16 bytes
├──────────────────┤
│  TextureRef[]    │  texture_count × 64 bytes
├──────────────────┤
│  vertex data     │  vertex_data_size bytes
├──────────────────┤
│  index data      │  index_data_size bytes (0 if non-indexed)
└──────────────────┘
```

All integers are **little-endian**. All multi-byte fields use fixed-width
`uint32_t` / `uint64_t` types. Structures are `#pragma pack(push, 4)`.

## FileHeader (88 bytes)

| Offset | Type | Field | Description |
|--------|------|-------|-------------|
| 0 | `char[4]` | `magic` | `"GRMF"` |
| 4 | `uint32` | `version` | Format version (currently `1`) |
| 8 | `uint32` | `header_size` | `sizeof(FileHeader)` = 88, for forward compat |
| 12 | `uint32` | `frame_index` | Which captured frame this draw belongs to |
| 16 | `uint64` | `timestamp` | Unix time (seconds) of capture |
| 24 | `char[64]` | `source_app` | Process name the capture came from |

## MeshAttribute (16 bytes)

| Offset | Type | Field | Description |
|--------|------|-------|-------------|
| 0 | `uint32` | `semantic` | Semantic enum |
| 4 | `uint32` | `semantic_index` | e.g. UV channel for `TexCoord` |
| 8 | `uint32` | `format` | AttributeFormat enum |
| 12 | `uint32` | `offset` | Byte offset within one vertex |

## TextureRef (64 bytes)

| Offset | Type | Field | Description |
|--------|------|-------|-------------|
| 0 | `uint64` | `content_hash` | FNV-1a 64-bit hash of dumped texture bytes |
| 8 | `uint32` | `slot` | Shader resource slot it was bound to |
| 12 | `uint32` | `usage` | TextureUsage enum (best guess) |
| 16 | `char[48]` | `file` | File name relative to the capture dir |

## MeshHeader (152 bytes)

| Offset | Type | Field | Description |
|--------|------|-------|-------------|
| 0 | `uint32` | `vertex_count` | Number of vertices |
| 4 | `uint32` | `vertex_stride` | Bytes per vertex |
| 8 | `uint32` | `index_count` | 0 for non-indexed draws |
| 12 | `uint32` | `index_format` | IndexFormat enum |
| 16 | `uint32` | `topology` | PrimitiveTopology enum |
| 20 | `uint32` | `attribute_count` | Number of MeshAttribute records |
| 24 | `uint32` | `start_index` | Original draw start index |
| 28 | `int32` | `base_vertex` | Original draw base vertex |
| 32 | `uint32` | `texture_count` | Number of TextureRef records |
| 36 | `uint32` | `vertex_data_size` | Vertex data section size (bytes) |
| 40 | `uint32` | `index_data_size` | Index data section size (bytes) |
| 44 | `uint32` | `flags` | Bitfield (bit 0 = AABB valid) |
| 48 | `uint32` | `attributes_offset` | Offset from file start |
| 52 | `uint32` | `textures_offset` | Offset from file start |
| 56 | `uint32` | `vertex_data_offset` | Offset from file start |
| 60 | `uint32` | `index_data_offset` | Offset from file start |
| 64 | `float[3]` | `aabb_min` | Bounding box minimum |
| 76 | `float[3]` | `aabb_max` | Bounding box maximum |
| 88 | `char[64]` | `name` | Mesh name (e.g. "Mesh_0000") |

## Enums

### Semantic

| Value | Name | Description |
|-------|------|-------------|
| 0 | `Position` | Vertex position (float3) |
| 1 | `Normal` | Vertex normal |
| 2 | `Tangent` | Vertex tangent |
| 3 | `Binormal` | Vertex binormal |
| 4 | `Color` | Vertex color |
| 5 | `TexCoord` | Texture coordinate (semantic_index = UV channel) |
| 6 | `BlendWeights` | Skinning blend weights |
| 7 | `BlendIndices` | Skinning bone indices |
| 0xFF | `Unknown` | Unrecognized semantic |

### AttributeFormat

| Value | Name | Bytes | Components |
|-------|------|-------|-----------|
| 0 | `Unknown` | 0 | 0 |
| 1 | `Float32` | 4 | 1 |
| 2 | `Float32x2` | 8 | 2 |
| 3 | `Float32x3` | 12 | 3 |
| 4 | `Float32x4` | 16 | 4 |
| 5 | `Float16x2` | 4 | 2 |
| 6 | `Float16x4` | 8 | 4 |
| 7 | `UNorm8x2` | 2 | 2 |
| 8 | `UNorm8x4` | 4 | 4 |
| 9 | `SNorm8x4` | 4 | 4 |
| 10 | `UInt8x4` | 4 | 4 |
| 11 | `UInt16x2` | 4 | 2 |
| 12 | `UInt16x4` | 8 | 4 |
| 13 | `UInt32` | 4 | 1 |
| 14 | `UInt32x2` | 8 | 2 |
| 15 | `UInt32x3` | 12 | 3 |
| 16 | `UInt32x4` | 16 | 4 |
| 17 | `SInt32` | 4 | 1 |
| 18 | `SInt32x4` | 16 | 4 |

### IndexFormat / PrimitiveTopology / Flags

| IndexFormat | 0=None, 1=UInt16, 2=UInt32 |
|-------------|---------------------------|
| PrimitiveTopology | 0=TriList, 1=TriStrip, 2=LineList, 3=LineStrip, 4=PointList |
| Flags bit 0 | `AabbValid` — aabb_min/max were computed |

## Vertex & index data

The vertex data section is `vertex_count × vertex_stride` bytes of raw GPU
vertex data: the window of vertices the draw actually referenced, i.e. it may
start at a non-zero vertex of the source buffer. The index data section holds
only the indices used by this draw.

**Addressing contract (important):** index values are stored *unchanged*, as
they appear in the source index buffer. A viewer addresses the stored vertex
data with

```
vertex = vertex_data[index[i] + base_vertex]
```

`base_vertex` may be negative (it is chosen by the hook so that the
referenced window maps to offset 0 of the stored slice). Readers should drop
indices whose effective value falls outside `[0, vertex_count)`. Concretely:

| API | stored `base_vertex` |
|-----|--------------------|
| D3D11 | the draw's `BaseVertexLocation` |
| D3D9 | `BaseVertexIndex − MinVertexIndex` (window starts at `MinVertexIndex`) |
| D3D12 | `BaseVertexLocation − min(index + BaseVertexLocation)` |
| Vulkan | `vertexOffset + bindingOffset/stride − min(effective vertex)` |

Non-indexed draws store `index_count = 0` and a `vertex_count`-sized slice;
viewers then use sequential vertex indices.

`start_index` / `base_vertex` in the header keep the original draw-call
parameters for reference.

## Texture hashing

Texture pixel data is hashed with FNV-1a 64-bit to deduplicate identical
textures within a frame. The hash is the DDS file name:
`textures/<hash>.dds`, written once per unique texture. D3D11 writes PS
shader resources (slots 0-7) and D3D9 writes texture stages 0-7 as plain 2D
textures: 32bpp A8R8G8B8 / X8R8G8B8 / A8B8G8R8 use the legacy DDS header,
block-compressed formats (BC1-BC7) use the DX10 extension header. `usage` is
a best guess from the slot (0 = Diffuse, otherwise Unknown) for now.

## Forward compatibility

`header_size` and the offset fields allow future versions to add sections
without breaking v1 readers. A reader should: check `magic == "GRMF"` and
`version == 1`, use offset fields to locate sections, and bounds-check every
section against the file size.

