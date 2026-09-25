// CPU decoder for block-compressed DDS surfaces (BC1-BC7) to RGBA8.
//
// Format sources: BC1-BC3 are classic S3TC/DXT, BC4-BC5 are RGTC (ATI1/2)
// and BC6H/BC7 are the bit-exact BPTC formats introduced with Direct3D 11.
// The two BPTC decoders follow the specs' mode tables field by field; the
// older formats use the reference decoder's rounding for the interpolated
// values. Only whole 4x4 blocks exist on disk, so edge blocks that stick out
// past width/height are decoded and then simply cropped.

#include "bc_decode.h"

#include <cstring>

namespace ghost {
namespace viewer {

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// DDS block data is little-endian; read the scalars byte-wise so this works
// on any host and without alignment requirements.
uint16_t load_u16(const uint8_t* p) {
    return uint16_t(p[0] | (p[1] << 8));
}

uint32_t load_u32(const uint8_t* p) {
    return load_u16(p) | (uint32_t(load_u16(p + 2)) << 16);
}

uint64_t load_u64(const uint8_t* p) {
    return load_u32(p) | (uint64_t(load_u32(p + 4)) << 32);
}

// floor(a / b) for b > 0. C++ integer division truncates toward zero, which
// is the wrong direction for the negative results BC6H interpolation makes.
int floor_div(int a, int b) {
    int q = a / b;
    if (a % b != 0 && a < 0) --q;
    return q;
}

// Round-to-nearest division. The 1/3, 1/5 and 1/7 interpolation steps never
// land on an exact half, so the tie rule never applies; this reproduces the
// reference decoder tables (BC4 endpoints 0 and 255 interpolate to
// 36, 73, 109, 146, 182, 219).
int div_round(int a, int b) {
    return floor_div(a + b / 2, b);
}

// Arithmetic right shift with floor semantics; C++17 leaves >> on negative
// ints implementation-defined, so spell it out.
int shift_right_floor(int value, int shift) {
    if (value >= 0) return value >> shift;
    return -((-value + (1 << shift) - 1) >> shift);
}

// The BPTC endpoint interpolation: (e0*(64-w) + e1*w + 32) >> 6.
int interpolate(int e0, int e1, int weight) {
    return shift_right_floor(e0 * (64 - weight) + e1 * weight + 32, 6);
}

// Interpolation weights for 2-, 3- and 4-bit indices (per the BPTC spec).
constexpr int kWeight2[4]  = { 0, 21, 43, 64 };
constexpr int kWeight3[8]  = { 0, 9, 18, 27, 37, 46, 55, 64 };
constexpr int kWeight4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38,
                               43, 47, 51, 55, 60, 64 };

const int* weights_for(int index_bits) {
    return index_bits == 2 ? kWeight2 : (index_bits == 3 ? kWeight3 : kWeight4);
}

// Every block decoder fills a complete 4x4 RGBA8 tile; the surface loop then
// copies only the texels that actually exist.
struct Tile {
    uint8_t rgba[4 * 4 * 4];
    uint8_t* pixel(int x, int y) { return rgba + (y * 4 + x) * 4; }
};

// IEEE 754 binary16 -> float32. Both BC6H variants leave their result as a
// half bit pattern, so the conversion lives here.
float half_to_float(uint16_t h) {
    const uint32_t sign = uint32_t(h & 0x8000) << 16;
    const uint32_t exp = (h >> 10) & 0x1F;
    uint32_t man = h & 0x3FF;

    uint32_t bits;
    if (exp == 0) {
        if (man == 0) {
            bits = sign;  // +-0
        } else {
            // Subnormal half: value = man * 2^-24. Shift until normalized and
            // adjust the exponent bias to compensate.
            int e = 0;
            do {
                ++e;
                man <<= 1;
            } while (man < 0x400);
            bits = sign | uint32_t((113 - e) << 23) | ((man - 0x400) << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (man << 13);  // Inf or NaN
    } else {
        bits = sign | ((exp + 112) << 23) | (man << 13);
    }
    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

// Displayable byte for a decoded BC6H half: HDR values are clamped into
// [0, 1] (negatives and NaNs go to 0) and rounded to 8 bits.
uint8_t half_to_unorm8(uint16_t h) {
    const float v = half_to_float(h);
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 255;
    return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

// ---------------------------------------------------------------------------
// BPTC partition tables
// ---------------------------------------------------------------------------
//
// One 4x4 pattern per partition index, laid out like the spec's figures
// (first row = y = 0). The low 2 bits are the subset a texel belongs to;
// bit 7 marks the anchor texel of a subset, whose stored index is one bit
// shorter (its top bit is implicitly zero). BC6H reuses the first 32 entries
// of the 2-subset table.
constexpr uint8_t kPartition2[64][4][4] = {
    { { 128,   0,   1,   1 }, {   0,   0,   1,   1 }, {   0,   0,   1,   1 }, {   0,   0,   1, 129 } },  //  0
    { { 128,   0,   0,   1 }, {   0,   0,   0,   1 }, {   0,   0,   0,   1 }, {   0,   0,   0, 129 } },  //  1
    { { 128,   1,   1,   1 }, {   0,   1,   1,   1 }, {   0,   1,   1,   1 }, {   0,   1,   1, 129 } },  //  2
    { { 128,   0,   0,   1 }, {   0,   0,   1,   1 }, {   0,   0,   1,   1 }, {   0,   1,   1, 129 } },  //  3
    { { 128,   0,   0,   0 }, {   0,   0,   0,   1 }, {   0,   0,   0,   1 }, {   0,   0,   1, 129 } },  //  4
    { { 128,   0,   1,   1 }, {   0,   1,   1,   1 }, {   0,   1,   1,   1 }, {   1,   1,   1, 129 } },  //  5
    { { 128,   0,   0,   1 }, {   0,   0,   1,   1 }, {   0,   1,   1,   1 }, {   1,   1,   1, 129 } },  //  6
    { { 128,   0,   0,   0 }, {   0,   0,   0,   1 }, {   0,   0,   1,   1 }, {   0,   1,   1, 129 } },  //  7
    { { 128,   0,   0,   0 }, {   0,   0,   0,   0 }, {   0,   0,   0,   1 }, {   0,   0,   1, 129 } },  //  8
    { { 128,   0,   1,   1 }, {   0,   1,   1,   1 }, {   1,   1,   1,   1 }, {   1,   1,   1, 129 } },  //  9
    { { 128,   0,   0,   0 }, {   0,   0,   0,   1 }, {   0,   1,   1,   1 }, {   1,   1,   1, 129 } },  // 10
    { { 128,   0,   0,   0 }, {   0,   0,   0,   0 }, {   0,   0,   0,   1 }, {   0,   1,   1, 129 } },  // 11
    { { 128,   0,   0,   1 }, {   0,   1,   1,   1 }, {   1,   1,   1,   1 }, {   1,   1,   1, 129 } },  // 12
    { { 128,   0,   0,   0 }, {   0,   0,   0,   0 }, {   1,   1,   1,   1 }, {   1,   1,   1, 129 } },  // 13
    { { 128,   0,   0,   0 }, {   1,   1,   1,   1 }, {   1,   1,   1,   1 }, {   1,   1,   1, 129 } },  // 14
    { { 128,   0,   0,   0 }, {   0,   0,   0,   0 }, {   0,   0,   0,   0 }, {   1,   1,   1, 129 } },  // 15
    { { 128,   0,   0,   0 }, {   1,   0,   0,   0 }, {   1,   1,   1,   0 }, {   1,   1,   1, 129 } },  // 16
    { { 128,   1, 129,   1 }, {   0,   0,   0,   1 }, {   0,   0,   0,   0 }, {   0,   0,   0,   0 } },  // 17
    { { 128,   0,   0,   0 }, {   0,   0,   0,   0 }, { 129,   0,   0,   0 }, {   1,   1,   1,   0 } },  // 18
    { { 128,   1, 129,   1 }, {   0,   0,   1,   1 }, {   0,   0,   0,   1 }, {   0,   0,   0,   0 } },  // 19
    { { 128,   0, 129,   1 }, {   0,   0,   0,   1 }, {   0,   0,   0,   0 }, {   0,   0,   0,   0 } },  // 20
    { { 128,   0,   0,   0 }, {   1,   0,   0,   0 }, { 129,   1,   0,   0 }, {   1,   1,   1,   0 } },  // 21
    { { 128,   0,   0,   0 }, {   0,   0,   0,   0 }, { 129,   0,   0,   0 }, {   1,   1,   0,   0 } },  // 22
    { { 128,   1,   1,   1 }, {   0,   0,   1,   1 }, {   0,   0,   1,   1 }, {   0,   0,   0, 129 } },  // 23
    { { 128,   0, 129,   1 }, {   0,   0,   0,   1 }, {   0,   0,   0,   1 }, {   0,   0,   0,   0 } },  // 24
    { { 128,   0,   0,   0 }, {   1,   0,   0,   0 }, { 129,   0,   0,   0 }, {   1,   1,   0,   0 } },  // 25
    { { 128,   1, 129,   0 }, {   0,   1,   1,   0 }, {   0,   1,   1,   0 }, {   0,   1,   1,   0 } },  // 26
    { { 128,   0, 129,   1 }, {   0,   1,   1,   0 }, {   0,   1,   1,   0 }, {   1,   1,   0,   0 } },  // 27
    { { 128,   0,   0,   1 }, {   0,   1,   1,   1 }, { 129,   1,   1,   0 }, {   1,   0,   0,   0 } },  // 28
    { { 128,   0,   0,   0 }, {   1,   1,   1,   1 }, { 129,   1,   1,   1 }, {   0,   0,   0,   0 } },  // 29
    { { 128,   1, 129,   1 }, {   0,   0,   0,   1 }, {   1,   0,   0,   0 }, {   1,   1,   1,   0 } },  // 30
    { { 128,   0, 129,   1 }, {   1,   0,   0,   1 }, {   1,   0,   0,   1 }, {   1,   1,   0,   0 } },  // 31
    { { 128,   1,   0,   1 }, {   0,   1,   0,   1 }, {   0,   1,   0,   1 }, {   0,   1,   0, 129 } },  // 32
    { { 128,   0,   0,   0 }, {   1,   1,   1,   1 }, {   0,   0,   0,   0 }, {   1,   1,   1, 129 } },  // 33
    { { 128,   1,   0,   1 }, {   1,   0, 129,   0 }, {   0,   1,   0,   1 }, {   1,   0,   1,   0 } },  // 34
    { { 128,   0,   1,   1 }, {   0,   0,   1,   1 }, { 129,   1,   0,   0 }, {   1,   1,   0,   0 } },  // 35
    { { 128,   0, 129,   1 }, {   1,   1,   0,   0 }, {   0,   0,   1,   1 }, {   1,   1,   0,   0 } },  // 36
    { { 128,   1,   0,   1 }, {   0,   1,   0,   1 }, { 129,   0,   1,   0 }, {   1,   0,   1,   0 } },  // 37
    { { 128,   1,   1,   0 }, {   1,   0,   0,   1 }, {   0,   1,   1,   0 }, {   1,   0,   0, 129 } },  // 38
    { { 128,   1,   0,   1 }, {   1,   0,   1,   0 }, {   1,   0,   1,   0 }, {   0,   1,   0, 129 } },  // 39
    { { 128,   1, 129,   1 }, {   0,   0,   1,   1 }, {   1,   1,   0,   0 }, {   1,   1,   1,   0 } },  // 40
    { { 128,   0,   0,   1 }, {   0,   0,   1,   1 }, { 129,   1,   0,   0 }, {   1,   0,   0,   0 } },  // 41
    { { 128,   0, 129,   1 }, {   0,   0,   1,   0 }, {   0,   1,   0,   0 }, {   1,   1,   0,   0 } },  // 42
    { { 128,   0, 129,   1 }, {   1,   0,   1,   1 }, {   1,   1,   0,   1 }, {   1,   1,   0,   0 } },  // 43
    { { 128,   1, 129,   0 }, {   1,   0,   0,   1 }, {   1,   0,   0,   1 }, {   0,   1,   1,   0 } },  // 44
    { { 128,   0,   1,   1 }, {   1,   1,   0,   0 }, {   1,   1,   0,   0 }, {   0,   0,   1, 129 } },  // 45
    { { 128,   1,   1,   0 }, {   0,   1,   1,   0 }, {   1,   0,   0,   1 }, {   1,   0,   0, 129 } },  // 46
    { { 128,   0,   0,   0 }, {   0,   1, 129,   0 }, {   0,   1,   1,   0 }, {   0,   0,   0,   0 } },  // 47
    { { 128,   1,   0,   0 }, {   1,   1, 129,   0 }, {   0,   1,   0,   0 }, {   0,   0,   0,   0 } },  // 48
    { { 128,   0, 129,   0 }, {   0,   1,   1,   1 }, {   0,   0,   1,   0 }, {   0,   0,   0,   0 } },  // 49
    { { 128,   0,   0,   0 }, {   0,   0, 129,   0 }, {   0,   1,   1,   1 }, {   0,   0,   1,   0 } },  // 50
    { { 128,   0,   0,   0 }, {   0,   1,   0,   0 }, { 129,   1,   1,   0 }, {   0,   1,   0,   0 } },  // 51
    { { 128,   1,   1,   0 }, {   1,   1,   0,   0 }, {   1,   0,   0,   1 }, {   0,   0,   1, 129 } },  // 52
    { { 128,   0,   1,   1 }, {   0,   1,   1,   0 }, {   1,   1,   0,   0 }, {   1,   0,   0, 129 } },  // 53
    { { 128,   1, 129,   0 }, {   0,   0,   1,   1 }, {   1,   0,   0,   1 }, {   1,   1,   0,   0 } },  // 54
    { { 128,   0, 129,   1 }, {   1,   0,   0,   1 }, {   1,   1,   0,   0 }, {   0,   1,   1,   0 } },  // 55
    { { 128,   1,   1,   0 }, {   1,   1,   0,   0 }, {   1,   1,   0,   0 }, {   1,   0,   0, 129 } },  // 56
    { { 128,   1,   1,   0 }, {   0,   0,   1,   1 }, {   0,   0,   1,   1 }, {   1,   0,   0, 129 } },  // 57
    { { 128,   1,   1,   1 }, {   1,   1,   1,   0 }, {   1,   0,   0,   0 }, {   0,   0,   0, 129 } },  // 58
    { { 128,   0,   0,   1 }, {   1,   0,   0,   0 }, {   1,   1,   1,   0 }, {   0,   1,   1, 129 } },  // 59
    { { 128,   0,   0,   0 }, {   1,   1,   1,   1 }, {   0,   0,   1,   1 }, {   0,   0,   1, 129 } },  // 60
    { { 128,   0, 129,   1 }, {   0,   0,   1,   1 }, {   1,   1,   1,   1 }, {   0,   0,   0,   0 } },  // 61
    { { 128,   0, 129,   0 }, {   0,   0,   1,   0 }, {   1,   1,   1,   0 }, {   1,   1,   1,   0 } },  // 62
    { { 128,   1,   0,   0 }, {   0,   1,   0,   0 }, {   0,   1,   1,   1 }, {   0,   1,   1, 129 } },  // 63
};

constexpr uint8_t kPartition3[64][4][4] = {
    { { 128,   0,   1, 129 }, {   0,   0,   1,   1 }, {   0,   2,   2,   1 }, {   2,   2,   2, 130 } },  //  0
    { { 128,   0,   0, 129 }, {   0,   0,   1,   1 }, { 130,   2,   1,   1 }, {   2,   2,   2,   1 } },  //  1
    { { 128,   0,   0,   0 }, {   2,   0,   0,   1 }, { 130,   2,   1,   1 }, {   2,   2,   1, 129 } },  //  2
    { { 128,   2,   2, 130 }, {   0,   0,   2,   2 }, {   0,   0,   1,   1 }, {   0,   1,   1, 129 } },  //  3
    { { 128,   0,   0,   0 }, {   0,   0,   0,   0 }, { 129,   1,   2,   2 }, {   1,   1,   2, 130 } },  //  4
    { { 128,   0,   1, 129 }, {   0,   0,   1,   1 }, {   0,   0,   2,   2 }, {   0,   0,   2, 130 } },  //  5
    { { 128,   0,   2, 130 }, {   0,   0,   2,   2 }, {   1,   1,   1,   1 }, {   1,   1,   1, 129 } },  //  6
    { { 128,   0,   1,   1 }, {   0,   0,   1,   1 }, { 130,   2,   1,   1 }, {   2,   2,   1, 129 } },  //  7
    { { 128,   0,   0,   0 }, {   0,   0,   0,   0 }, { 129,   1,   1,   1 }, {   2,   2,   2, 130 } },  //  8
    { { 128,   0,   0,   0 }, {   1,   1,   1,   1 }, { 129,   1,   1,   1 }, {   2,   2,   2, 130 } },  //  9
    { { 128,   0,   0,   0 }, {   1,   1, 129,   1 }, {   2,   2,   2,   2 }, {   2,   2,   2, 130 } },  // 10
    { { 128,   0,   1,   2 }, {   0,   0, 129,   2 }, {   0,   0,   1,   2 }, {   0,   0,   1, 130 } },  // 11
    { { 128,   1,   1,   2 }, {   0,   1, 129,   2 }, {   0,   1,   1,   2 }, {   0,   1,   1, 130 } },  // 12
    { { 128,   1,   2,   2 }, {   0, 129,   2,   2 }, {   0,   1,   2,   2 }, {   0,   1,   2, 130 } },  // 13
    { { 128,   0,   1, 129 }, {   0,   1,   1,   2 }, {   1,   1,   2,   2 }, {   1,   2,   2, 130 } },  // 14
    { { 128,   0,   1, 129 }, {   2,   0,   0,   1 }, { 130,   2,   0,   0 }, {   2,   2,   2,   0 } },  // 15
    { { 128,   0,   0, 129 }, {   0,   0,   1,   1 }, {   0,   1,   1,   2 }, {   1,   1,   2, 130 } },  // 16
    { { 128,   1,   1, 129 }, {   0,   0,   1,   1 }, { 130,   0,   0,   1 }, {   2,   2,   0,   0 } },  // 17
    { { 128,   0,   0,   0 }, {   1,   1,   2,   2 }, { 129,   1,   2,   2 }, {   1,   1,   2, 130 } },  // 18
    { { 128,   0,   2, 130 }, {   0,   0,   2,   2 }, {   0,   0,   2,   2 }, {   1,   1,   1, 129 } },  // 19
    { { 128,   1,   1, 129 }, {   0,   1,   1,   1 }, {   0,   2,   2,   2 }, {   0,   2,   2, 130 } },  // 20
    { { 128,   0,   0, 129 }, {   0,   0,   0,   1 }, { 130,   2,   2,   1 }, {   2,   2,   2,   1 } },  // 21
    { { 128,   0,   0,   0 }, {   0,   0, 129,   1 }, {   0,   1,   2,   2 }, {   0,   1,   2, 130 } },  // 22
    { { 128,   0,   0,   0 }, {   1,   1,   0,   0 }, { 130,   2, 129,   0 }, {   2,   2,   1,   0 } },  // 23
    { { 128,   1,   2, 130 }, {   0, 129,   2,   2 }, {   0,   0,   1,   1 }, {   0,   0,   0,   0 } },  // 24
    { { 128,   0,   1,   2 }, {   0,   0,   1,   2 }, { 129,   1,   2,   2 }, {   2,   2,   2, 130 } },  // 25
    { { 128,   1,   1,   0 }, {   1,   2, 130,   1 }, { 129,   2,   2,   1 }, {   0,   1,   1,   0 } },  // 26
    { { 128,   0,   0,   0 }, {   0,   1, 129,   0 }, {   1,   2, 130,   1 }, {   1,   2,   2,   1 } },  // 27
    { { 128,   0,   2,   2 }, {   1,   1,   0,   2 }, { 129,   1,   0,   2 }, {   0,   0,   2, 130 } },  // 28
    { { 128,   1,   1,   0 }, {   0, 129,   1,   0 }, {   2,   0,   0,   2 }, {   2,   2,   2, 130 } },  // 29
    { { 128,   0,   1,   1 }, {   0,   1,   2,   2 }, {   0,   1, 130,   2 }, {   0,   0,   1, 129 } },  // 30
    { { 128,   0,   0,   0 }, {   2,   0,   0,   0 }, { 130,   2,   1,   1 }, {   2,   2,   2, 129 } },  // 31
    { { 128,   0,   0,   0 }, {   0,   0,   0,   2 }, { 129,   1,   2,   2 }, {   1,   2,   2, 130 } },  // 32
    { { 128,   2,   2, 130 }, {   0,   0,   2,   2 }, {   0,   0,   1,   2 }, {   0,   0,   1, 129 } },  // 33
    { { 128,   0,   1, 129 }, {   0,   0,   1,   2 }, {   0,   0,   2,   2 }, {   0,   2,   2, 130 } },  // 34
    { { 128,   1,   2,   0 }, {   0, 129,   2,   0 }, {   0,   1, 130,   0 }, {   0,   1,   2,   0 } },  // 35
    { { 128,   0,   0,   0 }, {   1,   1, 129,   1 }, {   2,   2, 130,   2 }, {   0,   0,   0,   0 } },  // 36
    { { 128,   1,   2,   0 }, {   1,   2,   0,   1 }, { 130,   0, 129,   2 }, {   0,   1,   2,   0 } },  // 37
    { { 128,   1,   2,   0 }, {   2,   0,   1,   2 }, { 129, 130,   0,   1 }, {   0,   1,   2,   0 } },  // 38
    { { 128,   0,   1,   1 }, {   2,   2,   0,   0 }, {   1,   1, 130,   2 }, {   0,   0,   1, 129 } },  // 39
    { { 128,   0,   1,   1 }, {   1,   1, 130,   2 }, {   2,   2,   0,   0 }, {   0,   0,   1, 129 } },  // 40
    { { 128,   1,   0, 129 }, {   0,   1,   0,   1 }, {   2,   2,   2,   2 }, {   2,   2,   2, 130 } },  // 41
    { { 128,   0,   0,   0 }, {   0,   0,   0,   0 }, { 130,   1,   2,   1 }, {   2,   1,   2, 129 } },  // 42
    { { 128,   0,   2,   2 }, {   1, 129,   2,   2 }, {   0,   0,   2,   2 }, {   1,   1,   2, 130 } },  // 43
    { { 128,   0,   2, 130 }, {   0,   0,   1,   1 }, {   0,   0,   2,   2 }, {   0,   0,   1, 129 } },  // 44
    { { 128,   2,   2,   0 }, {   1,   2, 130,   1 }, {   0,   2,   2,   0 }, {   1,   2,   2, 129 } },  // 45
    { { 128,   1,   0,   1 }, {   2,   2, 130,   2 }, {   2,   2,   2,   2 }, {   0,   1,   0, 129 } },  // 46
    { { 128,   0,   0,   0 }, {   2,   1,   2,   1 }, { 130,   1,   2,   1 }, {   2,   1,   2, 129 } },  // 47
    { { 128,   1,   0, 129 }, {   0,   1,   0,   1 }, {   0,   1,   0,   1 }, {   2,   2,   2, 130 } },  // 48
    { { 128,   2,   2, 130 }, {   0,   1,   1,   1 }, {   0,   2,   2,   2 }, {   0,   1,   1, 129 } },  // 49
    { { 128,   0,   0,   2 }, {   1, 129,   1,   2 }, {   0,   0,   0,   2 }, {   1,   1,   1, 130 } },  // 50
    { { 128,   0,   0,   0 }, {   2, 129,   1,   2 }, {   2,   1,   1,   2 }, {   2,   1,   1, 130 } },  // 51
    { { 128,   2,   2,   2 }, {   0, 129,   1,   1 }, {   0,   1,   1,   1 }, {   0,   2,   2, 130 } },  // 52
    { { 128,   0,   0,   2 }, {   1,   1,   1,   2 }, { 129,   1,   1,   2 }, {   0,   0,   0, 130 } },  // 53
    { { 128,   1,   1,   0 }, {   0, 129,   1,   0 }, {   0,   1,   1,   0 }, {   2,   2,   2, 130 } },  // 54
    { { 128,   0,   0,   0 }, {   0,   0,   0,   0 }, {   2,   1, 129,   2 }, {   2,   1,   1, 130 } },  // 55
    { { 128,   1,   1,   0 }, {   0, 129,   1,   0 }, {   2,   2,   2,   2 }, {   2,   2,   2, 130 } },  // 56
    { { 128,   0,   2,   2 }, {   0,   0,   1,   1 }, {   0,   0, 129,   1 }, {   0,   0,   2, 130 } },  // 57
    { { 128,   0,   2,   2 }, {   1,   1,   2,   2 }, { 129,   1,   2,   2 }, {   0,   0,   2, 130 } },  // 58
    { { 128,   0,   0,   0 }, {   0,   0,   0,   0 }, {   0,   0,   0,   0 }, {   2, 129,   1, 130 } },  // 59
    { { 128,   0,   0, 130 }, {   0,   0,   0,   1 }, {   0,   0,   0,   2 }, {   0,   0,   0, 129 } },  // 60
    { { 128,   2,   2,   2 }, {   1,   2,   2,   2 }, {   0,   2,   2,   2 }, { 129,   2,   2, 130 } },  // 61
    { { 128,   1,   0, 129 }, {   2,   2,   2,   2 }, {   2,   2,   2,   2 }, {   2,   2,   2, 130 } },  // 62
    { { 128,   1,   1, 129 }, {   2,   0,   1,   1 }, { 130,   2,   0,   1 }, {   2,   2,   2,   0 } },  // 63
};

// ---------------------------------------------------------------------------
// BC1 / BC2 / BC3 (S3TC)
// ---------------------------------------------------------------------------

// RGB 5:6:5 -> 8 bits by replicating the top bits (31 -> 255, 63 -> 255).
int expand5(int v) { return (v << 3) | (v >> 2); }
int expand6(int v) { return (v << 2) | (v >> 4); }

// The 8-byte colour block shared by BC1/BC2/BC3. `opaque_only` forces the
// four-colour palette: BC2/BC3 code alpha separately, so their colour block
// never uses BC1's punch-through transparent third colour.
void decode_bc1_color_block(const uint8_t* block, Tile& tile, bool opaque_only) {
    const uint16_t c0 = load_u16(block);
    const uint16_t c1 = load_u16(block + 2);

    const int r0 = expand5((c0 >> 11) & 0x1F);
    const int g0 = expand6((c0 >> 5) & 0x3F);
    const int b0 = expand5(c0 & 0x1F);
    const int r1 = expand5((c1 >> 11) & 0x1F);
    const int g1 = expand6((c1 >> 5) & 0x3F);
    const int b1 = expand5(c1 & 0x1F);

    uint8_t pr[4] = { uint8_t(r0), uint8_t(r1), 0, 0 };
    uint8_t pg[4] = { uint8_t(g0), uint8_t(g1), 0, 0 };
    uint8_t pb[4] = { uint8_t(b0), uint8_t(b1), 0, 0 };
    uint8_t pa[4] = { 255, 255, 255, 255 };

    if (opaque_only || c0 > c1) {
        // Four-colour mode: two more colours at 1/3 and 2/3 of the way
        // between the endpoints.
        pr[2] = uint8_t(div_round(2 * r0 + r1, 3));
        pg[2] = uint8_t(div_round(2 * g0 + g1, 3));
        pb[2] = uint8_t(div_round(2 * b0 + b1, 3));
        pr[3] = uint8_t(div_round(r0 + 2 * r1, 3));
        pg[3] = uint8_t(div_round(g0 + 2 * g1, 3));
        pb[3] = uint8_t(div_round(b0 + 2 * b1, 3));
    } else {
        // Three-colour mode: one 50% mix, then transparent black.
        pr[2] = uint8_t(div_round(r0 + r1, 2));
        pg[2] = uint8_t(div_round(g0 + g1, 2));
        pb[2] = uint8_t(div_round(b0 + b1, 2));
        pr[3] = pg[3] = pb[3] = 0;
        pa[3] = 0;
    }

    // 16 two-bit indices, LSB-first in y-major texel order.
    uint32_t bits = load_u32(block + 4);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const int idx = bits & 3;
            bits >>= 2;
            uint8_t* px = tile.pixel(x, y);
            px[0] = pr[idx];
            px[1] = pg[idx];
            px[2] = pb[idx];
            px[3] = pa[idx];
        }
    }
}

// BC2: 16 explicit 4-bit alphas in the first 8 bytes (nibble * 17 is exact).
void decode_bc2_alpha_block(const uint8_t* block, Tile& tile) {
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const int nibble = (block[y * 2 + x / 2] >> (4 * (x & 1))) & 0xF;
            tile.pixel(x, y)[3] = uint8_t(nibble * 17);
        }
    }
}

// BC2 = explicit alphas followed by an always-opaque BC1 colour block.
void decode_bc2_tile(const uint8_t* block, Tile& tile) {
    decode_bc1_color_block(block + 8, tile, true);
    decode_bc2_alpha_block(block, tile);
}

// ---------------------------------------------------------------------------
// BC3 alpha / BC4 / BC5 (the 8-value single-channel block)
// ---------------------------------------------------------------------------

// Decodes one 8-byte single-channel block into 16 values (the BC3 alpha
// block, BC4, and each of BC5's two channels). Unsigned blocks give 0..255;
// signed blocks give -127..127 (a stored -128 endpoint is an alternative
// encoding of -127 per the spec).
void decode_bc4_channel_block(const uint8_t* block, bool snorm, int out[16]) {
    int a0 = block[0];
    int a1 = block[1];
    if (snorm) {
        a0 = static_cast<int8_t>(block[0]);
        a1 = static_cast<int8_t>(block[1]);
        if (a0 < -127) a0 = -127;
        if (a1 < -127) a1 = -127;
    }

    int palette[8];
    palette[0] = a0;
    palette[1] = a1;
    if (a0 > a1) {
        // The endpoints plus six 1/7 steps between them.
        for (int k = 1; k <= 6; ++k)
            palette[k + 1] = div_round(a0 * (7 - k) + a1 * k, 7);
    } else {
        // The endpoints plus four 1/5 steps, then the minimum and maximum
        // representable values at codes 6 and 7.
        for (int k = 1; k <= 4; ++k)
            palette[k + 1] = div_round(a0 * (5 - k) + a1 * k, 5);
        palette[6] = snorm ? -127 : 0;
        palette[7] = snorm ? 127 : 255;
    }

    // 16 three-bit indices in bytes 2..7, LSB-first in y-major texel order.
    uint64_t bits = load_u64(block) >> 16;
    for (int i = 0; i < 16; ++i) {
        out[i] = palette[int(bits & 7)];
        bits >>= 3;
    }
}

// BC3 = an interpolated alpha block followed by an always-opaque BC1 colour
// block (the alpha is always unsigned in the DXGI formats we support).
void decode_bc3_tile(const uint8_t* block, Tile& tile) {
    int a[16];
    decode_bc4_channel_block(block, false, a);
    decode_bc1_color_block(block + 8, tile, true);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            tile.pixel(x, y)[3] = uint8_t(a[y * 4 + x]);
}

// One BC4 block -> RGBA8. The single channel lands in R; the missing G/B sit
// at the format's zero point (0 unsigned, 128 = the display of signed zero).
void decode_bc4_tile(const uint8_t* block, bool snorm, Tile& tile) {
    int v[16];
    decode_bc4_channel_block(block, snorm, v);
    const uint8_t zero = snorm ? 128 : 0;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const int i = y * 4 + x;
            uint8_t* px = tile.pixel(x, y);
            px[0] = snorm ? uint8_t(v[i] + 128) : uint8_t(v[i]);
            px[1] = zero;
            px[2] = zero;
            px[3] = 255;
        }
    }
}

// BC5 = two independent BC4 blocks: red first, then green.
void decode_bc5_tile(const uint8_t* block, bool snorm, Tile& tile) {
    int r[16], g[16];
    decode_bc4_channel_block(block, snorm, r);
    decode_bc4_channel_block(block + 8, snorm, g);
    const uint8_t zero = snorm ? 128 : 0;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const int i = y * 4 + x;
            uint8_t* px = tile.pixel(x, y);
            px[0] = snorm ? uint8_t(r[i] + 128) : uint8_t(r[i]);
            px[1] = snorm ? uint8_t(g[i] + 128) : uint8_t(g[i]);
            px[2] = zero;  // blue is absent: the format's zero point
            px[3] = 255;
        }
    }
}

// ---------------------------------------------------------------------------
// BPTC bit reader
// ---------------------------------------------------------------------------

// Sequential LSB-first reader over a 16-byte block. The BPTC mode tables
// list their fields in stream order starting at bit 0 (the LSB of byte 0),
// so the decoder simply consumes them one after another.
class BitReader {
public:
    explicit BitReader(const uint8_t* block)
        : lo_(load_u64(block)), hi_(load_u64(block + 8)) {}

    // Consumes n bits (1..16); the first bit read lands in the result's LSB.
    uint32_t take(int n) {
        const uint32_t mask = (uint32_t(1) << n) - 1;
        const uint32_t bits = static_cast<uint32_t>(lo_) & mask;
        lo_ = (lo_ >> n) | ((hi_ & mask) << (64 - n));
        hi_ >>= n;
        return bits;
    }

    // BC6H modes 13 and 14 store two of endpoint 0's fields MSB-first (the
    // spec writes them as a descending bit range, e.g. rw^10..11^).
    uint32_t take_reversed(int n) {
        uint32_t bits = take(n);
        uint32_t out = 0;
        for (int i = 0; i < n; ++i) {
            out = (out << 1) | (bits & 1);
            bits >>= 1;
        }
        return out;
    }

private:
    uint64_t lo_, hi_;
};

// ---------------------------------------------------------------------------
// BC6H
// ---------------------------------------------------------------------------

// Per-mode parameters, indexed by spec mode number - 1. Endpoint 0 carries
// `endpoint_bits` per channel; the remaining endpoints hold `delta_bits`
// deltas when the mode is "transformed", or plain endpoint values otherwise.
struct Bc6hMode {
    int endpoint_bits;
    int delta_bits[3];  // R, G, B
    bool transformed;
    bool two_regions;
};

constexpr Bc6hMode kBc6hModes[14] = {
    { 10, {  5,  5,  5 }, true,  true  },  // mode 1:  10 + 5/5/5 deltas
    {  7, {  6,  6,  6 }, true,  true  },  // mode 2:  7 + 6/6/6
    { 11, {  5,  4,  4 }, true,  true  },  // mode 3:  11 + 5/4/4
    { 11, {  4,  5,  4 }, true,  true  },  // mode 4:  11 + 4/5/4
    { 11, {  4,  4,  5 }, true,  true  },  // mode 5:  11 + 4/4/5
    {  9, {  5,  5,  5 }, true,  true  },  // mode 6:  9 + 5/5/5
    {  8, {  6,  5,  5 }, true,  true  },  // mode 7:  8 + 6/5/5
    {  8, {  5,  6,  5 }, true,  true  },  // mode 8:  8 + 5/6/5
    {  8, {  5,  5,  6 }, true,  true  },  // mode 9:  8 + 5/5/6
    {  6, {  6,  6,  6 }, false, true  },  // mode 10: direct 6-bit, 2 regions
    { 10, { 10, 10, 10 }, false, false },  // mode 11: direct 10-bit, 1 region
    { 11, {  9,  9,  9 }, true,  false },  // mode 12: 11 + 9/9/9, 1 region
    { 12, {  8,  8,  8 }, true,  false },  // mode 13: 12 + 8/8/8, 1 region
    { 16, {  4,  4,  4 }, true,  false },  // mode 14: 16 + 4/4/4, 1 region
};

// Sign-extend a `bits`-wide two's complement field. Conversions of values
// above INT_MAX are implementation-defined before C++20 but every compiler
// this project builds with (gcc/clang/msvc) is two's complement.
int extend_sign(int value, int bits) {
    return (int(uint32_t(value) << (32 - bits))) >> (32 - bits);
}

// Unquantize one endpoint component onto the 16-bit half scale (bit-exact
// per the spec pseudocode).
int bc6h_unquantize(int value, int bits, bool is_signed) {
    if (!is_signed) {
        if (bits >= 15) return value;
        if (value == 0) return 0;
        if (value == (1 << bits) - 1) return 0xFFFF;
        return ((value << 16) + 0x8000) >> bits;
    }
    if (bits >= 16) return value;
    const bool negative = value < 0;
    if (negative) value = -value;
    int unq;
    if (value == 0) {
        unq = 0;
    } else if (value >= (1 << (bits - 1)) - 1) {
        unq = 0x7FFF;
    } else {
        unq = ((value << 15) + 0x4000) >> (bits - 1);
    }
    return negative ? -unq : unq;
}

// Final scaling back into the valid half range; the result is the binary16
// bit pattern (the signed format stores sign<<15 | magnitude).
uint16_t bc6h_finish_unquantize(int value, bool is_signed) {
    if (value < 0) return uint16_t(0x8000 | (((-value) * 31) >> 5));
    return uint16_t((value * 31) >> (is_signed ? 5 : 6));
}

// Reads one block's endpoint fields. `ep` is [channel][endpoint] with the
// channels in R, G, B order. Each case consumes its mode's fields in the
// exact stream order of the spec's mode table; the comments name the fields
// the way the spec does (rw..rz are the four red endpoints, gw..gz and
// bw..bz likewise). Returns the mode index, or -1 for reserved mode bits.
int read_bc6h_endpoints(BitReader& r, uint32_t mode_bits, int ep[3][4], int& partition) {
    int* re = ep[0];
    int* ge = ep[1];
    int* be = ep[2];
    switch (mode_bits) {
    case 0x00:  // mode 1: 10 + 5/5/5 deltas
        ge[2] |= r.take(1) << 4;       // gy[4]
        be[2] |= r.take(1) << 4;       // by[4]
        be[3] |= r.take(1) << 4;       // bz[4]
        re[0] |= r.take(10);           // rw[9:0]
        ge[0] |= r.take(10);           // gw[9:0]
        be[0] |= r.take(10);           // bw[9:0]
        re[1] |= r.take(5);            // rx[4:0]
        ge[3] |= r.take(1) << 4;       // gz[4]
        ge[2] |= r.take(4);            // gy[3:0]
        ge[1] |= r.take(5);            // gx[4:0]
        be[3] |= r.take(1);            // bz[0]
        ge[3] |= r.take(4);            // gz[3:0]
        be[1] |= r.take(5);            // bx[4:0]
        be[3] |= r.take(1) << 1;       // bz[1]
        be[2] |= r.take(4);            // by[3:0]
        re[2] |= r.take(5);            // ry[4:0]
        be[3] |= r.take(1) << 2;       // bz[2]
        re[3] |= r.take(5);            // rz[4:0]
        be[3] |= r.take(1) << 3;       // bz[3]
        partition = r.take(5);          // d[4:0]
        return 0;
    case 0x01:  // mode 2: 7 + 6/6/6 deltas
        ge[2] |= r.take(1) << 5;       // gy[5]
        ge[3] |= r.take(1) << 4;       // gz[4]
        ge[3] |= r.take(1) << 5;       // gz[5]
        re[0] |= r.take(7);            // rw[6:0]
        be[3] |= r.take(1);            // bz[0]
        be[3] |= r.take(1) << 1;       // bz[1]
        be[2] |= r.take(1) << 4;       // by[4]
        ge[0] |= r.take(7);            // gw[6:0]
        be[2] |= r.take(1) << 5;       // by[5]
        be[3] |= r.take(1) << 2;       // bz[2]
        ge[2] |= r.take(1) << 4;       // gy[4]
        be[0] |= r.take(7);            // bw[6:0]
        be[3] |= r.take(1) << 3;       // bz[3]
        be[3] |= r.take(1) << 5;       // bz[5]
        be[3] |= r.take(1) << 4;       // bz[4]
        re[1] |= r.take(6);            // rx[5:0]
        ge[2] |= r.take(4);            // gy[3:0]
        ge[1] |= r.take(6);            // gx[5:0]
        ge[3] |= r.take(4);            // gz[3:0]
        be[1] |= r.take(6);            // bx[5:0]
        be[2] |= r.take(4);            // by[3:0]
        re[2] |= r.take(6);            // ry[5:0]
        re[3] |= r.take(6);            // rz[5:0]
        partition = r.take(5);          // d[4:0]
        return 1;
    case 0x02:  // mode 3: 11 + 5/4/4 deltas
        re[0] |= r.take(10);           // rw[9:0]
        ge[0] |= r.take(10);           // gw[9:0]
        be[0] |= r.take(10);           // bw[9:0]
        re[1] |= r.take(5);            // rx[4:0]
        re[0] |= r.take(1) << 10;      // rw[10]
        ge[2] |= r.take(4);            // gy[3:0]
        ge[1] |= r.take(4);            // gx[3:0]
        ge[0] |= r.take(1) << 10;      // gw[10]
        be[3] |= r.take(1);            // bz[0]
        ge[3] |= r.take(4);            // gz[3:0]
        be[1] |= r.take(4);            // bx[3:0]
        be[0] |= r.take(1) << 10;      // bw[10]
        be[3] |= r.take(1) << 1;       // bz[1]
        be[2] |= r.take(4);            // by[3:0]
        re[2] |= r.take(5);            // ry[4:0]
        be[3] |= r.take(1) << 2;       // bz[2]
        re[3] |= r.take(5);            // rz[4:0]
        be[3] |= r.take(1) << 3;       // bz[3]
        partition = r.take(5);          // d[4:0]
        return 2;
    case 0x06:  // mode 4: 11 + 4/5/4 deltas
        re[0] |= r.take(10);           // rw[9:0]
        ge[0] |= r.take(10);           // gw[9:0]
        be[0] |= r.take(10);           // bw[9:0]
        re[1] |= r.take(4);            // rx[3:0]
        re[0] |= r.take(1) << 10;      // rw[10]
        ge[3] |= r.take(1) << 4;       // gz[4]
        ge[2] |= r.take(4);            // gy[3:0]
        ge[1] |= r.take(5);            // gx[4:0]
        ge[0] |= r.take(1) << 10;      // gw[10]
        ge[3] |= r.take(4);            // gz[3:0]
        be[1] |= r.take(4);            // bx[3:0]
        be[0] |= r.take(1) << 10;      // bw[10]
        be[3] |= r.take(1) << 1;       // bz[1]
        be[2] |= r.take(4);            // by[3:0]
        re[2] |= r.take(4);            // ry[3:0]
        be[3] |= r.take(1);            // bz[0]
        be[3] |= r.take(1) << 2;       // bz[2]
        re[3] |= r.take(4);            // rz[3:0]
        ge[2] |= r.take(1) << 4;       // gy[4]
        be[3] |= r.take(1) << 3;       // bz[3]
        partition = r.take(5);          // d[4:0]
        return 3;
    case 0x0A:  // mode 5: 11 + 4/4/5 deltas
        re[0] |= r.take(10);           // rw[9:0]
        ge[0] |= r.take(10);           // gw[9:0]
        be[0] |= r.take(10);           // bw[9:0]
        re[1] |= r.take(4);            // rx[3:0]
        re[0] |= r.take(1) << 10;      // rw[10]
        be[2] |= r.take(1) << 4;       // by[4]
        ge[2] |= r.take(4);            // gy[3:0]
        ge[1] |= r.take(4);            // gx[3:0]
        ge[0] |= r.take(1) << 10;      // gw[10]
        be[3] |= r.take(1);            // bz[0]
        ge[3] |= r.take(4);            // gz[3:0]
        be[1] |= r.take(5);            // bx[4:0]
        be[0] |= r.take(1) << 10;      // bw[10]
        be[2] |= r.take(4);            // by[3:0]
        re[2] |= r.take(4);            // ry[3:0]
        be[3] |= r.take(1) << 1;       // bz[1]
        be[3] |= r.take(1) << 2;       // bz[2]
        re[3] |= r.take(4);            // rz[3:0]
        be[3] |= r.take(1) << 4;       // bz[4]
        be[3] |= r.take(1) << 3;       // bz[3]
        partition = r.take(5);          // d[4:0]
        return 4;
    case 0x0E:  // mode 6: 9 + 5/5/5 deltas
        re[0] |= r.take(9);            // rw[8:0]
        be[2] |= r.take(1) << 4;       // by[4]
        ge[0] |= r.take(9);            // gw[8:0]
        ge[2] |= r.take(1) << 4;       // gy[4]
        be[0] |= r.take(9);            // bw[8:0]
        be[3] |= r.take(1) << 4;       // bz[4]
        re[1] |= r.take(5);            // rx[4:0]
        ge[3] |= r.take(1) << 4;       // gz[4]
        ge[2] |= r.take(4);            // gy[3:0]
        ge[1] |= r.take(5);            // gx[4:0]
        be[3] |= r.take(1);            // bz[0]
        ge[3] |= r.take(4);            // gz[3:0]
        be[1] |= r.take(5);            // bx[4:0]
        be[3] |= r.take(1) << 1;       // bz[1]
        be[2] |= r.take(4);            // by[3:0]
        re[2] |= r.take(5);            // ry[4:0]
        be[3] |= r.take(1) << 2;       // bz[2]
        re[3] |= r.take(5);            // rz[4:0]
        be[3] |= r.take(1) << 3;       // bz[3]
        partition = r.take(5);          // d[4:0]
        return 5;
    case 0x12:  // mode 7: 8 + 6/5/5 deltas
        re[0] |= r.take(8);            // rw[7:0]
        ge[3] |= r.take(1) << 4;       // gz[4]
        be[2] |= r.take(1) << 4;       // by[4]
        ge[0] |= r.take(8);            // gw[7:0]
        be[3] |= r.take(1) << 2;       // bz[2]
        ge[2] |= r.take(1) << 4;       // gy[4]
        be[0] |= r.take(8);            // bw[7:0]
        be[3] |= r.take(1) << 3;       // bz[3]
        be[3] |= r.take(1) << 4;       // bz[4]
        re[1] |= r.take(6);            // rx[5:0]
        ge[2] |= r.take(4);            // gy[3:0]
        ge[1] |= r.take(5);            // gx[4:0]
        be[3] |= r.take(1);            // bz[0]
        ge[3] |= r.take(4);            // gz[3:0]
        be[1] |= r.take(5);            // bx[4:0]
        be[3] |= r.take(1) << 1;       // bz[1]
        be[2] |= r.take(4);            // by[3:0]
        re[2] |= r.take(6);            // ry[5:0]
        re[3] |= r.take(6);            // rz[5:0]
        partition = r.take(5);          // d[4:0]
        return 6;
    case 0x16:  // mode 8: 8 + 5/6/5 deltas
        re[0] |= r.take(8);            // rw[7:0]
        be[3] |= r.take(1);            // bz[0]
        be[2] |= r.take(1) << 4;       // by[4]
        ge[0] |= r.take(8);            // gw[7:0]
        ge[2] |= r.take(1) << 5;       // gy[5]
        ge[2] |= r.take(1) << 4;       // gy[4]
        be[0] |= r.take(8);            // bw[7:0]
        ge[3] |= r.take(1) << 5;       // gz[5]
        be[3] |= r.take(1) << 4;       // bz[4]
        re[1] |= r.take(5);            // rx[4:0]
        ge[3] |= r.take(1) << 4;       // gz[4]
        ge[2] |= r.take(4);            // gy[3:0]
        ge[1] |= r.take(6);            // gx[5:0]
        ge[3] |= r.take(4);            // gz[3:0]
        be[1] |= r.take(5);            // bx[4:0]
        be[3] |= r.take(1) << 1;       // bz[1]
        be[2] |= r.take(4);            // by[3:0]
        re[2] |= r.take(5);            // ry[4:0]
        be[3] |= r.take(1) << 2;       // bz[2]
        re[3] |= r.take(5);            // rz[4:0]
        be[3] |= r.take(1) << 3;       // bz[3]
        partition = r.take(5);          // d[4:0]
        return 7;
    case 0x1A:  // mode 9: 8 + 5/5/6 deltas
        re[0] |= r.take(8);            // rw[7:0]
        be[3] |= r.take(1) << 1;       // bz[1]
        be[2] |= r.take(1) << 4;       // by[4]
        ge[0] |= r.take(8);            // gw[7:0]
        be[2] |= r.take(1) << 5;       // by[5]
        ge[2] |= r.take(1) << 4;       // gy[4]
        be[0] |= r.take(8);            // bw[7:0]
        be[3] |= r.take(1) << 5;       // bz[5]
        be[3] |= r.take(1) << 4;       // bz[4]
        re[1] |= r.take(5);            // rx[4:0]
        ge[3] |= r.take(1) << 4;       // gz[4]
        ge[2] |= r.take(4);            // gy[3:0]
        ge[1] |= r.take(5);            // gx[4:0]
        be[3] |= r.take(1);            // bz[0]
        ge[3] |= r.take(4);            // gz[3:0]
        be[1] |= r.take(6);            // bx[5:0]
        be[2] |= r.take(4);            // by[3:0]
        re[2] |= r.take(5);            // ry[4:0]
        be[3] |= r.take(1) << 2;       // bz[2]
        re[3] |= r.take(5);            // rz[4:0]
        be[3] |= r.take(1) << 3;       // bz[3]
        partition = r.take(5);          // d[4:0]
        return 8;
    case 0x1E:  // mode 10: direct 6-bit endpoints, 2 regions
        re[0] |= r.take(6);            // rw[5:0]
        ge[3] |= r.take(1) << 4;       // gz[4]
        be[3] |= r.take(1);            // bz[0]
        be[3] |= r.take(1) << 1;       // bz[1]
        be[2] |= r.take(1) << 4;       // by[4]
        ge[0] |= r.take(6);            // gw[5:0]
        ge[2] |= r.take(1) << 5;       // gy[5]
        be[2] |= r.take(1) << 5;       // by[5]
        be[3] |= r.take(1) << 2;       // bz[2]
        ge[2] |= r.take(1) << 4;       // gy[4]
        be[0] |= r.take(6);            // bw[5:0]
        ge[3] |= r.take(1) << 5;       // gz[5]
        be[3] |= r.take(1) << 3;       // bz[3]
        be[3] |= r.take(1) << 5;       // bz[5]
        be[3] |= r.take(1) << 4;       // bz[4]
        re[1] |= r.take(6);            // rx[5:0]
        ge[2] |= r.take(4);            // gy[3:0]
        ge[1] |= r.take(6);            // gx[5:0]
        ge[3] |= r.take(4);            // gz[3:0]
        be[1] |= r.take(6);            // bx[5:0]
        be[2] |= r.take(4);            // by[3:0]
        re[2] |= r.take(6);            // ry[5:0]
        re[3] |= r.take(6);            // rz[5:0]
        partition = r.take(5);          // d[4:0]
        return 9;
    case 0x03:  // mode 11: direct 10-bit endpoints, 1 region
        re[0] |= r.take(10);           // rw[9:0]
        ge[0] |= r.take(10);           // gw[9:0]
        be[0] |= r.take(10);           // bw[9:0]
        re[1] |= r.take(10);           // rx[9:0]
        ge[1] |= r.take(10);           // gx[9:0]
        be[1] |= r.take(10);           // bx[9:0]
        return 10;
    case 0x07:  // mode 12: 11 + 9/9/9 deltas, 1 region
        re[0] |= r.take(10);           // rw[9:0]
        ge[0] |= r.take(10);           // gw[9:0]
        be[0] |= r.take(10);           // bw[9:0]
        re[1] |= r.take(9);            // rx[8:0]
        re[0] |= r.take(1) << 10;      // rw[10]
        ge[1] |= r.take(9);            // gx[8:0]
        ge[0] |= r.take(1) << 10;      // gw[10]
        be[1] |= r.take(9);            // bx[8:0]
        be[0] |= r.take(1) << 10;      // bw[10]
        return 11;
    case 0x0B:  // mode 13: 12 + 8/8/8 deltas, 1 region
        re[0] |= r.take(10);           // rw[9:0]
        ge[0] |= r.take(10);           // gw[9:0]
        be[0] |= r.take(10);           // bw[9:0]
        re[1] |= r.take(8);            // rx[7:0]
        re[0] |= r.take_reversed(2) << 10;   // rw[11:10], stored MSB-first
        ge[1] |= r.take(8);            // gx[7:0]
        ge[0] |= r.take_reversed(2) << 10;   // gw[11:10]
        be[1] |= r.take(8);            // bx[7:0]
        be[0] |= r.take_reversed(2) << 10;   // bw[11:10]
        return 12;
    case 0x0F:  // mode 14: 16 + 4/4/4 deltas, 1 region
        re[0] |= r.take(10);           // rw[9:0]
        ge[0] |= r.take(10);           // gw[9:0]
        be[0] |= r.take(10);           // bw[9:0]
        re[1] |= r.take(4);            // rx[3:0]
        re[0] |= r.take_reversed(6) << 10;   // rw[15:10], stored MSB-first
        ge[1] |= r.take(4);            // gx[3:0]
        ge[0] |= r.take_reversed(6) << 10;   // gw[15:10]
        be[1] |= r.take(4);            // bx[3:0]
        be[0] |= r.take_reversed(6) << 10;   // bw[15:10]
        return 13;
    default:
        // Only 0x13, 0x17, 0x1b and 0x1f are reachable but unlisted; they
        // are reserved and must not be used by encoders.
        return -1;
    }
}

// Decodes one 16-byte BC6H block into the tile.
void decode_bc6h_tile(const uint8_t* block, bool is_signed, Tile& tile) {
    BitReader reader(block);
    int ep[3][4] = {};  // [channel][endpoint]
    int partition = 0;

    // The mode number is the low two bits, or the low five bits when those
    // two are >= 2.
    uint32_t mode_bits = reader.take(2);
    if (mode_bits > 1) mode_bits |= reader.take(3) << 2;
    const int mode = read_bc6h_endpoints(reader, mode_bits, ep, partition);

    if (mode < 0) {
        // Reserved encoding: the spec defines the result as (0, 0, 0, 1).
        for (int i = 0; i < 16; ++i) {
            uint8_t* px = tile.rgba + i * 4;
            px[0] = px[1] = px[2] = 0;
            px[3] = 255;
        }
        return;
    }

    const Bc6hMode& info = kBc6hModes[mode];
    const int endpoint_count = info.two_regions ? 4 : 2;
    const int epb = info.endpoint_bits;

    // Sign handling: endpoint 0 is signed only in SF16; the other endpoints
    // are signed in SF16 or whenever the mode stores transformed deltas.
    if (is_signed) {
        for (int c = 0; c < 3; ++c) ep[c][0] = extend_sign(ep[c][0], epb);
    }
    const bool deltas_signed = is_signed || info.transformed;
    for (int e = 1; e < endpoint_count; ++e) {
        for (int c = 0; c < 3; ++c) {
            if (deltas_signed) ep[c][e] = extend_sign(ep[c][e], info.delta_bits[c]);
            if (info.transformed) {
                // Undo the delta transform: B = (B + A) wrapped to epb bits.
                ep[c][e] = (ep[c][e] + ep[c][0]) & ((1 << epb) - 1);
                if (is_signed) ep[c][e] = extend_sign(ep[c][e], epb);
            }
        }
    }
    for (int e = 0; e < endpoint_count; ++e)
        for (int c = 0; c < 3; ++c)
            ep[c][e] = bc6h_unquantize(ep[c][e], epb, is_signed);

    const int* weights = info.two_regions ? kWeight3 : kWeight4;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            int subset = 0;
            bool anchor = (x == 0 && y == 0);  // subset 0 always anchors at 0,0
            if (info.two_regions) {
                const uint8_t cell = kPartition2[partition][y][x];
                anchor = (cell & 0x80) != 0;
                subset = cell & 0x01;
            }
            // Anchor indices are stored with one bit less.
            const int index = reader.take((info.two_regions ? 3 : 4) - (anchor ? 1 : 0));

            const int e0 = subset * 2;
            uint8_t* px = tile.pixel(x, y);
            px[0] = half_to_unorm8(bc6h_finish_unquantize(
                interpolate(ep[0][e0], ep[0][e0 + 1], weights[index]), is_signed));
            px[1] = half_to_unorm8(bc6h_finish_unquantize(
                interpolate(ep[1][e0], ep[1][e0 + 1], weights[index]), is_signed));
            px[2] = half_to_unorm8(bc6h_finish_unquantize(
                interpolate(ep[2][e0], ep[2][e0 + 1], weights[index]), is_signed));
            px[3] = 255;  // BC6H has no alpha channel
        }
    }
}

// ---------------------------------------------------------------------------
// BC7
// ---------------------------------------------------------------------------

// Per-mode parameters. `primary_index_bits`/`secondary_index_bits` are the
// fixed widths of the two index fields (mode 4's selector only swaps which
// field drives colour vs alpha, not the layout).
struct Bc7Mode {
    int subsets;               // 1, 2 or 3
    int partition_bits;        // 0, 4 (mode 0) or 6
    bool rotation;             // 2-bit rotation field present (modes 4/5)
    bool index_selection;      // 1-bit colour/alpha index swap (mode 4)
    int color_bits;            // stored bits per R/G/B endpoint component
    int alpha_bits;            // stored bits per alpha endpoint (0 = none)
    bool pbits;                // endpoint P-bits present
    bool shared_pbits;         // one P-bit per subset instead of per endpoint
    int primary_index_bits;    // 2, 3 or 4
    int secondary_index_bits;  // 0 (none), 2 or 3
};

constexpr Bc7Mode kBc7Modes[8] = {
    { 3, 4, false, false, 4, 0, true,  false, 3, 0 },  // mode 0: 3 subsets, 4.4.4.1 + P
    { 2, 6, false, false, 6, 0, true,  true,  3, 0 },  // mode 1: 2 subsets, 6.6.6.1 + shared P
    { 3, 6, false, false, 5, 0, false, false, 2, 0 },  // mode 2: 3 subsets, 5.5.5
    { 2, 6, false, false, 7, 0, true,  false, 2, 0 },  // mode 3: 2 subsets, 7.7.7.1 + P
    { 1, 0, true,  true,  5, 6, false, false, 2, 3 },  // mode 4: 1 subset, RGB 5.5.5 + A 6
    { 1, 0, true,  false, 7, 8, false, false, 2, 2 },  // mode 5: 1 subset, RGB 7.7.7 + A 8
    { 1, 0, false, false, 7, 7, true,  false, 4, 0 },  // mode 6: 1 subset, RGBA 7.7.7.7 + P
    { 2, 6, false, false, 5, 5, true,  false, 2, 0 },  // mode 7: 2 subsets, RGBA 5.5.5.5 + P
};

// Scales one endpoint component to 8 bits: shift the stored value (with its
// P-bit already folded in as the LSB) so the MSB lands in bit 7, then
// replicate the top bits into the low bits the shift revealed.
int expand_bc7_endpoint(int value, int bits) {
    const int shifted = value << (8 - bits);
    return shifted | (shifted >> bits);
}

// Decodes one 16-byte BC7 block into the tile.
void decode_bc7_tile(const uint8_t* block, Tile& tile) {
    BitReader reader(block);

    // The mode is a run of zero bits terminated by a one, counted from bit 0
    // (mode 0 is a single 1 bit; mode 7 has its 1 in bit 7). An all-zero
    // first byte is the reserved "mode 8".
    int mode = 0;
    while (mode < 8 && reader.take(1) == 0) ++mode;
    if (mode >= 8) {
        std::memset(tile.rgba, 0, sizeof(tile.rgba));
        return;
    }

    const Bc7Mode& info = kBc7Modes[mode];
    const int endpoint_count = info.subsets * 2;

    const int partition = info.partition_bits > 0 ? int(reader.take(info.partition_bits)) : 0;
    const int rotation = info.rotation ? int(reader.take(2)) : 0;
    const bool swap_indices = info.index_selection && reader.take(1) != 0;

    // Endpoints come channel-major: every endpoint's red, then green, then
    // blue, then alpha (only for the modes with explicit alpha bits).
    int ep[6][4] = {};
    for (int c = 0; c < 3; ++c)
        for (int e = 0; e < endpoint_count; ++e)
            ep[e][c] = int(reader.take(info.color_bits));
    if (info.alpha_bits > 0)
        for (int e = 0; e < endpoint_count; ++e)
            ep[e][3] = int(reader.take(info.alpha_bits));

    // P-bits follow the endpoints: one per subset (shared by its endpoint
    // pair) for mode 1, one per endpoint for modes 0/3/6/7.
    int pbit[6] = {};
    if (info.shared_pbits) {
        pbit[0] = pbit[1] = int(reader.take(1));
        pbit[2] = pbit[3] = int(reader.take(1));
    } else if (info.pbits) {
        for (int e = 0; e < endpoint_count; ++e) pbit[e] = int(reader.take(1));
    }

    for (int e = 0; e < endpoint_count; ++e) {
        const int cbits = info.color_bits + (info.pbits ? 1 : 0);
        for (int c = 0; c < 3; ++c) {
            int v = ep[e][c];
            if (info.pbits) v = (v << 1) | pbit[e];
            ep[e][c] = expand_bc7_endpoint(v, cbits);
        }
        if (info.alpha_bits > 0) {
            const int abits = info.alpha_bits + (info.pbits ? 1 : 0);
            int v = ep[e][3];
            if (info.pbits) v = (v << 1) | pbit[e];
            ep[e][3] = expand_bc7_endpoint(v, abits);
        } else {
            ep[e][3] = 255;  // no alpha coding: fully opaque
        }
    }

    // Which index field drives colour vs alpha: modes 4/5 carry a separate
    // secondary index set for alpha; mode 4's 1-bit selector swaps the roles.
    const bool has_secondary = info.secondary_index_bits > 0;
    const int color_index_bits = has_secondary
        ? (swap_indices ? info.secondary_index_bits : info.primary_index_bits)
        : info.primary_index_bits;
    const int alpha_index_bits = has_secondary
        ? (swap_indices ? info.primary_index_bits : info.secondary_index_bits)
        : info.primary_index_bits;
    const int* color_weights = weights_for(color_index_bits);
    const int* alpha_weights = weights_for(alpha_index_bits);

    // Texel -> subset / anchor lookups (single-subset modes cover the
    // whole block and anchor at 0,0).
    const auto subset_of = [&](int x, int y) -> int {
        if (info.subsets == 1) return 0;
        const uint8_t cell = info.subsets == 2 ? kPartition2[partition][y][x]
                                               : kPartition3[partition][y][x];
        return cell & 0x03;
    };
    const auto is_anchor = [&](int x, int y) -> bool {
        if (info.subsets == 1) return x == 0 && y == 0;
        const uint8_t cell = info.subsets == 2 ? kPartition2[partition][y][x]
                                               : kPartition3[partition][y][x];
        return (cell & 0x80) != 0;
    };

    // Pass 1: primary indices, y-major; anchor texels store one bit less
    // (their top index bit is implicitly zero).
    int primary[16] = {};
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            primary[y * 4 + x] =
                int(reader.take(info.primary_index_bits - (is_anchor(x, y) ? 1 : 0)));

    // Pass 2: secondary indices (modes 4/5), then interpolation, index
    // swap and rotation.
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const int pri = primary[y * 4 + x];
            int sec = pri;
            if (has_secondary)
                sec = int(reader.take(info.secondary_index_bits - ((x == 0 && y == 0) ? 1 : 0)));

            const int color_index = has_secondary ? (swap_indices ? sec : pri) : pri;
            const int alpha_index = has_secondary ? (swap_indices ? pri : sec) : pri;

            const int e0 = subset_of(x, y) * 2;
            const int e1 = e0 + 1;
            int r = interpolate(ep[e0][0], ep[e1][0], color_weights[color_index]);
            int g = interpolate(ep[e0][1], ep[e1][1], color_weights[color_index]);
            int b = interpolate(ep[e0][2], ep[e1][2], color_weights[color_index]);
            int a = interpolate(ep[e0][3], ep[e1][3], alpha_weights[alpha_index]);

            // Rotation remaps the separate scalar channel back into place.
            if (rotation == 1) { const int t = a; a = r; r = t; }
            else if (rotation == 2) { const int t = a; a = g; g = t; }
            else if (rotation == 3) { const int t = a; a = b; b = t; }

            uint8_t* px = tile.pixel(x, y);
            px[0] = uint8_t(r);
            px[1] = uint8_t(g);
            px[2] = uint8_t(b);
            px[3] = uint8_t(a);
        }
    }
}

// ---------------------------------------------------------------------------
// Block dispatch
// ---------------------------------------------------------------------------

// Decodes one block of `dxgi_format` into the tile. The format is known to
// be supported by the time this runs (decode_bc_surface checks up front).
void decode_block(uint32_t dxgi_format, const uint8_t* block, Tile& tile) {
    switch (dxgi_format) {
    case kDxgiBC1UNorm:
    case kDxgiBC1UNormSRGB:
        decode_bc1_color_block(block, tile, false);
        break;
    case kDxgiBC2UNorm:
    case kDxgiBC2UNormSRGB:
        decode_bc2_tile(block, tile);
        break;
    case kDxgiBC3UNorm:
    case kDxgiBC3UNormSRGB:
        decode_bc3_tile(block, tile);
        break;
    case kDxgiBC4UNorm:
        decode_bc4_tile(block, false, tile);
        break;
    case kDxgiBC4SNorm:
        decode_bc4_tile(block, true, tile);
        break;
    case kDxgiBC5UNorm:
        decode_bc5_tile(block, false, tile);
        break;
    case kDxgiBC5SNorm:
        decode_bc5_tile(block, true, tile);
        break;
    case kDxgiBC6HUF16:
        decode_bc6h_tile(block, false, tile);
        break;
    case kDxgiBC6HSF16:
        decode_bc6h_tile(block, true, tile);
        break;
    case kDxgiBC7UNorm:
    case kDxgiBC7UNormSRGB:
        decode_bc7_tile(block, tile);
        break;
    default:
        std::memset(tile.rgba, 0, sizeof(tile.rgba));
        break;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

bool bc_decode_supported(uint32_t dxgi_format) {
    return bc_block_bytes(dxgi_format) != 0;
}

uint32_t bc_block_bytes(uint32_t dxgi_format) {
    switch (dxgi_format) {
    case kDxgiBC1UNorm:
    case kDxgiBC1UNormSRGB:
    case kDxgiBC4UNorm:
    case kDxgiBC4SNorm:
        return 8;   // 64-bit blocks
    case kDxgiBC2UNorm:
    case kDxgiBC2UNormSRGB:
    case kDxgiBC3UNorm:
    case kDxgiBC3UNormSRGB:
    case kDxgiBC5UNorm:
    case kDxgiBC5SNorm:
    case kDxgiBC6HUF16:
    case kDxgiBC6HSF16:
    case kDxgiBC7UNorm:
    case kDxgiBC7UNormSRGB:
        return 16;  // 128-bit blocks
    default:
        return 0;
    }
}

size_t bc_surface_size(uint32_t dxgi_format, uint32_t width, uint32_t height) {
    const uint32_t block = bc_block_bytes(dxgi_format);
    if (block == 0 || width == 0 || height == 0) return 0;
    // Count in 64 bits: a 4-gigapixel edge case would wrap 32-bit maths.
    const uint64_t blocks = ((uint64_t(width) + 3) / 4) * ((uint64_t(height) + 3) / 4);
    return static_cast<size_t>(blocks * block);
}

bool bc_format_is_signed(uint32_t dxgi_format) {
    return dxgi_format == kDxgiBC4SNorm || dxgi_format == kDxgiBC5SNorm ||
           dxgi_format == kDxgiBC6HSF16;
}

bool decode_bc_surface(uint32_t dxgi_format, uint32_t width, uint32_t height,
                       const uint8_t* src, size_t src_size, uint8_t* out_rgba) {
    // Validate everything up front so a rejection never touches out_rgba.
    const size_t needed = bc_surface_size(dxgi_format, width, height);
    if (needed == 0) return false;  // unsupported format or invalid dimensions
    if (src == nullptr || out_rgba == nullptr) return false;
    if (src_size < needed) return false;

    const uint32_t block_bytes = bc_block_bytes(dxgi_format);
    const uint32_t blocks_x = (width + 3) / 4;
    const uint32_t blocks_y = (height + 3) / 4;

    for (uint32_t by = 0; by < blocks_y; ++by) {
        const uint32_t y0 = by * 4;
        const uint32_t rows = (height - y0) < 4 ? (height - y0) : 4;  // partial bottom block
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
            const uint32_t x0 = bx * 4;
            const uint32_t cols = (width - x0) < 4 ? (width - x0) : 4;  // partial right block
            const uint8_t* block = src + (size_t(by) * blocks_x + bx) * block_bytes;

            Tile tile = {};
            decode_block(dxgi_format, block, tile);

            // Blit the tile's valid texels into the tightly packed surface.
            for (uint32_t y = 0; y < rows; ++y) {
                std::memcpy(out_rgba + ((y0 + y) * size_t(width) + x0) * 4,
                            tile.rgba + size_t(y) * 16, size_t(cols) * 4);
            }
        }
    }
    return true;
}

} // namespace viewer
} // namespace ghost
