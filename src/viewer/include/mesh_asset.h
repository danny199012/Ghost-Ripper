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

    // First bound diffuse texture, if the capture recorded one. `texture_rel`
    // is the path as stored in the .grm ("textures/<hash>.dds"); `texture_abs`
    // resolves it against the capture directory. Empty when untextured.
    std::string texture_rel;
    std::string texture_abs;

    // Attribute layout this mesh was decoded with (guessed by the hook or
    // overridden by the user in the viewer).
    std::vector<fmt::MeshAttribute> attributes;
    uint32_t vertex_stride = 0;
};

// Viewer-side override of the heuristic vertex layout: the attributes are
// re-decoded from the raw vertex bytes with these semantics/formats/offsets.
struct LayoutOverride {
    std::vector<fmt::MeshAttribute> attributes;  // empty -> use the file's
};

// Load a .grm file and convert it to a GPUMesh, computing face normals when
// the source has none. Strip/line topologies are expanded to triangle lists.
// Pass an override to re-decode the raw vertex bytes with a user layout.
// Returns nullptr on failure with `err` set.
std::unique_ptr<GPUMesh> load_mesh(const std::string& path, std::string* err,
                                   const LayoutOverride* layout = nullptr);

// Sidecar helpers: `<path>.layout` stores one MeshAttribute per line as
// "semantic semantic_index format offset" so a corrected layout survives
// across viewer runs.
bool save_layout_override(const std::string& grm_path,
                          const std::vector<fmt::MeshAttribute>& attributes,
                          std::string* err);
bool load_layout_override(const std::string& grm_path,
                          std::vector<fmt::MeshAttribute>& attributes);

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
    std::string texture_rel;      // first diffuse texture ("" if none)
    uint32_t    texture_count = 0;
};

// Scan `dir` for .grm files. Both capture layouts are supported: a flat
// folder of .grm files (sample_gen) and a hook frame folder with a meshes/
// subdirectory.
std::vector<MeshInfo> scan_capture_dir(const std::string& dir, std::string* err = nullptr);

// Frame directories (`frame_XXXXXX`) inside a capture session folder, sorted.
std::vector<std::string> list_frame_dirs(const std::string& session_dir);

// Aggregate statistics for a capture folder (mesh counts + texture disk use).
struct CaptureStats {
    uint32_t mesh_count = 0;
    uint64_t vertex_count = 0;
    uint64_t triangle_count = 0;
    uint32_t texture_count = 0;
    uint64_t texture_bytes = 0;
    uint32_t frame_count = 0;
};
CaptureStats compute_capture_stats(const std::string& dir);

// Directory containing `path` (with trailing slash kept off), for resolving
// texture paths and sibling frame folders.
std::string parent_dir(const std::string& path);


} // namespace viewer
} // namespace ghost
