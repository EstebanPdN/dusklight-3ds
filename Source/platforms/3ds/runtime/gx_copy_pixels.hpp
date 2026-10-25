#pragma once
#include "gx_target_extent.hpp"
#include <span>
#include <cstddef>
namespace dusk::native {
struct ColorCopyPlan {
    gx::TargetExtent extent{};
    unsigned left{},top{},sourceWidth{},sourceHeight{},width{},height{},format{};
    std::size_t bytes{};
    bool redChannel{}; // GX_CTF_R8: pack original red indices, not luminance.
};
bool planColorCopy(const gx::TargetExtent&,unsigned left,unsigned top,unsigned sourceWidth,unsigned sourceHeight,
                   unsigned width,unsigned height,unsigned format,ColorCopyPlan&);
// Linear rotated PICA RGBA8 (A,B,G,R bytes) -> original GX tiled format.
// Unflipped color readback: LCD(x,y) is at (x*pixelHeight+pixelHeight-1-y)*4.
// This color contract does not describe raw tiled D24S8 depth storage.
// No raw game pointer dereference. Output belongs to the copy registry.
bool encodeColorCopy(const ColorCopyPlan&,std::span<const std::byte>,std::span<std::uint8_t>);
// GX Z16 copies are sampled by the original game as IA8. Read the actual
// rotated/tiled PICA D24S8 snapshot once, reverse its depth and pack GX Z[23:8].
bool planDepthCopy(const gx::TargetExtent&,unsigned left,unsigned top,unsigned sourceWidth,unsigned sourceHeight,
                   unsigned width,unsigned height,unsigned format,ColorCopyPlan&);
bool encodeDepthCopy(const ColorCopyPlan&,std::span<const std::byte>,std::span<std::uint8_t>);
}
