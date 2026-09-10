// Minimal DDS texture loader for the viewer. Loads uncompressed 2D DDS files
// (RGBA8/BGRA8) and uploads them to an OpenGL texture. BC-compressed formats
// are decoded on the CPU only if needed; for v1 the viewer supports
// uncompressed textures and falls back to a checker texture otherwise.
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
