#include "texture_i4.hpp"
#include <algorithm>

namespace dusk::native {
namespace {
unsigned be16(const std::uint8_t* p) { return (unsigned(p[0]) << 8) | p[1]; }
std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | p[3];
}
unsigned padded(unsigned n) { unsigned p = 8; while (p < n) p *= 2; return p; }
unsigned morton8(unsigned x, unsigned y) {
    unsigned result = 0;
    for (unsigned bit = 0; bit < 3; ++bit)
        result |= ((x >> bit) & 1) << (2 * bit) | ((y >> bit) & 1) << (2 * bit + 1);
    return result;
}
}
bool inspectBtiI4(std::span<const std::uint8_t> bti, TextureExtent& e) {
    e = {};
    if (bti.size() < 32 || bti[0] != 0 || bti[8] != 0 ||
        be16(bti.data() + 10) != 0 || bti[24] != 1) return false;
    const unsigned w = be16(bti.data() + 2), h = be16(bti.data() + 4);
    const std::size_t offset = be32(bti.data() + 28);
    if (!w || !h || w > 1024 || h > 1024 || offset < 32 || offset > bti.size()) return false;
    const std::size_t tiledBytes = ((w + 7) / 8) * ((h + 7) / 8) * 32;
    if (tiledBytes > bti.size() - offset) return false;
    e = {w, h, padded(w), padded(h), offset, std::size_t(padded(w)) * padded(h) * 4};
    return true;
}
bool decodeBtiI4(std::span<const std::uint8_t> bti, const TextureExtent& e,
                 std::span<std::uint8_t> output) {
    TextureExtent checked;
    if (!inspectBtiI4(bti, checked) || e.width != checked.width || e.height != checked.height ||
        e.paddedWidth != checked.paddedWidth || e.paddedHeight != checked.paddedHeight ||
        e.dataOffset != checked.dataOffset || e.outputBytes != checked.outputBytes ||
        output.size() < e.outputBytes) return false;
    const auto inStart = reinterpret_cast<std::uintptr_t>(bti.data());
    const auto outStart = reinterpret_cast<std::uintptr_t>(output.data());
    if ((outStart >= inStart && outStart - inStart < bti.size()) ||
        (inStart >= outStart && inStart - outStart < e.outputBytes)) return false;
    std::fill_n(output.data(), e.outputBytes, 0);
    for (unsigned y = 0; y < e.height; ++y) {
        for (unsigned x = 0; x < e.width; ++x) {
            const auto source = e.dataOffset + ((y / 8) * ((e.width + 7) / 8) + x / 8) * 32 +
                                (y % 8) * 4 + (x % 8) / 2;
            const auto nibble = (bti[source] >> ((x & 1) ? 0 : 4)) & 15;
            const auto intensity = static_cast<std::uint8_t>(nibble * 17);
            const auto destination = (((y / 8) * (e.paddedWidth / 8) + x / 8) * 64 +
                                      morton8(x % 8, y % 8)) * 4;
            // Pinned Aurora TextureDecoderI4 also maps texture alpha to intensity.
            // Original J2DPicture selects constant alpha when alphaEnabled is 0.
            std::fill_n(output.data() + destination, 4, intensity);
        }
    }
    return true;
}
}
