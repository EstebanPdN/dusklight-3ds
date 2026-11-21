#pragma once
// The second-core worker's sync point (gx_worker.hpp) for GX entry points
// that change something a draw reads outside the register bank: the capture
// state and the copy serial (GXCopyTex), the render target (the framebuffer
// calls), the GPU's texture images. The shapes queued for the worker were
// issued before the call, so they are drawn before it goes on.
//
// A hook and not the queue itself: these files are also built into host
// tests that have no queue. gx_worker_ctru.cpp installs it when the queue
// starts; without one the call does nothing.
#include <cstddef>
namespace dusk::native {
inline void (*gxWorkerSyncHook)()=nullptr;
inline void syncGxWorker(){if(const auto hook=gxWorkerSyncHook)hook();}
// Alpha 15 overlap (gx_worker.hpp Queue::beginOverlap): game code that frees,
// creates or rewrites what a queued draw may read calls this first. Reasons:
// 1 an allocation ends or the heap tree changes (JKRHeap.cpp), 2 a process
// deletion and 3 a creation is pending (f_pc_manager.cpp), 4 a model whose
// vertex arrays the CPU rewrites is calculated (J3DModel::calc). Installed by
// game_startup.cpp with gx-overlap.txt 2 (gx_worker_ctru.cpp gxOverlapSync);
// it returns at once on other threads and when nothing is left to draw.
inline void (*gxOverlapSyncHook)(int reason)=nullptr;
inline void syncGxOverlap(int reason){if(const auto hook=gxOverlapSyncHook)hook(reason);}
// True while the calling thread draws a shape that was queued (the worker, or
// the game thread at a sync): the draw was issued earlier, so state of the
// game thread that is not in the register bank is not the state it was
// issued under and must not be read.
inline bool (*gxWorkerConsumingHook)()=nullptr;
inline bool drawingQueuedShape(){const auto hook=gxWorkerConsumingHook;return hook&&hook();}
// A job: GPU work of the game thread that belongs between two shapes of the
// picture and whose result the game does not wait for. Queued, it is run in
// its place by the thread that draws (so the worker keeps the shapes queued
// before it); a sync would hand all of them to the game thread instead.
// False: not queued (no queue, or it does not accept now): the caller syncs
// and does the work itself. `arguments` are copied.
// Alpha 14: the bloom pass (gx_pica.cpp drawGxBloom: the game counts a pass
// it queued), the start of a shadow caster list (armGxShadowCasters) and the
// two ends of a conditional section (below) are jobs as well.
// Alpha 15: the HUD tape's replay and the two ends of its recording
// (hud_tape.hpp runHudTapeJob) while the 2D passes go to the queue.
// gxJobScreenTape: a retained screen's queued replay (screen_tape.hpp).
enum : unsigned {gxJobShadowCasters=1,gxJobCopyTex,gxJobInvalidateTextures,gxJobBloom,gxJobShadowArm,gxJobSectionBegin,gxJobSectionEnd,gxJobHudTape,
                 gxJobScreenTape};
inline bool (*gxWorkerJobHook)(unsigned job,const void* arguments,std::size_t bytes)=nullptr;
inline bool queueGxJob(unsigned job,const void* arguments,std::size_t bytes){const auto hook=gxWorkerJobHook;return hook&&hook(job,arguments,bytes);}
// A conditional section: the game thread issues the draws between
// beginGxSection and endGxSection as if `condition` held, and the thread that
// draws them decides, in their place in the queue (after every draw issued
// before the section), whether they are drawn at all. That is the answer a
// sync before the section would have given the game thread, without handing
// the queue back to it. A draw of the section that cannot be queued makes the
// game thread sync and drop it when the condition failed. The register words
// the section's GX calls store still reach the bank either way.
// beginGxSection false: no section (no queue, it does not accept, the SD
// switch is off, or one is open): the caller asks the condition itself.
enum : unsigned {gxSectionFrameCopyMade=1}; // gx::watchedCaptureUsedThisFrame()
inline bool (*gxWorkerSectionHook)(unsigned condition)=nullptr; // 0 ends the open section
inline bool beginGxSection(unsigned condition){const auto hook=gxWorkerSectionHook;return condition&&hook&&hook(condition);}
inline void endGxSection(){if(const auto hook=gxWorkerSectionHook)hook(0);}
}
