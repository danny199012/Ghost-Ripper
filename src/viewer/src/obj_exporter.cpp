#include "obj_exporter.h"

#include <cstdio>
#include <fstream>

namespace ghost {
namespace viewer {

namespace {

std::string base_name(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    return name;
}

std::string sibling(const std::string& path, const std::string& new_ext) {
    const size_t slash = path.find_last_of("\\/");
    const std::string dir = (slash == std::string::npos) ? "" : path.substr(0, slash + 1);
    return dir + base_name(path) + new_ext;
}

} // namespace

bool export_obj(const std::string& out_path, const GPUMesh& mesh, std::string* err) {
    const std::string mtl_path = sibling(out_path, ".mtl");
    const std::string mtl_name = base_name(out_path);

    // Material file.
    std::ofstream mtl(mtl_path, std::ios::trunc);
    if (!mtl) {
        if (err) *err = "cannot write " + mtl_path;
        return false;
    }
    mtl << "newmtl " << mtl_name << "\n"
        << "Kd 0.8 0.8 0.8\n"
        << "Ka 0.1 0.1 0.1\n"
        << "Ks 0.0 0.0 0.0\n"
        << "Ns 1\n";

    // Geometry file.
    std::ofstream obj(out_path, std::ios::trunc);
    if (!obj) {
        if (err) *err = "cannot write " + out_path;
        return false;
    }
    obj << "# Exported by Ghost-Ripper viewer\n"
        << "mtllib " << mtl_name << ".mtl\n"
        << "o " << mtl_name << "\n";

    const size_t vcount = mesh.positions.size() / 3;
    for (size_t i = 0; i < vcount; ++i) {
        obj << "v " << mesh.positions[i * 3 + 0] << ' '
            << mesh.positions[i * 3 + 1] << ' '
            << mesh.positions[i * 3 + 2] << '\n';
    }
    if (mesh.has_uvs) {
        for (size_t i = 0; i < vcount; ++i) {
            obj << "vt " << mesh.uvs[i * 2 + 0] << ' ' << mesh.uvs[i * 2 + 1] << '\n';
        }
    }
    if (mesh.has_normals) {
        for (size_t i = 0; i < vcount; ++i) {
            obj << "vn " << mesh.normals[i * 3 + 0] << ' '
                << mesh.normals[i * 3 + 1] << ' '
                << mesh.normals[i * 3 + 2] << '\n';
        }
    }

    obj << "usemtl " << mtl_name << "\n";
    obj << "s off\n";

    const size_t tris = mesh.indices.size() / 3;
    for (size_t t = 0; t < tris; ++t) {
        // OBJ indices are 1-based. Compose the face line depending on which
        // vertex attributes are present: v, v/vt, v//vn, or v/vt/vn.
        obj << 'f';
        for (int k = 0; k < 3; ++k) {
            const uint32_t idx = mesh.indices[t * 3 + k] + 1;
            obj << ' ' << idx;
            if (mesh.has_uvs && mesh.has_normals)      obj << '/' << idx << '/' << idx;
            else if (mesh.has_uvs)                     obj << '/' << idx;
            else if (mesh.has_normals)                 obj << "//" << idx;
        }
        obj << '\n';
    }
    return static_cast<bool>(obj);
}

} // namespace viewer
} // namespace ghost
