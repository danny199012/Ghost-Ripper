#include "ghost/capture_writer.h"

#include "ghost/attribute.h"

#include <cstring>
#include <ctime>
#include <fstream>

namespace ghost {

uint64_t fnv1a64(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

bool compute_mesh_aabb(MeshData& mesh) {
    const fmt::MeshAttribute* pos_attr = nullptr;
    for (const auto& a : mesh.attributes) {
        if (a.semantic == static_cast<uint32_t>(fmt::Semantic::Position)) {
            pos_attr = &a;
            break;
        }
    }
    if (!pos_attr || !mesh.vertex_data || mesh.vertex_count == 0)
        return false;

    const auto* base = static_cast<const uint8_t*>(mesh.vertex_data);
    Vec3 lo{1e30f, 1e30f, 1e30f};
    Vec3 hi{-1e30f, -1e30f, -1e30f};

    for (uint32_t i = 0; i < mesh.vertex_count; ++i) {
        const uint8_t* v = base + static_cast<size_t>(i) * mesh.vertex_stride + pos_attr->offset;
        float p[4];
        if (!decode_attribute(v, static_cast<fmt::AttributeFormat>(pos_attr->format), p))
            return false;
        // Skip obviously-broken values (NaN/inf) rather than poisoning the box.
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
            continue;
        lo.x = p[0] < lo.x ? p[0] : lo.x;  hi.x = p[0] > hi.x ? p[0] : hi.x;
        lo.y = p[1] < lo.y ? p[1] : lo.y;  hi.y = p[1] > hi.y ? p[1] : hi.y;
        lo.z = p[2] < lo.z ? p[2] : lo.z;  hi.z = p[2] > hi.z ? p[2] : hi.z;
    }

    if (lo.x > hi.x)  // every vertex was invalid
        return false;

    mesh.aabb_min = lo;
    mesh.aabb_max = hi;
    mesh.aabb_valid = true;
    return true;
}

bool write_mesh_file(const std::string& path,
                     const MeshData& mesh_in,
                     const std::string& source_app,
                     uint32_t frame_index,
                     std::string* err) {
    MeshData mesh = mesh_in;
    if (!mesh.aabb_valid)
        compute_mesh_aabb(mesh);

    if (!mesh.vertex_data || mesh.vertex_data_size == 0 || mesh.vertex_count == 0) {
        if (err) *err = "mesh has no vertex data";
        return false;
    }

    fmt::FileHeader fh{};
    std::memcpy(fh.magic, fmt::kMagic, sizeof(fh.magic));
    fh.version     = fmt::kVersion;
    fh.header_size = sizeof(fmt::FileHeader);
    fh.frame_index = frame_index;
    fh.timestamp   = static_cast<uint64_t>(std::time(nullptr));
    std::strncpy(fh.source_app, source_app.c_str(), sizeof(fh.source_app) - 1);

    fmt::MeshHeader mh{};
    mh.vertex_count      = mesh.vertex_count;
    mh.vertex_stride     = mesh.vertex_stride;
    mh.index_count       = mesh.index_count;
    mh.index_format      = static_cast<uint32_t>(mesh.index_format);
    mh.topology          = static_cast<uint32_t>(mesh.topology);
    mh.attribute_count   = static_cast<uint32_t>(mesh.attributes.size());
    mh.start_index       = mesh.start_index;
    mh.base_vertex       = mesh.base_vertex;
    mh.texture_count     = static_cast<uint32_t>(mesh.textures.size());
    mh.vertex_data_size  = mesh.vertex_data_size;
    mh.index_data_size   = mesh.index_data_size;
    mh.flags             = mesh.aabb_valid ? fmt::kFlagAabbValid : 0u;
    mh.aabb_min[0] = mesh.aabb_min.x; mh.aabb_min[1] = mesh.aabb_min.y; mh.aabb_min[2] = mesh.aabb_min.z;
    mh.aabb_max[0] = mesh.aabb_max.x; mh.aabb_max[1] = mesh.aabb_max.y; mh.aabb_max[2] = mesh.aabb_max.z;
    std::strncpy(mh.name, mesh.name.c_str(), sizeof(mh.name) - 1);

    uint32_t offset = sizeof(fmt::FileHeader) + sizeof(fmt::MeshHeader);
    mh.attributes_offset  = offset;
    offset += mh.attribute_count * sizeof(fmt::MeshAttribute);
    mh.textures_offset    = offset;
    offset += mh.texture_count * sizeof(fmt::TextureRef);
    mh.vertex_data_offset = offset;
    offset += mh.vertex_data_size;
    mh.index_data_offset  = offset;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (err) *err = "cannot open file for writing: " + path;
        return false;
    }

    out.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
    out.write(reinterpret_cast<const char*>(&mh), sizeof(mh));
    if (mh.attribute_count)
        out.write(reinterpret_cast<const char*>(mesh.attributes.data()),
                  mh.attribute_count * sizeof(fmt::MeshAttribute));
    if (mh.texture_count)
        out.write(reinterpret_cast<const char*>(mesh.textures.data()),
                  mh.texture_count * sizeof(fmt::TextureRef));
    out.write(static_cast<const char*>(mesh.vertex_data), mesh.vertex_data_size);
    if (mesh.index_data && mesh.index_data_size)
        out.write(static_cast<const char*>(mesh.index_data), mesh.index_data_size);

    if (!out) {
        if (err) *err = "write failed: " + path;
        return false;
    }
    return true;
}

} // namespace ghost
