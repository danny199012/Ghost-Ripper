// ghost-viewer: Dear ImGui + OpenGL3 + GLFW application for browsing and
// inspecting .grm capture files with a 3D viewport and OBJ export.
//
//   ghost-viewer [capture_dir]
#include "mesh_asset.h"
#include "mesh_renderer.h"
#include "obj_exporter.h"
#include "gltf_exporter.h"
#include "texture_loader.h"
#include "gl_extra.h"

#include "ghost/attribute.h"
#include "ghost/capture_format.h"
#include "ghost/log.h"
#include "ghost/math.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// Tell GLFW not to pull in system GL headers — we use ImGui's bundled loader
// (imgui_impl_opengl3_loader.h) plus our gl_extra.h supplement instead.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_opengl3_loader.h"

// Path separator as a string ("\\" on Windows, "/" elsewhere).

// Relative URI from a directory to a file, for glTF texture references: the
// exporter writes the .gltf into a specific folder, and the DDS may sit in a
// sibling folder (frame/meshes/ vs frame/textures/). Falls back to the plain
// path when the two share no root (e.g. different drives).
std::string relative_uri(const std::string& from_dir, const std::string& to) {
    const auto split = [](const std::string& s) {
        std::vector<std::string> parts;
        std::string cur;
        for (char c : s) {
            if (c == '/' || c == '\\') {
                if (!cur.empty()) parts.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) parts.push_back(cur);
        return parts;
    };
    const std::vector<std::string> a = split(from_dir);
    const std::vector<std::string> b = split(to);
    size_t common = 0;
    while (common < a.size() && common < b.size() && a[common] == b[common])
        ++common;
    if (common == 0)
        return to;  // no shared root: use the absolute path
    std::string out;
    for (size_t i = common; i < a.size(); ++i)
        out += "../";
    for (size_t i = common; i < b.size(); ++i) {
        out += b[i];
        if (i + 1 < b.size())
            out += '/';
    }
    return out;
}
#ifdef _WIN32
#define SEP_S "\\"
#else
#define SEP_S "/"
#endif

using namespace ghost;  // Vec3, Mat4, etc. from ghost/math.h

namespace {

// ---------------------------------------------------------------------------
// Orbit camera
// ---------------------------------------------------------------------------

struct Camera {
    float yaw      = -0.6f;     // radians
    float pitch    = 0.4f;
    float distance = 4.0f;
    Vec3  target{0, 0, 0};

    Mat4 view_matrix() const {
        const float cp = cosf(pitch), sp = sinf(pitch);
        const float cy = cosf(yaw),   sy = sinf(yaw);
        const Vec3 dir{cp * sy, sp, cp * cy};
        const Vec3 eye = target + dir * distance;
        return Mat4::look_at(eye, target, {0, 1, 0});
    }
};

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------

struct AppState {
    std::string capture_dir;
    std::vector<ghost::viewer::MeshInfo> meshes;
    int selected = -1;

    // Frame browser: sibling frame_* folders of the session this capture
    // belongs to (empty when the folder is a flat capture dir).
    std::vector<std::string> frames;
    int frame_index = -1;

    std::unique_ptr<ghost::viewer::GPUMesh> gpu_mesh;  // loaded geometry
    std::string loaded_path;

    Camera camera;
    bool wireframe  = false;
    bool show_tex   = true;
    bool auto_frame = true;  // re-center camera on a newly loaded mesh

    // Inspector: texture preview panel.
    bool show_texture_panel = true;

    // Vertex-layout editor: an editable copy of the loaded mesh's attributes
    // (heuristic guesses can be wrong; Apply re-decodes the raw bytes).
    std::vector<ghost::fmt::MeshAttribute> edit_attrs;
    int  edit_stride = 0;
    int  edit_vertex_count = 0;

    // Aggregate stats for the current folder.
    ghost::viewer::CaptureStats stats;

    // FBO for the 3D viewport.
    unsigned int fbo = 0, fbo_color = 0, fbo_depth = 0;
    int fbo_w = 0, fbo_h = 0;

    std::string status;
};

void recenter_camera(Camera& cam, const Vec3& aabb_min, const Vec3& aabb_max) {
    const Vec3 center = (aabb_min + aabb_max) * 0.5f;
    const Vec3 extent = aabb_max - aabb_min;
    const float size = extent.length();
    cam.target   = center;
    cam.distance = size > 0.0001f ? size * 1.6f + 0.5f : 4.0f;
    cam.yaw   = -0.6f;
    cam.pitch = 0.4f;
}

// (Re)scan the current capture folder: mesh list, sibling frames and stats.
void refresh_capture_dir(AppState& s) {
    std::string err;
    s.meshes = ghost::viewer::scan_capture_dir(s.capture_dir, &err);
    s.selected = -1;
    s.loaded_path.clear();
    s.gpu_mesh.reset();

    s.frames = ghost::viewer::list_frame_dirs(s.capture_dir);
    s.frame_index = -1;
    // When the session folder itself has no meshes but frame folders do,
    // open the first frame right away.
    if (s.meshes.empty() && !s.frames.empty()) {
        s.frame_index = 0;
        s.capture_dir = s.frames[0];
        s.meshes = ghost::viewer::scan_capture_dir(s.capture_dir, &err);
    } else if (!s.frames.empty()) {
        for (size_t i = 0; i < s.frames.size(); ++i)
            if (s.frames[i] == s.capture_dir)
                s.frame_index = static_cast<int>(i);
    } else {
        // Maybe this folder *is* a frame inside a session folder.
        const std::string parent = ghost::viewer::parent_dir(s.capture_dir);
        std::vector<std::string> siblings = ghost::viewer::list_frame_dirs(parent);
        if (!siblings.empty()) {
            s.frames = std::move(siblings);
            for (size_t i = 0; i < s.frames.size(); ++i)
                if (s.frames[i] == s.capture_dir)
                    s.frame_index = static_cast<int>(i);
        }
    }
    s.stats = ghost::viewer::compute_capture_stats(s.capture_dir);
}

void open_frame(AppState& s, int index, ghost::viewer::MeshRenderer& renderer) {
    if (index < 0 || index >= static_cast<int>(s.frames.size()))
        return;
    s.frame_index = index;
    s.capture_dir = s.frames[index];
    std::string err;
    s.meshes = ghost::viewer::scan_capture_dir(s.capture_dir, &err);
    s.selected = -1;
    s.loaded_path.clear();
    s.gpu_mesh.reset();
    s.stats = ghost::viewer::compute_capture_stats(s.capture_dir);
    s.status = "frame " + std::to_string(index + 1) + "/" +
               std::to_string(s.frames.size()) + ": " +
               std::to_string(s.meshes.size()) + " meshes";
    (void)renderer;
}

} // namespace

// ---------------------------------------------------------------------------
// FBO management for the 3D viewport
// ---------------------------------------------------------------------------

void ensure_fbo(AppState& s, int w, int h) {
    if (s.fbo_w == w && s.fbo_h == h && s.fbo != 0)
        return;
    if (s.fbo) {
        glDeleteFramebuffers(1, &s.fbo);
        glDeleteTextures(1, &s.fbo_color);
        glDeleteRenderbuffers(1, &s.fbo_depth);
        s.fbo = s.fbo_color = s.fbo_depth = 0;
    }
    s.fbo_w = w;
    s.fbo_h = (h < 1) ? 1 : h;

    glGenFramebuffers(1, &s.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);

    glGenTextures(1, &s.fbo_color);
    glBindTexture(GL_TEXTURE_2D, s.fbo_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s.fbo_w, s.fbo_h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           s.fbo_color, 0);

    glGenRenderbuffers(1, &s.fbo_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, s.fbo_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, s.fbo_w, s.fbo_h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, s.fbo_depth);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ---------------------------------------------------------------------------
// UI panels
// ---------------------------------------------------------------------------

void load_selected_mesh(AppState& s, ghost::viewer::MeshRenderer& renderer) {
    if (s.selected < 0 || s.selected >= static_cast<int>(s.meshes.size()))
        return;
    const std::string& path = s.meshes[s.selected].path;

    // A saved layout override from a previous session wins over the file's
    // heuristic guess.
    ghost::viewer::LayoutOverride layout;
    std::vector<ghost::fmt::MeshAttribute> saved;
    if (ghost::viewer::load_layout_override(path, saved))
        layout.attributes = saved;

    std::string err;
    auto mesh = ghost::viewer::load_mesh(path, &err, layout.attributes.empty() ? nullptr : &layout);
    if (!mesh) {
        s.status = "load failed: " + err;
        return;
    }
    s.gpu_mesh = std::move(mesh);
    s.loaded_path = path;
    renderer.upload(*s.gpu_mesh);
    if (s.auto_frame)
        recenter_camera(s.camera, renderer.aabb_min(), renderer.aabb_max());

    // Seed the layout editor from the layout actually used.
    s.edit_attrs = s.gpu_mesh->attributes;
    s.edit_stride = static_cast<int>(s.gpu_mesh->vertex_stride);
    s.edit_vertex_count = s.meshes[s.selected].vertex_count;

    s.status = "loaded " + s.meshes[s.selected].name +
               " (" + std::to_string(renderer.triangle_count()) + " tris)" +
               (s.gpu_mesh->texture_abs.empty() ? "" : ", texture: " + s.gpu_mesh->texture_rel);
}

// Re-decode the loaded file with the edited layout (Apply button).
void apply_layout_edit(AppState& s, ghost::viewer::MeshRenderer& renderer) {
    if (s.edit_attrs.empty() || s.loaded_path.empty()) {
        s.status = "layout edit: nothing to apply";
        return;
    }
    ghost::viewer::LayoutOverride layout;
    layout.attributes = s.edit_attrs;
    std::string err;
    auto mesh = ghost::viewer::load_mesh(s.loaded_path, &err, &layout);
    if (!mesh) {
        s.status = "layout re-decode failed: " + err;
        return;
    }
    s.gpu_mesh = std::move(mesh);
    renderer.upload(*s.gpu_mesh);
    if (s.auto_frame)
        recenter_camera(s.camera, renderer.aabb_min(), renderer.aabb_max());
    s.status = "re-decoded with " + std::to_string(s.edit_attrs.size()) + " attributes (" +
               std::to_string(renderer.triangle_count()) + " tris)";
}

void draw_mesh_list(AppState& s, ghost::viewer::MeshRenderer& renderer) {
    ImGui::Text("Capture folder");
    char dir_buf[512];
    std::strncpy(dir_buf, s.capture_dir.c_str(), sizeof(dir_buf) - 1);
    dir_buf[sizeof(dir_buf) - 1] = '\0';
    ImGui::PushItemWidth(-90);
    if (ImGui::InputText("##dir", dir_buf, sizeof(dir_buf),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        s.capture_dir = dir_buf;
        refresh_capture_dir(s);
        s.status = "scanned " + std::to_string(s.meshes.size()) + " meshes";
    }
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Browse"))
        ImGui::OpenPopup("Folder Picker");
    // Minimal folder picker: lets the user type/edit the path and confirm.
    if (ImGui::BeginPopup("Folder Picker")) {
        ImGui::Text("Enter capture folder:");
        char pick[512];
        std::strncpy(pick, s.capture_dir.c_str(), sizeof(pick) - 1);
        pick[sizeof(pick) - 1] = '\0';
        ImGui::PushItemWidth(360);
        if (ImGui::InputText("##pick", pick, sizeof(pick),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            s.capture_dir = pick;
            refresh_capture_dir(s);
            s.status = "scanned " + std::to_string(s.meshes.size()) + " meshes";
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ImGui::Button("OK")) {
            s.capture_dir = pick;
            refresh_capture_dir(s);
            s.status = "scanned " + std::to_string(s.meshes.size()) + " meshes";
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::Separator();

    // ---- Frame browser: navigate the frame_* folders of a session ----
    if (!s.frames.empty()) {
        ImGui::TextDisabled("Frame");
        if (ImGui::Button("<<##prev") && s.frame_index > 0)
            open_frame(s, s.frame_index - 1, renderer);
        ImGui::SameLine();
        ImGui::BeginDisabled(s.frame_index < 0 ||
                             s.frame_index + 1 >= static_cast<int>(s.frames.size()));
        if (ImGui::Button(">>##next"))
            open_frame(s, s.frame_index + 1, renderer);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Text("%d/%d", s.frame_index + 1, static_cast<int>(s.frames.size()));

        ImGui::PushItemWidth(-1);
        if (ImGui::BeginCombo("##framecombo",
                              s.frame_index >= 0 ? s.frames[s.frame_index].c_str()
                                                 : "(select)")) {
            for (int i = 0; i < static_cast<int>(s.frames.size()); ++i) {
                char label[128];
                std::snprintf(label, sizeof(label), "frame %d  (%s)", i + 1,
                              s.frames[i].c_str() +
                                  (s.frames[i].find_last_of("/\\") == std::string::npos
                                       ? 0
                                       : s.frames[i].find_last_of("/\\") + 1));
                if (ImGui::Selectable(label, i == s.frame_index))
                    open_frame(s, i, renderer);
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        ImGui::Separator();
    }

    // ---- Statistics for the current folder ----
    ImGui::TextDisabled("Statistics");
    ImGui::Text("%d meshes, %llu verts, %llu tris", static_cast<int>(s.stats.mesh_count),
                static_cast<unsigned long long>(s.stats.vertex_count),
                static_cast<unsigned long long>(s.stats.triangle_count));
    ImGui::Text("%u texture refs, %.1f MB DDS on disk", s.stats.texture_count,
                static_cast<double>(s.stats.texture_bytes) / (1024.0 * 1024.0));
    if (s.stats.frame_count > 0)
        ImGui::Text("%u frames in session", s.stats.frame_count);
    ImGui::Separator();

    ImGui::Text("%d meshes", static_cast<int>(s.meshes.size()));
    if (ImGui::BeginChild("##meshlist", ImVec2(0, 0), true)) {
        for (int i = 0; i < static_cast<int>(s.meshes.size()); ++i) {
            const auto& m = s.meshes[i];
            ImGui::PushID(i);
            const bool is_sel = (s.selected == i);
            char label[160];
            std::snprintf(label, sizeof(label), "%s  [%u v / %u t]", m.name.c_str(),
                          m.vertex_count, m.triangle_count);
            if (ImGui::Selectable(label, is_sel)) {
                s.selected = i;
                load_selected_mesh(s, renderer);
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}


// ---------------------------------------------------------------------------
// 3D viewport: render the mesh into an FBO and show it as an ImGui image.
// Mouse input over the image drives the orbit camera.
// ---------------------------------------------------------------------------

void draw_viewport(AppState& s, ghost::viewer::MeshRenderer& renderer) {
    ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoScrollbar);

    ImVec2 avail = ImGui::GetContentRegionAvail();
    int w = static_cast<int>(avail.x);
    int h = static_cast<int>(avail.y);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    ensure_fbo(s, w, h);

    // ---- camera input over the viewport image ----
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool hovered = ImGui::IsItemHovered() ||
        (io.MousePos.x >= pos.x && io.MousePos.x <= pos.x + w &&
         io.MousePos.y >= pos.y && io.MousePos.y <= pos.y + h &&
         ImGui::IsWindowHovered());

    if (hovered) {
        const float dx = io.MouseDelta.x * 0.01f;
        const float dy = io.MouseDelta.y * 0.01f;
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            s.camera.yaw   -= dx;
            s.camera.pitch += dy;
            s.camera.pitch = (s.camera.pitch < -1.5f) ? -1.5f : s.camera.pitch;
            s.camera.pitch = (s.camera.pitch >  1.5f) ?  1.5f : s.camera.pitch;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
            // Pan in the plane facing the camera.
            const Vec3 right{s.camera.view_matrix().m[0],
                             s.camera.view_matrix().m[1],
                             s.camera.view_matrix().m[2]};
            const Vec3 up{s.camera.view_matrix().m[4],
                          s.camera.view_matrix().m[5],
                          s.camera.view_matrix().m[6]};
            const float scale = s.camera.distance * 0.0015f;
            s.camera.target = s.camera.target - right * dx * scale + up * dy * scale;
        }
        s.camera.distance *= (1.0f - io.MouseWheel * 0.1f);
        s.camera.distance = (s.camera.distance < 0.05f) ? 0.05f : s.camera.distance;
    }

    // ---- render to FBO ----
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(0, 0, s.fbo_w, s.fbo_h);
    glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    if (renderer.has_mesh()) {
        const Mat4 view = s.camera.view_matrix();
        const Mat4 proj = Mat4::perspective(0.9f,
                                            static_cast<float>(s.fbo_w) / s.fbo_h,
                                            0.05f, 10000.0f);
        const Mat4 mvp = Mat4::multiply(proj, view);
        renderer.draw(mvp.m, view.m, s.wireframe, s.show_tex);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // ---- display ----
    // ImTextureID is ImU64 in ImGui >= 1.91; a static_cast from the GL
    // texture handle is sufficient (zero-extended to 64-bit).
    ImGui::Image(static_cast<ImTextureID>(s.fbo_color),
                 ImVec2(static_cast<float>(w), static_cast<float>(h)),
                 ImVec2(0, 1), ImVec2(1, 0));  // flip Y for OpenGL
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Inspector / render options panel
// ---------------------------------------------------------------------------

void draw_inspector(AppState& s, ghost::viewer::MeshRenderer& renderer) {
    ImGui::Begin("Inspector");

    if (s.selected >= 0 && s.selected < static_cast<int>(s.meshes.size())) {
        const auto& m = s.meshes[s.selected];
        ImGui::TextDisabled("Mesh");
        ImGui::Text("name:    %s", m.name.c_str());
        ImGui::Text("source:  %s", m.source_app.c_str());
        ImGui::Text("verts:   %u", m.vertex_count);
        ImGui::Text("tris:    %u", m.triangle_count);
        ImGui::Text("normals: %s", m.has_normals ? "yes" : "no");
        ImGui::Text("uvs:     %s", m.has_uvs ? "yes" : "no");
        ImGui::Separator();
        ImGui::TextDisabled("Bounding box");
        ImGui::Text("min: %.3f %.3f %.3f", m.aabb_min.x, m.aabb_min.y, m.aabb_min.z);
        ImGui::Text("max: %.3f %.3f %.3f", m.aabb_max.x, m.aabb_max.y, m.aabb_max.z);
    } else {
        ImGui::TextDisabled("no mesh selected");
    }

    ImGui::Separator();
    ImGui::TextDisabled("View");
    ImGui::Checkbox("Wireframe", &s.wireframe);
    ImGui::Checkbox("Show texture", &s.show_tex);
    ImGui::Checkbox("Auto-frame on load", &s.auto_frame);
    ImGui::Checkbox("Texture panel", &s.show_texture_panel);
    if (ImGui::Button("Reset camera") && renderer.has_mesh())
        recenter_camera(s.camera, renderer.aabb_min(), renderer.aabb_max());

    // ---- Texture preview (the mesh's bound DDS, decoded by the viewer) ----
    if (s.show_texture_panel && renderer.has_mesh()) {
        ImGui::Separator();
        ImGui::TextDisabled("Texture");
        if (renderer.has_real_texture()) {
            ImGui::Text("size: %dx%d", renderer.texture_width(), renderer.texture_height());
            const std::string& path = renderer.texture_path();
            const size_t slash = path.find_last_of("/\\");
            ImGui::Text("file: %s", slash == std::string::npos ? path.c_str()
                                                              : path.c_str() + slash + 1);
            // Fit the preview into the panel width, preserving aspect ratio.
            const float avail = ImGui::GetContentRegionAvail().x;
            float w = static_cast<float>(renderer.texture_width());
            float h = static_cast<float>(renderer.texture_height());
            if (w > avail && w > 0.0f) {
                h *= avail / w;
                w = avail;
            }
            if (w > 0.0f && h > 0.0f)
                ImGui::Image(static_cast<ImTextureID>(renderer.texture_id()), ImVec2(w, h));
        } else {
            ImGui::TextDisabled("no captured texture bound to this mesh");
        }
    }

    // ---- Vertex layout editor -------------------------------------------
    ImGui::Separator();
    ImGui::TextDisabled("Vertex layout");
    if (s.selected >= 0) {
        static const struct { const char* name; uint32_t value; } kSemantics[] = {
            {"Position", 0}, {"Normal", 1}, {"Tangent", 2}, {"Binormal", 3}, {"Color", 4},
            {"TexCoord", 5}, {"BlendWeights", 6}, {"BlendIndices", 7}, {"Unknown", 0xFF},
        };
        static const struct { const char* name; uint32_t value; } kFormats[] = {
            {"Unknown", 0}, {"F32", 1}, {"F32x2", 2}, {"F32x3", 3}, {"F32x4", 4},
            {"F16x2", 5}, {"F16x4", 6}, {"UN8x2", 7}, {"UN8x4", 8}, {"SN8x4", 9},
            {"U8x4", 10}, {"U16x2", 11}, {"U16x4", 12}, {"U32", 13}, {"U32x2", 14},
            {"U32x3", 15}, {"U32x4", 16}, {"S32", 17}, {"S32x4", 18},
        };
        ImGui::Text("stride: %d  attrs: %d", s.edit_stride,
                    static_cast<int>(s.edit_attrs.size()));
        int remove_index = -1;
        for (int i = 0; i < static_cast<int>(s.edit_attrs.size()); ++i) {
            ghost::fmt::MeshAttribute& a = s.edit_attrs[i];
            ImGui::PushID(i);
            const char* sem_label = "?";
            for (const auto& e : kSemantics)
                if (e.value == a.semantic) sem_label = e.name;
            ImGui::SetNextItemWidth(96);
            if (ImGui::BeginCombo("##sem", sem_label)) {
                for (const auto& e : kSemantics) {
                    if (ImGui::Selectable(e.name, e.value == a.semantic))
                        a.semantic = e.value;
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            const char* fmt_label = "?";
            for (const auto& e : kFormats)
                if (e.value == a.format) fmt_label = e.name;
            ImGui::SetNextItemWidth(64);
            if (ImGui::BeginCombo("##fmt", fmt_label)) {
                for (const auto& e : kFormats) {
                    if (ImGui::Selectable(e.name, e.value == a.format))
                        a.format = e.value;
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(48);
            int off = static_cast<int>(a.offset);
            if (ImGui::InputInt("##off", &off, 0, 0))
                a.offset = off < 0 ? 0 : static_cast<uint32_t>(off);
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                remove_index = i;
            ImGui::PopID();
        }
        if (remove_index >= 0)
            s.edit_attrs.erase(s.edit_attrs.begin() + remove_index);

        if (ImGui::Button("+ Attribute")) {
            ghost::fmt::MeshAttribute a{};
            a.semantic = static_cast<uint32_t>(ghost::fmt::Semantic::Unknown);
            a.format = static_cast<uint32_t>(ghost::fmt::AttributeFormat::Float32x2);
            s.edit_attrs.push_back(a);
        }
        ImGui::SameLine();
        if (ImGui::Button("Apply"))
            apply_layout_edit(s, renderer);
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            s.edit_attrs = s.gpu_mesh ? s.gpu_mesh->attributes
                                      : std::vector<ghost::fmt::MeshAttribute>{};
            s.status = "layout reset to the loaded layout";
        }
        if (ImGui::Button("Save layout") && !s.loaded_path.empty()) {
            std::string err;
            if (ghost::viewer::save_layout_override(s.loaded_path, s.edit_attrs, &err))
                s.status = "saved " + s.loaded_path + ".layout";
            else
                s.status = "layout save failed: " + err;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(auto-applied on load)");
    } else {
        ImGui::TextDisabled("select a mesh to edit its layout");
    }

    ImGui::Separator();
    ImGui::TextDisabled("Export");
    ImGui::BeginDisabled(!renderer.has_mesh());
    if (ImGui::Button("Export OBJ")) {
        // Save next to the loaded capture file with an .obj extension.
        std::string out = s.loaded_path;
        const size_t dot = out.find_last_of('.');
        if (dot != std::string::npos) out = out.substr(0, dot);
        out += ".obj";
        std::string err;
        if (ghost::viewer::export_obj(out, *s.gpu_mesh, &err))
            s.status = "exported " + out;
        else
            s.status = "export failed: " + err;
    }
    ImGui::SameLine();
    if (ImGui::Button("Export glTF")) {
        std::string out = s.loaded_path;
        const size_t dot = out.find_last_of('.');
        if (dot != std::string::npos) out = out.substr(0, dot);
        out += ".gltf";
        std::string err;
        ghost::viewer::GltfMeshInput in;
        in.mesh = s.gpu_mesh.get();
        in.name = s.meshes[s.selected].name;
        if (!s.gpu_mesh->texture_rel.empty())
            in.texture_uri = relative_uri(ghost::viewer::parent_dir(out),
                                          s.gpu_mesh->texture_abs);
        const std::vector<ghost::viewer::GltfMeshInput> inputs{in};
        if (ghost::viewer::export_gltf(out, inputs, &err))
            s.status = "exported " + out;
        else
            s.status = "glTF export failed: " + err;
    }
    ImGui::EndDisabled();

    ImGui::BeginDisabled(s.meshes.empty());
    if (ImGui::Button("Export all OBJ")) {
        uint32_t ok_count = 0;
        for (const auto& info : s.meshes) {
            std::string err;
            auto mesh = ghost::viewer::load_mesh(info.path, &err);
            if (!mesh)
                continue;
            const std::string out = info.path.substr(0, info.path.find_last_of('.')) + ".obj";
            if (ghost::viewer::export_obj(out, *mesh, &err))
                ++ok_count;
        }
        s.status = "batch OBJ: " + std::to_string(ok_count) + "/" +
                   std::to_string(s.meshes.size()) + " meshes";
    }
    ImGui::SameLine();
    if (ImGui::Button("Export frame glTF")) {
        // All meshes of the folder become nodes of one scene.
        std::string out_path = s.capture_dir;
        if (!out_path.empty() && (out_path.back() == '/' || out_path.back() == '\\'))
            out_path.pop_back();
        out_path += SEP_S + std::string("frame.gltf");
        std::vector<ghost::viewer::GltfMeshInput> inputs;
        std::vector<std::unique_ptr<ghost::viewer::GPUMesh>> keep;
        for (const auto& info : s.meshes) {
            std::string err;
            auto mesh = ghost::viewer::load_mesh(info.path, &err);
            if (!mesh)
                continue;
            ghost::viewer::GltfMeshInput in;
            in.name = info.name;
            if (!mesh->texture_abs.empty())
                in.texture_uri = relative_uri(ghost::viewer::parent_dir(out_path),
                                              mesh->texture_abs);
            keep.push_back(std::move(mesh));
            in.mesh = keep.back().get();
            inputs.push_back(in);
        }
        std::string err;
        if (!inputs.empty() && ghost::viewer::export_gltf(out_path, inputs, &err))
            s.status = "exported " + std::to_string(inputs.size()) + " meshes -> " + out_path;
        else
            s.status = "frame glTF export failed: " + err;
    }
    ImGui::EndDisabled();

    ImGui::End();
}


// ---------------------------------------------------------------------------
// Window callbacks
// ---------------------------------------------------------------------------

void glfw_error_cb(int code, const char* msg) {
    ghost::log::error("GLFW %d: %s", code, msg);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    ghost::log::info("ghost-viewer starting");

    if (!glfwInit()) {
        ghost::log::error("glfwInit failed");
        return 1;
    }
    glfwSetErrorCallback(glfw_error_cb);

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(1400, 860, "Ghost-Ripper Viewer", nullptr, nullptr);
    if (!window) {
        ghost::log::error("glfwCreateWindow failed");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // vsync

    // --- ImGui init ---
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init();  // populates ImGui's bundled GL loader

    // Load the extra GL functions (FBOs, mipmaps, depth) that ImGui's stripped
    // loader doesn't include. glfwGetProcAddress is the same callback ImGui used.
    if (!gl_extra_init(reinterpret_cast<GLExtraGetProcAddress>(glfwGetProcAddress)))
        ghost::log::warn("gl_extra_init: some GL functions missing");

    // --- our renderer (after ImGui's GL backend initializes the loader) ---
    ghost::viewer::MeshRenderer renderer;
    if (!renderer.initialize())
        ghost::log::error("MeshRenderer::initialize failed");

    AppState s;
    s.capture_dir = (argc > 1) ? argv[1] : "captures";
    refresh_capture_dir(s);
    s.status = "scanned " + std::to_string(s.meshes.size()) + " meshes in " + s.capture_dir;

    // --- main loop ---
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Menu bar
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Rescan folder")) {
                    refresh_capture_dir(s);
                    s.status = "rescanned: " + std::to_string(s.meshes.size()) + " meshes";
                }
                if (ImGui::MenuItem("Next frame") && s.frame_index >= 0 &&
                    s.frame_index + 1 < static_cast<int>(s.frames.size()))
                    open_frame(s, s.frame_index + 1, renderer);
                if (ImGui::MenuItem("Previous frame") && s.frame_index > 0)
                    open_frame(s, s.frame_index - 1, renderer);
                if (ImGui::MenuItem("Exit"))
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        // --- Layout: three panels + a status bar, positioned explicitly.
        // WorkPos/WorkSize already account for the main menu bar height.
        ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImVec2 work_pos = vp->WorkPos;
        const ImVec2 work_size = vp->WorkSize;
        const float side = 300.0f;
        const float status_h = 24.0f;
        const float center_w = work_size.x - side * 2.0f;

        // Left: capture browser / mesh list
        ImGui::SetNextWindowPos(ImVec2(work_pos.x, work_pos.y));
        ImGui::SetNextWindowSize(ImVec2(side, work_size.y - status_h));
        ImGui::Begin("Browser", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
        draw_mesh_list(s, renderer);
        ImGui::End();

        // Center: 3D viewport
        ImGui::SetNextWindowPos(ImVec2(work_pos.x + side, work_pos.y));
        ImGui::SetNextWindowSize(ImVec2(center_w, work_size.y - status_h));
        draw_viewport(s, renderer);

        // Right: inspector
        ImGui::SetNextWindowPos(ImVec2(work_pos.x + side + center_w, work_pos.y));
        ImGui::SetNextWindowSize(ImVec2(side, work_size.y - status_h));
        draw_inspector(s, renderer);

        // Status bar
        ImGui::SetNextWindowPos(ImVec2(work_pos.x, work_pos.y + work_size.y - status_h));
        ImGui::SetNextWindowSize(ImVec2(work_size.x, status_h));
        ImGui::Begin("Status", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
        ImGui::TextUnformatted(s.status.c_str());
        ImGui::End();

        ImGui::Render();
        int fb_w = 0, fb_h = 0;
        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // --- cleanup ---
    if (s.fbo) {
        glDeleteFramebuffers(1, &s.fbo);
        glDeleteTextures(1, &s.fbo_color);
        glDeleteRenderbuffers(1, &s.fbo_depth);
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

