// DDS texture writer. Captured 2D textures are dumped as .dds files using a
// legacy header for common uncompressed RGBA/BGRA formats and the DX10
// extension header for block-compressed (BC1-7) formats, so standard tools
// (Blender, Compressonator, texdiag, ...) can open them directly.
#pragma once

#include <cstdint>
#include <string>

namespace ghost {

// True if we know how to store this DXGI_FORMAT as a DDS file.
bool dds_format_supported(uint32_t dxgi_format);

// True for block-compressed formats (BC1-7).
bool dds_is_compressed(uint32_t dxgi_format);

// Tightly packed row pitch in bytes for the given format/width
// (0 for unsupported formats).
uint32_t dds_tight_row_pitch(uint32_t dxgi_format, uint32_t width);

// Write `path` as a DDS file. `data` uses `row_pitch` bytes per row (staging
// textures may be padded); rows are packed tightly in the output file.
bool write_dds(const std::string& path,
               uint32_t width, uint32_t height,
               uint32_t dxgi_format,
               const void* data, uint32_t row_pitch);

} // namespace ghost
