// Unit test for the GRMF capture format: writer/reader round-trip, AABB
// computation, section offsets, negative base_vertex handling and attribute
// decoding. Headless (no GL, no ImGui).
#include "ghost/attribute.h"
#include "ghost/attribute_guess.h"
#include "ghost/capture_reader.h"
#include "ghost/capture_writer.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

// Write a quad (4 vertices, 6 indices) with the given base_vertex.
std::string write_quad(const std::string& path, int32_t base_vertex, float offset) {
    const Vertex verts[4] = {
        {offset + 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f},
        {offset + 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f},
        {offset + 1.0f, 0.0f, 2.0f, 0.0f, 1.0f, 0.0f, 1.0f, 2.0f},
        {offset + 0.0f, 0.0f, 2.0f, 0.0f, 1.0f, 0.0f, 0.0f, 2.0f},
    };
    const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};

    ghost::MeshData mesh;
    mesh.name = "Quad";
    mesh.attributes = ghost::guess_vertex_layout(sizeof(Vertex));
    mesh.vertex_data = verts;
    mesh.vertex_data_size = sizeof(verts);
    mesh.vertex_count = 4;
    mesh.vertex_stride = sizeof(Vertex);
    mesh.index_data = indices;
    mesh.index_data_size = sizeof(indices);
    mesh.index_count = 6;
    mesh.index_format = ghost::fmt::IndexFormat::UInt16;
    mesh.topology = ghost::fmt::PrimitiveTopology::TriangleList;
    mesh.base_vertex = base_vertex;

    ghost::fmt::TextureRef tex{};
    tex.content_hash = 0xDEADBEEF12345678ull;
    tex.slot = 0;
    tex.usage = static_cast<uint32_t>(ghost::fmt::TextureUsage::Diffuse);
    std::strncpy(tex.file, "textures/0badc0de.dds", sizeof(tex.file) - 1);
    mesh.textures.push_back(tex);

    std::string err;
    check(ghost::write_mesh_file(path, mesh, "test_app", 7, &err), "write_mesh_file");
    return path;
}

} // namespace

void test_roundtrip() {
    std::printf("[check] writer/reader round-trip\n");
    const std::string path = write_quad("/tmp/ghost_test_quad.grm", -3, 10.0f);

    ghost::MeshFile mesh;
    std::string err;
    check(mesh.load(path, &err), "reader loads the file");
    check(std::memcmp(mesh.file_header().magic, "GRMF", 4) == 0, "magic");
    check(mesh.file_header().version == ghost::fmt::kVersion, "format version");
    check(mesh.file_header().frame_index == 7, "frame index");
    check(std::strcmp(mesh.file_header().source_app, "test_app") == 0, "source app");
    check(mesh.header().vertex_count == 4, "vertex count");
    check(mesh.header().vertex_stride == sizeof(Vertex), "vertex stride");
    check(mesh.header().index_count == 6, "index count");
    check(mesh.header().index_format == static_cast<uint32_t>(ghost::fmt::IndexFormat::UInt16),
          "index format");
    check(mesh.header().base_vertex == -3, "base vertex (negative)");
    check(std::strcmp(mesh.header().name, "Quad") == 0, "mesh name");
    check(mesh.attributes().size() == 3, "attribute count");
    check(mesh.textures().size() == 1, "texture count");
    check(mesh.textures()[0].content_hash == 0xDEADBEEF12345678ull, "texture hash");
    check(mesh.textures()[0].usage == static_cast<uint32_t>(ghost::fmt::TextureUsage::Diffuse),
          "texture usage");
    check(std::strcmp(mesh.textures()[0].file, "textures/0badc0de.dds") == 0, "texture file");

    const auto* v = reinterpret_cast<const Vertex*>(mesh.vertex_data());
    check(v != nullptr, "vertex data pointer");
    check(v && v[2].px == 11.0f && v[2].pz == 2.0f, "vertex bytes survive the trip");

    // Index values are stored raw; the reader exposes them unchanged.
    check(mesh.index(0) == 0 && mesh.index(1) == 1 && mesh.index(2) == 2, "indices");
    check(mesh.triangle_count() == 2, "triangle count");

    // AABB computed from positions: x in [10, 11], z in [0, 2], y = 0.
    check((mesh.header().flags & ghost::fmt::kFlagAabbValid) != 0, "aabb flag");
    check(mesh.header().aabb_min[0] == 10.0f && mesh.header().aabb_min[2] == 0.0f, "aabb min");
    check(mesh.header().aabb_max[0] == 11.0f && mesh.header().aabb_max[2] == 2.0f, "aabb max");

    ghost::Vec3 p, n;
    ghost::Vec2 uv;
    check(mesh.position(1, p) && p.x == 11.0f, "position() decodes");
    check(mesh.normal(1, n) && n.y == 1.0f, "normal() decodes");
    check(mesh.texcoord(2, 0, uv) && uv.y == 2.0f, "texcoord() decodes");
}

void test_corrupt() {
    std::printf("[check] corrupt/truncated files are rejected\n");
    std::vector<uint8_t> bytes;
    {
        FILE* f = std::fopen("/tmp/ghost_test_quad.grm", "rb");
        check(f != nullptr, "read back for corruption test");
        if (f) {
            std::fseek(f, 0, SEEK_END);
            const long size = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            bytes.resize(static_cast<size_t>(size));
            check(std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size(), "fread");
            std::fclose(f);
        }
    }

    {  // Truncated: the reader's section bounds checks must reject it.
        const std::string path = "/tmp/ghost_test_trunc.grm";
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(bytes.data(), 1, bytes.size() / 2, f);
            std::fclose(f);
        }
        ghost::MeshFile mesh;
        std::string err;
        check(!mesh.load(path, &err), "truncated file rejected");
    }

    {  // Bad magic.
        const std::string path = "/tmp/ghost_test_magic.grm";
        std::vector<uint8_t> bad = bytes;
        bad[0] = 'X';
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(bad.data(), 1, bad.size(), f);
            std::fclose(f);
        }
        ghost::MeshFile mesh;
        check(!mesh.load(path, nullptr), "bad magic rejected");
    }
}

void test_attribute_decoding() {
    std::printf("[check] attribute decode helpers\n");
    using AF = ghost::fmt::AttributeFormat;
    check(ghost::attribute_element_size(AF::Float32x3) == 12, "element size f32x3");
    check(ghost::attribute_element_size(AF::UNorm8x4) == 4, "element size unorm8x4");
    check(ghost::attribute_component_count(AF::Float16x2) == 2, "component count f16x2");

    const uint16_t one_half = 0x3C00;  // half(1.0)
    float out[4]{};
    check(ghost::decode_attribute(&one_half, AF::Float16x2, out), "half decode ok");
    check(out[0] == 1.0f, "half(1.0) -> 1.0");

    const uint8_t bytes4[4] = {255, 128, 0, 64};
    check(ghost::decode_attribute(bytes4, AF::UNorm8x4, out), "unorm8x4 decode ok");
    check(out[0] == 1.0f && std::fabs(out[1] - 128.0f / 255.0f) < 1e-6f, "unorm8x4 values");

    const int8_t snorm[4] = {-128, 0, 127, 0};
    check(ghost::decode_attribute(snorm, AF::SNorm8x4, out), "snorm8x4 decode ok");
    check(out[0] == -1.0f && out[1] == 0.0f && out[2] == 1.0f, "snorm8x4 values");

    // Layout guessing places POSITION first at offset 0 for every stride.
    for (uint32_t stride : {12u, 20u, 32u, 40u, 56u, 99u}) {
        const auto attrs = ghost::guess_vertex_layout(stride);
        check(!attrs.empty() &&
                  attrs[0].semantic == static_cast<uint32_t>(ghost::fmt::Semantic::Position) &&
                  attrs[0].offset == 0,
              "guessed layout starts with POSITION at 0");
    }
}

int main() {
    test_roundtrip();
    test_corrupt();
    test_attribute_decoding();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
