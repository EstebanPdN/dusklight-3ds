#pragma once
#include "texture_ia8.hpp"

namespace dusk::native {
// GX I4, one mip, no palette. Uses the shared extent contract and writes PICA
// tiled RGBA8 memory (A,B,G,R). All four channels equal the expanded intensity;
// BTI alphaEnabled affects the caller's TEV choice, not texture decoding.
// No allocation, GPU access or I/O; malformed/unsupported input returns false.
bool inspectBtiI4(std::span<const std::uint8_t> bti, TextureExtent& extent);
bool decodeBtiI4(std::span<const std::uint8_t> bti, const TextureExtent& extent,
                 std::span<std::uint8_t> picaRgba8);
}
