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
enum class RasterCarrier : std::uint8_t { none, raster1RgbRaster0GrayAlpha, raster0RgbaRaster1Lookup };
// Diagnostics: the planner rules a plan needed (PicaMaterialPlan::lowering).
namespace MaterialLowering {
constexpr std::uint16_t halve=1;       // GX divide-by-2 scale beyond a plain sampled replace
constexpr std::uint16_t compare=2;     // GX compare operation as a 256x gain mask
constexpr std::uint16_t finalClamp=4;  // clamp off on the last stage read as saturated (unbounded only)
constexpr std::uint16_t signedD=8;     // signed or >255 initial register as D, split add/subtract
constexpr std::uint16_t hoist=16;      // computed register moved first and kept in the buffer
constexpr std::uint16_t softCompare=32;// last-stage compare with gain 32 to leave a stage free
constexpr std::uint16_t passThrough=64;// clamp-off scaled channel passed through a second stage
// Sum rules (Alpha 7): only on a second planner pass, after the rules above refused.
constexpr std::uint16_t runningSum=128;     // PREV as D of a two-sided interpolation: two multiply-adds
constexpr std::uint16_t biasFold=256;       // PREV + one value - 1/2 as one ADD_SIGNED
constexpr std::uint16_t previousCapture=512;// PREV as D kept in the combiner buffer past the term
constexpr std::uint16_t subtractTerms=1024; // D minus one value (one SUBTRACT) or minus a blend
constexpr std::uint16_t sumRules=runningSum|biasFold|previousCapture|subtractTerms;
// Operation rules (Alpha 12): only on a third planner pass, after the sum rules refused.
constexpr std::uint16_t addHalf=2048;              // GX ADDHALF bias (+128/255): a folded literal D or an ADD of constant 128
constexpr std::uint16_t saturateIntermediate=4096; // clamp-off value read only as a saturating D, clamped early
constexpr std::uint16_t registerLiveness=8192;     // dead register writes dropped, a register value renamed onto PREV
constexpr std::uint16_t scaledRegister=16384;      // 2x/4x register value kept in PREVIOUS by the register route
// Alpha 13: one computed RGB register written by every stage from its first
// write to the stage before the last (a chain such as REG2 = T + K, then
// REG2 = REG2 + RASA; the Ordon spring floor MA04 with K1 = fog density),
// kept in PREVIOUS while PREV waits in the combiner buffer.
constexpr std::uint16_t registerChain=32768;
constexpr std::uint16_t operationRules=addHalf|saturateIntermediate|registerLiveness|scaledRegister|registerChain;
constexpr unsigned count=16;
}
// Plans with any of these bits took the one-stage stand-in in Alpha 6 (the
// first seven) or in Alpha 7 wave 1 (the sum rules).
struct PicaMaterialPlan {
    std::array<PicaMaterialStage,6> stages{};
    // Bind these map/coordinate pairs to PICA texture units in this order.
    std::array<MaterialTextureBinding,3> textures{};
    std::uint8_t stageCount{},textureCount{};
    std::array<std::uint8_t,4> bufferConstant{}; // Initial combiner buffer; usable after stage0.
    std::uint8_t bufferUpdateRgb{}; // Capture stage0..3 RGB; visible two stages later.
    std::uint8_t bufferUpdateAlpha{};
    std::array<std::uint8_t,2> rasterReadMask{}; // RGB=1, alpha=2, after dead-input elimination.
    bool projectiveTexture0{};
    bool quantizedConstants{};
    RasterCarrier rasterCarrier{};
    // Plans are compared as bytes (applied state filter in gx_pica.cpp, the
    // shape chain): no member may leave indeterminate padding behind it.
    std::uint8_t reserved{};
    std::uint16_t lowering{}; // MaterialLowering bits; deterministic per material state.
    unsigned sourceTextures()const{return rasterCarrier==RasterCarrier::raster0RgbaRaster1Lookup?textureCount-2:textureCount;}
};
enum class MaterialPlanError {
    none, stages, operation, indirect, source, uninitialized, constantRange,
    konstFraction, constantConflict, swap, textureUnits, equation
};
enum class MaterialPolicy { exactConstants, quantizedConstants };
// Diagnostics (Alpha 12): which rule refused when the error is
// MaterialPlanError::operation, one code per refusal site. none otherwise.
enum class MaterialOperationReason : std::uint8_t {
    none,
    clampOffSigned,   // clamp off with a signed step (ADD_SIGNED/SUBTRACT); PICA always clamps
    clampOffBound,    // clamp off and the unclamped value may leave [0,1]
    registerValue,    // intermediate register write that is not a copy of a stable value
    registerSplit,    // intermediate register needing a two-stage multiply-add or interpolation
    registerSubtract, // intermediate register written by a subtraction
    registerHalve,    // intermediate register written with the divide-by-2 scale
    registerBias,     // intermediate register written with the SUBHALF bias
    registerCompare,  // intermediate register written by a compare
    registerSignedD,  // intermediate register with a signed or >255 initial register as D
    registerPending,  // intermediate register value not recorded (planner invariant)
    halveExact,       // divide-by-2 scale under the exact-constant policy
    halveSubtract,    // divide-by-2 subtraction with a bias or clamp off
    halveBias,        // divide-by-2 with SUBHALF and a D or clamp off
    compareForm,      // compare EQ, GR16 or BGR24 (or an undefined encoding)
    compareClampOff,  // clamp-off compare whose C+D may exceed one
    signedDForm,      // signed/>255 initial D with a bias, subtraction, divide-by-2 or clamp off
    addHalf,          // GX ADDHALF bias (+0.5) the operation rules could not lower
    scaledRegister,   // 2x/4x/0.5x result written to an intermediate register
    invalidOperation, // undefined operation, bias, scale, clamp or register encoding
    subtractForm,     // subtraction with a bias or clamp off
    subHalfExact,     // SUBHALF bias under the exact-constant policy
    subHalfClampOff,  // SUBHALF bias with clamp off
    hoistBuffer,      // hoisted register could not take the combiner buffer
    count
};
// Common direct TEV equations, allocation-free. Unsupported operations reject.
// Initial signed register inputs A/B/C use GX low-byte wrapping. A signed or
// >255 initial register as D splits into a clamped add and subtract (clamp on,
// scale 1/2/4). Clamp off on the last stage equals the saturated output, as
// the aurora reference renders it (whether GX hardware wraps there instead is
// unverified). GX compare R8_GT/RGB8_GT (colour) and A8_GT (alpha) become a
// 256x gain mask that is exact for 8-bit operands; EQ and GR16/BGR24 forms
// still reject. One computed RGB register that reads nothing an earlier stage
// wrote may move to the first stage and live in the combiner buffer.
// Quantized policy additionally admits bounded fractions and the divide-by-2
// scale (128/255 and halved literals, at most 0.5/255 real error), and for a
// last-stage compare that would fill all six stages, gain 32 when literal C
// and D saturate from two levels up (a one-level difference gives D+C/8).
// Exact policy does not silently opt in. Unbounded computed values still reject.
// Real arithmetic agreement is not fixed-point GPU proof.
// The caller must bind the declared textures/UVs and supply correct raster color.
// Clears output on failure. No GPU calls, pixel reads or geometry processing.
// A material the rules above refuse is planned a second time with the sum
// rules (MaterialLowering::sumRules): PREV as D of a two-sided interpolation,
// PREV plus one value with the half bias, PREV kept in the combiner buffer
// past a halved or half-biased term, and subtraction of one value or a blend.
// Only plans that need one of them are kept, so a plan of the first pass
// never changes; a refusal reports the first pass's error.
bool planPicaMaterial(const MaterialState&,PicaMaterialPlan&,MaterialPlanError* = nullptr,MaterialPolicy = MaterialPolicy::exactConstants);
// Same, and on an operation refusal the rule that refused (none otherwise).
bool planPicaMaterial(const MaterialState&,PicaMaterialPlan&,MaterialPlanError*,MaterialPolicy,MaterialOperationReason*);
// Split a saturated PREV + register * texture final stage when its fourth
// texture exceeds the PICA limit. The caller must composite the contribution
// over the base before fog, with depth, alpha and geometry preserved.
bool planAdditiveTextureSplit(const MaterialState&,PicaMaterialPlan& base,
                              PicaMaterialPlan& contribution,MaterialPolicy);
// Development switch (sdmc:/3ds/dusklight/tev-sum-rules.txt, '0' = off):
// without the second pass the planner is the Alpha 7 wave 1 one. On by
// default; set once at boot, read by any thread.
void setMaterialSumRules(bool enabled);
bool materialSumRules();
// Third pass (Alpha 12), after both refused: the operation rules
// (MaterialLowering::operationRules). Needs the sum rules on; plans of the
// first two passes never change. On by default; set once at boot.
void setMaterialOperationRules(bool enabled);
bool materialOperationRules();
// The GX program rewrites of the third pass (dead writes dropped, register
// values renamed onto PREV, clamp-off intermediates saturated), to a fixed
// point; rules receives the MaterialLowering bits used. The rewritten program
// observes the same last-stage RGB and alpha as the original (tests).
// variant 0 renames the earliest values first, 1 the latest first (the
// third pass tries both).
constexpr unsigned lowerMaterialVariants=2;
void lowerMaterialProgram(const MaterialState&,MaterialState& lowered,std::uint16_t* rules=nullptr,unsigned variant=0);
// The third pass alone (diagnostics and tests): plans with an operation rule
// only; on refusal its own error and rule (error none: planned without one).
bool planMaterialOperationRules(const MaterialState&,PicaMaterialPlan&,MaterialPlanError*,MaterialPolicy,MaterialOperationReason*);
// Operation refusals of planPicaMaterial calls by rule (every call counts,
// including replans of one draw), and a report of the nonzero ones as
// "Native TEV operation refusals by rule (planner calls): add-half=N ..."
// lines for OSReport. Thread-safe counters.
void materialOperationRefusals(std::array<std::uint32_t,unsigned(MaterialOperationReason::count)>&);
void reportMaterialOperationRefusals(void(*sink)(const char*));
const char* materialOperationReasonText(MaterialOperationReason);
// Conservative semantic admission: both RGB streams, no raster alpha reads,
// identity raster swaps, no indirect. Never enables this route implicitly.
// All original stage boundaries, register allocation and constants use Builder.
bool planRasterCarrierMaterial(const MaterialState&,PicaMaterialPlan&,MaterialPlanError* = nullptr);
// Transport a second full RGBA stream through two identity lookup textures.
// At most one original texture and four original PICA stages; no preexisting
// combiner-buffer dependency. Original equations and fragment interpolation stay intact.
bool planRasterLookupMaterial(const MaterialState&,PicaMaterialPlan&,MaterialPlanError* = nullptr,MaterialPolicy = MaterialPolicy::exactConstants);
// PICA provides homogeneous texture sampling on unit 0 only. Route a single
// consumed projective binding there and rewrite every TEV source consistently.
// Multiple consumed projective bindings reject; the input stays intact on error.
bool routeProjectiveTexture(PicaMaterialPlan&,std::uint8_t projectiveCoordinates);
// Band alpha test (lower <= alpha <= upper) with one PICA comparison: the
// hardware tests the lower bound and three appended stages take the alpha to
// zero above the upper bound, over a ramp of the eight alpha levels below it,
// so those fragments fail the same test. The colour passes through. Needs
// three free stages and a final original stage the alpha buffer can capture
// (stage 0..3); the plan is unchanged on failure.
bool appendAlphaBand(PicaMaterialPlan&,std::uint8_t upper);
const char* materialPlanErrorText(MaterialPlanError);
}
