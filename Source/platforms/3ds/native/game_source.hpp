#pragma once
#include "contracts.hpp"
namespace dusk::pica {
// Immutable original-file identity, captured before the game mutates its data.
// bytes==0 means unidentified. No addresses or command state are file keys.
struct SourceIdentity {
    std::array<std::uint8_t,32> sha256{};
    std::uint32_t bytes{};
    bool operator==(const SourceIdentity&)const=default;
};
enum ModelInput:std::uint32_t {poseInput=1,texgenInput=2,lightingInput=4,fogInput=8,orderingInput=16,colorInput=32};
bool identifySource(std::span<const std::byte> original,SourceIdentity&);
// Native producer inputs, compiled offline; IDs are not hardware matrix slots.
struct UniformBinding {
    std::uint32_t material{},anchor{},paletteCount{};
    std::array<std::uint32_t,8> palette{};
};
using GameSourceProducer=bool(*)(const void* instance,const UniformBinding&,DrawUniforms&,void* user) noexcept;
// Copy already-calculated per-joint/per-envelope matrices, never vertices.
// Native palette IDs are one-based draw-matrix identities authored offline.
// Input rows may be unaligned. Failure leaves out empty, never stale/identity.
bool copyGamePalette(const UniformBinding&,unsigned matrixCount,
                     const float (*position)[3][4],const float (*normal)[3][3],Palette& out);
}
