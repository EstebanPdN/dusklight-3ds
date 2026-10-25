#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
namespace dusk::native {
enum class NativeTextureFormat : std::uint8_t { rgba8, la8, la4, rgb565 };
constexpr unsigned nativeTextureBytesPerPixel(NativeTextureFormat format) {
    switch(format) {
    case NativeTextureFormat::rgba8: return 4;
    case NativeTextureFormat::la8: return 2;
    case NativeTextureFormat::la4: return 1;
    case NativeTextureFormat::rgb565: return 2;
    }
    return 0;
}
struct NativeTexturePlan {
    unsigned width{},height{},paddedWidth{},paddedHeight{};
    std::size_t dataOffset{},sourceBytes{},outputBytes{};
    std::uint8_t gxFormat{};
    NativeTextureFormat format{};
    std::uint8_t mipCount{1};
    std::size_t paletteOffset{};
    std::uint16_t paletteEntries{};
    std::uint8_t paletteFormat{};
    // Source descriptor remains the original BTI; width/height/mipCount above
    // describe GPU storage after selecting an authored base mip.
    unsigned originalWidth{},originalHeight{};
    std::uint8_t originalMipCount{},sourceMip{};
    bool operator==(const NativeTexturePlan&) const = default;
};
// CI4/CI8/CI14 use bounded embedded IA8/RGB565/RGB5A3 palettes and RGBA8 output.
// Mip pyramids require power-of-two extents and a >=8x8 last level. I4/IA4 use LA4; I8/IA8 use LA8,
// preserving texture alpha even when BTI alphaEnabled is zero. RGBA8/CMPR
// retain full RGBA8. GX RGB565 retains its exact R5/G6/B5 word in little-endian
// PICA RGB565 storage; no channel expansion/requantization. No allocation or
// retained input pointers. Native normalized GPU sampling still needs validation
// against the former bit-replicated RGBA8 path, including filtered samples.
// Diagnostic A/B selection affects only direct GX RGB565 images. All other
// formats and CPU source bytes retain the production path. Decode validates
// either complete plan; callers must freeze the selection while images live.
bool inspectNativeTexture(std::span<const std::uint8_t>,NativeTexturePlan&,
                          bool rgb565Rgba8Reference=false,bool halfResolution=false);
// Half mode selects authored mip 1 only: direct RGB565 or logically opaque CMPR,
// RGB565 alpha header zero, >=16 per axis,
// >=2 provided mips with a >=8x8 tail. No resampling or CPU asset mutation.
// CMPR scans all original logical texels in every mip (including skipped base).
// CMPR header alpha capability does not override proven logical opacity.
// Transparent palette entries are allowed only when never selected. Output
// remains exact RGBA8; no alpha resampling or native compressed PICA storage.
// The caller must separately prove immutable pixels and safe resource usage.
using NativeTextureFingerprint=std::array<std::uint8_t,32>;
// SHA-256 over [GX format, BE16 width, BE16 height, mipCount] followed by the
// entire bounded authored GX mip payload, for direct GX4/GX14 only. The GX4
// hash domain is unchanged. Ignores offsets, sampler and padding
// outside the payload so normalized retained copies/aliases share identity.
bool nativeTextureFingerprint(std::span<const std::uint8_t>,NativeTextureFingerprint&);
// Writes PICA Morton tiles directly, including clamp-to-edge padding, once per
// output texel. No intermediate image/clear/edge pass. Rejects overlapping or
// undersized buffers and a stale plan before any output mutation.
// leanPadding skips padding tiles more than one tile beyond the image (never
// sampled by clamped draws: UV scales stop at the image edge and a bilinear
// fetch reads at most the adjacent padding tile). Repeat-wrapped padded textures
// are rejected at bind time, so the skipped texels are never read.
// Box-filtered half resolution for textures without authored mips (sourceMip=2):
// GPU storage is half size per axis; decodeNativeTexture averages 2x2 texels.
bool inspectNativeTextureBoxHalf(std::span<const std::uint8_t>,NativeTexturePlan&,bool rgb565Rgba8Reference);
bool decodeNativeTexture(std::span<const std::uint8_t>,const NativeTexturePlan&,
                         std::span<std::uint8_t>,bool leanPadding=false);
}
