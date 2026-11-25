#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace dusk::native {
// Native bloom. The GameCube filter copies the EFB five times and blurs with
// eight TEV stages and eight texture coordinates. Here the transfer engine
// copies the frame once into a texture, a quad keeps what is brighter than
// the threshold in a 64x128 target, a three-tap quad blurs that into a 32x64
// target, and one bilinear quad adds the result over the frame. Everything
// stays on the GPU. The monochrome filter of the same original function (the
// frame's red channel times a colour, blended over the frame) is one more
// quad from the same copy.
struct BloomSettings {
    bool glow{};           // the bloom itself
    unsigned point{};      // threshold, 0..255
    unsigned blurSize{};   // tap radius, original units (pixels * 16)
    unsigned blurRatio{};  // tap weight, 0..255 (eight taps in the original)
    unsigned mode{};       // 1 = screen blend, 0 = additive
    std::array<std::uint8_t,4> blend{}; // colour of the glow; alpha scales the frame under it
    std::array<std::uint8_t,4> mono{};  // monochrome filter colour; alpha 0 = no filter
    // Heat haze: the frame copy drawn back over the frame through a grid whose
    // texture coordinates sway (the original warps the copy with an indirect
    // texture, which the PICA does not have).
    unsigned haze{};        // opacity 0..255; 0 = no haze
    float hazeAmplitude{};  // largest shift, as a fraction of the screen
    unsigned hazePhase{};   // frame counter driving the sway
};
// Only while the main framebuffer is the active target. False: nothing drawn.
// With the second-core worker the pass is a queued job once its surfaces
// exist (gx_worker_sync.hpp gxJobBloom): true then means it was queued.
bool drawGxBloom(const BloomSettings&);
// The queued pass, run by the thread that draws (gx_command_draw.cpp).
void runGxBloomJob(const void* arguments,std::size_t bytes);
// drawGxBloom may queue (SD switch gx-bloom-job-off.txt); passes queued and
// queued passes that drew nothing.
void setGxBloomJobs(bool on);
void gxBloomJobStatistics(std::uint64_t& queued,std::uint64_t& failed);
}
