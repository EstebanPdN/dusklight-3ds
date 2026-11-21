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
// The colour a lighting channel reads as its vertex source (GX_SRC_VTX). A
// vertex format without it gets the colour Dolphin feeds (VertexShaderGen.cpp):
// channel 0 takes CLR1 when CLR0 is absent, and with no colour left the
// channel reads Dolphin's missing-colour value, GFX_HACK_MISSING_COLOR_VALUE
// 0xFFFFFFFF (opaque white; Dolphin's GameSettings keep it for this game).
// The console feeds the last colour it received instead, an order-dependent
// value no emulator reproduces (Dolphin issue 13783). Channel 1 with CLR1 but
// no CLR0 keeps CLR1, as drawn before (Dolphin would take white there).
// Before 2026-11-16 such draws were refused ("GX raster vertex color
// missing"): Darkhammer's room, D_MN11B r49, was left out whole.
inline constexpr std::array<float,4> missingVertexColor{1,1,1,1};
inline constexpr std::array<std::uint8_t,4> missingVertexColorBytes{255,255,255,255};
const float* rasterVertexColor(const DecodedVertex&,unsigned channel);
// Diagnostics: vertices whose consumed vertex colour was substituted (CPU
// route) and GPU decodes whose format had no CLR0 while the plan reads it.
inline std::uint64_t gxMissingColorVertices{},gxMissingColorDecodes{};
// Multi-texture compact decodes that stored texture coordinates one or more
// levels coarser (float ones with fewer than ten fraction bits, U16 ones
// halved), and attributes of their formats that no plan reads (TEX2-7, CLR1),
// left out (gx_rigid_decode.cpp).
inline std::uint64_t gxCoarseTexcoordDecodes{},gxUnreadAttributeDecodes{};
// Multi-texture decodes that converted an indexed array stored in the other
// byte order (gx_rigid_decode.cpp), and the switch that refuses them as
// before Alpha 8 (SD file swapped-arrays-off.txt: reject 5, CPU vertex route).
inline std::uint64_t gxSwappedArrayDecodes{};
inline bool gxCompactSwappedArrays{true};
const char* rasterColorErrorText(RasterColorError);
}
