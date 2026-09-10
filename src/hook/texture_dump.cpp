#include "texture_dump.h"

#include <cstring>
#include <fstream>
#include <vector>

#include <dxgiformat.h>

namespace ghost {

namespace {

#pragma pack(push, 1)
struct DdsPixelFormat {
    uint32_t size;
    uint32_t flags;
    uint32_t four_cc;
    uint32_t rgb_bit_count;
    uint32_t r_mask, g_mask, b_mask, a_mask;
};

struct DdsHeader {
    uint32_t size;
    uint32_t flags;
    uint32_t height;
    uint32_t width;
    uint32_t pitch_or_linear_size;
    uint32_t depth;
    uint32_t mip_map_count;
    uint32_t reserved1[11];
    DdsPixelFormat ddspf;
    uint32_t caps, caps2, caps3, caps4, reserved2;
};

struct DdsHeaderDx10 {
    uint32_t dxgi_format;
    uint32_t resource_dimension;  // 3 = TEXTURE2D
    uint32_t misc_flag;
    uint32_t array_size;
    uint32_t misc_flags2;
};
#pragma pack(pop)

constexpr uint32_t kDdsMagic     = 0x20534444;  // "DDS "
constexpr uint32_t kDDSD_CAPS    = 0x1;
constexpr uint32_t kDDSD_HEIGHT  = 0x2;
constexpr uint32_t kDDSD_WIDTH   = 0x4;
constexpr uint32_t kDDSD_PITCH   = 0x8;
constexpr uint32_t kDDSD_PIXELFORMAT = 0x1000;
constexpr uint32_t kDDSD_LINEARSIZE  = 0x80000;
constexpr uint32_t kDDPF_ALPHAPIXELS = 0x1;
constexpr uint32_t kDDPF_FOURCC      = 0x4;
constexpr uint32_t kDDPF_RGB         = 0x40;
constexpr uint32_t kDDSCAPS_TEXTURE  = 0x1000;
constexpr uint32_t kFourCC_DX10      = 0x30315844;  // "DX10"

struct FormatInfo {
    uint32_t dxgi;
    bool     compressed;
    uint32_t block_bytes;   // uncompressed: bytes per pixel; BC: bytes per 4x4 block
    uint32_t bit_count;     // legacy pixel format (uncompressed only)
    uint32_t r, g, b, a;
};

bool format_info(uint32_t dxgi, FormatInfo& info) {
    info = {dxgi, false, 0, 0, 0, 0, 0, 0};
    switch (dxgi) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            info.block_bytes = 4; info.bit_count = 32;
            info.r = 0x000000FF; info.g = 0x0000FF00; info.b = 0x00FF0000; info.a = 0xFF000000;
            return true;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            info.block_bytes = 4; info.bit_count = 32;
            info.r = 0x00FF0000; info.g = 0x0000FF00; info.b = 0x000000FF; info.a = 0xFF000000;
            return true;
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            info.block_bytes = 4; info.bit_count = 32;
            info.r = 0x00FF0000; info.g = 0x0000FF00; info.b = 0x000000FF;
            return true;
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_UNORM:
            info.compressed = true; info.block_bytes = 8;
            return true;
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            info.compressed = true; info.block_bytes = 16;
            return true;
        default:
            return false;
    }
}

} // namespace
bool dds_format_supported(uint32_t dxgi_format) {
    FormatInfo info;
    return format_info(dxgi_format, info);
}

bool dds_is_compressed(uint32_t dxgi_format) {
    FormatInfo info;
    return format_info(dxgi_format, info) && info.compressed;
}

uint32_t dds_tight_row_pitch(uint32_t dxgi_format, uint32_t width) {
    FormatInfo info;
    if (!format_info(dxgi_format, info))
        return 0;
    if (info.compressed)
        return ((width + 3) / 4) * info.block_bytes;
    return width * info.block_bytes;
}

bool write_dds(const std::string& path,
               uint32_t width, uint32_t height,
               uint32_t dxgi_format,
               const void* data, uint32_t row_pitch) {
    FormatInfo info;
    if (!format_info(dxgi_format, info))
        return false;

    DdsHeader header{};
    header.size  = sizeof(DdsHeader);
    header.flags = kDDSD_CAPS | kDDSD_HEIGHT | kDDSD_WIDTH | kDDSD_PIXELFORMAT |
                   (info.compressed ? kDDSD_LINEARSIZE : kDDSD_PITCH);
    header.height = height;
    header.width  = width;
    header.mip_map_count = 1;
    header.caps   = kDDSCAPS_TEXTURE;

    const uint32_t tight_pitch = dds_tight_row_pitch(dxgi_format, width);
    header.pitch_or_linear_size = info.compressed
        ? ((width + 3) / 4) * ((height + 3) / 4) * info.block_bytes
        : tight_pitch;

    header.ddspf.size = sizeof(DdsPixelFormat);
    const bool use_dx10 = info.compressed;  // BC formats go through the DX10 header
    if (use_dx10) {
        header.ddspf.flags   = kDDPF_FOURCC;
        header.ddspf.four_cc = kFourCC_DX10;
    } else {
        header.ddspf.flags = kDDPF_RGB | (info.a ? kDDPF_ALPHAPIXELS : 0u);
        header.ddspf.rgb_bit_count = info.bit_count;
        header.ddspf.r_mask = info.r;
        header.ddspf.g_mask = info.g;
        header.ddspf.b_mask = info.b;
        header.ddspf.a_mask = info.a;
    }

    DdsHeaderDx10 dx10{};
    dx10.dxgi_format        = dxgi_format;
    dx10.resource_dimension = 3;  // D3D10_RESOURCE_DIMENSION_TEXTURE2D
    dx10.array_size         = 1;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;

    out.write(reinterpret_cast<const char*>(&kDdsMagic), sizeof(kDdsMagic));
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (use_dx10)
        out.write(reinterpret_cast<const char*>(&dx10), sizeof(dx10));

    // Pack rows tightly (staging row pitch may be padded).
    const uint32_t rows = info.compressed ? (height + 3) / 4 : height;
    const auto* src = static_cast<const uint8_t*>(data);
    if (row_pitch == tight_pitch) {
        out.write(reinterpret_cast<const char*>(src),
                  static_cast<std::streamsize>(tight_pitch) * rows);
    } else {
        for (uint32_t y = 0; y < rows; ++y)
            out.write(reinterpret_cast<const char*>(src + static_cast<size_t>(y) * row_pitch),
                      tight_pitch);
    }
    return static_cast<bool>(out);
}

} // namespace ghost

