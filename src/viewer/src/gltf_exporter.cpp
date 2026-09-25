// glTF 2.0 exporter: writes the given meshes as a .gltf + .bin pair.
//
// Layout per mesh: one primitive, one material. Accessors are emitted in a
// fixed order (indices, POSITION, NORMAL, TEXCOORD_0) so the bufferViews and
// the .bin stay tightly packed and 4-byte aligned.

#include "gltf_exporter.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace ghost {
namespace viewer {

namespace {

// File name without directory and extension ("a/b/out.gltf" -> "out").
std::string base_name(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot != 0) name = name.substr(0, dot);
    return name;
}

// JSON string literal (with surrounding quotes), escaping specials and
// control characters; names/URIs may contain quotes or backslashes.
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {  // remaining control chars as \u00XX
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
    return out;
}

// Shortest round-trip-ish float representation for JSON numbers.
std::string fmt_float(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
    return buf;
}

std::string fmt_size(size_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%zu", v);
    return buf;
}

// Comma between array elements except after the last one.
std::string sep(size_t i, size_t n) {
    return (i + 1 < n) ? "," : "";
}

// Case-insensitive ".dds" suffix check (URIs may be "Texture.DDS").
bool has_dds_uri(const std::string& uri) {
    if (uri.size() < 4) return false;
    const std::string tail = uri.substr(uri.size() - 4);
    for (size_t i = 0; i < 4; ++i) {
        if (std::tolower(static_cast<unsigned char>(tail[i])) != ".dds"[i]) return false;
    }
    return true;
}

// Append `bytes` from `data` into `bin`, padded so the returned offset is a
// multiple of 4 (glTF buffers are conventionally 4-byte aligned).
size_t append_aligned(std::vector<char>* bin, const void* data, size_t bytes) {
    while (bin->size() % 4 != 0) bin->push_back('\0');
    const size_t offset = bin->size();
    const char* p = static_cast<const char*>(data);
    bin->insert(bin->end(), p, p + bytes);
    return offset;
}

constexpr size_t kNone = static_cast<size_t>(-1);

// Per-mesh bookkeeping while building the buffer and the accessors.
struct MeshEntry {
    const GPUMesh* mesh = nullptr;
    std::string name;
    std::string texture_uri;

    size_t idx_acc = 0, pos_acc = 0;
    size_t nrm_acc = kNone;  // kNone when the mesh has no normals
    size_t uv_acc  = kNone;  // kNone when the mesh has no UVs

    size_t idx_view = 0, pos_view = 0, nrm_view = 0, uv_view = 0;
    size_t idx_off = 0, idx_len = 0;
    size_t pos_off = 0, pos_len = 0;
    size_t nrm_off = 0, nrm_len = 0;
    size_t uv_off  = 0, uv_len  = 0;

    size_t material = 0;
    size_t image    = kNone;  // only for textured meshes
    size_t texture  = kNone;

    float pos_min[3] = {0, 0, 0};
    float pos_max[3] = {0, 0, 0};
};


std::string accessor_json(size_t view, int component_type, size_t count,
                          const char* type, const float* mn, const float* mx) {
    std::string j = "      {\n        \"bufferView\": " + fmt_size(view) +
                    ",\n        \"componentType\": " + fmt_size(static_cast<size_t>(component_type)) +
                    ",\n        \"count\": " + fmt_size(count) +
                    ",\n        \"type\": " + json_escape(type);
    if (mn && mx) {
        j += ",\n        \"min\": [" + fmt_float(mn[0]) + ", " + fmt_float(mn[1]) + ", " + fmt_float(mn[2]) + "]";
        j += ",\n        \"max\": [" + fmt_float(mx[0]) + ", " + fmt_float(mx[1]) + ", " + fmt_float(mx[2]) + "]";
    }
    j += "\n      }";
    return j;
}

std::string buffer_view_json(size_t offset, size_t length, int target) {
    return "      {\n        \"buffer\": 0,\n        \"byteOffset\": " + fmt_size(offset) +
           ",\n        \"byteLength\": " + fmt_size(length) +
           ",\n        \"target\": " + fmt_size(static_cast<size_t>(target)) + "\n      }";
}

} // namespace


bool export_gltf(const std::string& out_path,
                 const std::vector<GltfMeshInput>& meshes,
                 std::string* err) {
    if (meshes.empty()) {
        if (err) *err = "no meshes to export";
        return false;
    }

    // Validate up front so a bad input never leaves a partial file pair.
    for (const GltfMeshInput& in : meshes) {
        if (!in.mesh) {
            if (err) *err = "null mesh in input list";
            return false;
        }
        if (in.mesh->positions.empty() || in.mesh->positions.size() % 3 != 0) {
            if (err) *err = "mesh with empty positions";
            return false;
        }
    }

    std::vector<char> bin;
    std::vector<MeshEntry> entries;
    size_t mat_count = 0, img_count = 0;

    for (size_t i = 0; i < meshes.size(); ++i) {
        const GltfMeshInput& in = meshes[i];
        const GPUMesh& m = *in.mesh;
        const size_t vcount = m.positions.size() / 3;

        MeshEntry e;
        e.mesh = &m;
        e.name = in.name.empty() ? ("mesh_" + fmt_size(i)) : in.name;
        e.texture_uri = in.texture_uri;

        // Sequential 0..N-1 indices for non-indexed meshes so every primitive
        // is indexed and all meshes share the same accessor layout.
        std::vector<uint32_t> sequential;
        const uint32_t* idx = m.indices.data();
        size_t idx_count = m.indices.size();
        if (idx_count == 0) {
            sequential.resize(vcount);
            for (size_t v = 0; v < vcount; ++v) sequential[v] = static_cast<uint32_t>(v);
            idx = sequential.data();
            idx_count = vcount;
        }

        // Indices first, then POSITION/NORMAL/TEXCOORD_0; offsets come from
        // append_aligned(), which keeps every view 4-byte aligned.
        e.idx_off = append_aligned(&bin, idx, idx_count * sizeof(uint32_t));
        e.idx_len = idx_count * sizeof(uint32_t);

        float mn[3] = {m.positions[0], m.positions[1], m.positions[2]};
        float mx[3] = {mn[0], mn[1], mn[2]};
        for (size_t v = 1; v < vcount; ++v) {
            for (int k = 0; k < 3; ++k) {
                const float c = m.positions[v * 3 + k];
                if (c < mn[k]) mn[k] = c;
                if (c > mx[k]) mx[k] = c;
            }
        }
        e.pos_min[0] = mn[0]; e.pos_min[1] = mn[1]; e.pos_min[2] = mn[2];
        e.pos_max[0] = mx[0]; e.pos_max[1] = mx[1]; e.pos_max[2] = mx[2];
        e.pos_off = append_aligned(&bin, m.positions.data(), m.positions.size() * sizeof(float));
        e.pos_len = m.positions.size() * sizeof(float);

        if (m.has_normals && !m.normals.empty()) {
            e.nrm_off = append_aligned(&bin, m.normals.data(), m.normals.size() * sizeof(float));
            e.nrm_len = m.normals.size() * sizeof(float);
        }
        if (m.has_uvs && !m.uvs.empty()) {
            e.uv_off = append_aligned(&bin, m.uvs.data(), m.uvs.size() * sizeof(float));
            e.uv_len = m.uvs.size() * sizeof(float);
        }

        e.material = mat_count++;
        if (!in.texture_uri.empty()) {
            e.image   = img_count++;
            e.texture = e.image;  // one texture per image, index-aligned
        }
        entries.push_back(e);
    }

    // Assign accessor/bufferView indices now that the per-mesh view count is
    // known (2-4 views depending on normals/UVs).
    size_t acc = 0, view = 0;
    for (MeshEntry& e : entries) {
        e.idx_acc = acc++;
        e.idx_view = view++;
        e.pos_acc = acc++;
        e.pos_view = view++;
        if (e.nrm_len) { e.nrm_acc = acc++; e.nrm_view = view++; }
        if (e.uv_len)  { e.uv_acc  = acc++; e.uv_view  = view++; }
    }

    // Round the total buffer length up to a multiple of 4 as glTF expects.
    while (bin.size() % 4 != 0) bin.push_back('\0');

    // Write the .bin first: no point in a .gltf pointing at a missing buffer.
    const size_t slash = out_path.find_last_of("\\/");
    const std::string dir = (slash == std::string::npos) ? "" : out_path.substr(0, slash + 1);
    const std::string bin_path = dir + base_name(out_path) + ".bin";
    std::ofstream bin_file(bin_path, std::ios::binary | std::ios::trunc);
    if (!bin_file) {
        if (err) *err = "cannot write " + bin_path;
        return false;
    }
    if (!bin.empty()) bin_file.write(bin.data(), static_cast<std::streamsize>(bin.size()));
    bin_file.close();
    if (!bin_file) {
        if (err) *err = "failed writing " + bin_path;
        return false;
    }

    bool any_dds = false;
    for (const MeshEntry& e : entries) {
        if (!e.texture_uri.empty() && has_dds_uri(e.texture_uri)) any_dds = true;
    }

    std::string j;
    j += "{\n";
    j += "  \"asset\": {\n    \"version\": \"2.0\",\n    \"generator\": \"Ghost-Ripper viewer\"\n  },\n";
    j += "  \"scene\": 0,\n";
    j += "  \"scenes\": [\n    {\n      \"nodes\": [";
    for (size_t i = 0; i < entries.size(); ++i) j += (i ? ", " : "") + fmt_size(i);
    j += "]\n    }\n  ],\n";

    j += "  \"nodes\": [\n";
    for (size_t i = 0; i < entries.size(); ++i) {
        j += "    {\n      \"mesh\": " + fmt_size(i) + ",\n      \"name\": "
             + json_escape(entries[i].name) + "\n    }" + sep(i, entries.size()) + "\n";
    }
    j += "  ],\n";

    j += "  \"meshes\": [\n";
    for (size_t i = 0; i < entries.size(); ++i) {
        const MeshEntry& e = entries[i];
        j += "    {\n      \"name\": " + json_escape(e.name) + ",\n";
        j += "      \"primitives\": [\n        {\n          \"attributes\": {\n";
        std::vector<std::string> attrs;
        attrs.push_back("\"POSITION\": " + fmt_size(e.pos_acc));
        if (e.nrm_acc != kNone) attrs.push_back("\"NORMAL\": " + fmt_size(e.nrm_acc));
        if (e.uv_acc != kNone)  attrs.push_back("\"TEXCOORD_0\": " + fmt_size(e.uv_acc));
        for (size_t a = 0; a < attrs.size(); ++a) {
            j += "            " + attrs[a] + (a + 1 < attrs.size() ? "," : "") + "\n";
        }
        j += "          },\n";
        j += "          \"indices\": " + fmt_size(e.idx_acc) + ",\n";
        j += "          \"material\": " + fmt_size(e.material) + ",\n";
        j += "          \"mode\": 4\n        }\n      ]\n    }" + sep(i, entries.size()) + "\n";
    }
    j += "  ],\n";

    j += "  \"materials\": [\n";
    for (size_t i = 0; i < entries.size(); ++i) {
        const MeshEntry& e = entries[i];
        j += "    {\n      \"name\": " + json_escape(e.name) + ",\n";
        j += "      \"pbrMetallicRoughness\": {\n";
        if (e.texture != kNone) {
            j += "        \"baseColorTexture\": { \"index\": " + fmt_size(e.texture) + " },\n";
        }
        j += "        \"baseColorFactor\": [1, 1, 1, 1],\n";
        j += "        \"metallicFactor\": 0,\n";
        j += "        \"roughnessFactor\": 0.9\n";
        j += "      }\n    }" + sep(i, entries.size()) + "\n";
    }
    j += "  ],\n";

    if (img_count > 0) {
        j += "  \"textures\": [\n";
        for (size_t i = 0; i < img_count; ++i) {
            j += "    { \"sampler\": 0, \"source\": " + fmt_size(i) + " }"
                 + sep(i, img_count) + "\n";
        }
        j += "  ],\n";
        j += "  \"images\": [\n";
        size_t written = 0;
        for (const MeshEntry& e : entries) {
            if (e.image == kNone) continue;
            j += "    { \"uri\": " + json_escape(e.texture_uri) + " }"
                 + sep(written, img_count) + "\n";
            ++written;
        }
        j += "  ],\n";
        j += "  \"samplers\": [\n    {\n";
        j += "      \"magFilter\": 9729,\n      \"minFilter\": 9987,\n";
        j += "      \"wrapS\": 10497,\n      \"wrapT\": 10497\n    }\n  ],\n";
    }

    j += "  \"accessors\": [\n";
    for (size_t i = 0; i < entries.size(); ++i) {
        const MeshEntry& e = entries[i];
        const GPUMesh& m = *e.mesh;
        const size_t vcount = m.positions.size() / 3;
        const size_t idx_count = m.indices.empty() ? vcount : m.indices.size();
        j += accessor_json(e.idx_view, 5125, idx_count, "SCALAR", nullptr, nullptr) + ",\n";
        j += accessor_json(e.pos_view, 5126, vcount, "VEC3", e.pos_min, e.pos_max);
        if (e.nrm_acc != kNone) {
            j += ",\n" + accessor_json(e.nrm_view, 5126, vcount, "VEC3", nullptr, nullptr);
        }
        if (e.uv_acc != kNone) {
            j += ",\n" + accessor_json(e.uv_view, 5126, vcount, "VEC2", nullptr, nullptr);
        }
        j += sep(i, entries.size()) + "\n";
    }
    j += "  ],\n";

    j += "  \"bufferViews\": [\n";
    for (size_t i = 0; i < entries.size(); ++i) {
        const MeshEntry& e = entries[i];
        std::vector<std::string> views;
        views.push_back(buffer_view_json(e.idx_off, e.idx_len, 34963));  // ELEMENT_ARRAY
        views.push_back(buffer_view_json(e.pos_off, e.pos_len, 34962));  // ARRAY_BUFFER
        if (e.nrm_len) views.push_back(buffer_view_json(e.nrm_off, e.nrm_len, 34962));
        if (e.uv_len)  views.push_back(buffer_view_json(e.uv_off, e.uv_len, 34962));
        for (size_t v = 0; v < views.size(); ++v) {
            j += views[v] + (v + 1 < views.size() ? "," : sep(i, entries.size())) + "\n";
        }
    }
    j += "  ],\n";

    j += "  \"buffers\": [\n    {\n      \"byteLength\": " + fmt_size(bin.size())
         + ",\n      \"uri\": " + json_escape(base_name(out_path) + ".bin") + "\n    }\n  ]\n";
    if (any_dds) {
        // extensionsUsed only (not extensionsRequired): loaders without DDS
        // support should still import the geometry, just without textures.
        j += ",\n  \"extensionsUsed\": [\"MSFT_texture_dds\"]\n";
    }
    j += "}\n";

    std::ofstream gltf_file(out_path, std::ios::trunc);
    if (!gltf_file) {
        if (err) *err = "cannot write " + out_path;
        return false;
    }
    gltf_file.write(j.data(), static_cast<std::streamsize>(j.size()));
    gltf_file.close();
    if (!gltf_file) {
        if (err) *err = "failed writing " + out_path;
        return false;
    }
    return true;
}

} // namespace viewer
} // namespace ghost

