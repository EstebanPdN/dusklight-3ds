#pragma once
#include <cstdint>
#include <span>
namespace dusk::native {
// Retain the actual initialized original capture allocation before J3D builds
// its display lists. Subsequent real EFB copies refresh this same image/token.
// This does not perform a capture or imply that a frame has been rendered.
bool registerCopyImage(std::span<const std::uint8_t> bti, const void* identity);
// Explicit owner-proven CI8 destination and live palette extents. The owner
// must destroy the copy before freeing either original allocation.
bool registerIndexedCopyImage(std::span<const std::uint8_t> header,const void* identity,
                              std::size_t imageBytes,std::span<const std::uint8_t> palette);
bool refreshCopyImagePalette(const void* identity);
std::size_t copyImageBytes();
}
