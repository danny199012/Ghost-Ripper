// Reader for .grm capture files. Loads the whole file into memory and
// provides typed views over each section plus convenience decoders.
#pragma once

#include "capture_format.h"
#include "math.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ghost {

class MeshFile {
public:
    bool load(const std::string& path, std::string* err = nullptr);

    const fmt::FileHeader& file_header() const { return file_header_; }
    const fmt::MeshHeader& header() const { return header_; }

    const std::vector<fmt::MeshAttribute>& attributes() const { return attributes_; }
    const std::vector<fmt::TextureRef>&    textures() const { return textures_; }

    const uint8_t* vertex_data() const;
    const uint8_t* index_data() const;

    // Decode attribute `attr_index` of vertex `i` into out[4].
    bool decode_vertex_attribute(uint32_t i, uint32_t attr_index, float out[4]) const;

    // Convenience accessors (false when the semantic is absent/undecodable).
    bool position(uint32_t i, Vec3& out) const;
    bool normal(uint32_t i, Vec3& out) const;
    bool texcoord(uint32_t i, uint32_t channel, Vec2& out) const;

    // Index at position `i` of the index buffer (0 for non-indexed meshes).
    uint32_t index(uint32_t i) const;

    uint32_t triangle_count() const;

private:
    const fmt::MeshAttribute* find_attribute(fmt::Semantic semantic, uint32_t index) const;

    fmt::FileHeader file_header_{};
    fmt::MeshHeader header_{};
    std::vector<fmt::MeshAttribute> attributes_;
    std::vector<fmt::TextureRef>    textures_;
    std::vector<uint8_t>            vertex_blob_;
    std::vector<uint8_t>            index_blob_;
};

} // namespace ghost
