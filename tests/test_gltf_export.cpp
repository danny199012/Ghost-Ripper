// Tests for the glTF 2.0 exporter: a textured cube-ish mesh (UVs + normals +
// DDS texture) and a bare triangle (no UVs, no indices) must export to a
// .gltf/.bin pair that python's json.tool accepts.

#include "gltf_exporter.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using ghost::viewer::export_gltf;
using ghost::viewer::GltfMeshInput;
using ghost::viewer::GPUMesh;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            return 1;                                                        \
        }                                                                    \
    } while (0)

namespace {

std::string read_text(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

std::vector<char> read_binary(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
}

// Number that follows `key` searched from `from` ( crude but enough for
// the flat accessor/bufferView objects the exporter writes ).
long int_after(const std::string& s, size_t from, const char* key) {
    const size_t k = s.find(key, from);
    if (k == std::string::npos) return -1;
    return std::strtol(s.c_str() + s.find(':', k) + 1, nullptr, 10);
}

// Position of the n-th '{' inside the array named `array_key`.
size_t nth_object(const std::string& s, const char* array_key, size_t n) {
    size_t pos = s.find(array_key);
    if (pos == std::string::npos) return std::string::npos;
    pos = s.find('[', pos);
    for (size_t i = 0; i <= n && pos != std::string::npos; ++i)
        pos = s.find('{', pos + 1);
    return pos;
}

} // namespace

int main() {
    // Mesh 1: cube-ish (8 verts, 12 indices, normals + UVs), DDS textured.
    GPUMesh cube;
    cube.has_normals = true;
    cube.has_uvs = true;
    cube.positions = {-1, -1, -1,  1, -1, -1,  1, 1, -1, -1, 1, -1,
                      -1, -1,  1,  1, -1,  1,  1, 1,  1, -1, 1,  1};
    cube.normals.assign(cube.positions.size(), 0.5f);
    cube.uvs = {0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1};
    cube.indices = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};

    // Mesh 2: bare triangle, no UVs/normals and no indices (exporter must
    // generate sequential indices).
    GPUMesh tri;
    tri.positions = {0, 0, 0, 1, 0, 0, 0, 1, 0};

    std::vector<GltfMeshInput> inputs;
    GltfMeshInput in_cube;
    in_cube.mesh = &cube;
    in_cube.name = "cube \"A\"";  // exercises JSON escaping
    in_cube.texture_uri = "../textures/crate.dds";
    inputs.push_back(in_cube);
    GltfMeshInput in_tri;
    in_tri.mesh = &tri;
    in_tri.name = "tri_angle";
    inputs.push_back(in_tri);

    CHECK(std::system("mkdir -p /tmp/gltf_test") == 0);
    const std::string out = "/tmp/gltf_test/out.gltf";
    const std::string out_bin = "/tmp/gltf_test/out.bin";

    std::string err;
    CHECK(export_gltf(out, inputs, &err));

    // Both files must exist and the .gltf must be valid JSON.
    CHECK(!read_text(out).empty());
    CHECK(!read_binary(out_bin).empty());
    const std::string cmd = "python3 -m json.tool " + out + " > /dev/null 2>&1";
    CHECK(std::system(cmd.c_str()) == 0);

    const std::string gltf = read_text(out);
    const std::vector<char> bin = read_binary(out_bin);

    CHECK(gltf.find("\"version\"") != std::string::npos);
    CHECK(gltf.find("2.0") != std::string::npos);
    CHECK(gltf.find("MSFT_texture_dds") != std::string::npos);
    CHECK(gltf.find("\"POSITION\"") != std::string::npos);
    CHECK(gltf.find("\"TEXCOORD_0\"") != std::string::npos);
    CHECK(gltf.find("baseColorTexture") != std::string::npos);
    CHECK(gltf.find("cube \\\"A\\\"") != std::string::npos);  // escaped name
    CHECK(gltf.find("tri_angle") != std::string::npos);

    // The buffer byteLength must match the .bin file and be 4-aligned.
    const long byte_len = int_after(gltf, gltf.find("\"buffers\""), "\"byteLength\"");
    CHECK(byte_len > 0);
    CHECK(static_cast<size_t>(byte_len) == bin.size());
    CHECK(byte_len % 4 == 0);

    // Walk POSITION attribute -> accessor -> bufferView to find where the
    // first mesh's positions live in the .bin, then compare the raw bytes.
    const long pos_acc = int_after(gltf, gltf.find("\"POSITION\""), "\"POSITION\"");
    CHECK(pos_acc >= 0);
    const size_t acc_obj = nth_object(gltf, "\"accessors\"", static_cast<size_t>(pos_acc));
    CHECK(acc_obj != std::string::npos);
    const long pos_view = int_after(gltf, acc_obj, "\"bufferView\"");
    CHECK(pos_view >= 0);
    const size_t view_obj = nth_object(gltf, "\"bufferViews\"", static_cast<size_t>(pos_view));
    CHECK(view_obj != std::string::npos);
    const long pos_off = int_after(gltf, view_obj, "\"byteOffset\"");
    const long pos_len = int_after(gltf, view_obj, "\"byteLength\"");
    CHECK(pos_off >= 0 && pos_len == 96);  // 8 verts * 3 floats * 4 bytes
    CHECK(static_cast<size_t>(pos_off + pos_len) <= bin.size());
    CHECK(std::memcmp(bin.data() + pos_off, cube.positions.data(), 24) == 0);

    // A textured mesh and an untextured one both exported above; also check
    // an untextured-only export succeeds without the DDS extension.
    std::vector<GltfMeshInput> only_tri;
    GltfMeshInput plain = in_tri;
    plain.name.clear();  // exercise the "mesh_N" fallback name
    only_tri.push_back(plain);
    const std::string out2 = "/tmp/gltf_test/plain.gltf";
    std::string err2;
    CHECK(export_gltf(out2, only_tri, &err2));
    const std::string gltf2 = read_text(out2);
    CHECK(gltf2.find("MSFT_texture_dds") == std::string::npos);
    CHECK(gltf2.find("baseColorTexture") == std::string::npos);
    CHECK(gltf2.find("mesh_0") != std::string::npos);  // name fallback

    // Error paths: empty list, null mesh, empty positions.
    std::vector<GltfMeshInput> empty;
    CHECK(!export_gltf(out, empty, &err));
    CHECK(!err.empty());
    std::vector<GltfMeshInput> null_mesh;
    GltfMeshInput bad;
    bad.mesh = nullptr;
    bad.name = "null";
    null_mesh.push_back(bad);
    CHECK(!export_gltf(out, null_mesh, &err));
    GPUMesh no_verts;
    std::vector<GltfMeshInput> no_pos;
    GltfMeshInput in_no;
    in_no.mesh = &no_verts;
    in_no.name = "empty";
    no_pos.push_back(in_no);
    CHECK(!export_gltf(out, no_pos, &err));

    std::printf("all gltf export tests passed\n");
    return 0;
}

