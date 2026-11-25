#pragma once
#include <cstdint>
#include "shadow_gpu_tags.h"
struct OSThread;

namespace dusk::native {
enum class GpuCompletion { complete, notReady, wrongThread, timedOut, exhausted };
// Flush queued PICA commands and wait on the actual bound Citro3D GX queue.
// The render owner retains command, vertex and texture storage through this wait.
// Waits are counted per reason; `other` callers are also counted per call
// site (return address, for addr2line) in a periodic "Native GPU completes" line.
enum class GpuCompleteReason : unsigned { other, reclaim, canary, count };
GpuCompletion platformCompleteGpu(std::uint64_t timeoutNanoseconds);
GpuCompletion platformCompleteGpuFor(GpuCompleteReason reason,std::uint64_t timeoutNanoseconds);
// Native frame boundaries serialize against the original GSP queue callback.
// Queue waits are bounded; FrameSync retains the SDK's VBlank synchronization.
bool platformBeginGpuFrame(std::uint8_t flags);
bool platformEndGpuFrame(std::uint8_t flags);
// Capture the public GPU command span immediately after C3D_FrameBegin.
bool platformBeginGpuCommands();
// Keep a conservative complete-draw reserve. Reclaim only after real completion.
bool platformPrepareGpuDraw();
// Reserve consecutive public GX entries before a compound SDK operation. This
// drains/reclaims completed queue entries only; CPU command/geometry storage is
// retained. Main render owner only; no submission may intervene before use.
GpuCompletion platformReserveGpuEntries(unsigned count,std::uint64_t timeoutNanoseconds);
// Hand the PICA commands assembled so far to the GPU without waiting, so it
// renders this part of the frame while the CPU builds the rest. Command order
// is unchanged (one queue). Geometry written so far is published first.
// Returns false only for a real queue/flush failure.
bool platformSubmitGpu();
// SD opt-in. On the render owner, delimit and tag atlas lists using the
// normal asynchronous GPU queue. A zero tag restores the painter phase.
enum class GpuShadowProfileMode : std::uint8_t { off, pass, runs };
inline GpuShadowProfileMode gpuShadowProfileMode{};
bool platformSubmitGpuShadowBoundary();
void platformSetGpuShadowTag(unsigned tag);
// Submit automatically every N prepared draws (0 disables). Render owner only.
void platformSetGpuChunkDraws(unsigned draws);
// While held, prepared draws do not start an automatic submission, so the
// command words of a pass stay one unbroken run (the HUD tape and the
// retained screens record them). Holds nest: one release per hold.
void platformHoldGpuChunks(bool hold);
// Counts the times the frame's command span was split or started again
// (submission, completion, reclaim). Equal values around a pass, with the
// same span base, prove its words are contiguous.
std::uint32_t platformGpuSpanSerial();
// Per-frame accounting of the time the render owner spent blocked on the GPU
// queue or on presentation. Taking it clears the accumulators.
struct GpuFrameTelemetry {
    std::uint64_t beginWaitTicks{},endWaitTicks{},completeWaitTicks{},presentWaitTicks{};
    std::uint32_t completes{},chunks{},reclaims{};
    // Part of completeWaitTicks spent draining for a command-arena reclaim.
    std::uint64_t reclaimWaitTicks{};
    // The GPU's own time since the previous picture, from the GSP jobs'
    // submission and completion ticks (ctru_gxqueue.c, one job in flight):
    // command lists, transfers and copies, fills; idle while no job was
    // queued; turnaround while the next job waited for the event handler;
    // the longest single command list; jobs and command lists completed.
    std::uint64_t gpuListTicks{},gpuCopyTicks{},gpuFillTicks{},gpuIdleTicks{},gpuTurnTicks{},gpuLongestListTicks{};
    std::uint32_t gpuJobs{},gpuLists{};
};
GpuFrameTelemetry platformTakeGpuTelemetry();
// GPU time per painter phase (sdmc:/3ds/DuskLight/gpu-phases.txt). The
// painter marks each pass (m_Do_graphic.cpp GX_DEBUG_GROUP); with phases on,
// a mark takes the GPU (the owner check syncs the second-core worker), splits
// the command list there whatever the PICA has queued, and tags the lists
// after it. The command-list time of each phase is reported every 300
// pictures ("Native GPU time per phase"). Off: a mark is one load and a test.
enum GpuPhase : unsigned {gpuPhaseFrame,gpuPhaseSky,gpuPhaseWorld,gpuPhaseShadows,gpuPhaseActors,gpuPhaseTranslucent,
                          gpuPhaseParticles,gpuPhaseEffects,gpuPhase2D,gpuPhaseCount,gpuPhaseNone=~0u};
inline bool gpuPhasesOn{};
// The phase of a painter call by its name (once per call site); gpuPhaseNone
// leaves the current phase.
unsigned gpuPhaseForPainterCall(const char* name);
void markGpuPhaseSlow(unsigned phase);
inline void markGpuPhase(unsigned phase){if(gpuPhasesOn)[[unlikely]]markGpuPhaseSlow(phase);}
// Diagnostic and submission switches, set once at start (game_startup.cpp):
// phases (above); drawTimes: every prepared draw closes the list before it
// and no split waits for the PICA, so each draw is one GSP job whose time the
// job ring keeps (a dump holds the last 512: Tools/3ds/gpu_jobs_report.py);
// firstPart: the first automatic split of a picture comes after this many
// draws and each next one after twice as many, up to the chunk size (0: the
// chunk size from the start, Alpha 15). The PICA starts on the clear and the
// sky while the CPU prepares the rest instead of waiting for a whole chunk.
void platformSetGpuDiagnostics(bool phases,bool drawTimes);
void platformSetGpuFirstPart(unsigned draws);
// Nonblocking observation of the real queue and pending CPU command buffer.
GpuCompletion platformQueryGpu();
// Sticky failure state: fatal containment must bypass APT/UI/GPU teardown.
// A deliberate nonblocking FrameBegin miss does not set it.
bool platformGpuFailurePending();
// Actual Dolphin TLS identity of the owner that bound the Citro3D queue.
// Null means no queue has been bound. The only transfer is the loan below.
OSThread* platformGpuOwner();
// Second-core worker (gx_worker_runtime.hpp): the game thread lends the GPU,
// Citro3D and the draw adapter's tables to the shape queue (toQueue) or
// takes them back. `worker` is the thread that owns them meanwhile, null
// when the game thread itself draws the queued shapes later (deferred mode).
// Only the game thread calls this, with the worker idle on both sides of the
// change. While they are lent, a platformGpuOwner() call made on the game
// thread first runs `wanted`, which returns with the loan ended
// (gxworker::sync): every function that checks the owner before it touches
// the GPU thereby draws what is queued first instead of failing. `site` is
// the code address that asked for the owner (for the worker's report of
// where the game thread had the queue drawn).
void platformLendGpu(OSThread* game,OSThread* worker,bool toQueue,void (*wanted)(void* site));
// Render-owner cache invalidation after a successful queue completion. Marks
// actual converted images for reconversion on their next validated binding.
bool platformInvalidateGpuTextures();
// True when the original GX syncs (DrawDone, PixModeSync, InvalidateTexAll,
// display copy) wait for the whole GPU (SD switch gx-sync-wait.txt).
bool nativeGxSyncWaits();
// Host tests: 1 waits, 0 never waits, -1 reads the SD switch (default).
void setGxSyncWaitsForTest(int value);
}
