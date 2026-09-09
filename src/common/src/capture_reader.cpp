#include "ghost/capture_reader.h"

#include "ghost/attribute.h"

#include <cstring>
#include <fstream>

namespace ghost {

bool MeshFile::load(const std::string& path, std::string* err) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        if (err) *err = "cannot open file: " + path;
        return false;
    }
    const std::streamoff file_size = in.tellg();
    in.seekg(0);

    if (file_size < static_cast<std::streamoff>(sizeof(fmt::FileHeader) + sizeof(fmt::MeshHeader))) {
        if (err) *err = "file too small: " + path;
        return false;
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(file_size));
    if (!in.read(reinterpret_cast<char*>(bytes.data()), file_size)) {
        if (err) *err = "failed to read file: " + path;
        return false;
    }

    std::memcpy(&file_header_, bytes.data(), sizeof(file_header_));
    if (std::memcmp(file_header_.magic, fmt::kMagic, sizeof(fmt::kMagic)) != 0) {
        if (err) *err = "bad magic (not a GRMF file): " + path;
        return false;
    }
    if (file_header_.version != fmt::kVersion) {
        if (err) *err = "unsupported version " + std::to_string(file_header_.version);
        return false;
    }
    std::memcpy(&header_, bytes.data() + sizeof(fmt::FileHeader), sizeof(header_));

    // Bounds-check every section before touching it.
    const auto fits = [&](uint32_t offset, uint64_t size) {
        return static_cast<uint64_t>(offset) + size <= static_cast<uint64_t>(file_size);
    };
    const uint64_t attrs_size = static_cast<uint64_t>(header_.attribute_count) * sizeof(fmt::MeshAttribute);
    const uint64_t texs_size  = static_cast<uint64_t>(header_.texture_count) * sizeof(fmt::TextureRef);
    if (!fits(header_.attributes_offset, attrs_size) ||
        !fits(header_.textures_offset, texs_size) ||
        !fits(header_.vertex_data_offset, header_.vertex_data_size) ||
        !fits(header_.index_data_offset, header_.index_data_size)) {
        if (err) *err = "truncated or corrupt sections in: " + path;
        return false;
    }

    attributes_.resize(header_.attribute_count);
    if (header_.attribute_count)
        std::memcpy(attributes_.data(), bytes.data() + header_.attributes_offset, attrs_size);

    textures_.resize(header_.texture_count);
    if (header_.texture_count)
        std::memcpy(textures_.data(), bytes.data() + header_.textures_offset, texs_size);

    const uint8_t* vb = bytes.data() + header_.vertex_data_offset;
    vertex_blob_.assign(vb, vb + header_.vertex_data_size);

    if (header_.index_data_size) {
        const uint8_t* ib = bytes.data() + header_.index_data_offset;
        index_blob_.assign(ib, ib + header_.index_data_size);
    } else {
        index_blob_.clear();
    }

    return true;
}

const uint8_t* MeshFile::vertex_data() const {
    return vertex_blob_.empty() ? nullptr : vertex_blob_.data();
}

const uint8_t* MeshFile::index_data() const {
    return index_blob_.empty() ? nullptr : index_blob_.data();
}

const fmt::MeshAttribute* MeshFile::find_attribute(fmt::Semantic semantic, uint32_t index) const {
    for (const auto& a : attributes_) {
        if (a.semantic == static_cast<uint32_t>(semantic) && a.semantic_index == index)
            return &a;
    }
    return nullptr;
}

bool MeshFile::decode_vertex_attribute(uint32_t i, uint32_t attr_index, float out[4]) const {
    if (i >= header_.vertex_count || attr_index >= attributes_.size() || vertex_blob_.empty())
        return false;
    const fmt::MeshAttribute& a = attributes_[attr_index];
    const uint8_t* src = vertex_blob_.data() +
        static_cast<size_t>(i) * header_.vertex_stride + a.offset;
    return decode_attribute(src, static_cast<fmt::AttributeFormat>(a.format), out);
}
bool MeshFile::position(uint32_t i, Vec3& out) const {
    const fmt::MeshAttribute* a = find_attribute(fmt::Semantic::Position, 0);
    if (!a || i >= header_.vertex_count) return false;
    float v[4];
    const uint8_t* src = vertex_blob_.data() +
        static_cast<size_t>(i) * header_.vertex_stride + a->offset;
    if (!decode_attribute(src, static_cast<fmt::AttributeFormat>(a->format), v))
        return false;
    out = {v[0], v[1], v[2]};
    return true;
}

bool MeshFile::normal(uint32_t i, Vec3& out) const {
    const fmt::MeshAttribute* a = find_attribute(fmt::Semantic::Normal, 0);
    if (!a || i >= header_.vertex_count) return false;
    float v[4];
    const uint8_t* src = vertex_blob_.data() +
        static_cast<size_t>(i) * header_.vertex_stride + a->offset;
    if (!decode_attribute(src, static_cast<fmt::AttributeFormat>(a->format), v))
        return false;
    out = {v[0], v[1], v[2]};
    return true;
}

bool MeshFile::texcoord(uint32_t i, uint32_t channel, Vec2& out) const {
    const fmt::MeshAttribute* a = find_attribute(fmt::Semantic::TexCoord, channel);
    if (!a || i >= header_.vertex_count) return false;
    float v[4];
    const uint8_t* src = vertex_blob_.data() +
        static_cast<size_t>(i) * header_.vertex_stride + a->offset;
    if (!decode_attribute(src, static_cast<fmt::AttributeFormat>(a->format), v))
        return false;
    out = {v[0], v[1]};
    return true;
}

uint32_t MeshFile::index(uint32_t i) const {
    if (header_.index_format == static_cast<uint32_t>(fmt::IndexFormat::UInt16)) {
        uint16_t v = 0;
        if (static_cast<size_t>(i) * 2 + 2 <= index_blob_.size())
            std::memcpy(&v, index_blob_.data() + static_cast<size_t>(i) * 2, 2);
        return v;
    }
    if (header_.index_format == static_cast<uint32_t>(fmt::IndexFormat::UInt32)) {
        uint32_t v = 0;
        if (static_cast<size_t>(i) * 4 + 4 <= index_blob_.size())
            std::memcpy(&v, index_blob_.data() + static_cast<size_t>(i) * 4, 4);
        return v;
    }
    return i;  // non-indexed
}

uint32_t MeshFile::triangle_count() const {
    const uint32_t count = header_.index_count ? header_.index_count : header_.vertex_count;
    switch (static_cast<fmt::PrimitiveTopology>(header_.topology)) {
        case fmt::PrimitiveTopology::TriangleList:  return count / 3;
        case fmt::PrimitiveTopology::TriangleStrip: return count > 2 ? count - 2 : 0;
        case fmt::PrimitiveTopology::LineList:      return count / 2;
        default:                                    return count;
    }
}

} // namespace ghost

