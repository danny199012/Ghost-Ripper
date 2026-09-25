#include "mesh_asset.h"

#include "ghost/attribute.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

namespace ghost {
namespace viewer {

namespace {

// Expand strip/line topologies into triangle lists so the renderer only needs
// one draw mode. Also skips degenerate / unknown primitives.
// Indices are stored as raw buffer indices; base_vertex (which may be
// negative after slicing a captured range) maps them into the stored slice.
void expand_triangles(const MeshFile& mesh, GPUMesh& out) {
    const fmt::PrimitiveTopology topo =
        static_cast<fmt::PrimitiveTopology>(mesh.header().topology);
    const uint32_t count = mesh.header().index_count ? mesh.header().index_count
                                                      : mesh.header().vertex_count;
    const int64_t  base_vertex = mesh.header().base_vertex;
    const uint32_t vc = mesh.header().vertex_count;

    auto push_tri = [&](int64_t a, int64_t b, int64_t c) {
        if (a < 0 || b < 0 || c < 0)
            return;  // index outside the captured vertex slice
        if (static_cast<uint64_t>(a) >= vc || static_cast<uint64_t>(b) >= vc ||
            static_cast<uint64_t>(c) >= vc)
            return;
        out.indices.push_back(static_cast<uint32_t>(a));
        out.indices.push_back(static_cast<uint32_t>(b));
        out.indices.push_back(static_cast<uint32_t>(c));
    };
    auto v = [&](uint32_t i) -> int64_t {
        return static_cast<int64_t>(mesh.index(i)) + base_vertex;
    };

    if (topo == fmt::PrimitiveTopology::TriangleList) {
        for (uint32_t i = 0; i + 2 < count; i += 3)
            push_tri(v(i), v(i + 1), v(i + 2));
    } else if (topo == fmt::PrimitiveTopology::TriangleStrip) {
        for (uint32_t i = 0; i + 2 < count; ++i)
            push_tri(v(i), v(i + (i & 1) ? 2 : 1), v(i + (i & 1) ? 1 : 2));
    }
    // Line/point topologies are ignored for the 3D viewport for now.
}

void compute_face_normals(GPUMesh& out) {
    if (out.positions.empty() || out.indices.empty())
        return;
    out.normals.assign(out.positions.size(), 0.0f);
    const size_t tris = out.indices.size() / 3;
    for (size_t t = 0; t < tris; ++t) {
        const uint32_t i0 = out.indices[t * 3 + 0];
        const uint32_t i1 = out.indices[t * 3 + 1];
        const uint32_t i2 = out.indices[t * 3 + 2];
        const Vec3 p0{out.positions[i0 * 3], out.positions[i0 * 3 + 1], out.positions[i0 * 3 + 2]};
        const Vec3 p1{out.positions[i1 * 3], out.positions[i1 * 3 + 1], out.positions[i1 * 3 + 2]};
        const Vec3 p2{out.positions[i2 * 3], out.positions[i2 * 3 + 1], out.positions[i2 * 3 + 2]};
        const Vec3 n = (p1 - p0).cross(p2 - p0).normalized();
        for (const uint32_t idx : {i0, i1, i2}) {
            out.normals[idx * 3 + 0] += n.x;
            out.normals[idx * 3 + 1] += n.y;
            out.normals[idx * 3 + 2] += n.z;
        }
    }
    for (size_t i = 0; i < out.normals.size(); i += 3) {
        const Vec3 n{out.normals[i], out.normals[i + 1], out.normals[i + 2]};
        const Vec3 nn = n.normalized();
        out.normals[i + 0] = nn.x;
        out.normals[i + 1] = nn.y;
        out.normals[i + 2] = nn.z;
    }
}

bool mesh_has_position(const MeshFile& mesh) {
    for (const auto& a : mesh.attributes())
        if (a.semantic == static_cast<uint32_t>(fmt::Semantic::Position))
            return true;
    return false;
}

} // namespace

std::unique_ptr<GPUMesh> load_mesh(const std::string& path, std::string* err) {
    auto out = std::make_unique<GPUMesh>();
    MeshFile mesh;
    if (!mesh.load(path, err))
        return nullptr;
    if (!mesh_has_position(mesh)) {
        if (err) *err = "no POSITION attribute in " + path;
        return nullptr;
    }

    const uint32_t vc = mesh.header().vertex_count;
    out->positions.resize(static_cast<size_t>(vc) * 3);

    for (uint32_t i = 0; i < vc; ++i) {
        Vec3 p;
        if (!mesh.position(i, p)) {
            if (err) *err = "failed to decode position " + std::to_string(i);
            return nullptr;
        }
        out->positions[i * 3 + 0] = p.x;
        out->positions[i * 3 + 1] = p.y;
        out->positions[i * 3 + 2] = p.z;
    }

    // Normals (optional).
    for (uint32_t i = 0; i < vc; ++i) {
        Vec3 n;
        if (mesh.normal(i, n)) {
            if (!out->has_normals) {
                out->has_normals = true;
                out->normals.resize(static_cast<size_t>(vc) * 3, 0.0f);
            }
            out->normals[i * 3 + 0] = n.x;
            out->normals[i * 3 + 1] = n.y;
            out->normals[i * 3 + 2] = n.z;
        }
    }

    // UVs, channel 0 only (optional).
    Vec2 probe;
    if (mesh.texcoord(0, 0, probe)) {
        out->has_uvs = true;
        out->uvs.resize(static_cast<size_t>(vc) * 2, 0.0f);
        for (uint32_t i = 0; i < vc; ++i) {
            Vec2 u;
            if (mesh.texcoord(i, 0, u)) {
                out->uvs[i * 2 + 0] = u.x;
                out->uvs[i * 2 + 1] = u.y;
            }
        }
    }

    expand_triangles(mesh, *out);

    if (!out->has_normals)
        compute_face_normals(*out);

    out->aabb_min = {mesh.header().aabb_min[0], mesh.header().aabb_min[1],
                     mesh.header().aabb_min[2]};
    out->aabb_max = {mesh.header().aabb_max[0], mesh.header().aabb_max[1],
                     mesh.header().aabb_max[2]};
    out->triangle_count = static_cast<uint32_t>(out->indices.size() / 3);
    return out;
}

namespace {

bool has_suffix(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
}

std::vector<std::string> list_grm_files(const std::string& dir) {
    std::vector<std::string> files;
#ifdef _WIN32
    std::string pattern = dir + "\\*.grm";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return files;
    do {
        files.push_back(dir + "\\" + fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return files;
    while (struct dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (has_suffix(name, ".grm"))
            files.push_back(dir + "/" + name);
    }
    closedir(d);
#endif
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace

std::vector<MeshInfo> scan_capture_dir(const std::string& dir, std::string* err) {
    std::vector<MeshInfo> infos;
    const std::vector<std::string> files = list_grm_files(dir);
    for (const std::string& f : files) {
        MeshFile mesh;
        if (!mesh.load(f)) {
            if (err) *err = "skip unreadable: " + f;
            continue;
        }
        MeshInfo info;
        info.path = f;
        info.name = mesh.header().name;
        info.vertex_count = mesh.header().vertex_count;
        info.triangle_count = mesh.triangle_count();
        info.source_app = mesh.file_header().source_app;
        info.aabb_min = {mesh.header().aabb_min[0], mesh.header().aabb_min[1],
                         mesh.header().aabb_min[2]};
        info.aabb_max = {mesh.header().aabb_max[0], mesh.header().aabb_max[1],
                         mesh.header().aabb_max[2]};
        for (const auto& a : mesh.attributes()) {
            if (a.semantic == static_cast<uint32_t>(fmt::Semantic::Normal))   info.has_normals = true;
            if (a.semantic == static_cast<uint32_t>(fmt::Semantic::TexCoord)) info.has_uvs = true;
        }
        infos.push_back(std::move(info));
    }
    return infos;
}

} // namespace viewer
} // namespace ghost

