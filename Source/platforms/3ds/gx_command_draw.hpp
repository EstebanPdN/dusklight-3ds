#pragma once
#include "runtime/gx_alpha_interval.hpp"
#include <array>
#include <cstdint>
#include <span>
namespace dusk::native::gxworker {struct Adapter;}
namespace dusk::native::gx {class CommandProcessor;}
namespace dusk::native::gx { struct CommandDraw; struct PicaMaterialPlan; class CompactVertexCache; }
namespace dusk::native {
// Borrowed, bounded pre-submit report. This is an attempt, not a success event.
// Matrix/texgen source registers remain available in the borrowed CommandDraw.
struct SignedQVertexSample {
    unsigned index{},textureMatrix{};
    std::array<std::uint32_t,4> clipBits{};
    std::array<std::uint32_t,3> transformedBits{},gpuBits{}; // original S,T,Q; actual UQ,VQ,Q
};
struct SignedQDrawSample {
    unsigned vertices{},indices{},negativeVertices{},mixedTriangles{},coordinate{},map{};
    std::array<unsigned,4> target{}; // logical width/height, pixel width/height
    std::array<std::uint32_t,2> texgen{}; // original XF texgen/post register words
    unsigned samples{};
    std::array<SignedQVertexSample,6> vertex{}; // first mixed (else negative) triangle and next
};
}
namespace dusk::native {
enum class DrawFailureKind : unsigned { state, protocol, resource, geometry, submission, count };
// projectiveDivisor: a visible triangle of a projected texture has a vertex at
// or behind the projector plane (q <= 0). GX would mirror the image there;
// PICA cannot take a nonpositive divisor, so that vertex samples the texture
// far outside its range (its border under clamp) and the geometry is kept.
// alphaCompare: "alpha > a AND alpha < 255" (translucent-edge pass) is drawn
// with its lower bound only (see gx::relaxAlpha). Other pairs still reject.
// sharedIndirect: one texture is read by two stages of which only one warps
// its coordinates; a PICA unit has one coordinate set, so the warp is left out.
// indirectDivisor: a visible triangle has a warp coordinate at or behind its
// projector (the per-vertex warp is undefined there); the draw is translated
// again without the warp, unless its base texture is projected as well.
enum class DrawVisualFallback : unsigned { tevMaterial, clippedDepth, projectiveDivisor, alphaCompare, sharedIndirect, indirectDivisor, count };
struct SignedQPositiveRigidCounts {
    struct Total { std::uint64_t draws{},vertices{}; };
    // Fixed-size aggregates. Attempts/proofs are not GPU submissions.
    Total attempts{},planningFailures{},packingFailures{},proofPasses{},proofFailures{},submitted{};
};
struct CameraAlignedIndirectCounts {
    struct Total { std::uint64_t draws{},vertices{}; };
    // Cold exceptional attempts; proof passes are not submissions.
    Total attempts{},proofPasses{},proofFailures{},submitted{};
};
enum class CameraAlignedIndirectProofReason : std::uint32_t {
    none,decode,transform,baseNonfinite,baseZero,cameraMismatch,
    baseCoordinateNonfinite,sourceCoordinateNonfinite,sourceDivisor,
    indirectEvaluation,outputNonfinite
};
struct CameraAlignedIndirectProofFailure {
    CameraAlignedIndirectProofReason reason{};
    std::uint32_t index=UINT32_MAX,unit=UINT32_MAX;
    std::uint32_t validBits{}; // bit0: baseQ/clipW; bit1: effective sourceQ.
    std::uint32_t baseQBits{},clipWBits{},sourceQBits{};
    std::uint32_t transformError{}; // VertexTransformError only for transform.
};
struct CommandDrawContext {
    const char* failure="none";
    std::uint64_t submitted{},culled{},rejected{};
    // Optional diagnostic observer. The draw is borrowed only for this call;
    // the observer must not mutate GX state or retain any resource pointer.
    void (*reportFailure)(const gx::CommandDraw&,const char*){};
    // DEFAULT OFF. Perspective ordinary DIRECT projective texture0 only.
    // Mixed q/internal poles are intentionally preserved: diagnostic risk.
    bool allowDirectSignedQDiagnostic{};
    // DEFAULT OFF. Let otherwise-supported diagnostic draws attempt the
    // unchanged strict positive-q rigid proof. Negative q stays CPU-only.
    bool allowSignedQPositiveRigidGpu{};
    SignedQPositiveRigidCounts signedQPositiveRigid{};
    std::uint64_t signedQNegativeDraws{},signedQMixedDraws{},signedQReports{};
    // At most four calls per context lifetime, only after complete validation.
    // Must not mutate state, retain borrowed pointers or reset report budget.
    void (*reportSignedQ)(const gx::CommandDraw&,const gx::PicaMaterialPlan&,const SignedQDrawSample&){};
    // Host-tested diagnostic only. Native primary RGB/A transport is unproved.
    bool diagnosticRasterCarrier{}; // default OFF; startup opt-in is parent work.
    std::uint64_t rasterCarrierAttempts{},rasterCarrierDraws{},rasterCarrierRejects{};
    const char* rasterCarrierFirstFailure{}; // first attempt failure, stable literal
    std::uint32_t rasterCarrierFirstVertex=UINT32_MAX; // no vertex failure yet
    bool allowRigidGpu{};
    // gx_rigid.v.pica (single texture): the console PICA stopped on every
    // draw of it (gpu-hang 2026-11-13, CIA g, h and i). Off by default since
    // 2026-11-14: the multi-texture shader, which always completed on the
    // console, plans the same draws (projected unit included). SD file
    // rigid-single-on.txt turns it on for an A/B test of its repair.
    bool allowSingleRigidShader{};
    // Multi-texture shader with unit 0 projected from the position (terrain
    // with a projected shadow or light map). Off after that route stopped the
    // console PICA (the guard records it) or with SD file mt-projected-off.txt:
    // those draws then take the CPU vertex route.
    bool allowMultiProjected{true};
    // CPU vertex route, projected texture on unit 0: sample it in the PICA
    // projection mode (per-pixel divide). Off (SD file projection-mode-off.txt)
    // divides per vertex on the CPU instead, which is wrong for textures
    // mapped in screen space.
    bool allowProjectionMode{true};
    // GPU vertex route, warped unit 0 projected through the camera and
    // divided per pixel: keep that division when vertices lie behind the
    // camera plane (gx::projectedUnitFollowsCamera; the PICA clips the
    // triangle before any pixel divides). Off (SD file
    // camera-divisor-off.txt): such a draw divides per vertex with a clamped
    // divisor, as in Alpha 8, which samples the image's border beside the
    // camera.
    bool allowCameraDivisor{true};
    // A draw whose compact records were decoded into the frame arena returns
    // the part of its reservation they do not fill (gx_command_draw.cpp).
    // compactReservationBytes: bytes returned so far.
    bool shrinkCompactReservations{true};
    std::uint64_t compactReservationBytes{};
    gx::CompactVertexCache* compactCache{};
    bool allowBehindCameraCull{};
    std::uint64_t cameraCulled{};
    // Complete finite-position/topology side proof after normal length overflow.
    // No submission or degraded skip; increment only after reservation cancellation.
    std::uint64_t normalOverflowCulled{};
    bool allowRgba6Storage{};
    bool allowQuantizedConstants{};
    std::uint64_t quantizedDraws{};
    bool allowVertexFog{};
    bool allowTriangleFog{};
    std::uint64_t triangleFogDraws{};
    std::uint64_t vertexFogDraws{};
    // Draws whose fog curve went to the ramp texture instead of the primary
    // alpha (preferFogRampForAlpha).
    std::uint64_t fogAlphaRampDraws{};
    bool allowVertexIndirect{};
    // DEFAULT OFF, experimental. ACTIVATION HOLD (review107): fp32 alignment
    // does not prove float24 transport/visible-W margin/sampler ratio bounds.
    // CPU indirect/projected final texture0 only.
    // A cold proof must establish bit-exact base q==clip.w for EVERY original
    // vertex and finite positive effective selected source q before signed q.
    // Root owns activation/reporting. No direct diagnostic or rigid policy change.
    bool allowCameraAlignedIndirectQ{};
    // DEFAULT OFF. Complete original proof and CPU-only canonical clipping;
    // original signed Q is never submitted. Output remains positiveOnly.
    bool allowCameraAlignedCpuClip{};
    std::uint64_t cameraClipAttempts{},cameraClipDraws{},cameraClipCulled{},cameraClipKernelRejected{};
    std::uint64_t cameraClipOriginalVertices{},cameraClipOutputVertices{},cameraClipOutputIndices{};
    std::uint32_t cameraClipFirstError{};
    std::uint32_t cameraClipFirstVertex=UINT32_MAX,cameraClipFirstTriangle=UINT32_MAX;
    // DEFAULT TRUE: observe proof only; NEVER select signed policy or retry.
    // Root58 must keep true with no SD switch. False is private host prototype
    // admission only while HIGH107 numerical-domain proof remains unresolved.
    bool cameraAlignedIndirectProofOnly=true;
    CameraAlignedIndirectCounts cameraAlignedIndirect{};
    // First cold proof failure per context lifetime; root reports at checkpoints.
    // No per-vertex logging. A later proof never overwrites this attribution.
    CameraAlignedIndirectProofFailure cameraAlignedIndirectFirstFailure{};
    std::uint64_t vertexIndirectDraws{};
    bool allowConstantEarlyDepth{};
    std::uint64_t constantEarlyDepthDraws{};
    bool allowAlphaUnion{};
    std::uint64_t alphaUnionDraws{};
    gx::AlphaIntervalPolicy alphaIntervalPolicy{}; // default-off; see ownership contract.
    std::uint64_t alphaIntervalDraws{};
    // Band alpha tests drawn with the lower bound on the hardware test and
    // the upper bound folded into the combiner alpha (gx::appendAlphaBand).
    std::uint64_t alphaBandDraws{};
    // Vertices left on the CPU path, by the first reason the GPU shader path
    // was not usable: alpha-union/interval, triangle-fog, fog-red, line/point,
    // clip-disabled, indirect, untextured, multitexture, coordinate, rigid-plan.
    std::array<std::uint64_t,10> cpuVertexReasons{};
    // Enables only the two classified approximations below. All failures stay
    // rejected. No unsupported draw is silently skipped. By default only
    // perspective draws qualify, protecting orthographic UI/text passes.
    bool allowDegradedDraws{};
    // Two further approximations, each opted into separately on top of the
    // master switch and the per-pass policy; off keeps the strict rejection.
    // Divisor: a visible triangle with a nonpositive projected-texture divisor
    // keeps its geometry and samples the clamped border at those vertices.
    // Alpha: `lower bound AND not fully opaque` keeps only the lower bound.
    bool allowProjectiveDivisorFallback{};
    bool allowAlphaCompareFallback{};
    // Warp: a mesh whose warp cannot be evaluated at a visible vertex is
    // drawn without the warp (only when its base texture is not projected).
    bool allowIndirectDivisorFallback{};
    // Fog factor through the ramp texture instead of the red channel when a
    // texture unit is free: the red route exists only on the CPU vertex path.
    bool preferFogRamp{};
    // The same for a factor carried in the primary alpha when the fog curve
    // is not the linear one (the GPU vertex route makes only that one).
    bool preferFogRampForAlpha{};
    // Game policy: a draw this translator cannot express (unsupported GX
    // state, invalid vertex data, a texture it cannot resolve) is left out and
    // counted in rejected and skippedDraws instead of ending the stream. Its
    // geometry reservation is already cancelled at that point. Failed
    // cancellations and submission failures stay fatal. Off: strict rejection.
    bool skipUnsupportedDraws{};
    // Optional per-pass policy, consulted after the master switch. A caller
    // can explicitly admit a validated orthographic pass or refuse a 3D pass.
    bool (*allowVisualFallback)(const gx::CommandDraw&,DrawVisualFallback){};
    DrawFailureKind failureKind{DrawFailureKind::state};
    std::array<std::uint64_t,unsigned(DrawFailureKind::count)> rejectedByKind{};
    std::uint64_t simplifiedDraws{},skippedDraws{}; // skippedDraws: only with skipUnsupportedDraws.
    // Rejections by reason (static strings). `reported` is cleared by the
    // owner (each test zone) so the first draw of each reason is described
    // again; `mark` is the owner's count at its last report.
    struct RejectReason { const char* why; std::uint64_t draws,vertices,mark; bool reported; };
    std::array<RejectReason,24> rejectReasons{};
    unsigned rejectReasonCount{};
    // Successful submissions only; callers can difference/reset these at frame
    // and scene boundaries. Rejected attempts never inflate visual acceptance.
    std::array<std::uint64_t,unsigned(DrawVisualFallback::count)> simplifiedByReason{};
    // Successful homogeneous side-plane clipping of primitives with invalid
    // base texture or indirect source divisors. Surviving attributes are exact.
    // Exact clipping only; excluded from skipped/simplified draw counts.
    std::uint64_t projectiveClipDraws{},projectiveClipTriangles{};
    // Successful ordinary depth-clip approximation after strict full-batch Q proof.
    std::uint64_t positiveQDepthFallbackDraws{},positiveQDepthFallbackVertices{};
    // Successful exact Z-transport normalization; never a visual fallback/skip.
    std::uint64_t depthlessOrthoDraws{},depthlessOrthoVertices{};
    // DEFAULT OFF. Exact byte-domain nonzero nearest/clamped I4 quarter bake;
    // ordinary orthographic CPU draws only. Physical PICA remains unverified.
    bool allowExactI4Bake{};
    std::uint64_t exactI4Draws{},exactI4Vertices{};
    unsigned exactI4CpuImages{}; // Published CPU resources, not GPU residency.

};
// Install explicitly as CommandProcessor's synchronous sink, supplying a live
// context. Requires an open indexed GPU frame and registered typed GX map
// bindings. Raw BP image-address resources remain explicit rejection.
bool drawGxCommand(const gx::CommandDraw&,void* context);
void preparedDrawStatistics(std::uint64_t& hits,std::uint64_t& misses,std::uint64_t& stores,std::uint64_t& fallbacks);
void nativeShapeStatistics(std::uint64_t& hits,std::uint64_t& misses,std::uint64_t& records,std::uint64_t& keyMisses);
// Constant refreshes/refusals of recorded materials and, for key misses on
// profiled frames, the key component that differed (bp, texture, cp, xf,
// matrix index, target, no record).
void nativeShapeDiagnostics(std::uint64_t& refreshes,std::uint64_t& refusals,std::array<std::uint64_t,7>& missParts);
// Same-state continuations of replayed shapes and reuses of the cached key
// components; the switch disables continuation for an A/B comparison.
void nativeShapeChainStatistics(std::uint64_t& chained,std::uint64_t& keyCacheHits);
void setNativeShapeChain(bool enabled);
// Lean shape replay (multi-texture records, exact; on by default): the clip
// projection and the fog plan come from caches that hold while their
// register words hold, the texgens are checked without a transform plan and
// the uniform plan is built in one reused storage; continuations keep only
// the unit rows. The switch is an A/B (SD file native-lean-replay.txt, '0').
void setNativeLeanReplay(bool enabled);
// Cumulative, printed on profiled frames as "Native shape replay": lean
// plans, lean replays refused at the plan step, replays planned the complete
// way, continuations served from the kept unit rows, and the projection and
// fog cache lookups by outcome (same state epoch, words compared equal,
// planned again).
struct NativeLeanReplayStatistics {
    std::uint64_t leanPlans{},leanRefusals{},fullPlans{},rowContinuations{};
    std::uint64_t projectionEpochHits{},projectionEqualHits{},projectionPlanned{};
    std::uint64_t fogEpochHits{},fogEqualHits{},fogPlanned{};
};
void nativeLeanReplayStatistics(NativeLeanReplayStatistics& out);
// Why static batches were or were not recorded (see nativeRecordOutcome).
void nativeShapeRecordOutcomes(std::array<std::uint64_t,13>& outcomes);
// Profiled-frame 2D draws: total, with only zero vertex alpha, with standard alpha blending.
void native2dStatistics(std::uint64_t& draws,std::uint64_t& zeroAlpha,std::uint64_t& alphaBlend);
// Immediate draw records (HUD, particles): replays, first sightings, records
// made and replays that fell back to the complete path. The switch is an A/B.
void immediateDrawStatistics(std::uint64_t& hits,std::uint64_t& misses,std::uint64_t& stores,std::uint64_t& fallbacks);
void setImmediateDrawRecords(bool enabled);
// Cumulative, printed on profiled frames as "Native immediate draw detail".
// repeats: replays that reused the previous replay's uniform block and
// vertices (inputs unchanged); keyMemoHits: lookups that reused the last key
// (state epoch unchanged); evictions: records replaced while in use; misses
// by cause: no record of the state key, a record whose other extra state
// differs, one that differs in colours only (TEV/konst/fog/channel colours);
// zeroVertexAlpha: replays whose every vertex alpha is zero under a
// vertex-alpha channel (hidden panes still drawn).
struct ImmediateDrawDetail {
    std::uint64_t repeats{},keyMemoHits{},evictions{},missState{},missExtra{},missColor{},zeroVertexAlpha{};
};
void immediateDrawDetail(ImmediateDrawDetail& detail);
// A/B switch for the repeats and the key memo (both exact, on by default).
void setImmediateDrawShortcuts(bool enabled);
// Shape records that keep a deterministic visual fallback (the TEV stand-in,
// the relaxed dual alpha test): their pictures are those of the complete
// path, which drew them every frame before 2026-11-16. A/B switch, on by
// default (SD file fallback-records.txt, '0' refuses them as before).
void setNativeFallbackRecords(bool enabled);
// Second-core worker (runtime/gx_worker.hpp): the adapter's entry point for
// the queue.
gxworker::Adapter nativeWorkerAdapter();
// The processor queued display lists run on (its own batch scratch; the game
// processor's memory registry, array resolver, topology cache and sink).
void setNativeListProcessor(gx::CommandProcessor* processor);
// gxWorkerJobHook (runtime/gx_worker_sync.hpp).
bool queueNativeJob(unsigned job,const void* arguments,std::size_t bytes);
// gxWorkerSectionHook (runtime/gx_worker_sync.hpp), its SD switch
// (gx-sections-off.txt) and its counts: sections queued, sections whose
// condition failed and the shapes and runs left out with them.
bool nativeSection(unsigned condition);
void setNativeSections(bool on);
void nativeSectionStatistics(std::uint64_t& queued,std::uint64_t& skipped,std::uint64_t& shapesSkipped,std::uint64_t& runsDropped);
// Late runs (duskNativeDeferRuns(2), SD switch gx-late-runs-off.txt).
void setNativeLateRuns(bool on);
// The frame loop opens the queue before the game's frame and the painter's
// end closes it (duskNativePainterDone): a shape issued after the painter
// (none is expected) is drawn at once, on the game thread, as with the
// worker off. Open from the start, for a frame without a painter.
void setNativePainterOpen(bool open);
// Alpha 15 (gx-overlap.txt 2): runs stay deferred through the bloom and the
// 2D passes until the painter's end (duskNativeDeferRuns(0) is then ignored).
void setNativeLate2D(bool on);
// Shapes queued, those drawn through their display list, lists not drawn.
void nativeWorkerStatistics(std::uint64_t& queued,std::uint64_t& lists,std::uint64_t& failures);
// The run deferral of the game thread's processor (context: that processor):
// see CommandProcessor::setRunDeferral and duskNativeDeferRuns.
bool deferNativeRun(std::span<const std::uint8_t> run,bool arraysFixed,void* context);
// Primitive runs queued, and runs kept on the game thread because their
// arrays could change.
void nativeRunStatistics(std::uint64_t& queued,std::uint64_t& refusedForArrays);
// Palette split: lit multi-texture draws whose matrices overflow the palette
// are drawn as consecutive runs of their triangles, each with its own
// palette, instead of on the CPU vertex route. On by default (SD file
// palette-split.txt, '0' = the CPU route as before).
void setPaletteSplit(bool enabled);
// Vertex formats with one attribute (position only) on the multi-texture GPU
// route (gx::gxCompactSingleAttribute). On by default (SD file
// mt-single-attribute.txt, '0' = the CPU vertex route as before).
void setCompactSingleAttribute(bool enabled);
// Cumulative, printed on profiled frames as "Native fallback records" and
// "Native draw gaps": fallback records stored, replays of a stand-in and of a
// relaxed alpha test, stand-in decisions repeated on new TEV colours and those
// refused; split draws, their runs, and splits refused (CPU route).
struct DrawGapStatistics {
    std::uint64_t fallbackStored{},standInReplays{},relaxedAlphaReplays{},standInRefreshes{},standInRefusals{};
    std::uint64_t paletteSplitDraws{},paletteSplitRuns{},paletteSplitRefusals{};
};
void drawGapStatistics(DrawGapStatistics& out);
// Triangle-fog draws submitted by the GPU rigid path (two draws per mesh).
void setRigidTriangleFog(bool enabled); // development switch, default off (never exercised)
// gates: why others stayed on the CPU (0 restarted or switched off, 1 unavailable, 2 fog
// function, 3 fog parameters/orthographic, 4 projected texture, 5 texture
// count, 6 other policy, 7 rigid plan rejected).
std::uint64_t rigidTriangleFogStatistics(std::array<std::uint64_t,8>* gates=nullptr);
}
