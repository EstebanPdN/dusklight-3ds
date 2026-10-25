#pragma once
#include "gx_pica_material.hpp"
namespace dusk::native::gx {
struct CommandRegisters;
struct DecodedVertex;
struct TransformedVertex;
struct RasterColorPlan {
    struct Light {std::array<float,4> color{};std::array<float,3> position{},direction{},cosine{},distance{};};
    struct Channel {std::uint8_t mask{},diffuse{},attenuation{};};
    std::array<Light,8> lights{};
    std::array<Channel,2> channels{};
    std::array<float,4> material{},ambient{};
    std::uint8_t lit{},ambientVertex{};
    bool needsPosition{},needsNormal{};
    bool strictFinite{}; // Diagnostic channel API also validates unclamped intermediates.
    std::uint8_t rasterChannel{}; // GX channel index; channels[] means RGB vs alpha.
    std::uint8_t required{},vertex{}; // bit 0 RGB, bit 1 alpha; only consumed components.
};
enum class RasterColorError { none, material, missingState, lighting, transform, vertex, nonfinite };
// Evaluate only components
// actually consumed by emitted PICA equations. Unused components stay zero;
// that is not a replacement for missing consumed lighting/color state.
bool planRasterColor(const CommandRegisters&,const PicaMaterialPlan&,RasterColorPlan&,RasterColorError* = nullptr);
// Explicit channel/component API. Required bit 0=RGB, bit 1=alpha. Never
// substitutes channel0 for channel1. Unused components remain zero.
bool planRasterColorChannel(const CommandRegisters&,unsigned channel,std::uint8_t required,RasterColorPlan&,RasterColorError* = nullptr,bool strictFinite = true);
bool evaluateRasterColor(const RasterColorPlan&,const DecodedVertex&,std::array<float,4>&,RasterColorError* = nullptr,const TransformedVertex* = nullptr);
const char* rasterColorErrorText(RasterColorError);
}
