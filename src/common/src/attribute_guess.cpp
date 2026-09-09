#include "ghost/attribute_guess.h"

#include "ghost/attribute.h"

namespace ghost {

namespace {

using S  = fmt::Semantic;
using AF = fmt::AttributeFormat;

fmt::MeshAttribute attr(S semantic, uint32_t index, AF format, uint32_t offset) {
    fmt::MeshAttribute a{};
    a.semantic       = static_cast<uint32_t>(semantic);
    a.semantic_index = index;
    a.format         = static_cast<uint32_t>(format);
    a.offset         = offset;
    return a;
}

} // namespace

std::vector<fmt::MeshAttribute> guess_vertex_layout(uint32_t stride) {
    // POSITION float3 at offset 0 is nearly universal in real-time rendering.
    std::vector<fmt::MeshAttribute> out{attr(S::Position, 0, AF::Float32x3, 0)};

    switch (stride) {
        case 12:  // P
            break;
        case 16:  // P + half2 UV (common in older/console ports)
            out.push_back(attr(S::TexCoord, 0, AF::Float16x2, 12));
            break;
        case 20:  // P + UV
            out.push_back(attr(S::TexCoord, 0, AF::Float32x2, 12));
            break;
        case 24:  // P + N
            out.push_back(attr(S::Normal, 0, AF::Float32x3, 12));
            break;
        case 28:  // P + N + half2 UV
            out.push_back(attr(S::Normal, 0, AF::Float32x3, 12));
            out.push_back(attr(S::TexCoord, 0, AF::Float16x2, 24));
            break;
        case 32:  // P + N + UV
            out.push_back(attr(S::Normal, 0, AF::Float32x3, 12));
            out.push_back(attr(S::TexCoord, 0, AF::Float32x2, 24));
            break;
        case 36:  // P + N + UV + color
            out.push_back(attr(S::Normal, 0, AF::Float32x3, 12));
            out.push_back(attr(S::TexCoord, 0, AF::Float32x2, 24));
            out.push_back(attr(S::Color, 0, AF::UNorm8x4, 32));
            break;
        case 40:  // P + N + T (with handedness in w) + UV
            out.push_back(attr(S::Normal, 0, AF::Float32x3, 12));
            out.push_back(attr(S::Tangent, 0, AF::Float32x4, 24));
            out.push_back(attr(S::TexCoord, 0, AF::Float32x2, 40 - 8));
            break;
        case 44:  // P + N + T + UV + color
            out.push_back(attr(S::Normal, 0, AF::Float32x3, 12));
            out.push_back(attr(S::Tangent, 0, AF::Float32x4, 24));
            out.push_back(attr(S::TexCoord, 0, AF::Float32x2, 36));
            break;
        case 48:  // P + N + T + UV + second UV
            out.push_back(attr(S::Normal, 0, AF::Float32x3, 12));
            out.push_back(attr(S::Tangent, 0, AF::Float32x4, 24));
            out.push_back(attr(S::TexCoord, 0, AF::Float32x2, 32));
            out.push_back(attr(S::TexCoord, 1, AF::Float32x2, 40));
            break;
        case 56:  // P + N + T + B + UV + blend weights/indices (skinned)
            out.push_back(attr(S::Normal, 0, AF::Float32x3, 12));
            out.push_back(attr(S::Tangent, 0, AF::Float32x4, 24));
            out.push_back(attr(S::TexCoord, 0, AF::Float32x2, 40));
            out.push_back(attr(S::BlendWeights, 0, AF::UNorm8x4, 48));
            out.push_back(attr(S::BlendIndices, 0, AF::UInt8x4, 52));
            break;
        default:
            // Unknown stride: POSITION only. The viewer can still render the
            // geometry and the user can refine the layout later.
            break;
    }

    return out;
}

} // namespace ghost
