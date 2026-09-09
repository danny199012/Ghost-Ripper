// Vertex attribute helpers: element sizes and decoding raw vertex bytes
// into floats. Shared by the writer (AABB computation), the reader and the
// viewer's mesh conversion.
#pragma once

#include "capture_format.h"

#include <cstddef>
#include <cstdint>

namespace ghost {

// Byte size of one element of the given format (0 for Unknown).
uint32_t attribute_element_size(fmt::AttributeFormat format);

// Number of float components the format decodes to (0 for Unknown).
uint32_t attribute_component_count(fmt::AttributeFormat format);

// IEEE-754 half -> single conversion.
float half_to_float(uint16_t half);

// Decode one attribute element from `src` into `out` (up to 4 floats).
// Integer formats are converted to float; normalized formats to [0,1]/[-1,1].
// Returns false for unknown formats.
bool decode_attribute(const void* src, fmt::AttributeFormat format, float out[4]);

const char* semantic_name(fmt::Semantic semantic);
const char* attribute_format_name(fmt::AttributeFormat format);
const char* topology_name(fmt::PrimitiveTopology topology);

} // namespace ghost
