#pragma once
#include <cstddef>
#include <cstdint>
// Console GPU guard. The full renderer was validated only in Azahar, which
// executes every PICA job at once and never stops on one. On a New 3DS a draw
// the PICA does not finish used to end the game with no way to tell which
// draw it was. This guard makes the console find out by itself:
//   - every draw is parsed as it is emitted (shadow PICA registers) and gets a
//     signature of its vertex format, shader, textures and raster state;
//   - the first draw of each signature runs alone in its own command list and
//     must complete (the "canary"); later draws of that signature are batched;
//   - a job silent for six seconds is a hang: the draw, the PICA registers and
//     its vertex data are saved to the SD, its signature is added to
//     sdmc:/3ds/DuskLight/gpu-skip.txt and the game starts again;
//   - draws with a learned signature are left out (their state still applies),
//     so each start gets further and the files say exactly what to fix.
namespace dusk::native {
// Main thread, once the SD card is readable and before the first game frame.
void initializeGpuCanary();
// One line with the counters of this session (profiled frames).
void reportGpuCanary();
// Queue waits (gpu_sync_ctru.cpp): a job in flight that has not signalled
// within the patience window. Saves the evidence and restarts; never returns.
[[noreturn]] void gpuHang(const std::uint32_t entryWords[8],unsigned index,unsigned count,unsigned waitedMilliseconds);
// A job that completed after the ordinary timeout (slow, not stopped).
void gpuSlowJob(const std::uint32_t entryWords[8],unsigned waitedMilliseconds);
// True after a restart caused by a learned hang, until the world is entered:
// the pad presses A by itself to pass the warning screens and logos.
bool gpuCanaryAutoAdvance();
// A learned hang used the single-texture rigid shader program: route its
// draws through the multi-texture shader or the CPU path on this start.
bool gpuCanaryRigidSingleRefused();
// The same for the multi-texture program with a unit projected from the
// position: those draws take the CPU vertex route on this start.
bool gpuCanaryMultiProjectedRefused();
// Transfer-engine stops recorded with this build: 1 = no transfer jobs inside
// a frame (GPU-resident captures and bloom off), 2 = no offscreen targets.
unsigned gpuCanaryTransferLevel();
bool gpuCanaryPressA();
void gpuCanaryWorldEntered();
// Main thread, after every frame: closes the journal of the draws that ran
// alone in it and, every 60 frames, records that the session is alive.
void gpuCanaryFrameEnd(unsigned long long frame,const char* stage);
// The PICA state rebuilt from every command sent: 0x300 registers and the 96
// float uniform rows as uploaded (application dumps include both).
void gpuCanaryShadowState(const std::uint32_t*& registers,std::size_t& registerWords,const std::uint32_t*& uniformRows,std::size_t& uniformWords);
// Main thread, after the game loop ended normally.
void gpuCanarySessionEnd();
}
