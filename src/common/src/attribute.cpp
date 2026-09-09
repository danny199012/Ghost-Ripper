#include "ghost/attribute.h"

#include <cstring>

namespace ghost {

uint32_t attribute_element_size(fmt::AttributeFormat f) {
    using AF = fmt::AttributeFormat;
    switch (f) {
        case AF::Float32:   return 4;
        case AF::Float32x2: return 8;
        case AF::Float32x3: return 12;
        case AF::Float32x4: return 16;
        case AF::Float16x2: return 4;
        case AF::Float16x4: return 8;
        case AF::UNorm8x2:  return 2;
        case AF::UNorm8x4:  return 4;
        case AF::SNorm8x4:  return 4;
        case AF::UInt8x4:   return 4;
        case AF::UInt16x2:  return 4;
        case AF::UInt16x4:  return 8;
        case AF::UInt32:    return 4;
        case AF::UInt32x2:  return 8;
        case AF::UInt32x3:  return 12;
        case AF::UInt32x4:  return 16;
        case AF::SInt32:    return 4;
        case AF::SInt32x4:  return 16;
        default:            return 0;
    }
}

uint32_t attribute_component_count(fmt::AttributeFormat f) {
    using AF = fmt::AttributeFormat;
    switch (f) {
        case AF::Float32:   return 1;
        case AF::Float32x2: return 2;
        case AF::Float32x3: return 3;
        case AF::Float32x4: return 4;
        case AF::Float16x2: return 2;
        case AF::Float16x4: return 4;
        case AF::UNorm8x2:  return 2;
        case AF::UNorm8x4:  return 4;
        case AF::SNorm8x4:  return 4;
        case AF::UInt8x4:   return 4;
        case AF::UInt16x2:  return 2;
        case AF::UInt16x4:  return 4;
        case AF::UInt32:    return 1;
        case AF::UInt32x2:  return 2;
        case AF::UInt32x3:  return 3;
        case AF::UInt32x4:  return 4;
        case AF::SInt32:    return 1;
        case AF::SInt32x4:  return 4;
        default:            return 0;
    }
}

float half_to_float(uint16_t h) {
    const uint32_t sign = (h & 0x8000u) << 16;
    const uint32_t exp  = (h >> 10) & 0x1Fu;
    const uint32_t mant = h & 0x03FFu;

    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            // Subnormal half: normalize into a float.
            int e = -1;
            uint32_t m = mant;
            while ((m & 0x0400u) == 0) { m <<= 1; --e; }
            m &= 0x03FFu;
            bits = sign | (static_cast<uint32_t>(127 - 15 + e) << 23) | (m << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);  // inf / nan
    } else {
        bits = sign | ((exp + 112) << 23) | (mant << 13);
    }

    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

bool decode_attribute(const void* src, fmt::AttributeFormat f, float out[4]) {
    using AF = fmt::AttributeFormat;
    out[0] = out[1] = out[2] = out[3] = 0.0f;

    const auto* u8  = static_cast<const uint8_t*>(src);
    const auto* u16 = static_cast<const uint16_t*>(src);
    const auto* u32 = static_cast<const uint32_t*>(src);
    const auto* f32 = static_cast<const float*>(src);

    switch (f) {
        case AF::Float32:
            out[0] = f32[0]; return true;
        case AF::Float32x2:
            out[0] = f32[0]; out[1] = f32[1]; return true;
        case AF::Float32x3:
            out[0] = f32[0]; out[1] = f32[1]; out[2] = f32[2]; return true;
        case AF::Float32x4:
            out[0] = f32[0]; out[1] = f32[1]; out[2] = f32[2]; out[3] = f32[3]; return true;
        case AF::Float16x2:
            out[0] = half_to_float(u16[0]); out[1] = half_to_float(u16[1]); return true;
        case AF::Float16x4:
            for (int i = 0; i < 4; ++i) out[i] = half_to_float(u16[i]);
            return true;
        case AF::UNorm8x2:
            out[0] = u8[0] / 255.0f; out[1] = u8[1] / 255.0f; return true;
        case AF::UNorm8x4:
            for (int i = 0; i < 4; ++i) out[i] = u8[i] / 255.0f;
            return true;
        case AF::SNorm8x4: {
            const auto* s8 = static_cast<const int8_t*>(src);
            for (int i = 0; i < 4; ++i) {
                const float v = s8[i] / 127.0f;
                out[i] = v < -1.0f ? -1.0f : v;
            }
            return true;
        }
        case AF::UInt8x4:
            for (int i = 0; i < 4; ++i) out[i] = static_cast<float>(u8[i]);
            return true;
        case AF::UInt16x2:
            out[0] = static_cast<float>(u16[0]); out[1] = static_cast<float>(u16[1]);
            return true;
        case AF::UInt16x4:
            for (int i = 0; i < 4; ++i) out[i] = static_cast<float>(u16[i]);
            return true;
        case AF::UInt32:
            out[0] = static_cast<float>(u32[0]); return true;
        case AF::UInt32x2:
            out[0] = static_cast<float>(u32[0]); out[1] = static_cast<float>(u32[1]);
            return true;
        case AF::UInt32x3:
            out[0] = static_cast<float>(u32[0]); out[1] = static_cast<float>(u32[1]);
            out[2] = static_cast<float>(u32[2]); return true;
        case AF::UInt32x4:
            for (int i = 0; i < 4; ++i) out[i] = static_cast<float>(u32[i]);
            return true;
        case AF::SInt32: {
            const auto* s32 = static_cast<const int32_t*>(src);
            out[0] = static_cast<float>(s32[0]); return true;
        }
        case AF::SInt32x4: {
            const auto* s32 = static_cast<const int32_t*>(src);
            for (int i = 0; i < 4; ++i) out[i] = static_cast<float>(s32[i]);
            return true;
        }
        default:
            return false;
    }
}

const char* semantic_name(fmt::Semantic s) {
    using S = fmt::Semantic;
    switch (s) {
        case S::Position:     return "POSITION";
        case S::Normal:       return "NORMAL";
        case S::Tangent:      return "TANGENT";
        case S::Binormal:     return "BINORMAL";
        case S::Color:        return "COLOR";
        case S::TexCoord:     return "TEXCOORD";
        case S::BlendWeights: return "BLENDWEIGHT";
        case S::BlendIndices: return "BLENDINDICES";
        default:              return "UNKNOWN";
    }
}

const char* attribute_format_name(fmt::AttributeFormat f) {
    using AF = fmt::AttributeFormat;
    switch (f) {
        case AF::Float32:   return "float";
        case AF::Float32x2: return "float2";
        case AF::Float32x3: return "float3";
        case AF::Float32x4: return "float4";
        case AF::Float16x2: return "half2";
        case AF::Float16x4: return "half4";
        case AF::UNorm8x2:  return "unorm8x2";
        case AF::UNorm8x4:  return "unorm8x4";
        case AF::SNorm8x4:  return "snorm8x4";
        case AF::UInt8x4:   return "uint8x4";
        case AF::UInt16x2:  return "uint16x2";
        case AF::UInt16x4:  return "uint16x4";
        case AF::UInt32:    return "uint";
        case AF::UInt32x2:  return "uint2";
        case AF::UInt32x3:  return "uint3";
        case AF::UInt32x4:  return "uint4";
        case AF::SInt32:    return "sint";
        case AF::SInt32x4:  return "sint4";
        default:            return "unknown";
    }
}

const char* topology_name(fmt::PrimitiveTopology t) {
    using T = fmt::PrimitiveTopology;
    switch (t) {
        case T::TriangleList:  return "triangle list";
        case T::TriangleStrip: return "triangle strip";
        case T::LineList:      return "line list";
        case T::LineStrip:     return "line strip";
        case T::PointList:     return "point list";
        default:               return "unknown";
    }
}

} // namespace ghost


