// glTF 2.0 exporter: writes one or more GPUMeshes as a .gltf + .bin pair.
//
// Geometry uses standard accessors (POSITION/NORMAL/TEXCOORD_0/indices).
// Textures are referenced by URI; DDS images are declared through the
// MSFT_texture_dds extension (widely supported by Blender/three.js loaders)
// since transcoding to PNG is out of scope for now.
#pragma once

#include "mesh_asset.h"

#include <string>
#include <vector>

namespace ghost {
namespace viewer {

// One mesh to include in the exported scene. `texture_uri` is the image URI
// relative to the .gltf file (e.g. "../textures/0012....dds"), or empty for
// an untextured material.
struct GltfMeshInput {
    const GPUMesh* mesh = nullptr;
    std::string    name;          // node + mesh name ("" -> "mesh_N")
    std::string    texture_uri;   // optional base-color texture
};

// Write `out_path` (.gltf) plus a sibling .bin file holding the buffers.
// Returns false and sets `err` on failure (empty list, unwritable path, ...).
bool export_gltf(const std::string& out_path,
                 const std::vector<GltfMeshInput>& meshes,
                 std::string* err);

} // namespace viewer
} // namespace ghost
