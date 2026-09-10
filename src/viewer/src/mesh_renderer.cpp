#include "mesh_renderer.h"

#include "texture_loader.h"
#include "gl_extra.h"

// ImGui's bundled loader provides the core GL 3.3 functions; gl_extra.h adds
// the FBO/mipmap functions that the stripped loader omits.
#include "imgui_impl_opengl3_loader.h"

namespace ghost {
namespace viewer {

namespace {

const char* kVertexShader = R"glsl(
#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
uniform mat4 u_mvp;
uniform mat4 u_model_view;
out vec3 v_normal;
out vec2 v_uv;
void main() {
    v_normal = mat3(u_model_view) * a_normal;
    v_uv = a_uv;
    gl_Position = u_mvp * vec4(a_pos, 1.0);
}
)glsl";

const char* kFragmentShader = R"glsl(
#version 330 core
in vec3 v_normal;
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform bool u_textured;
void main() {
    vec3 n = normalize(v_normal);
    vec3 light = normalize(vec3(0.4, 0.8, 0.5));
    float diff = max(dot(n, light), 0.0);
    float amb = 0.35;
    vec3 base = u_textured ? texture(u_tex, v_uv).rgb : vec3(0.7, 0.75, 0.8);
    frag = vec4(base * (amb + diff * 0.8), 1.0);
}
)glsl";

unsigned int compile(int type, const char* src) {
    unsigned int s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        // surfaced via the fact that the program fails to link; keep it quiet.
        (void)log;
    }
    return s;
}

} // namespace

MeshRenderer::MeshRenderer() = default;
MeshRenderer::~MeshRenderer() {
    if (pos_buffer_)   glDeleteBuffers(1, &pos_buffer_);
    if (norm_buffer_)  glDeleteBuffers(1, &norm_buffer_);
    if (uv_buffer_)    glDeleteBuffers(1, &uv_buffer_);
    if (index_buffer_) glDeleteBuffers(1, &index_buffer_);
    if (vao_)          glDeleteVertexArrays(1, &vao_);
    if (program_)      glDeleteProgram(program_);
    if (checker_tex_)  glDeleteTextures(1, &checker_tex_);
}

bool MeshRenderer::initialize() {
    const unsigned int vs = compile(GL_VERTEX_SHADER, kVertexShader);
    const unsigned int fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    int ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok)
        return false;
    checker_tex_ = create_checker_texture();
    return true;
}

void MeshRenderer::upload(const GPUMesh& mesh) {
    if (vao_) {
        glDeleteVertexArrays(1, &vao_);
        glDeleteBuffers(1, &pos_buffer_);
        glDeleteBuffers(1, &norm_buffer_);
        if (uv_buffer_) glDeleteBuffers(1, &uv_buffer_);
        glDeleteBuffers(1, &index_buffer_);
        vao_ = pos_buffer_ = norm_buffer_ = uv_buffer_ = index_buffer_ = 0;
    }

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);

    glGenBuffers(1, &pos_buffer_);
    glBindBuffer(GL_ARRAY_BUFFER, pos_buffer_);
    glBufferData(GL_ARRAY_BUFFER, mesh.positions.size() * sizeof(float),
                 mesh.positions.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    glGenBuffers(1, &norm_buffer_);
    glBindBuffer(GL_ARRAY_BUFFER, norm_buffer_);
    glBufferData(GL_ARRAY_BUFFER, mesh.normals.size() * sizeof(float),
                 mesh.normals.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    has_uvs_ = mesh.has_uvs;
    if (has_uvs_) {
        glGenBuffers(1, &uv_buffer_);
        glBindBuffer(GL_ARRAY_BUFFER, uv_buffer_);
        glBufferData(GL_ARRAY_BUFFER, mesh.uvs.size() * sizeof(float),
                     mesh.uvs.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    }

    glGenBuffers(1, &index_buffer_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_buffer_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, mesh.indices.size() * sizeof(uint32_t),
                 mesh.indices.data(), GL_STATIC_DRAW);

    glBindVertexArray(0);

    triangle_count_ = mesh.triangle_count;
    aabb_min_ = mesh.aabb_min;
    aabb_max_ = mesh.aabb_max;
}

void MeshRenderer::draw(const float mvp[16], const float model_view[16],
                        bool wireframe, bool show_texture) const {
    if (!vao_ || !program_)
        return;

    glUseProgram(program_);
    glUniformMatrix4fv(glGetUniformLocation(program_, "u_mvp"), 1, GL_FALSE, mvp);
    glUniformMatrix4fv(glGetUniformLocation(program_, "u_model_view"), 1, GL_FALSE,
                       model_view);
    glUniform1i(glGetUniformLocation(program_, "u_textured"),
                show_texture && has_uvs_ ? 1 : 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, checker_tex_);
    glUniform1i(glGetUniformLocation(program_, "u_tex"), 0);

    if (wireframe)
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(triangle_count_ * 3),
                   GL_UNSIGNED_INT, nullptr);
    if (wireframe)
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glBindVertexArray(0);
}

} // namespace viewer
} // namespace ghost
