// OpenGL mesh renderer: uploads a GPUMesh to GL buffers and draws it with a
// simple directional-lit shader. Owns the shader program and a checker
// fallback texture.
#pragma once

#include "mesh_asset.h"

#include <memory>

namespace ghost {
namespace viewer {

class MeshRenderer {
public:
    MeshRenderer();
    ~MeshRenderer();

    MeshRenderer(const MeshRenderer&) = delete;
    MeshRenderer& operator=(const MeshRenderer&) = delete;

    // Build the shaders and fallback texture. Returns false on GL error.
    bool initialize();

    // Upload a mesh to GPU buffers; returns a handle (id) for drawing.
    // Replaces any previously uploaded mesh.
    void upload(const GPUMesh& mesh);

    // Draw the uploaded mesh with the current camera matrices.
    // `mvp` is model-view-projection, `model_view` is view-space normals.
    void draw(const float mvp[16], const float model_view[16], bool wireframe,
              bool show_texture) const;

    bool has_mesh() const { return vao_ != 0; }
    uint32_t triangle_count() const { return triangle_count_; }
    const Vec3& aabb_min() const { return aabb_min_; }
    const Vec3& aabb_max() const { return aabb_max_; }

private:
    unsigned int program_       = 0;
    unsigned int vao_           = 0;
    unsigned int pos_buffer_    = 0;
    unsigned int norm_buffer_   = 0;
    unsigned int uv_buffer_     = 0;
    unsigned int index_buffer_  = 0;
    unsigned int checker_tex_   = 0;
    uint32_t     triangle_count_ = 0;
    bool         has_uvs_ = false;
    Vec3         aabb_min_{};
    Vec3         aabb_max_{};
};

} // namespace viewer
} // namespace ghost
