// Writer for .grm capture files. Used by the hook DLL and by tools that
// synthesize captures (sample_gen).
#pragma once

#include "capture_format.h"
#include "math.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ghost {

// In-memory description of a single captured draw call.
struct MeshData {
    std::string name;

    std::vector<fmt::MeshAttribute> attributes;

    const void* vertex_data = nullptr;
    uint32_t    vertex_data_size = 0;   // bytes
    uint32_t    vertex_count = 0;
    uint32_t    vertex_stride = 0;

    const void* index_data = nullptr;   // may be null for non-indexed draws
    uint32_t    index_data_size = 0;    // bytes
    uint32_t    index_count = 0;
    fmt::IndexFormat index_format = fmt::IndexFormat::None;

    fmt::PrimitiveTopology topology = fmt::PrimitiveTopology::TriangleList;

    // Original draw-call parameters, for reference/rendering the exact subrange.
    uint32_t start_index = 0;
    int32_t  base_vertex = 0;

    std::vector<fmt::TextureRef> textures;

    // Optional; computed from positions when aabb_valid is false at write time.
    Vec3 aabb_min{};
    Vec3 aabb_max{};
    bool aabb_valid = false;
};

// Write one mesh to `path`. Computes the AABB from the position attribute
// when MeshData::aabb_valid is false. Returns false and sets `err` on failure.
bool write_mesh_file(const std::string& path,
                     const MeshData& mesh,
                     const std::string& source_app,
                     uint32_t frame_index,
                     std::string* err);

// Compute the AABB over all vertices using the position attribute.
// Returns false if there is no decodable position attribute.
bool compute_mesh_aabb(MeshData& mesh);

// FNV-1a 64-bit hash, used for texture content hashing.
uint64_t fnv1a64(const void* data, size_t size);

} // namespace ghost
