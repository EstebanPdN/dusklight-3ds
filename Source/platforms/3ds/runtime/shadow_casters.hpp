#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
namespace dusk::native {
enum class ShadowCasterOwner : std::uint8_t { other, link, shared };
// Native model shadows. A shadow caster is not drawn a second time through GX:
// while the scene draws a tagged model, each of its PICA draws (vertex buffer
// already on the GPU, matrix palette already built) is remembered. The caster
// pass replays those draws white into the tile of the shadow image, replacing
// only the projection rows of their uniform block by camera space -> tile.
struct ShadowCasterView {
    bool used{};
    ShadowCasterOwner owner{}; // Written only with GPU shadow profiling enabled.
    // Camera view space -> tile-local (s,t) in [0,1]; GX orientation, t down.
    std::array<std::array<float,4>,2> st{};
    unsigned x{},y{},size{}; // tile in image pixels
};
// Start remembering (once per frame, before the scene draws its casters).
void armGxShadowCasters();
// Render target is the resident texture of the copy image `imageIdentity`
// (width x height, RGBA8). views[slot-1] is the caster selected with slot.
// False: nothing usable was drawn (the image keeps its previous content).
bool drawGxShadowCasters(const void* imageIdentity,unsigned width,unsigned height,std::span<const ShadowCasterView> views);
struct ShadowCasterStatistics {
    unsigned remembered{},dropped{},passes{};
    unsigned retained{},retainedReplays{},reclaimedReplays{},retainFailed{},publishFailed{};
};
ShadowCasterStatistics takeGxShadowCasterStatistics();
// The caster selected last (duskNativeShadowCaster): what a shape issued now
// belongs to.
unsigned gxShadowCasterSlot();
// The thread that draws a queued shape: the caster the shape was issued for
// (decided then; the pass may be armed or over by now).
void setGxShadowCasterSlot(unsigned slot);
// The caster pass as a queued job (gx_worker_sync.hpp, gxJobShadowCasters):
// run by the thread that draws, with the arguments drawGxShadowCasters queued.
void runGxShadowCasterJob(const void* arguments,std::size_t bytes);
// The start of a caster list as a queued job (gxJobShadowArm): the thread that
// draws forgets the casters it remembered (armGxShadowCasters queues it).
void runGxShadowArmJob();
// armGxShadowCasters may queue the start (SD switch gx-shadow-arm-job-off.txt).
void setGxShadowArmJobs(bool on);
// Passes queued as jobs, and queued passes that then drew nothing usable
// (the next ones are run by the game thread again until one succeeds).
void gxShadowCasterJobStatistics(std::uint64_t& queued,std::uint64_t& failed);
}
// Selects the caster the following rigid draws belong to (0 = none).
extern "C" void duskNativeShadowCaster(unsigned slot);
