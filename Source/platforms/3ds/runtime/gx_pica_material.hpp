#pragma once
#include "gx_material.hpp"
namespace dusk::native::gx {
enum class MaterialSource : std::uint8_t { primary, texture0, texture1, texture2, previous, constant, buffer };
enum class MaterialOperand : std::uint8_t { color, alpha, red, green, blue };
enum class MaterialFunction : std::uint8_t { replace, modulate, add, interpolate, multiplyAdd, addSigned, subtract };
struct MaterialInput {
    MaterialSource source{};
    MaterialOperand operand{};
    bool inverse{};
    bool operator==(const MaterialInput&) const = default;
};
struct PicaMaterialChannel {
    MaterialFunction function{};
    std::array<MaterialInput,3> inputs{};
    std::uint8_t scale{}; // 0/1/2 => 1x/2x/4x
};
struct PicaMaterialStage {
    PicaMaterialChannel rgb{},alpha{};
    std::array<std::uint8_t,4> constant{};
};
struct MaterialTextureBinding {
    std::uint8_t map{},coordinate{};
    bool operator==(const MaterialTextureBinding&) const = default;
};
enum class RasterCarrier : std::uint8_t { none, raster1RgbRaster0GrayAlpha };
struct PicaMaterialPlan {
    std::array<PicaMaterialStage,6> stages{};
    // Bind these map/coordinate pairs to PICA texture units in this order.
    std::array<MaterialTextureBinding,3> textures{};
    std::uint8_t stageCount{},textureCount{};
    std::array<std::uint8_t,4> bufferConstant{}; // Initial combiner buffer; usable after stage0.
    std::uint8_t bufferUpdateRgb{}; // Capture stage0..3 RGB; visible two stages later.
    bool projectiveTexture0{};
    bool quantizedConstants{};
    RasterCarrier rasterCarrier{}; // Diagnostic packing only; requires whole-draw grayscale proof.
};
enum class MaterialPlanError {
    none, stages, operation, indirect, source, uninitialized, constantRange,
    konstFraction, constantConflict, swap, textureUnits, equation
};
enum class MaterialPolicy { exactConstants, quantizedConstants };
// Common direct TEV equations, allocation-free. Unsupported operations reject.
// Initial signed register inputs A/B/C use GX low-byte wrapping. Initial
// all-nonpositive D in [-255,0] uses a clamped subtraction; other signed D and
// unbounded computed values remain unsupported. Quantized policy additionally
// admits bounded fractions/half-scale. Exact policy does not silently opt in.
// Real arithmetic agreement is not fixed-point GPU proof.
// The caller must bind the declared textures/UVs and supply correct raster color.
// Clears output on failure. No GPU calls, pixel reads or geometry processing.
bool planPicaMaterial(const MaterialState&,PicaMaterialPlan&,MaterialPlanError* = nullptr,MaterialPolicy = MaterialPolicy::exactConstants);
// Conservative semantic admission: both RGB streams, no raster alpha reads,
// identity raster swaps, no indirect. Never enables this route implicitly.
// All original stage boundaries, register allocation and constants use Builder.
bool planRasterCarrierMaterial(const MaterialState&,PicaMaterialPlan&,MaterialPlanError* = nullptr);
// PICA provides homogeneous texture sampling on unit 0 only. Route a single
// consumed projective binding there and rewrite every TEV source consistently.
// Multiple consumed projective bindings reject; the input stays intact on error.
bool routeProjectiveTexture(PicaMaterialPlan&,std::uint8_t projectiveCoordinates);
const char* materialPlanErrorText(MaterialPlanError);
}
