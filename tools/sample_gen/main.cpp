// sample_gen: writes sample .grm capture files (cube, UV sphere, ground grid)
// so the viewer and tools can be exercised without a live game capture.
//
// Usage: sample_gen [output_dir]
#include "ghost/attribute_guess.h"
#include "ghost/capture_writer.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#define mkdir_one(path) _mkdir(path)
#else
#include <sys/stat.h>
#define mkdir_one(path) mkdir(path, 0755)
#endif

namespace {

// P (float3) + N (float3) + UV (float2) = 32 bytes, matches the guesser table.
struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};

void make_cube(std::vector<Vertex>& verts, std::vector<uint32_t>& indices) {
    const float p = 0.5f;
    struct Face { float n[3]; float corners[4][3]; };
    const Face faces[6] = {
        {{ 1, 0, 0}, {{ p,-p, p},{ p,-p,-p},{ p, p,-p},{ p, p, p}}},
        {{-1, 0, 0}, {{-p,-p,-p},{-p,-p, p},{-p, p, p},{-p, p,-p}}},
        {{ 0, 1, 0}, {{-p, p, p},{ p, p, p},{ p, p,-p},{-p, p,-p}}},
        {{ 0,-1, 0}, {{-p,-p,-p},{ p,-p,-p},{ p,-p, p},{-p,-p, p}}},
        {{ 0, 0, 1}, {{-p,-p, p},{ p,-p, p},{ p, p, p},{-p, p, p}}},
        {{ 0, 0,-1}, {{ p,-p,-p},{-p,-p,-p},{-p, p,-p},{ p, p,-p}}},
    };
    for (const Face& f : faces) {
        const uint32_t base = static_cast<uint32_t>(verts.size());
        const float uvs[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
        for (int c = 0; c < 4; ++c)
            verts.push_back({f.corners[c][0], f.corners[c][1], f.corners[c][2],
                             f.n[0], f.n[1], f.n[2], uvs[c][0], uvs[c][1]});
        indices.insert(indices.end(), {base, base + 1, base + 2,
                                       base, base + 2, base + 3});
    }
}

void make_sphere(std::vector<Vertex>& verts, std::vector<uint32_t>& indices,
                 int rings = 24, int sectors = 48) {
    for (int r = 0; r <= rings; ++r) {
        const float v = static_cast<float>(r) / rings;
        const float phi = 3.14159265f * v;  // 0 at north pole
        for (int s = 0; s <= sectors; ++s) {
            const float u = static_cast<float>(s) / sectors;
            const float theta = 2.0f * 3.14159265f * u;
            const float x = std::sin(phi) * std::cos(theta);
            const float y = std::cos(phi);
            const float z = std::sin(phi) * std::sin(theta);
            verts.push_back({x * 0.5f, y * 0.5f, z * 0.5f, x, y, z, u, v});
        }
    }
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < sectors; ++s) {
            const uint32_t a = r * (sectors + 1) + s;
            const uint32_t b = a + sectors + 1;
            indices.insert(indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
}

void make_grid(std::vector<Vertex>& verts, std::vector<uint32_t>& indices,
               int n = 10, float extent = 5.0f) {
    // Flat quad used as a ground plane in the sample scene.
    (void)n;
    const float e = extent;
    const float y = -0.5f;
    verts = {
        {-e, y, -e, 0, 1, 0, 0, 0},
        { e, y, -e, 0, 1, 0, 1, 0},
        { e, y,  e, 0, 1, 0, 1, 1},
        {-e, y,  e, 0, 1, 0, 0, 1},
    };
    indices = {0, 1, 2, 0, 2, 3};
}

bool write(const std::string& dir, const char* name,
           const std::vector<Vertex>& verts, const std::vector<uint32_t>& indices) {
    ghost::MeshData mesh;
    mesh.name        = name;
    mesh.attributes  = ghost::guess_vertex_layout(sizeof(Vertex));
    mesh.vertex_data = verts.data();
    mesh.vertex_data_size = static_cast<uint32_t>(verts.size() * sizeof(Vertex));
    mesh.vertex_count  = static_cast<uint32_t>(verts.size());
    mesh.vertex_stride = sizeof(Vertex);
    mesh.index_data    = indices.data();
    mesh.index_data_size = static_cast<uint32_t>(indices.size() * sizeof(uint32_t));
    mesh.index_count   = static_cast<uint32_t>(indices.size());
    mesh.index_format  = ghost::fmt::IndexFormat::UInt32;
    mesh.topology      = ghost::fmt::PrimitiveTopology::TriangleList;

    const std::string path = dir + "/" + name + ".grm";
    std::string err;
    if (!ghost::write_mesh_file(path, mesh, "sample_gen", 0, &err)) {
        std::fprintf(stderr, "sample_gen: %s\n", err.c_str());
        return false;
    }
    std::printf("wrote %s (%zu verts, %zu tris)\n", path.c_str(), verts.size(),
                indices.size() / 3);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "captures/sample";
    mkdir_one(dir.c_str());  // best effort; single level

    std::vector<Vertex> verts;
    std::vector<uint32_t> indices;

    verts.clear(); indices.clear();
    make_cube(verts, indices);
    if (!write(dir, "cube", verts, indices)) return 1;

    verts.clear(); indices.clear();
    make_sphere(verts, indices);
    if (!write(dir, "sphere", verts, indices)) return 1;

    verts.clear(); indices.clear();
    make_grid(verts, indices);
    if (!write(dir, "grid", verts, indices)) return 1;

    return 0;
}
