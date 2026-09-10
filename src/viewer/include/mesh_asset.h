// Runtime mesh asset: a loaded .grm capture converted into GPU-friendly
// interleaved float buffers (position, normal, uv) plus indices, ready for
// the viewer's OpenGL renderer and the OBJ exporter.
#pragma once

#include "ghost/capture_reader.h"
#include "ghost/math.h"

#include <memory>
#include <string>
#include <vector>

namespace ghost {
namespace viewer {

struct GPUMesh {
    std::vector<float>    positions;  // xyz, stride 3
    std::vector<float>    normals;    // xyz, stride 3 (computed if absent)
    std::vector<float>    uvs;        // xy, stride 2 (empty if absent)
    std::vector<uint32_t> indices;

    Vec3 aabb_min{};
    Vec3 aabb_max{};

    uint32_t triangle_count = 0;
    bool     has_normals = false;
    bool     has_uvs = false;
};

// Load a .grm file and convert it to a GPUMesh, computing face normals when
// the source has none. Strip/line topologies are expanded to triangle lists.
// Returns nullptr on failure with `err` set.
std::unique_ptr<GPUMesh> load_mesh(const std::string& path, std::string* err);

// Metadata shown in the viewer's mesh list / inspector, without loading the
// full geometry. Cheap enough to read for every file in a folder.
struct MeshInfo {
    std::string path;
    std::string name;
    uint32_t    vertex_count = 0;
    uint32_t    triangle_count = 0;
    bool        has_normals = false;
    bool        has_uvs = false;
    Vec3        aabb_min{};
    Vec3        aabb_max{};
    std::string source_app;
};

std::vector<MeshInfo> scan_capture_dir(const std::string& dir, std::string* err = nullptr);

} // namespace viewer
} // namespace ghost
