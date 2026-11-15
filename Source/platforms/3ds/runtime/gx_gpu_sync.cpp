#include "gpu_sync.hpp"
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <atomic>
#include <cstdio>

namespace {
std::atomic<GXDrawDoneCallback> doneCallback{};
std::atomic_flag completing = ATOMIC_FLAG_INIT;
struct CompletionGuard {
    CompletionGuard() {
        if (completing.test_and_set(std::memory_order_acquire))
            OSPanic(__FILE__, __LINE__, "Reentrant native GPU draw completion");
    }
    ~CompletionGuard() { completing.clear(std::memory_order_release); }
};
void finishPixels() {
    GXFlush(); // Reject incomplete CPU commands before a GPU wait or callback.
    const auto result = dusk::native::platformCompleteGpu(500000000);
    if (result != dusk::native::GpuCompletion::complete)
        OSPanic(__FILE__, __LINE__, "Native GPU draw completion failed: %u", unsigned(result));
}
// Original GX syncs. On the GameCube only GXDrawDone stalls the CPU; the other
// calls are tokens the GP meets in order. The PICA also runs every job in
// order, and each native path that lets the CPU read or overwrite GPU memory
// (readbacks, eviction, texture re-decode) waits for completion itself. Alpha 0
// waited for the whole GPU at every one of these calls: about ten complete
// stalls and 11 ms of each title frame on a New 3DS, with no CPU/GPU overlap
// left. gx-sync-wait.txt on the SD restores those waits.
void finishDraw() {
    CompletionGuard guard;
    if (dusk::native::nativeGxSyncWaits()) finishPixels();
    else GXFlush();
    if (auto callback = doneCallback.load(std::memory_order_acquire)) callback();
}
}
namespace dusk::native {
namespace { int syncWaitOverride = -1; }
void setGxSyncWaitsForTest(int value) { syncWaitOverride = value; }
bool nativeGxSyncWaits() {
    if (syncWaitOverride >= 0) return syncWaitOverride != 0;
    static int value = -1;
    if (value < 0) {
        auto* file = std::fopen("sdmc:/3ds/DuskLight/gx-sync-wait.txt", "rb");
        value = file != nullptr;
        if (file) std::fclose(file);
    }
    return value != 0;
}
}
extern "C" {
OSThread* GXGetCurrentGXThread() { return dusk::native::platformGpuOwner(); }
OSThread* GXSetCurrentGXThread() {
    auto* previous = dusk::native::platformGpuOwner();
    if (!previous || previous != OSGetCurrentThread())
        OSPanic(__FILE__, __LINE__, "Native GX owner transfer requires an initialized queue on the calling render thread");
    // Citro3D already belongs to this thread; report the real previous owner.
    // Moving its queue/context to an unrelated reset worker is not implemented.
    return previous;
}
void GXDrawDone() { finishDraw(); }
// JUT's draw-done callback only rotates XFB bookkeeping; it runs at once.
void GXSetDrawDone() { finishDraw(); }
void GXPixModeSync() {
    CompletionGuard guard;
    if (dusk::native::nativeGxSyncWaits()) finishPixels();
    else GXFlush();
}
void GXInvalidateTexAll() {
    CompletionGuard guard;
    if (dusk::native::nativeGxSyncWaits()) finishPixels();
    else GXFlush();
    // Marks the images dirty; a changed image waits for the GPU before its
    // pixels are rewritten (bindGxTexture).
    if (!dusk::native::platformInvalidateGpuTextures())
        OSPanic(__FILE__, __LINE__, "Native GPU texture invalidation failed");
}
GXDrawDoneCallback GXSetDrawDoneCallback(GXDrawDoneCallback callback) {
    return doneCallback.exchange(callback, std::memory_order_acq_rel);
}
}
