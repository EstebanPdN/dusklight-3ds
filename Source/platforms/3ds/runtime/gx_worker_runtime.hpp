#pragma once
// The second-core worker in the running game (gx_worker.hpp has the queue;
// this is its thread, its wake-ups and the GPU changing hands).
#include "gx_worker.hpp"
#include <cstddef>
#include <cstdint>

namespace dusk::native {
// Starts the queue in `mode` with the draw adapter's two entry points.
// `storage` is linear memory of gxworker::Queue::minimumStorage bytes or
// more. In worker mode a thread is created on core 2 below the audio
// thread's priority; when that fails the queue runs deferred instead and
// the reason is logged. False: nothing was started.
bool startGxWorker(gxworker::Mode mode,bool verify,const gxworker::Adapter& adapter,void* storage,std::size_t bytes);
// Drains, stops the thread and joins it (before Citro3D is torn down).
void stopGxWorker();
// One log line every 300 pictures while the queue is on, and at `always`.
void reportGxWorker(bool always);
// Once per picture, after it was painted, with the painter's time on the game
// thread (system ticks) and the picture's draws: the queue rests only where a
// trial without the worker painted faster (see gx_worker_ctru.cpp).
void gxWorkerPictureEnd(std::uint64_t paintTicks,std::uint32_t draws);
// SD switch gx-rest-measured-off.txt: false = the Alpha 13 rule (rest while
// the worker draws under half of the queued shapes).
void setGxWorkerMeasuredRests(bool on);
// The automatic test's onecore phase: no shape is queued while false.
// The automatic test (dev_console.hpp, SecondCoreTest): the worker on or off,
// and whether a rest is suspended for a measurement.
void wantGxWorker(bool on,bool measuring);
// A measurement (measured_dump.hpp) must not straddle a rest or a trial: its
// pictures would be painted by the game thread and say nothing of ordinary
// play. True while the queue rests or a trial runs; while held, no new trial
// starts (a rest that already runs goes on).
bool gxWorkerResting();
void holdGxWorkerTrials(bool hold);
// The automatic test's zone report: "mode:... queued:... " (counters since the start).
void describeGxWorker(char* text,std::size_t size);
// gxOverlapSyncHook's target (gx_worker_sync.hpp), installed with gx-overlap.txt 2.
void gxOverlapSync(int reason);
// The game processor's draw barrier: draws what is queued first (and reports
// the place when shapes were queued: Alpha 15 diagnostics).
void syncGxWorkerForDraw();
}
