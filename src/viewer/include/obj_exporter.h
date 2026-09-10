// OBJ exporter: writes a .obj (+ .mtl) for a loaded GPUMesh.
#pragma once

#include "mesh_asset.h"

#include <string>

namespace ghost {
namespace viewer {

// Write `out_path` (.obj) and a sibling .mtl with a basic material.
// Returns false and sets `err` on failure.
bool export_obj(const std::string& out_path, const GPUMesh& mesh, std::string* err);

} // namespace viewer
} // namespace ghost
