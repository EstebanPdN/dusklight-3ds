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
//     must complete (the "canary"); later draws of that signature are batched.
//     Completed signatures are kept in sdmc:/3ds/DuskLight/gpu-verified.bin
//     (the most recent 8192 are loaded) and never run alone again in a
//     session; SD switch gpu-canary-coarse.txt also treats a draw that differs
//     from a completed one only in fragment state as known;
//   - a canary runs only after every job queued before it completed, and a
//     stop is blamed on it only when its own list is the job that went silent;
//     the SD journal names it only while its own list can run (a console that
//     freezes there learns it on the next start);
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
// A learned hang of a variant of the multi-texture program (kind 4,
// features::Id::vertexVariants): the general program draws their plans.
bool gpuCanaryVertexVariantsRefused();
// Transfer-engine stops recorded with this build: 1 = no transfer jobs inside
// a frame (GPU-resident captures and bloom off), 2 = no offscreen targets.
unsigned gpuCanaryTransferLevel();
// A command list that drew into the model-shadow image stopped the PICA with
// this build: blob shadows only on this start (no caster pass).
bool gpuCanaryShadowRefused();
// A command list with the HUD tape's 2D pass (recorded or replayed) stopped
// the PICA with this build: the pass is drawn in every picture on this start.
bool gpuCanaryHudTapeRefused();
// hud_tape_ctru.cpp: the command list queued next carries the 2D pass.
void gpuCanaryHudTapeList();
// gx_pica.cpp: physical address of the shadow image before each caster pass,
// so a stop can be traced to the pass.
void gpuCanaryShadowTarget(std::uint32_t physicalAddress);
// gx_pica.cpp: around the model-shadow pass; lists queued meanwhile belong to it.
void gpuCanaryShadowPass(bool active);
bool gpuCanaryPressA();
void gpuCanaryWorldEntered();
// Main thread, after every frame: closes the journal of the draws that ran
// alone in it and, every 60 frames, records that the session is alive.
void gpuCanaryFrameEnd(unsigned long long frame,const char* stage);
// SD writer thread (main.cpp): writes the session record gpuCanaryFrameEnd
// prepared, unless a final record (fatal, hang) was written meanwhile.
void gpuCanaryWriteSessionRecord();
// The PICA state rebuilt from every command sent: 0x300 registers and the 96
// float uniform rows as uploaded (application dumps include both).
void gpuCanaryShadowState(const std::uint32_t*& registers,std::size_t& registerWords,const std::uint32_t*& uniformRows,std::size_t& uniformWords);
// Development: one log line per draw of the next whole picture (its place in
// the picture, signature, index count and a sum of the float uniforms it
// runs with; no addresses, so two runs of the same game frame can be
// compared line by line). Asked for between two pictures.
void gpuCanaryTraceNextPicture();
// Main thread, after the game loop ended normally.
void gpuCanarySessionEnd();
// A fatal error or CPU exception ends this start (any thread, never fails):
// the session record says so, and the next start writes a note naming it
// instead of "switched off, or frozen". No GSP or GPU call.
void gpuCanarySessionFatal(const char* text);
// How the start before this one ended, from the session record it left
// (after initializeGpuCanary): it exited, it ended on a fatal error or CPU
// exception, the PICA stopped (a recorded hang, or a freeze on a draw that
// ran alone), or it never ended (switched off while playing, or frozen
// outside a draw: the record cannot tell). unknown: no record (first start,
// or the guard was off). fontOom: confirmed font-pane allocation failure with
// an idle GPU journal.
enum class GpuPreviousEnd:std::uint8_t {unknown,clean,fatal,gpuStop,unfinished,fontOom};
GpuPreviousEnd gpuCanaryPreviousEnd();
// A word of up to 15 letters carried by every session record written from
// now on, and whether the previous start's record carried it: a feature
// that may be to blame for a bad end marks the starts it was on in (the
// second-core worker, gx_worker_runtime.hpp).
void gpuCanarySessionWord(const char* word);
bool gpuCanaryPreviousSessionWord(const char* word);
}
