#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace dusk::native {
struct TextureExtent {
    unsigned width = 0, height = 0, paddedWidth = 0, paddedHeight = 0;
    std::size_t dataOffset = 0, outputBytes = 0;
};
// First measured startup texture path only: GX IA8, mip zero. No GPU calls,
// allocation or I/O. Other formats fail explicitly rather than drawing garbage.
bool inspectBtiIa8(std::span<const std::uint8_t> bti, TextureExtent& extent);
bool decodeBtiIa8(std::span<const std::uint8_t> bti, const TextureExtent& extent,
                  std::span<std::uint8_t> picaRgba8);
}
