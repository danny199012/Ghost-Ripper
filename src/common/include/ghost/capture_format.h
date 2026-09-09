// Ghost-Ripper mesh capture format ("GRMF"), version 1.
//
// One file describes a single captured draw call:
//
//   [FileHeader]            fixed-size file metadata
//   [MeshHeader]            fixed-size mesh metadata + section offsets
//   [MeshAttribute x N]     vertex attribute layout
//   [TextureRef    x M]     textures bound when the draw was captured
//   [vertex data]           raw vertex buffer bytes (vertex_count * stride)
//   [index data]            raw index buffer bytes (optional)
//
// All integers are little-endian. All multi-byte fields use fixed-width types.
// See docs/CAPTURE_FORMAT.md for the human-readable specification.
#pragma once

#include <cstdint>

namespace ghost {
namespace fmt {

constexpr char     kMagic[4] = {'G', 'R', 'M', 'F'};
constexpr uint32_t kVersion  = 1;

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

enum class Semantic : uint32_t {
    Position     = 0,
    Normal       = 1,
    Tangent      = 2,
    Binormal     = 3,
    Color        = 4,
    TexCoord     = 5,   // semantic_index selects UV channel
    BlendWeights = 6,
    BlendIndices = 7,
    Unknown      = 0xFF,
};

enum class AttributeFormat : uint32_t {
    Unknown    = 0,
    Float32    = 1,
    Float32x2  = 2,
    Float32x3  = 3,
    Float32x4  = 4,
    Float16x2  = 5,
    Float16x4  = 6,
    UNorm8x2   = 7,
    UNorm8x4   = 8,
    SNorm8x4   = 9,
    UInt8x4    = 10,
    UInt16x2   = 11,
    UInt16x4   = 12,
    UInt32     = 13,
    UInt32x2   = 14,
    UInt32x3   = 15,
    UInt32x4   = 16,
    SInt32     = 17,
    SInt32x4   = 18,
};

enum class IndexFormat : uint32_t {
    None   = 0,   // non-indexed draw
    UInt16 = 1,
    UInt32 = 2,
};

enum class PrimitiveTopology : uint32_t {
    TriangleList  = 0,
    TriangleStrip = 1,
    LineList      = 2,
    LineStrip     = 3,
    PointList     = 4,
};

enum class TextureUsage : uint32_t {
    Unknown  = 0,
    Diffuse  = 1,
    Normal   = 2,
    Specular = 3,
    Emissive = 4,
};

// MeshHeader flags.
constexpr uint32_t kFlagAabbValid = 1u << 0;  // aabb_min/max were computed

// ---------------------------------------------------------------------------
// Structures
// ---------------------------------------------------------------------------

#pragma pack(push, 4)

struct FileHeader {
    char     magic[4];          // kMagic
    uint32_t version;           // kVersion
    uint32_t header_size;       // sizeof(FileHeader), for forward compat
    uint32_t frame_index;       // which captured frame this draw belongs to
    uint64_t timestamp;         // Unix time (seconds) of capture
    char     source_app[64];    // process name the capture came from
};                              // 88 bytes

struct MeshAttribute {
    uint32_t semantic;          // Semantic
    uint32_t semantic_index;    // e.g. UV channel for TexCoord
    uint32_t format;            // AttributeFormat
    uint32_t offset;            // byte offset within the vertex
};                              // 16 bytes

struct TextureRef {
    uint64_t content_hash;      // FNV-1a of the dumped texture bytes
    uint32_t slot;              // shader resource slot it was bound to
    uint32_t usage;             // TextureUsage (best guess)
    char     file[48];          // file name relative to the capture directory
};                              // 64 bytes

struct MeshHeader {
    uint32_t vertex_count;
    uint32_t vertex_stride;     // bytes per vertex
    uint32_t index_count;       // 0 for non-indexed draws
    uint32_t index_format;      // IndexFormat
    uint32_t topology;          // PrimitiveTopology
    uint32_t attribute_count;
    uint32_t start_index;       // original draw parameters
    int32_t  base_vertex;
    uint32_t texture_count;
    uint32_t vertex_data_size;  // bytes
    uint32_t index_data_size;   // bytes
    uint32_t flags;             // kFlag*
    uint32_t attributes_offset; // from start of file
    uint32_t textures_offset;
    uint32_t vertex_data_offset;
    uint32_t index_data_offset;
    float    aabb_min[3];
    float    aabb_max[3];
    char     name[64];
};                              // 152 bytes

#pragma pack(pop)

static_assert(sizeof(FileHeader) == 88, "FileHeader layout changed");
static_assert(sizeof(MeshAttribute) == 16, "MeshAttribute layout changed");
static_assert(sizeof(TextureRef) == 64, "TextureRef layout changed");
static_assert(sizeof(MeshHeader) == 152, "MeshHeader layout changed");

} // namespace fmt
} // namespace ghost
