// Integration test for the viewer's headless asset layer (ghost_viewer_core):
// .grm loading with base_vertex handling, captured-texture path resolution,
// frame-folder scanning/statistics and vertex-layout overrides with sidecar
// persistence. No GPU needed.
#include "mesh_asset.h"

#include "ghost/attribute_guess.h"
#include "ghost/capture_writer.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using ghost::viewer::GPUMesh;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL: %s\n", what);
    }
}

struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};

// Build <root>/frame_000001/{meshes/Mesh_0000.grm, textures/0test.dds} the way
// the hook lays a capture out, with a sliced vertex window (base_vertex != 0).
void build_capture_tree(const fs::path& root) {
    fs::remove_all(root);
    fs::create_directories(root / "frame_000001" / "meshes");
    fs::create_directories(root / "frame_000001" / "textures");

    // Ten buffer vertices; the draw uses vertices 4..7, so the hook stores a
    // four-vertex slice and base_vertex = -4.
    std::vector<Vertex> buffer(10);
    for (int i = 0; i < 10; ++i) {
        buffer[i] = {static_cast<float>(i), 0.0f, 1.0f, 0.0f, 1.0f, 0.0f,
                     static_cast<float>(i) * 0.1f, 0.5f};
    }
    const Vertex* slice = buffer.data() + 4;
    const uint16_t indices[6] = {4, 5, 6, 4, 7, 5};  // raw buffer indices

    ghost::MeshData mesh;
    mesh.name = "Mesh_0000";
    mesh.attributes = ghost::guess_vertex_layout(sizeof(Vertex));
    mesh.vertex_data = slice;
    mesh.vertex_data_size = 4 * sizeof(Vertex);
    mesh.vertex_count = 4;
    mesh.vertex_stride = sizeof(Vertex);
    mesh.index_data = indices;
    mesh.index_data_size = sizeof(indices);
    mesh.index_count = 6;
    mesh.index_format = ghost::fmt::IndexFormat::UInt16;
    mesh.topology = ghost::fmt::PrimitiveTopology::TriangleList;
    mesh.base_vertex = -4;

    ghost::fmt::TextureRef tex{};
    tex.content_hash = 0x1234ull;
    tex.slot = 0;
    tex.usage = static_cast<uint32_t>(ghost::fmt::TextureUsage::Diffuse);
    std::snprintf(tex.file, sizeof(tex.file), "textures/0test.dds");
    mesh.textures.push_back(tex);

    std::string err;
    const std::string grm = (root / "frame_000001" / "meshes" / "Mesh_0000.grm").string();
    check(ghost::write_mesh_file(grm, mesh, "test_app", 1, &err), "write frame capture");

    // A stand-in DDS so the path resolution has something real to find.
    std::ofstream dds((root / "frame_000001" / "textures" / "0test.dds").string(),
                      std::ios::binary);
    dds << "DDS fake";
}

void test_load_and_texture() {
    std::printf("[check] load_mesh: vertex slice, base_vertex and texture path\n");
    const fs::path root = fs::temp_directory_path() / "ghost_viewer_assets";
    build_capture_tree(root);

    const std::string grm = (root / "frame_000001" / "meshes" / "Mesh_0000.grm").string();
    std::string err;
    auto mesh = ghost::viewer::load_mesh(grm, &err);
    check(mesh != nullptr, "load_mesh succeeds");
    if (!mesh)
        return;

    check(mesh->positions.size() == 12, "4 vertices decoded");
    // Slice vertex 0 is buffer vertex 4.
    check(mesh->positions[0] == 4.0f && mesh->positions[3] == 5.0f, "slice decoded");
    check(mesh->has_normals && mesh->has_uvs, "normals + uvs present");
    check(mesh->uvs[0] == 0.4f, "uv decoded from the slice");

    // Indices are raw (4..7) and base_vertex maps them into the slice: every
    // triangle must reference the 4 stored vertices.
    check(mesh->indices.size() == 6, "two triangles kept");
    for (uint32_t idx : mesh->indices)
        check(idx < 4, "base_vertex applied when expanding indices");

    check(mesh->texture_rel == "textures/0test.dds", "texture ref kept");
    // Resolved against the frame root, not the meshes/ subfolder.
    check(fs::exists(mesh->texture_abs), "texture path resolves to an existing file");

    check(mesh->vertex_stride == sizeof(Vertex), "stride recorded");
    check(mesh->attributes.size() == 3, "attributes recorded");
}

void test_scan_and_stats() {
    std::printf("[check] frame scanning and statistics\n");
    const fs::path root = fs::temp_directory_path() / "ghost_viewer_assets";
    const std::string frame = (root / "frame_000001").string();

    auto frames = ghost::viewer::list_frame_dirs(root.string());
    check(frames.size() == 1, "session lists its frame folder");

    auto meshes = ghost::viewer::scan_capture_dir(frame);
    check(meshes.size() == 1, "frame scan finds meshes/ contents");
    if (!meshes.empty()) {
        check(meshes[0].name == "Mesh_0000", "mesh name");
        check(meshes[0].vertex_count == 4, "vertex count");
        check(meshes[0].triangle_count == 2, "triangle count");
        check(meshes[0].texture_count == 1, "texture ref counted");
        check(fs::is_regular_file(meshes[0].path), "mesh path exists");
    }

    const ghost::viewer::CaptureStats stats =
        ghost::viewer::compute_capture_stats(frame);
    check(stats.mesh_count == 1, "stats: meshes");
    check(stats.vertex_count == 4, "stats: vertices");
    check(stats.triangle_count == 2, "stats: triangles");
    check(stats.texture_bytes > 0, "stats: texture bytes counted");
}

void test_layout_override() {
    std::printf("[check] vertex layout override + sidecar\n");
    const fs::path root = fs::temp_directory_path() / "ghost_viewer_assets";
    const std::string grm = (root / "frame_000001" / "meshes" / "Mesh_0000.grm").string();

    // Override: pretend the normal is at offset 0 - the decoded positions must
    // change accordingly (and the file on disk is untouched).
    ghost::viewer::LayoutOverride layout;
    ghost::fmt::MeshAttribute pos{};
    pos.semantic = static_cast<uint32_t>(ghost::fmt::Semantic::Position);
    pos.format = static_cast<uint32_t>(ghost::fmt::AttributeFormat::Float32x3);
    pos.offset = 12;  // the real normals
    layout.attributes.push_back(pos);

    std::string err;
    auto mesh = ghost::viewer::load_mesh(grm, &err, &layout);
    check(mesh != nullptr, "override load succeeds");
    if (mesh) {
        check(mesh->positions[0] == 0.0f, "override changed the decoded data");
        check(mesh->attributes.size() == 1, "override layout recorded");
    }

    // Sidecar round-trip.
    const std::string sidecar = grm + ".layout";
    check(ghost::viewer::save_layout_override(grm, layout.attributes, &err),
          "layout sidecar written");
    check(fs::exists(sidecar), "sidecar file exists");
    std::vector<ghost::fmt::MeshAttribute> loaded;
    check(ghost::viewer::load_layout_override(grm, loaded), "sidecar read back");
    check(loaded.size() == 1 && loaded[0].offset == 12 &&
              loaded[0].semantic == pos.semantic,
          "sidecar round-trips the attributes");
    std::remove(sidecar.c_str());
    check(!ghost::viewer::load_layout_override(grm, loaded),
          "missing sidecar reports false");
}

} // namespace

int main() {
    test_load_and_texture();
    test_scan_and_stats();
    test_layout_override();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
