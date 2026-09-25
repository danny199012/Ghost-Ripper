// DDS texture loader for the viewer. Handles uncompressed 32bpp RGB(A)
// (legacy header) and block-compressed BC1-BC7 (DX10 header, decoded on the
// CPU via bc_decode.h) and uploads the result as an RGBA8 OpenGL texture.
#pragma once

#include <cstdint>
#include <string>

namespace ghost {
namespace viewer {

// Load `path` as an OpenGL 2D texture and return the texture id, or 0 on
// failure. `width`/`height` are filled on success.
unsigned int load_dds_texture(const std::string& path, int& width, int& height);

// Creates a small procedural checker texture, used as a placeholder when a
// mesh has no (or an unreadable) texture.
unsigned int create_checker_texture();

} // namespace viewer
} // namespace ghost
