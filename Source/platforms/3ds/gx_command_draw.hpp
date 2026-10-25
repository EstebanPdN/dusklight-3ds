#pragma once
#include "runtime/gx_alpha_interval.hpp"
#include <array>
#include <cstdint>
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
enum class DrawVisualFallback : unsigned { tevMaterial, clippedDepth, count };
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
    // Vertices left on the CPU path, by the first reason the GPU shader path
    // was not usable: alpha-union/interval, triangle-fog, fog-red, line/point,
    // clip-disabled, indirect, untextured, multitexture, coordinate, rigid-plan.
    std::array<std::uint64_t,10> cpuVertexReasons{};
    // Enables only the two classified approximations below. All failures stay
    // rejected. No unsupported draw is silently skipped. By default only
    // perspective draws qualify, protecting orthographic UI/text passes.
    bool allowDegradedDraws{};
    // Optional per-pass policy, consulted after the master switch. A caller
    // can explicitly admit a validated orthographic pass or refuse a 3D pass.
    bool (*allowVisualFallback)(const gx::CommandDraw&,DrawVisualFallback){};
    DrawFailureKind failureKind{DrawFailureKind::state};
    std::array<std::uint64_t,unsigned(DrawFailureKind::count)> rejectedByKind{};
    std::uint64_t simplifiedDraws{},skippedDraws{}; // skippedDraws stays zero.
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
}
