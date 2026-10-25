#pragma once
#include <array>
#include <cstdint>

namespace dusk::native::gx {
struct CommandRegisters;
struct MaterialOperation {
    std::uint8_t operation, bias, scale, clamp, output;
};
struct MaterialStage {
    std::array<std::uint8_t,4> color{}, alpha{};
    MaterialOperation colorOp{}, alphaOp{};
    // Hardware raster selector: 0/1 channels, 5/6 bump alpha, 7 zero.
    std::uint8_t coordinate{}, texture{}, raster{}, rasterSwap{}, textureSwap{};
    std::uint8_t konstColor{}, konstAlpha{};
    bool textureEnabled{};
    std::uint32_t indirect{};
};
struct MaterialState {
    std::array<MaterialStage,16> stages{};
    std::array<std::array<std::int16_t,4>,4> colors{};
    std::array<std::array<std::uint8_t,4>,4> konst{}, swaps{};
    // Full RGBA validity requires both independently written RA and BG halves.
    std::uint8_t colorValid{}, konstValid{}, swapValid{};
    std::uint8_t stageCount{}, texgenCount{}, channelCount{}, indirectCount{};
};
enum class MaterialError { none, missingState, invalidCount };
// Decode active stages only; no allocation, approximation or GPU admission.
// Missing colors/swaps remain explicitly invalid; a renderer must validate its
// actual dependencies. Indirect state is retained, not interpreted as direct.
// Output is cleared on failure. Texture resources and lighting are separate.
bool decodeMaterial(const CommandRegisters&, MaterialState&, MaterialError* = nullptr);
}
