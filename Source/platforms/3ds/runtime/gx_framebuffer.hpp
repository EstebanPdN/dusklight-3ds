#pragma once
#include "gx_target_extent.hpp"
#include <cstddef>
#include <span>
namespace dusk::native {
using FramebufferHandle=void*;
FramebufferHandle platformCreateFramebuffer(unsigned pixelWidth,unsigned pixelHeight);
bool platformBindFramebuffer(FramebufferHandle);
bool platformClearFramebuffer(FramebufferHandle,std::uint32_t color,std::uint32_t depth);
bool platformReadFramebufferColor(FramebufferHandle,std::span<const std::byte>&);
bool platformReadFramebufferDepth(FramebufferHandle,unsigned logicalX,unsigned logicalY,std::uint32_t&);
bool platformReadFramebufferDepthImage(FramebufferHandle,std::span<const std::byte>&);
// Private opt-in diagnostic only. A borrows the existing completed depth
// readback; B is one independent, bounded linear allocation. Retention blocks
// ALL depth readback reuse until the caller finishes its final observation.
inline constexpr std::size_t retainedDepthProbeBytes=400*240*4;
struct DepthProbeRange {std::uintptr_t address{},physical{};std::size_t bytes{},capacity{};};
struct DepthProbeState {
    std::uint64_t tick{};std::uintptr_t commands{},caller{},owner{};
    std::uint32_t commandWords{},commandOffset{};
};
struct RetainedDepthProbe {
    FramebufferHandle target{};
    std::uint64_t targetGeneration{}; // Nonzero only under interval ownership tracking.
    DepthProbeRange a{},b{},depth{},color{},bottom{};
    std::span<const std::byte> imageA{},imageB{};
};
struct DepthProbeCopy {
    DepthProbeState beforePrior{},beforeCopy{},afterCopy{},afterWait{};
    int priorCompletion{-1},copyCompletion{-1},invalidateResult{-1};
};
DepthProbeState platformDepthProbeState(); // Public CPU buffer observations only.
bool platformRetainDepthReadback(FramebufferHandle,std::span<const std::byte>,RetainedDepthProbe&);
bool platformCaptureRetainedDepthB(RetainedDepthProbe&,DepthProbeCopy&);
int platformInvalidateRetainedDepthA(); // Separately labeled FINAL observation.
bool platformReleaseRetainedDepth(); // Does not free either live allocation.
bool initializeFramebuffers(FramebufferHandle,unsigned pixelWidth,unsigned pixelHeight);
bool beginFramebufferFrame();
// Outer frame begin retains previous depth for the game's queued peeks. The
// renderer must cross this boundary before its first new draw, after peeks and
// after the previous C3D_FrameEnd presentation. A full explicit render clear
// supersedes the queued display-copy clear at the same boundary.
bool beginFramebufferRender();
bool clearFramebufferForRender(std::uint32_t color,std::uint32_t depth);
bool endFramebufferFrame();
bool configureMainFramebuffer(unsigned logicalWidth,unsigned logicalHeight);
bool beginOffscreenFramebuffer(unsigned width,unsigned height);
bool restoreMainFramebuffer();
FramebufferHandle activeFramebuffer();
std::size_t offscreenFramebufferBytes();
// C3D_Fini owns destruction after completion; discard borrowed handles afterward.
void releaseFramebufferRecords();
void releaseFramebufferReadback();
void releaseCopyImages(); // While completed C3D textures can still be destroyed.
bool deferMainFramebufferClear(std::uint32_t color,std::uint32_t depth);
FramebufferHandle mainFramebuffer();
bool sampleFramebufferDepthImage(const gx::TargetExtent&,std::span<const std::byte>,
                                 unsigned logicalX,unsigned logicalY,std::uint32_t&);
struct FramebufferDepthQuery {std::uint16_t x{},y{};std::uint32_t* output{};};
inline constexpr std::size_t maxFramebufferDepthQueries=64;
// Synchronous, bounded queue drain: finish commands, capture once, gather all
// outputs, then discard the borrowed snapshot before returning. No draw/copy/
// clear/target switch or caller callback can occur inside this batch.
void peekFramebufferDepthBatch(std::span<const FramebufferDepthQuery>);
}
