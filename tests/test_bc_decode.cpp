// Unit test for the BC1-BC7 block decoder (bc_decode.h).
//
// Hand-encodes a single block per feature and checks the decoded RGBA8
// against values derived from the format specifications. Plain main() with
// a tiny assert helper: prints one line per check group and returns
// non-zero when any check failed.

#include "bc_decode.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace ghost::viewer;

namespace {

int g_checks = 0;
int g_failures = 0;

void expect(bool condition, const char* what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("    FAILED: %s\n", what);
    }
}

void expect_pixel(const uint8_t* img, uint32_t width, uint32_t x, uint32_t y,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t a, const char* what) {
    ++g_checks;
    const uint8_t* px = img + (size_t(y) * width + x) * 4;
    if (px[0] != r || px[1] != g || px[2] != b || px[3] != a) {
        ++g_failures;
        std::printf("    FAILED: %s: pixel (%u, %u) is (%u, %u, %u, %u), want (%u, %u, %u, %u)\n",
                    what, x, y, px[0], px[1], px[2], px[3], r, g, b, a);
    }
}

void begin_group(const char* name) {
    std::printf("[check] %s\n", name);
}

// ---- block encoding helpers ------------------------------------------------

// BC1 colour block: two RGB565 endpoints + 16 two-bit indices (y-major).
std::vector<uint8_t> make_bc1_block(uint16_t c0, uint16_t c1, const uint8_t(&idx)[16]) {
    std::vector<uint8_t> block(8);
    block[0] = uint8_t(c0 & 0xFF);
    block[1] = uint8_t(c0 >> 8);
    block[2] = uint8_t(c1 & 0xFF);
    block[3] = uint8_t(c1 >> 8);
    uint32_t bits = 0;
    for (int i = 0; i < 16; ++i) bits |= uint32_t(idx[i]) << (2 * i);
    for (int i = 0; i < 4; ++i) block[4 + i] = uint8_t(bits >> (8 * i));
    return block;
}

// 8-byte single-channel block (BC3 alpha, BC4, one BC5 channel): two 8-bit
// endpoints + 16 three-bit indices (y-major, LSB-first).
std::vector<uint8_t> make_bc4_block(int a0, int a1, const uint8_t(&idx)[16]) {
    std::vector<uint8_t> block(8);
    block[0] = uint8_t(a0);
    block[1] = uint8_t(a1);
    uint64_t bits = 0;
    for (int i = 0; i < 16; ++i) bits |= uint64_t(idx[i]) << (3 * i);
    for (int i = 0; i < 6; ++i) block[2 + i] = uint8_t(bits >> (8 * i));
    return block;
}

// 16 indices with every texel set to the same code.
void fill_indices(uint8_t(&idx)[16], uint8_t code) {
    for (int i = 0; i < 16; ++i) idx[i] = code;
}

// Indices 0..7 in the first eight texels (used to sweep a palette).
void ramp_indices(uint8_t(&idx)[16]) {
    for (int i = 0; i < 16; ++i) idx[i] = uint8_t(i < 8 ? i : 0);
}

// LSB-first bit writer for hand-assembling 16-byte BPTC blocks.
struct BitWriter {
    uint8_t bytes[16] = {};
    int bit = 0;
    void put(uint32_t value, int n) {
        for (int i = 0; i < n; ++i, ++bit)
            if (value & (1u << i)) bytes[bit / 8] |= uint8_t(1 << (bit % 8));
    }
};

// Decodes one block as a w x h surface (w, h <= 4).
bool decode_tile(uint32_t fmt, const uint8_t* block, uint32_t w, uint32_t h, uint8_t* out) {
    return decode_bc_surface(fmt, w, h, block, bc_block_bytes(fmt), out);
}

// ---- check groups ----------------------------------------------------------

void check_surface_queries() {
    begin_group("surface size, block size and format queries");
    expect(bc_block_bytes(kDxgiBC1UNorm) == 8 && bc_block_bytes(kDxgiBC1UNormSRGB) == 8 &&
           bc_block_bytes(kDxgiBC4UNorm) == 8 && bc_block_bytes(kDxgiBC4SNorm) == 8,
           "BC1/BC4 blocks are 8 bytes");
    expect(bc_block_bytes(kDxgiBC2UNorm) == 16 && bc_block_bytes(kDxgiBC2UNormSRGB) == 16 &&
           bc_block_bytes(kDxgiBC3UNorm) == 16 && bc_block_bytes(kDxgiBC3UNormSRGB) == 16 &&
           bc_block_bytes(kDxgiBC5UNorm) == 16 && bc_block_bytes(kDxgiBC5SNorm) == 16 &&
           bc_block_bytes(kDxgiBC6HUF16) == 16 && bc_block_bytes(kDxgiBC6HSF16) == 16 &&
           bc_block_bytes(kDxgiBC7UNorm) == 16 && bc_block_bytes(kDxgiBC7UNormSRGB) == 16,
           "BC2/3/5/6H/7 blocks are 16 bytes");
    expect(bc_block_bytes(999) == 0, "unknown formats have no block size");
    expect(bc_surface_size(kDxgiBC1UNorm, 8, 8) == 32, "8x8 BC1 is 4 blocks * 8 bytes");
    expect(bc_surface_size(kDxgiBC3UNorm, 8, 8) == 64, "8x8 BC3 is 4 blocks * 16 bytes");
    expect(bc_surface_size(kDxgiBC1UNorm, 2, 2) == 8, "2x2 BC1 is one block");
    expect(bc_surface_size(kDxgiBC7UNorm, 5, 3) == 32, "5x3 BC7 is 2 blocks");
    expect(bc_surface_size(kDxgiBC1UNorm, 0, 4) == 0 && bc_surface_size(kDxgiBC1UNorm, 4, 0) == 0,
           "zero dimensions are invalid");
    expect(bc_surface_size(999, 4, 4) == 0, "unsupported formats have no surface size");

    const uint32_t formats[13] = {
        kDxgiBC1UNorm, kDxgiBC1UNormSRGB, kDxgiBC2UNorm, kDxgiBC2UNormSRGB,
        kDxgiBC3UNorm, kDxgiBC3UNormSRGB, kDxgiBC4UNorm, kDxgiBC4SNorm,
        kDxgiBC5UNorm, kDxgiBC5SNorm, kDxgiBC6HUF16, kDxgiBC6HSF16,
        kDxgiBC7UNorm,
    };
    for (uint32_t fmt : formats) expect(bc_decode_supported(fmt), "format is supported");
    expect(bc_decode_supported(kDxgiBC7UNormSRGB), "BC7 sRGB is supported");
    expect(!bc_decode_supported(999), "unknown format is unsupported");

    expect(bc_format_is_signed(kDxgiBC4SNorm) && bc_format_is_signed(kDxgiBC5SNorm) &&
           bc_format_is_signed(kDxgiBC6HSF16), "SNorm/SF16 formats report signed");
    expect(!bc_format_is_signed(kDxgiBC4UNorm) && !bc_format_is_signed(kDxgiBC6HUF16),
           "UNorm/UF16 formats report unsigned");
}

void check_bc1() {
    begin_group("BC1 endpoints and 4-colour palette");
    // c0 = 0xF800 is pure red, c1 = 0x07E0 is pure green after 565 expansion.
    const uint8_t want[4][4] = {
        { 255,   0,   0, 255 },  // index 0: endpoint 0 (red)
        {   0, 255,   0, 255 },  // index 1: endpoint 1 (green)
        { 170,  85,   0, 255 },  // index 2: 2/3 red + 1/3 green
        {  85, 170,   0, 255 },  // index 3: 1/3 red + 2/3 green
    };
    uint8_t out[4 * 4 * 4];
    for (int code = 0; code < 4; ++code) {
        uint8_t idx[16];
        fill_indices(idx, uint8_t(code));
        const auto block = make_bc1_block(0xF800, 0x07E0, idx);
        expect(decode_tile(kDxgiBC1UNorm, block.data(), 4, 4, out), "BC1 block decodes");
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                expect_pixel(out, 4, x, y, want[code][0], want[code][1], want[code][2],
                             want[code][3], "BC1 4-colour palette");
    }
    // The sRGB variant decodes identically (no linearisation here).
    uint8_t idx0[16];
    fill_indices(idx0, 0);
    const auto block = make_bc1_block(0xF800, 0x07E0, idx0);
    expect(decode_tile(kDxgiBC1UNormSRGB, block.data(), 4, 4, out), "BC1 sRGB decodes");
    expect_pixel(out, 4, 2, 2, 255, 0, 0, 255, "BC1 sRGB decodes like UNorm");

    begin_group("BC1 3-colour mode with punch-through alpha");
    // Endpoints swapped so color_0 <= color_1: three-colour mode.
    const uint8_t want3[4][4] = {
        {   0, 255,   0, 255 },  // index 0: endpoint 0 (green)
        { 255,   0,   0, 255 },  // index 1: endpoint 1 (red)
        { 128, 128,   0, 255 },  // index 2: 50% mix
        {   0,   0,   0,   0 },  // index 3: transparent black
    };
    for (int code = 0; code < 4; ++code) {
        uint8_t idx[16];
        fill_indices(idx, uint8_t(code));
        const auto block3 = make_bc1_block(0x07E0, 0xF800, idx);
        expect(decode_tile(kDxgiBC1UNorm, block3.data(), 4, 4, out), "BC1 3-colour decodes");
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                expect_pixel(out, 4, x, y, want3[code][0], want3[code][1], want3[code][2],
                             want3[code][3], "BC1 3-colour palette");
    }
}

void check_bc1_partial() {
    begin_group("BC1 partial edge block");
    // A 2x2 surface from one 8-byte block: the four existing pixels decode,
    // the rest of the tile is dropped.
    uint8_t idx[16] = {};
    idx[0] = 0;   // (0,0)
    idx[1] = 1;   // (1,0)
    idx[4] = 2;   // (0,1)
    idx[5] = 3;   // (1,1)
    const auto block = make_bc1_block(0xF800, 0x07E0, idx);

    uint8_t out[4 * 4 * 4];
    std::memset(out, 0xEE, sizeof(out));
    expect(decode_bc_surface(kDxgiBC1UNorm, 2, 2, block.data(), 8, out),
           "2x2 partial surface decodes");
    expect_pixel(out, 2, 0, 0, 255,   0,   0, 255, "partial (0,0)");
    expect_pixel(out, 2, 1, 0,   0, 255,   0, 255, "partial (1,0)");
    expect_pixel(out, 2, 0, 1, 170,  85,   0, 255, "partial (0,1)");
    expect_pixel(out, 2, 1, 1,  85, 170,   0, 255, "partial (1,1)");
    bool untouched = true;
    for (size_t i = 2 * 2 * 4; i < sizeof(out); ++i) untouched = untouched && out[i] == 0xEE;
    expect(untouched, "partial decode does not write past the surface");

    // 5x1: two blocks wide; the second block supplies pixel (4,0).
    uint8_t idx0[16], idx1[16];
    fill_indices(idx0, 0);
    fill_indices(idx1, 1);
    const auto a = make_bc1_block(0xF800, 0x07E0, idx0);
    const auto b = make_bc1_block(0xF800, 0x07E0, idx1);
    std::vector<uint8_t> two(a);
    two.insert(two.end(), b.begin(), b.end());
    uint8_t out5[5 * 4];
    expect(decode_bc_surface(kDxgiBC1UNorm, 5, 1, two.data(), two.size(), out5),
           "5x1 surface decodes");
    expect_pixel(out5, 5, 0, 0, 255, 0, 0, 255, "5x1 first block pixel");
    expect_pixel(out5, 5, 3, 0, 255, 0, 0, 255, "5x1 first block pixel 3");
    expect_pixel(out5, 5, 4, 0, 0, 255, 0, 255, "5x1 second block pixel");
}

void check_bc2() {
    begin_group("BC2 explicit alpha");
    // Alpha nibbles 0xA, 0x5 in the first two texels, 0xF everywhere else;
    // the colour block sits after the 8 alpha bytes.
    uint8_t idx3[16];
    fill_indices(idx3, 3);
    std::vector<uint8_t> block = make_bc1_block(0x07E0, 0xF800, idx3);
    std::vector<uint8_t> full(8, 0xFF);
    full[0] = 0xFA;  // nibbles: (0,0) = 0xA, (1,0) = 0xF
    full[1] = 0xF5;  // (2,0) = 0xF, (3,0) = 0x5... low nibble of byte 1
    // Rebuild the alpha bytes explicitly for clarity:
    //   texel n lives in byte n/2 at nibble (n % 2).
    for (int i = 0; i < 8; ++i) full[i] = 0xFF;
    full[0] = 0xFA;  // (0,0)=0xA in the low nibble, (1,0)=0xF
    full[2] = 0xF5;  // (0,1)=0x5 in the low nibble, (1,1)=0xF
    full.insert(full.end(), block.begin(), block.end());

    // c0 <= c1 but BC2 colour blocks never use the transparent mode, so
    // index 3 is the 1/3-c0 + 2/3-c1 mix, not punch-through alpha.
    uint8_t out[4 * 4 * 4];
    expect(decode_tile(kDxgiBC2UNorm, full.data(), 4, 4, out), "BC2 block decodes");
    expect_pixel(out, 4, 0, 0, 170,  85, 0, 170, "BC2 (0,0) alpha 0xA*17");
    expect_pixel(out, 4, 0, 1, 170,  85, 0,  85, "BC2 (0,1) alpha 0x5*17");
    expect_pixel(out, 4, 3, 3, 170,  85, 0, 255, "BC2 (3,3) alpha 0xF*17");
    // Every colour is the 2/3-c1 mix with a fully opaque alpha value.
    expect(decode_tile(kDxgiBC2UNormSRGB, full.data(), 4, 4, out), "BC2 sRGB decodes");
    expect_pixel(out, 4, 2, 2, 170, 85, 0, 255, "BC2 sRGB decodes like UNorm");
}

void check_bc3() {
    begin_group("BC3 interpolated alpha");
    // Endpoints (255, 0) select the six-interpolation palette; the eight
    // codes are 255, 0 and the rounded steps 219, 182, 146, 109, 73, 36
    // (= k*255/7 with round-to-nearest).
    uint8_t idx[16];
    ramp_indices(idx);
    std::vector<uint8_t> alpha = make_bc4_block(255, 0, idx);
    uint8_t idx0[16];
    fill_indices(idx0, 0);
    std::vector<uint8_t> color = make_bc1_block(0xF800, 0x07E0, idx0);
    std::vector<uint8_t> full(alpha);
    full.insert(full.end(), color.begin(), color.end());

    const uint8_t want_a[8] = { 255, 0, 219, 182, 146, 109, 73, 36 };
    uint8_t out[4 * 4 * 4];
    expect(decode_tile(kDxgiBC3UNorm, full.data(), 4, 4, out), "BC3 6-interp decodes");
    for (int i = 0; i < 8; ++i) {
        const uint8_t* px = out + i * 4;
        expect(px[3] == want_a[i], "BC3 6-interp alpha sweep");
        expect(px[0] == 255 && px[1] == 0 && px[2] == 0, "BC3 colour stays endpoint 0");
    }

    // Endpoints (0, 255) select the four-interpolation palette plus the
    // special minimum (code 6) and maximum (code 7) values.
    std::vector<uint8_t> alpha2 = make_bc4_block(0, 255, idx);
    std::vector<uint8_t> full2(alpha2);
    full2.insert(full2.end(), color.begin(), color.end());
    const uint8_t want_b[8] = { 0, 255, 51, 102, 153, 204, 0, 255 };
    expect(decode_tile(kDxgiBC3UNorm, full2.data(), 4, 4, out), "BC3 4-interp decodes");
    for (int i = 0; i < 8; ++i)
        expect(out[i * 4 + 3] == want_b[i], "BC3 4-interp alpha sweep");

    expect(decode_tile(kDxgiBC3UNormSRGB, full2.data(), 4, 4, out), "BC3 sRGB decodes");
    expect_pixel(out, 4, 1, 0, 255, 0, 0, 255, "BC3 sRGB decodes like UNorm");
}

void check_bc4() {
    begin_group("BC4 UNorm palette");
    uint8_t idx[16];
    ramp_indices(idx);
    uint8_t out[4 * 4 * 4];

    // Endpoints (255, 0): the six rounded intermediates {36, 73, 109, 146,
    // 182, 219} of 255/7 appear at codes 2..7.
    std::vector<uint8_t> block = make_bc4_block(255, 0, idx);
    const uint8_t want_a[8] = { 255, 0, 219, 182, 146, 109, 73, 36 };
    expect(decode_tile(kDxgiBC4UNorm, block.data(), 4, 4, out), "BC4 UNorm 6-interp decodes");
    for (int i = 0; i < 8; ++i)
        expect_pixel(out, 4, i % 4, i / 4, want_a[i], 0, 0, 255, "BC4 UNorm 6-interp sweep");

    // Endpoints (0, 255): four interpolations plus the 0/255 special codes.
    std::vector<uint8_t> block2 = make_bc4_block(0, 255, idx);
    const uint8_t want_b[8] = { 0, 255, 51, 102, 153, 204, 0, 255 };
    expect(decode_tile(kDxgiBC4UNorm, block2.data(), 4, 4, out), "BC4 UNorm 4-interp decodes");
    for (int i = 0; i < 8; ++i)
        expect_pixel(out, 4, i % 4, i / 4, want_b[i], 0, 0, 255, "BC4 UNorm 4-interp sweep");
}

void check_bc4_snorm() {
    begin_group("BC4 SNorm palette");
    uint8_t idx[16];
    ramp_indices(idx);
    uint8_t out[4 * 4 * 4];

    // Signed endpoints 127 and -127 (a0 > a1) give the six-interpolation
    // palette; the decoder maps signed values v to bytes v + 128, so the
    // endpoints land on 255 and 1.
    std::vector<uint8_t> block = make_bc4_block(127, 0x81, idx);
    const uint8_t want_a[8] = { 255, 1, 219, 182, 146, 110, 74, 37 };
    expect(decode_tile(kDxgiBC4SNorm, block.data(), 4, 4, out), "BC4 SNorm 6-interp decodes");
    for (int i = 0; i < 8; ++i)
        expect_pixel(out, 4, i % 4, i / 4, want_a[i], 128, 128, 255,
                     "BC4 SNorm 6-interp sweep");

    // Endpoints -127 and 127 (a0 <= a1) give the four-interpolation palette
    // plus the -127/127 special codes.
    std::vector<uint8_t> block2 = make_bc4_block(0x81, 127, idx);
    const uint8_t want_b[8] = { 1, 255, 52, 103, 153, 204, 1, 255 };
    expect(decode_tile(kDxgiBC4SNorm, block2.data(), 4, 4, out), "BC4 SNorm 4-interp decodes");
    for (int i = 0; i < 8; ++i)
        expect_pixel(out, 4, i % 4, i / 4, want_b[i], 128, 128, 255,
                     "BC4 SNorm 4-interp sweep");

    // A stored -128 endpoint is an alternative encoding of -127.
    uint8_t idx0[16];
    fill_indices(idx0, 0);
    std::vector<uint8_t> block3 = make_bc4_block(0x80, 0, idx0);
    expect(decode_tile(kDxgiBC4SNorm, block3.data(), 4, 4, out), "BC4 SNorm clamp decodes");
    expect_pixel(out, 4, 0, 0, 1, 128, 128, 255, "BC4 SNorm -128 clamps to -127 (byte 1)");
}

void check_bc5() {
    begin_group("BC5 two independent channels");
    uint8_t idx[16];
    ramp_indices(idx);
    uint8_t out[4 * 4 * 4];

    // UNorm: the red block uses endpoints (255, 0), the green block (0, 255)
    // - the two channels must not influence each other. Blue is absent and
    // therefore 0.
    std::vector<uint8_t> block = make_bc4_block(255, 0, idx);
    std::vector<uint8_t> green = make_bc4_block(0, 255, idx);
    block.insert(block.end(), green.begin(), green.end());
    expect(decode_tile(kDxgiBC5UNorm, block.data(), 4, 4, out), "BC5 UNorm decodes");
    expect_pixel(out, 4, 0, 0, 255,   0, 0, 255, "BC5 UNorm texel 0");
    expect_pixel(out, 4, 1, 0,   0, 255, 0, 255, "BC5 UNorm texel 1");
    expect_pixel(out, 4, 2, 0, 219,  51, 0, 255, "BC5 UNorm texel 2");
    expect_pixel(out, 4, 3, 0, 182, 102, 0, 255, "BC5 UNorm texel 3");

    // SNorm: red block endpoints (127, -127), green block (-127, 127);
    // blue is absent and sits at the signed zero point (128).
    std::vector<uint8_t> sblock = make_bc4_block(127, 0x81, idx);
    std::vector<uint8_t> sgreen = make_bc4_block(0x81, 127, idx);
    sblock.insert(sblock.end(), sgreen.begin(), sgreen.end());
    expect(decode_tile(kDxgiBC5SNorm, sblock.data(), 4, 4, out), "BC5 SNorm decodes");
    expect_pixel(out, 4, 0, 0, 255,   1, 128, 255, "BC5 SNorm texel 0");
    expect_pixel(out, 4, 1, 0,   1, 255, 128, 255, "BC5 SNorm texel 1");
    expect_pixel(out, 4, 2, 0, 219,  52, 128, 255, "BC5 SNorm texel 2");
    expect_pixel(out, 4, 3, 0, 182, 103, 128, 255, "BC5 SNorm texel 3");
}

// ---------------------------------------------------------------------------
// BC6H: hand-built single blocks with spec-derived expected colours
// ---------------------------------------------------------------------------

// Mode 11 (direct 10-bit endpoints, one region) with every texel on
// endpoint 0. The endpoint bits pass through unquantize(64*x + 32 for EPB
// 10), interpolation with weight 0 (identity) and the final 31/64 scaling,
// which was used to pick the raw endpoint values below.
void check_bc6h() {
    begin_group("BC6H UF16 mode 11");
    {
        BitWriter w;
        w.put(0x03, 5);        // mode bits 0b00011 = mode 11
        w.put(462, 10);        // rw: unquantizes to 29600, finish 0x3801 = 0.50049
        w.put(429, 10);        // gw: finish 0x3402 = 0.25049
        w.put(900, 10);        // bw: finish value 5165 (clamps to 1.0)
        w.put(0, 10);          // rx
        w.put(0, 10);          // gx
        w.put(0, 10);          // bx
        w.put(0, 3);           // texel 0 anchors: 4-bit index minus one bit
        for (int i = 0; i < 15; ++i) w.put(0, 4);
        uint8_t out[4 * 4 * 4];
        expect(decode_tile(kDxgiBC6HUF16, w.bytes, 4, 4, out), "BC6H UF16 mode 11 decodes");
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                expect_pixel(out, 4, x, y, 128, 64, 255, 255, "BC6H UF16 mode 11 endpoint 0");
    }

    begin_group("BC6H UF16 mode 10 (two regions)");
    {
        // Mode 10 stores two direct 6-bit endpoint pairs and a 5-bit
        // partition. Endpoint value 28 unquantizes to 28*1024 + 512 =
        // 29184, which finishes to half 0x3748 = 0.45117 -> byte 115.
        // Partition 0 splits the block into x < 2 and x >= 2.
        BitWriter w;
        w.put(0x1E, 5);        // mode bits 0b11110 = mode 10
        w.put(28, 6);          // rw
        w.put(0, 1);           // gz[4]
        w.put(0, 1);           // bz[0]
        w.put(0, 1);           // bz[1]
        w.put(0, 1);           // by[4]
        w.put(28, 6);          // gw
        w.put(0, 1);           // gy[5]
        w.put(0, 1);           // by[5]
        w.put(0, 1);           // bz[2]
        w.put(0, 1);           // gy[4]
        w.put(28, 6);          // bw
        w.put(0, 1);           // gz[5]
        w.put(0, 1);           // bz[3]
        w.put(0, 1);           // bz[5]
        w.put(0, 1);           // bz[4]
        w.put(28, 6);          // rx
        w.put(0, 4);           // gy[3:0]
        w.put(28, 6);          // gx
        w.put(0, 4);           // gz[3:0]
        w.put(28, 6);          // bx
        w.put(0, 4);           // by[3:0]
        w.put(0, 6);           // ry
        w.put(0, 6);           // rz
        w.put(0, 5);           // partition 0
        w.put(0, 2);           // texel 0 anchors subset 0
        for (int i = 0; i < 14; ++i) w.put(0, 3);
        w.put(0, 2);           // texel 15 anchors subset 1
        uint8_t out[4 * 4 * 4];
        expect(decode_tile(kDxgiBC6HUF16, w.bytes, 4, 4, out), "BC6H UF16 mode 10 decodes");
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                if (x < 2)
                    expect_pixel(out, 4, x, y, 115, 115, 115, 255, "BC6H mode 10 subset 0");
                else
                    expect_pixel(out, 4, x, y, 0, 0, 0, 255, "BC6H mode 10 subset 1");
            }
        }
    }

    begin_group("BC6H SF16 mode 11 (signed)");
    {
        // SF16 sign-extends the 10-bit endpoints: 200 and 100 are positive,
        // 824 (= 0b1100111000) has its sign bit set and reads as -200.
        // The half results are 0.14246 (byte 36), 0.00212 (byte 1) and
        // -0.14246 (clamps to 0).
        BitWriter w;
        w.put(0x03, 5);        // mode 11
        w.put(200, 10);        // rw = +200
        w.put(100, 10);        // gw = +100
        w.put(824, 10);        // bw = -200 (10-bit two's complement)
        w.put(0, 10);          // rx
        w.put(0, 10);          // gx
        w.put(0, 10);          // bx
        w.put(0, 3);           // anchored texel 0
        for (int i = 0; i < 15; ++i) w.put(0, 4);
        uint8_t out[4 * 4 * 4];
        expect(decode_tile(kDxgiBC6HSF16, w.bytes, 4, 4, out), "BC6H SF16 mode 11 decodes");
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                expect_pixel(out, 4, x, y, 36, 1, 0, 255, "BC6H SF16 mode 11 endpoint 0");
    }
}

// ---------------------------------------------------------------------------
// BC7: hand-built single blocks per mode family
// ---------------------------------------------------------------------------

void check_bc7() {
    uint8_t out[4 * 4 * 4];

    begin_group("BC7 mode 6 (single subset, P-bits, 4-bit indices)");
    {
        // Endpoint 0 holds 7-bit RGB + 7-bit alpha values plus a P-bit each;
        // the P-bit becomes the LSB, so e.g. 85|P=1 expands to 171. Every
        // texel uses index 0, which reproduces endpoint 0 exactly.
        BitWriter w;
        w.put(1u << 6, 7);    // mode 6: six zero bits then a 1
        w.put(85, 7);         // R0 -> 171
        w.put(0, 7);          // R1
        w.put(21, 7);         // G0 -> 43
        w.put(0, 7);          // G1
        w.put(63, 7);         // B0 -> 127
        w.put(0, 7);          // B1
        w.put(0, 7);          // A0 -> 1
        w.put(0, 7);          // A1
        w.put(1, 1);          // EPB0: endpoint 0's P-bit
        w.put(0, 1);          // EPB1
        w.put(0, 3);          // texel 0 anchors: 4-bit index minus one bit
        for (int i = 0; i < 15; ++i) w.put(0, 4);
        expect(decode_tile(kDxgiBC7UNorm, w.bytes, 4, 4, out), "BC7 mode 6 decodes");
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                expect_pixel(out, 4, x, y, 171, 43, 127, 1, "BC7 mode 6 endpoint 0");
    }

    begin_group("BC7 mode 1 (two subsets, shared P-bits, anchors)");
    {
        // Two subsets with endpoints E0..E3; the 6-bit values 63/0 expand
        // to 255/2 once the shared subset P-bit (1) is folded in. Partition
        // 0 puts x < 2 in subset 0 and x >= 2 in subset 1. Texel 5 stores
        // index 1 and texel 15 (subset 1's anchor) also stores index 1; any
        // anchor mishandling shifts the bit stream and changes texel 15.
        // Index 1 is the 9/64 interpolation step toward the second
        // endpoint, not the endpoint itself: e.g. between E0=(255,2,2) and
        // E1=(2,255,2) it lands on (219, 38, 2).
        BitWriter w;
        w.put(2, 2);          // mode 1: bits 0,1
        w.put(0, 6);          // partition 0
        w.put(63, 6); w.put(0, 6);   // R0, R1
        w.put(0, 6);  w.put(63, 6);  // R2, R3
        w.put(0, 6);  w.put(63, 6);   // G0, G1
        w.put(0, 6);  w.put(63, 6);   // G2, G3
        w.put(0, 6);  w.put(0, 6);   // B0, B1
        w.put(63, 6); w.put(0, 6);   // B2, B3
        w.put(1, 1);          // shared P-bit of subset 0
        w.put(1, 1);          // shared P-bit of subset 1
        // E0 = (255, 2, 2), E1 = (2, 255, 2), E2 = (2, 2, 255),
        // E3 = (255, 255, 2) after expansion.
        w.put(0, 2);          // texel 0 (subset 0 anchor)
        for (int i = 1; i < 15; ++i) {
            if (i == 5) w.put(1, 3);   // texel 5: index 1 = 9/64 toward E1
            else        w.put(0, 3);
        }
        w.put(1, 2);          // texel 15 (subset 1 anchor): index 1 = 9/64 toward E3
        expect(decode_tile(kDxgiBC7UNorm, w.bytes, 4, 4, out), "BC7 mode 1 decodes");
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                const int t = y * 4 + x;
                if (x < 2 && t == 5)
                    expect_pixel(out, 4, x, y, 219, 38, 2, 255, "BC7 mode 1 subset 0 index 1");
                else if (x < 2)
                    expect_pixel(out, 4, x, y, 255, 2, 2, 255, "BC7 mode 1 subset 0 index 0");
                else if (t == 15)
                    expect_pixel(out, 4, x, y, 38, 38, 219, 255, "BC7 mode 1 subset 1 anchor");
                else
                    expect_pixel(out, 4, x, y, 2, 2, 255, 255, "BC7 mode 1 subset 1 index 0");
            }
        }
        // The sRGB variant decodes identically.
        expect(decode_tile(kDxgiBC7UNormSRGB, w.bytes, 4, 4, out), "BC7 sRGB decodes");
        expect_pixel(out, 4, 0, 0, 255, 2, 2, 255, "BC7 sRGB decodes like UNorm");
    }

    begin_group("BC7 mode 4 (rotation + index selection)");
    {
        // Mode 4 carries RGB 5.5.5 endpoints, 6-bit alpha endpoints, a
        // 2-bit rotation and a 1-bit index selector. Endpoints 31/0 expand
        // to 255/0, alpha 63/0 to 255/0. The primary field is always 2 bits
        // per texel and the secondary 3 bits; the selector decides which
        // drives colour vs alpha.
        const uint8_t rgb[10] = { 31, 0, 31, 0, 31, 0 };  // R0,R1,G0,G1,B0,B1

        // (a) selector 0: colour = primary (index 0 -> 255), alpha =
        // secondary (index 1 with 3-bit weights -> 219). Rotation 1 then
        // swaps A and R.
        BitWriter a;
        a.put(1u << 4, 5);    // mode 4: four zero bits then a 1
        a.put(1, 2);          // rotation 1
        a.put(0, 1);          // index selector = 0
        for (int i = 0; i < 6; ++i) a.put(rgb[i], 5);
        a.put(63, 6);         // A0
        a.put(0, 6);          // A1
        a.put(0, 1);          // primary index of texel 0 (anchor)
        for (int i = 1; i < 16; ++i) a.put(0, 2);
        a.put(1, 2);          // secondary index of texel 0 (anchor)
        for (int i = 1; i < 16; ++i) a.put(1, 3);
        expect(decode_tile(kDxgiBC7UNorm, a.bytes, 4, 4, out), "BC7 mode 4(a) decodes");
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                expect_pixel(out, 4, x, y, 219, 255, 255, 255,
                             "BC7 mode 4 selector 0 + rotation 1");

        // (b) selector 1: colour = secondary (index 2 with 3-bit weights
        // -> 183), alpha = primary (index 1 with 2-bit weights -> 171),
        // no rotation.
        BitWriter b;
        b.put(1u << 4, 5);    // mode 4
        b.put(0, 2);          // rotation 0
        b.put(1, 1);          // index selector = 1
        for (int i = 0; i < 6; ++i) b.put(rgb[i], 5);
        b.put(63, 6);         // A0
        b.put(0, 6);          // A1
        b.put(1, 1);          // primary index of texel 0 (anchor)
        for (int i = 1; i < 16; ++i) b.put(1, 2);
        b.put(2, 2);          // secondary index of texel 0 (anchor)
        for (int i = 1; i < 16; ++i) b.put(2, 3);
        expect(decode_tile(kDxgiBC7UNorm, b.bytes, 4, 4, out), "BC7 mode 4(b) decodes");
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                expect_pixel(out, 4, x, y, 183, 183, 183, 171,
                             "BC7 mode 4 selector 1 swaps colour/alpha indices");
    }

    begin_group("BC7 mode 0 (three subsets, 4-bit partition)");
    {
        // Mode 0 has three subsets with 4-bit RGB endpoints and one P-bit
        // per endpoint. E0/E1 = (255, 8, 8), E2/E3 = (8, 255, 8),
        // E4/E5 = (8, 8, 255) after expansion (15|P=1 -> 255, 0|P=1 -> 8).
        // Partition 0 of the 3-subset table:
        //   y0/y1: 0 0 1 1, y2: 0 2 2 1, y3: 2 2 2 2.
        BitWriter w;
        w.put(1, 1);          // mode 0: a single 1 bit
        w.put(0, 4);          // partition 0
        const int rgb6[3][6] = {
            { 15, 15, 0, 0, 0, 0 },   // R: E0, E1, E2, E3, E4, E5
            { 0, 0, 15, 15, 0, 0 },   // G
            { 0, 0, 0, 0, 15, 15 },   // B
        };
        for (int c = 0; c < 3; ++c)
            for (int e = 0; e < 6; ++e) w.put(rgb6[c][e], 4);
        for (int e = 0; e < 6; ++e) w.put(1, 1);  // per-endpoint P-bits
        w.put(0, 2);          // texel 0 (subset 0 anchor)
        w.put(0, 3);          // texel 1
        w.put(0, 3);          // texel 2
        w.put(0, 2);          // texel 3 (subset 1 anchor)
        for (int i = 4; i < 15; ++i) w.put(0, 3);
        w.put(0, 2);          // texel 15 (subset 2 anchor)
        expect(decode_tile(kDxgiBC7UNorm, w.bytes, 4, 4, out), "BC7 mode 0 decodes");
        const int subset0[4][4] = {
            { 0, 0, 1, 1 },
            { 0, 0, 1, 1 },
            { 0, 2, 2, 1 },
            { 2, 2, 2, 2 },
        };
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                const int s = subset0[y][x];
                const uint8_t want[3][4] = {
                    { 255, 8, 8, 255 }, { 8, 255, 8, 255 }, { 8, 8, 255, 255 },
                };
                expect_pixel(out, 4, x, y, want[s][0], want[s][1], want[s][2], want[s][3],
                             "BC7 mode 0 subset colours");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Smoke + error handling
// ---------------------------------------------------------------------------

void check_smoke() {
    begin_group("smoke: zero blocks decode for every format");
    const uint32_t formats[13] = {
        kDxgiBC1UNorm, kDxgiBC1UNormSRGB, kDxgiBC2UNorm, kDxgiBC2UNormSRGB,
        kDxgiBC3UNorm, kDxgiBC3UNormSRGB, kDxgiBC4UNorm, kDxgiBC4SNorm,
        kDxgiBC5UNorm, kDxgiBC5SNorm, kDxgiBC6HUF16, kDxgiBC6HSF16,
        kDxgiBC7UNorm,
    };
    uint8_t out[4 * 4 * 4];
    for (uint32_t fmt : formats) {
        uint8_t block[16] = {};
        std::memset(out, 0xCC, sizeof(out));
        expect(decode_tile(fmt, block, 4, 4, out), "zero block decodes");
    }
    // A zero BC1 block is the 3-colour mode with every index 0: opaque black.
    {
        uint8_t block[16] = {};
        std::memset(out, 0xCC, sizeof(out));
        expect(decode_tile(kDxgiBC1UNorm, block, 4, 4, out), "zero BC1 decodes");
        expect_pixel(out, 4, 0, 0, 0, 0, 0, 255, "zero BC1 is opaque black");
        // BC6H blocks have no alpha channel: alpha is always 255.
        std::memset(out, 0xCC, sizeof(out));
        expect(decode_tile(kDxgiBC6HUF16, block, 4, 4, out), "zero BC6H decodes");
        expect_pixel(out, 4, 0, 0, 0, 0, 0, 255, "zero BC6H is black with A=255");
        // BC5 SNorm's absent blue sits at the signed zero point (128).
        std::memset(out, 0xCC, sizeof(out));
        expect(decode_tile(kDxgiBC5SNorm, block, 4, 4, out), "zero BC5 SNorm decodes");
        expect_pixel(out, 4, 0, 0, 128, 128, 128, 255, "zero BC5 SNorm is 128/128/128");
    }
}

void check_errors() {
    begin_group("error handling");
    uint8_t block8[8] = {};
    uint8_t block16[16] = {};
    uint8_t out[4 * 4 * 4];
    std::memset(out, 0xEE, sizeof(out));

    expect(!decode_bc_surface(999, 4, 4, block16, 16, out), "unsupported format rejected");
    expect(!decode_bc_surface(kDxgiBC1UNorm, 4, 4, block8, 7, out), "short source rejected");
    expect(!decode_bc_surface(kDxgiBC7UNorm, 4, 4, block16, 15, out), "short 16-byte source rejected");
    expect(!decode_bc_surface(kDxgiBC1UNorm, 0, 4, block8, 8, out), "zero width rejected");
    expect(!decode_bc_surface(kDxgiBC1UNorm, 4, 0, block8, 8, out), "zero height rejected");
    expect(!decode_bc_surface(kDxgiBC1UNorm, 4, 4, block8, 8, nullptr), "null output rejected");
    expect(!decode_bc_surface(kDxgiBC1UNorm, 4, 4, nullptr, 8, out), "null source rejected");

    // A rejected decode must not touch the output buffer.
    bool untouched = true;
    for (size_t i = 0; i < sizeof(out); ++i) untouched = untouched && out[i] == 0xEE;
    expect(untouched, "rejected decodes do not touch the output");
}

} // namespace

int main() {
    check_surface_queries();
    check_bc1();
    check_bc1_partial();
    check_bc2();
    check_bc3();
    check_bc4();
    check_bc4_snorm();
    check_bc5();
    check_bc6h();
    check_bc7();
    check_smoke();
    check_errors();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
