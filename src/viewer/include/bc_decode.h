// CPU decoder for block-compressed DDS surfaces (BC1-BC7) to RGBA8.
//
// The hook dumps textures as DDS files; games overwhelmingly use block
// compression (DXT1/3/5 = BC1/2/3, ATI1/2 = BC4/5, BC6H/BC7 on newer
// titles). Standard OpenGL can't sample those, so the viewer decodes them
// on the CPU into RGBA8 before uploading.
//
// Pure C++ with no external dependencies (no dxgiformat.h: DXGI format
// codes are spelled out as constants) so it can be unit-tested headless.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ghost {
namespace viewer {

// DXGI_FORMAT codes this decoder understands (subset of dxgiformat.h).
enum BcDxgiFormat : uint32_t {
    kDxgiBC1UNorm       = 71,
    kDxgiBC1UNormSRGB   = 72,
    kDxgiBC2UNorm       = 74,
    kDxgiBC2UNormSRGB   = 75,
    kDxgiBC3UNorm       = 77,
    kDxgiBC3UNormSRGB   = 78,
    kDxgiBC4UNorm       = 80,
    kDxgiBC4SNorm       = 81,
    kDxgiBC5UNorm       = 83,
    kDxgiBC5SNorm       = 84,
    kDxgiBC6HUF16       = 95,
    kDxgiBC6HSF16       = 96,
    kDxgiBC7UNorm       = 98,
    kDxgiBC7UNormSRGB   = 99,
};

// True if decode_bc_surface() supports the format.
bool bc_decode_supported(uint32_t dxgi_format);

// Bytes per 4x4 block: 8 for BC1/BC4, 16 for the rest (0 if unsupported).
uint32_t bc_block_bytes(uint32_t dxgi_format);

// Expected size in bytes of one mip level of a BC surface
// (0 if the format is unsupported or the dimensions are invalid).
size_t bc_surface_size(uint32_t dxgi_format, uint32_t width, uint32_t height);

// True if the format stores signed data (BC4/BC5 SNorm, BC6H SF16).
bool bc_format_is_signed(uint32_t dxgi_format);

// Decode one mip level of a block-compressed surface to tightly packed
// RGBA8 (width*height*4 bytes in `out_rgba`). `src` holds
// ((width+3)/4) * ((height+3)/4) blocks. Partial blocks at the right/bottom
// edge decode only the pixels that exist. sRGB variants are *not* linearized
// here (the viewer wants sRGB bytes for direct display); they decode
// identically to their UNorm counterpart.
// Returns false (without touching out_rgba) if the format is unsupported or
// src_size is smaller than bc_surface_size().
bool decode_bc_surface(uint32_t dxgi_format, uint32_t width, uint32_t height,
                       const uint8_t* src, size_t src_size, uint8_t* out_rgba);

} // namespace viewer
} // namespace ghost
