#pragma once
#include "contracts.hpp"
namespace dusk::pica {
// Immutable original-file identity, captured before the game mutates its data.
// bytes==0 means unidentified. No addresses or command state are file keys.
struct SourceIdentity {
    std::array<std::uint8_t,32> sha256{};
    std::uint32_t bytes{};
    std::array<std::uint8_t,32> imageContext{};
    bool hasImageContext=false;
    bool operator==(const SourceIdentity&)const=default;
};
enum ModelInput:std::uint32_t {poseInput=1,texgenInput=2,lightingInput=4,fogInput=8,orderingInput=16,colorInput=32};
bool identifySource(std::span<const std::byte> original,SourceIdentity&);
// Canonical v1 identity: SHA256("DUSKIMG1" + original BE 32-byte
// header with offset bytes 12..15/28..31 zeroed + LE32 image/palette
// lengths + exact authored mip bytes + exact palette bytes).
// Context: SHA256("DUSKCTX1" + LE32 table count + ordered image digests).
// Offsets/addresses and allocation padding never participate.
bool identifyImage(std::span<const std::uint8_t> header,std::span<const std::uint8_t> image,
                   std::span<const std::uint8_t> palette,std::array<std::uint8_t,32>&);
// Dynamic framebuffer role: SHA256("DUSKROLE1framebuffer-color").
std::array<std::uint8_t,32> identifyFramebufferColor();
bool identifyImageContext(std::span<const std::array<std::uint8_t,32>>,std::array<std::uint8_t,32>&);
// Native producer inputs, compiled offline; IDs are not hardware matrix slots.
struct UniformBinding {
    std::uint32_t material{},anchor{},paletteCount{};
    std::array<std::uint32_t,paletteSize> palette{};
    std::array<std::uint8_t,3> textureSlots{255,255,255},textureGenerators{255,255,255};
    std::uint32_t sourceAttributes=0;
    std::uint8_t channelReadMask=15;
    // Per-RGBA lanes; wrap/signed are disjoint read subsets on registers 0..2.
    std::array<std::uint8_t,7> parameterReadMasks{},parameterWrapMasks{},parameterSignedMasks{};
    std::uint32_t sourceShape=0,billboardMode=0;
    // Zero stage count means no source program proof (library-only bindings).
    std::uint8_t sourceStageCount=0,sourceTexGenCount=0;
};
using GameSourceProducer=bool(*)(const void* instance,const UniformBinding&,DrawUniforms&,void* user) noexcept;
// Copy already-calculated per-joint/per-envelope matrices, never vertices.
// Native palette IDs are one-based draw-matrix identities authored offline.
// Input rows may be unaligned. Failure leaves out empty, never stale/identity.
bool copyGamePalette(const UniformBinding&,unsigned matrixCount,
                     const float (*position)[3][4],const float (*normal)[3][3],Palette& out);
}
