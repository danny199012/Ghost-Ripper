#include "texture_loader.h"

#include "gl_extra.h"

#include <cstring>
#include <fstream>
#include <vector>

// ImGui's bundled loader provides the core GL 3.3 functions; gl_extra.h adds
// the mipmap function that the stripped loader omits.
#include "imgui_impl_opengl3_loader.h"

namespace ghost {
namespace viewer {

namespace {

#pragma pack(push, 1)
struct DdsPixelFormat {
    uint32_t size, flags, four_cc, rgb_bit_count;
    uint32_t r_mask, g_mask, b_mask, a_mask;
};
struct DdsHeader {
    uint32_t size, flags, height, width, pitch_or_linear_size, depth, mip_map_count;
    uint32_t reserved1[11];
    DdsPixelFormat ddspf;
    uint32_t caps, caps2, caps3, caps4, reserved2;
};
#pragma pack(pop)

constexpr uint32_t kDdsMagic    = 0x20534444;
constexpr uint32_t kDDPF_FOURCC = 0x4;
constexpr uint32_t kDDPF_RGB    = 0x40;
constexpr uint32_t kDDPF_ALPHA  = 0x1;
constexpr uint32_t kFourCC_DX10 = 0x30315844;

struct DdsHeaderDx10 {
    uint32_t dxgi_format, resource_dimension, misc_flag, array_size, misc_flags2;
};

bool read_all(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamoff size = in.tellg();
    in.seekg(0);
    out.resize(static_cast<size_t>(size));
    return static_cast<bool>(in.read(reinterpret_cast<char*>(out.data()), size));
}

} // namespace

unsigned int load_dds_texture(const std::string& path, int& width, int& height) {
    std::vector<uint8_t> bytes;
    if (!read_all(path, bytes))
        return 0;
    if (bytes.size() < sizeof(uint32_t) + sizeof(DdsHeader))
        return 0;

    uint32_t magic = 0;
    std::memcpy(&magic, bytes.data(), sizeof(magic));
    if (magic != kDdsMagic)
        return 0;

    DdsHeader header{};
    std::memcpy(&header, bytes.data() + sizeof(uint32_t), sizeof(header));

    width = static_cast<int>(header.width);
    height = static_cast<int>(header.height);

    const bool has_alpha = (header.ddspf.flags & kDDPF_ALPHA) != 0;
    const bool is_rgb    = (header.ddspf.flags & kDDPF_RGB) != 0;
    const bool is_dx10   = (header.ddspf.flags & kDDPF_FOURCC) != 0 &&
                           header.ddspf.four_cc == kFourCC_DX10;

    // For v1 we support the common uncompressed 32bpp RGB(A) layouts. BC
    // textures (which use the DX10 extension) are not decoded in the viewer
    // yet; the caller falls back to a checker texture in that case.
    if (!is_rgb || header.ddspf.rgb_bit_count != 32) {
        (void)is_dx10;
        return 0;
    }

    const size_t data_offset = sizeof(uint32_t) + sizeof(DdsHeader) +
                               (is_dx10 ? sizeof(DdsHeaderDx10) : 0);
    if (bytes.size() < data_offset + static_cast<size_t>(width) * height * 4)
        return 0;

    const uint8_t* pixels = bytes.data() + data_offset;
    const uint32_t r_mask = header.ddspf.r_mask;
    // BGRA (B in low bits) vs RGBA. D3D's B8G8R8A8 is far more common.
    const bool is_bgra = (r_mask == 0x00FF0000);

    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    // Upload as RGBA8, swapping channels if the source was BGRA.
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    for (int i = 0; i < width * height; ++i) {
        const uint8_t* src = pixels + static_cast<size_t>(i) * 4;
        uint8_t* dst = rgba.data() + static_cast<size_t>(i) * 4;
        if (is_bgra) { dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = src[3]; }
        else         { dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3]; }
        if (!has_alpha) dst[3] = 255;
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgba.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    return tex;
}

unsigned int create_checker_texture() {
    const int size = 64;
    std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const bool light = ((x / 16) + (y / 16)) % 2 == 0;
            const uint8_t c = light ? 220 : 90;
            uint8_t* p = &pixels[static_cast<size_t>((y * size + x) * 4)];
            p[0] = c; p[1] = c; p[2] = c; p[3] = 255;
        }

    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 pixels.data());
    return tex;
}

} // namespace viewer
} // namespace ghost
