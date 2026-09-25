#include "mesh_asset.h"

#include "ghost/attribute.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#define SEP '\\'
#define SEP_STR "\\"
#else
#include <dirent.h>
#define SEP '/'
#define SEP_STR "/"
#endif

namespace ghost {
namespace viewer {

namespace {

bool dir_exists_impl(const std::string& path) {
#ifdef _WIN32
    const DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

uint64_t file_size_impl(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0)
        return 0;
    return static_cast<uint64_t>(st.st_size);
}

} // namespace

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

// Decode one attribute element from the mesh's raw vertex blob using an
// explicit attribute record (the file's own or a user override).
bool decode_with(const MeshFile& mesh, uint32_t stride, const fmt::MeshAttribute& a,
                 uint32_t i, float out[4]) {
    const uint8_t* raw = mesh.vertex_data();
    if (!raw || i >= mesh.header().vertex_count)
        return false;
    return decode_attribute(raw + static_cast<size_t>(i) * stride + a.offset,
                            static_cast<fmt::AttributeFormat>(a.format), out);
}

const fmt::MeshAttribute* find_attr(const std::vector<fmt::MeshAttribute>& attrs,
                                    fmt::Semantic semantic, uint32_t index) {
    for (const auto& a : attrs)
        if (a.semantic == static_cast<uint32_t>(semantic) && a.semantic_index == index)
            return &a;
    return nullptr;
}

} // namespace

std::unique_ptr<GPUMesh> load_mesh(const std::string& path, std::string* err,
                                   const LayoutOverride* layout) {
    auto out = std::make_unique<GPUMesh>();
    MeshFile mesh;
    if (!mesh.load(path, err))
        return nullptr;

    const std::vector<fmt::MeshAttribute>& attrs =
        (layout && !layout->attributes.empty()) ? layout->attributes : mesh.attributes();
    const uint32_t stride = mesh.header().vertex_stride;
    const uint32_t vc     = mesh.header().vertex_count;

    const fmt::MeshAttribute* pos_attr = find_attr(attrs, fmt::Semantic::Position, 0);
    if (!pos_attr) {
        if (err) *err = "no POSITION attribute in " + path;
        return nullptr;
    }

    out->attributes    = attrs;
    out->vertex_stride = stride;
    out->positions.resize(static_cast<size_t>(vc) * 3);
    for (uint32_t i = 0; i < vc; ++i) {
        float p[4]{};
        if (decode_with(mesh, stride, *pos_attr, i, p)) {
            out->positions[i * 3 + 0] = p[0];
            out->positions[i * 3 + 1] = p[1];
            out->positions[i * 3 + 2] = p[2];
        }
    }

    // Normals (optional).
    if (const fmt::MeshAttribute* n_attr = find_attr(attrs, fmt::Semantic::Normal, 0)) {
        std::vector<float> normals(static_cast<size_t>(vc) * 3, 0.0f);
        bool any = false;
        for (uint32_t i = 0; i < vc; ++i) {
            float n[4]{};
            if (decode_with(mesh, stride, *n_attr, i, n)) {
                normals[i * 3 + 0] = n[0];
                normals[i * 3 + 1] = n[1];
                normals[i * 3 + 2] = n[2];
                any = true;
            }
        }
        if (any) {
            out->has_normals = true;
            out->normals = std::move(normals);
        }
    }

    // UVs, channel 0 only (optional).
    if (const fmt::MeshAttribute* uv_attr = find_attr(attrs, fmt::Semantic::TexCoord, 0)) {
        std::vector<float> uvs(static_cast<size_t>(vc) * 2, 0.0f);
        bool any = false;
        for (uint32_t i = 0; i < vc; ++i) {
            float u[4]{};
            if (decode_with(mesh, stride, *uv_attr, i, u)) {
                uvs[i * 2 + 0] = u[0];
                uvs[i * 2 + 1] = u[1];
                any = true;
            }
        }
        if (any) {
            out->has_uvs = true;
            out->uvs = std::move(uvs);
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

    // First diffuse texture, resolved against the capture folder.
    for (const auto& t : mesh.textures()) {
        if (t.usage == static_cast<uint32_t>(fmt::TextureUsage::Diffuse) && t.file[0]) {
            out->texture_rel = t.file;
            break;
        }
    }
    if (out->texture_rel.empty()) {
        for (const auto& t : mesh.textures()) {
            if (t.file[0]) {  // any bound texture is better than none
                out->texture_rel = t.file;
                break;
            }
        }
    }
    if (!out->texture_rel.empty()) {
        std::string rel = out->texture_rel;
        for (char& c : rel)
            if (c == '/')
                c = SEP;
        out->texture_abs = parent_dir(path) + SEP + rel;
    }
    return out;
}

std::string parent_dir(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return (slash == std::string::npos) ? std::string() : path.substr(0, slash);
}

bool save_layout_override(const std::string& grm_path,
                          const std::vector<fmt::MeshAttribute>& attributes,
                          std::string* err) {
    std::ofstream out(grm_path + ".layout", std::ios::trunc);
    if (!out) {
        if (err) *err = "cannot write " + grm_path + ".layout";
        return false;
    }
    out << "# Ghost-Ripper viewer vertex layout override\n"
        << "# semantic semantic_index format offset\n";
    for (const auto& a : attributes)
        out << a.semantic << ' ' << a.semantic_index << ' ' << a.format << ' ' << a.offset
            << '\n';
    return static_cast<bool>(out);
}

bool load_layout_override(const std::string& grm_path,
                          std::vector<fmt::MeshAttribute>& attributes) {
    std::ifstream in(grm_path + ".layout");
    if (!in)
        return false;
    attributes.clear();
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream ls(line);
        fmt::MeshAttribute a{};
        if (ls >> a.semantic >> a.semantic_index >> a.format >> a.offset)
            attributes.push_back(a);
    }
    return !attributes.empty();
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

std::vector<std::string> list_subdirs(const std::string& dir, const std::string& prefix) {
    std::vector<std::string> dirs;
#ifdef _WIN32
    std::string pattern = dir + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return dirs;
    do {
        const std::string name = fd.cFileName;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name != "." && name != ".." &&
            name.rfind(prefix, 0) == 0)
            dirs.push_back(dir + "\\" + name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return dirs;
    while (struct dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == ".." || name.rfind(prefix, 0) != 0)
            continue;
        if (dir_exists_impl(dir + "/" + name))
            dirs.push_back(dir + "/" + name);
    }
    closedir(d);
#endif
    std::sort(dirs.begin(), dirs.end());
    return dirs;
}

} // namespace

std::vector<std::string> list_frame_dirs(const std::string& session_dir) {
    return list_subdirs(session_dir, "frame_");
}

std::vector<MeshInfo> scan_capture_dir(const std::string& dir, std::string* err) {
    std::vector<MeshInfo> infos;
    // Hook frames keep meshes in a meshes/ subfolder; sample_gen writes a flat
    // folder. Accept both.
    std::vector<std::string> files = list_grm_files(dir);
    const std::vector<std::string> nested = list_grm_files(dir + SEP_STR + "meshes");
    files.insert(files.end(), nested.begin(), nested.end());
    std::sort(files.begin(), files.end());
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
        info.texture_count = static_cast<uint32_t>(mesh.textures().size());
        for (const auto& t : mesh.textures()) {
            if (t.file[0]) {
                info.texture_rel = t.file;
                break;
            }
        }
        infos.push_back(std::move(info));
    }
    return infos;
}

CaptureStats compute_capture_stats(const std::string& dir) {
    CaptureStats stats;
    const std::vector<MeshInfo> infos = scan_capture_dir(dir);
    stats.mesh_count = static_cast<uint32_t>(infos.size());
    for (const auto& m : infos) {
        stats.vertex_count += m.vertex_count;
        stats.triangle_count += m.triangle_count;
        stats.texture_count += m.texture_count;
    }

    // Texture bytes on disk: the frame's textures/ folder, or (for a session
    // folder) every frame's textures/ folder.
    std::vector<std::string> texture_dirs{dir + SEP_STR + "textures"};
    for (const std::string& frame : list_frame_dirs(dir))
        texture_dirs.push_back(frame + SEP_STR + "textures");
    for (const std::string& tex_dir : texture_dirs) {
#ifdef _WIN32
        const std::string pattern = tex_dir + "\\*.dds";
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        do {
            stats.texture_bytes += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) |
                                   fd.nFileSizeLow;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
#else
        DIR* d = opendir(tex_dir.c_str());
        if (!d)
            continue;
        while (struct dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (has_suffix(name, ".dds"))
                stats.texture_bytes += file_size_impl(tex_dir + "/" + name);
        }
        closedir(d);
#endif
    }
    stats.frame_count = static_cast<uint32_t>(list_frame_dirs(dir).size());
    return stats;
}

} // namespace viewer
} // namespace ghost

