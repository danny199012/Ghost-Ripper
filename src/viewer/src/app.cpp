// ghost-viewer: Dear ImGui + OpenGL3 + GLFW application for browsing and
// inspecting .grm capture files with a 3D viewport and OBJ export.
//
//   ghost-viewer [capture_dir]
#include "mesh_asset.h"
#include "mesh_renderer.h"
#include "obj_exporter.h"
#include "texture_loader.h"
#include "gl_extra.h"

#include "ghost/log.h"
#include "ghost/math.h"

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

    std::unique_ptr<ghost::viewer::GPUMesh> gpu_mesh;  // loaded geometry
    std::string loaded_path;

    Camera camera;
    bool wireframe  = false;
    bool show_tex   = true;
    bool auto_frame = true;  // re-center camera on a newly loaded mesh

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
    std::string err;
    auto mesh = ghost::viewer::load_mesh(path, &err);
    if (!mesh) {
        s.status = "load failed: " + err;
        return;
    }
    s.gpu_mesh = std::move(mesh);
    s.loaded_path = path;
    renderer.upload(*s.gpu_mesh);
    if (s.auto_frame)
        recenter_camera(s.camera, renderer.aabb_min(), renderer.aabb_max());
    s.status = "loaded " + s.meshes[s.selected].name +
               " (" + std::to_string(renderer.triangle_count()) + " tris)";
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
        s.meshes = ghost::viewer::scan_capture_dir(s.capture_dir);
        s.selected = -1;
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
            s.meshes = ghost::viewer::scan_capture_dir(s.capture_dir);
            s.selected = -1;
            s.status = "scanned " + std::to_string(s.meshes.size()) + " meshes";
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ImGui::Button("OK")) {
            s.capture_dir = pick;
            s.meshes = ghost::viewer::scan_capture_dir(s.capture_dir);
            s.selected = -1;
            s.status = "scanned " + std::to_string(s.meshes.size()) + " meshes";
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

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
    if (ImGui::Button("Reset camera") && renderer.has_mesh())
        recenter_camera(s.camera, renderer.aabb_min(), renderer.aabb_max());

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
    s.meshes = ghost::viewer::scan_capture_dir(s.capture_dir);
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
                    s.meshes = ghost::viewer::scan_capture_dir(s.capture_dir);
                    s.selected = -1;
                    s.status = "rescanned: " + std::to_string(s.meshes.size()) + " meshes";
                }
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

