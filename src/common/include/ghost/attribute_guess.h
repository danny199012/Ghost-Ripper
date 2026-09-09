// Heuristic vertex-layout guessing.
//
// D3D11+ does not let us reflect an ID3D11InputLayout at runtime, so when a
// game draws with an opaque layout we only know the vertex stride. This table
// maps common strides to widely-used layouts (Ninja Ripper uses a similar
// approach). Guesses are recorded in the capture file; the viewer lets the
// user override them later.
#pragma once

#include "capture_format.h"

#include <cstdint>
#include <vector>

namespace ghost {

// Guess the vertex attribute layout for a raw buffer with the given stride.
// Always places POSITION (float3) at offset 0; fills in additional attributes
// for well-known strides. Returns a vector possibly containing only POSITION.
std::vector<fmt::MeshAttribute> guess_vertex_layout(uint32_t vertex_stride);

} // namespace ghost
