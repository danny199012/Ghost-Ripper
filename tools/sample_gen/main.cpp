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

std::string parent_of(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// Create every missing component of `path` (single-level mkdir fails on
// nested paths like captures/session/frame_000001/meshes).
void make_dirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        const bool last = (i + 1 == path.size());
        if (last || path[i] == '/' || path[i] == '\\')
            mkdir_one(cur.c_str());  // EEXIST is fine
    }
}

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

// Minimal 32bpp BGRA DDS writer (the same shape the hook dumps) so sample
// captures can carry a texture without a game. Draws a checker with a colour
// gradient so it is obvious in the viewer's texture panel.
bool write_sample_dds(const std::string& path, uint32_t w, uint32_t h) {
    struct PixelFormat {
        uint32_t size, flags, four_cc, bit_count, r, g, b, a;
    };
    struct Header {
        uint32_t size, flags, height, width, pitch, depth, mips, reserved[11];
        PixelFormat pf;
        uint32_t caps, caps2, caps3, caps4, reserved2;
    };

    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* px = pixels.data() + (static_cast<size_t>(y) * w + x) * 4;
            const bool light = (((x / 16) + (y / 16)) % 2) == 0;
            const uint8_t base = light ? 220 : 80;
            px[0] = static_cast<uint8_t>(base / 2);                 // B
            px[1] = static_cast<uint8_t>((x * 255) / (w ? w : 1));  // G
            px[2] = static_cast<uint8_t>((y * 255) / (h ? h : 1));  // R
            px[3] = 255;
        }
    }

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    const uint32_t magic = 0x20534444;  // "DDS "
    std::fwrite(&magic, sizeof(magic), 1, f);

    Header hdr{};
    hdr.size = 124;
    hdr.flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000;  // caps|height|width|pitch|pf
    hdr.height = h;
    hdr.width = w;
    hdr.pitch = w * 4;
    hdr.mips = 1;
    hdr.pf.size = 32;
    hdr.pf.flags = 0x40 | 0x1;  // RGB | ALPHAPIXELS
    hdr.pf.bit_count = 32;
    hdr.pf.r = 0x00FF0000;
    hdr.pf.g = 0x0000FF00;
    hdr.pf.b = 0x000000FF;
    hdr.pf.a = 0xFF000000;
    hdr.caps = 0x1000;  // DDSCAPS_TEXTURE
    std::fwrite(&hdr, sizeof(hdr), 1, f);
    std::fwrite(pixels.data(), 1, pixels.size(), f);
    std::fclose(f);
    return true;
}

// Fill MeshData for a textured sample mesh (textures/checker.dds).
void fill_mesh(ghost::MeshData& mesh, const char* name, const std::vector<Vertex>& verts,
               const std::vector<uint32_t>& indices) {
    mesh.name = name;
    mesh.attributes = ghost::guess_vertex_layout(sizeof(Vertex));
    mesh.vertex_data = verts.data();
    mesh.vertex_data_size = static_cast<uint32_t>(verts.size() * sizeof(Vertex));
    mesh.vertex_count = static_cast<uint32_t>(verts.size());
    mesh.vertex_stride = sizeof(Vertex);
    mesh.index_data = indices.data();
    mesh.index_data_size = static_cast<uint32_t>(indices.size() * sizeof(uint32_t));
    mesh.index_count = static_cast<uint32_t>(indices.size());
    mesh.index_format = ghost::fmt::IndexFormat::UInt32;
    mesh.topology = ghost::fmt::PrimitiveTopology::TriangleList;

    ghost::fmt::TextureRef tex{};
    tex.content_hash = 0xC0FFEE12ull;
    tex.slot = 0;
    tex.usage = static_cast<uint32_t>(ghost::fmt::TextureUsage::Diffuse);
    std::snprintf(tex.file, sizeof(tex.file), "textures/checker.dds");
    mesh.textures.push_back(tex);
}

// Write a textured mesh into a flat capture folder.
bool write_textured(const std::string& dir, const char* name,
                    const std::vector<Vertex>& verts, const std::vector<uint32_t>& indices) {
    ghost::MeshData mesh;
    fill_mesh(mesh, name, verts, indices);
    const std::string path = dir + "/" + name + ".grm";
    std::string err;
    if (!ghost::write_mesh_file(path, mesh, "sample_gen", 0, &err)) {
        std::fprintf(stderr, "sample_gen: %s\n", err.c_str());
        return false;
    }
    std::printf("wrote %s (%zu verts, %zu tris, 1 texture)\n", path.c_str(), verts.size(),
                indices.size() / 3);
    return true;
}

void write_manifest(const std::string& frame_dir, const char* api, uint32_t frame,
                    uint32_t mesh_count) {
    FILE* f = std::fopen((frame_dir + "/manifest.json").c_str(), "w");
    if (!f)
        return;
    std::fprintf(f,
                 "{\n"
                 "  \"tool\": \"Ghost-Ripper\",\n"
                 "  \"format\": \"GRMF\",\n"
                 "  \"format_version\": %u,\n"
                 "  \"api\": \"%s\",\n"
                 "  \"app\": \"sample_gen\",\n"
                 "  \"pid\": 0,\n"
                 "  \"frame\": %u,\n"
                 "  \"mesh_count\": %u\n"
                 "}\n",
                 ghost::fmt::kVersion, api, frame, mesh_count);
    std::fclose(f);
}

// Mimic the hook's session layout (session/frame_XXXXXX/{meshes,textures}) with
// two frames so the viewer's frame browser has something to navigate.
bool write_session(const std::string& session) {
    std::vector<Vertex> verts;
    std::vector<uint32_t> indices;

    for (uint32_t frame = 1; frame <= 2; ++frame) {
        char name[64];
        std::snprintf(name, sizeof(name), "frame_%06u", frame);
        const std::string frame_dir = session + "/" + name;
        make_dirs(frame_dir + "/meshes");
        make_dirs(frame_dir + "/textures");

        if (!write_sample_dds(frame_dir + "/textures/checker.dds", 128, 128))
            return false;

        verts.clear();
        indices.clear();
        make_cube(verts, indices);
        if (frame == 2) {  // make the second frame visibly different
            for (Vertex& v : verts) {
                v.px *= 1.6f;
                v.py *= 1.6f;
                v.pz *= 1.6f;
            }
        }

        ghost::MeshData mesh;
        fill_mesh(mesh, "Mesh_0000", verts, indices);
        std::string err;
        const std::string path = frame_dir + "/meshes/Mesh_0000.grm";
        if (!ghost::write_mesh_file(path, mesh, "sample_gen", frame, &err)) {
            std::fprintf(stderr, "sample_gen: %s\n", err.c_str());
            return false;
        }
        write_manifest(frame_dir, "sample", frame, 1);
        std::printf("wrote %s (frame %u)\n", path.c_str(), frame);
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "captures/sample";
    make_dirs(dir);

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

    // A textured mesh with its DDS (flat layout), plus a hook-style session
    // tree with two frames for the viewer's frame browser.
    const std::string prev_dir = parent_of(dir);
    const std::string session = (prev_dir.empty() ? dir : prev_dir) + "/session";
    make_dirs(dir + "/textures");
    if (!write_sample_dds(dir + "/textures/checker.dds", 128, 128)) return 1;
    verts.clear(); indices.clear();
    make_cube(verts, indices);
    if (!write_textured(dir, "cube_textured", verts, indices)) return 1;
    if (!write_session(session)) return 1;

    return 0;
}
