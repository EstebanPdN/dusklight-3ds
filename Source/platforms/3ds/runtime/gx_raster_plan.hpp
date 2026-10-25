#pragma once
#include "gx_pica_math.hpp"
#include "gx_alpha_interval.hpp"
namespace dusk::native::gx {
struct CommandRegisters;
struct RasterPlan {
    PicaScissor scissor{};
    Blend blend{};
    Depth depth{};
    AlphaPlan alpha{};
    AlphaPlan alphaSecond{};
    bool alphaUnion{};
    AlphaInterval alphaInterval{};
    AlphaIntervalPolicy alphaIntervalPolicy{};
    std::uint8_t cull{};
    bool writeRgb{},writeAlpha{};
    bool storesAlpha{};
    bool clipDisabled{};
    bool earlyDepthAlpha{};
    bool subtractBlend{};
};
enum class ClipPolicy { requireEnabled, validateGeometry };
enum class FogPolicy { requireSupported, diagnosticDisabled, vertexApproximation };
enum class ColorStoragePolicy { rgb8Only, rgba8ForRgba6 };
enum class RasterError { none, missingState, blend, earlyDepth, alpha, pixelFormat, destinationAlpha, fog, zTexture, scissorOffset, coPlanar, clipMode, alphaIntervalOwnership, alphaIntervalUi };
// Current direct-color target: RGB8_Z24, no fog/z-texture/destination-alpha
// override. Do not infer missing global pixel-engine state from material data.
// RGB8 has no stored alpha: disable alpha writes and treat destination-alpha
// blend factors as one/zero independently of the PICA target's prior contents.
// Original game startup explicitly permits RGBA6 storage in the real RGBA8
// target. Writes and blend alpha are retained; 6-bit quantization is a recorded
// visual divergence. Diagnostics retain the default strict RGB8 policy.
bool planRaster(const CommandRegisters&,RasterPlan&,RasterError* = nullptr,
                FogPolicy = FogPolicy::requireSupported,ColorStoragePolicy = ColorStoragePolicy::rgb8Only,ClipPolicy = ClipPolicy::requireEnabled,
                bool validateConstantEarlyAlpha=false,bool allowAlphaUnion=false,AlphaIntervalPolicy intervalPolicy={});
const char* rasterErrorText(RasterError);
}
