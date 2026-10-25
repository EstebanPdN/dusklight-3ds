#include "gpu_sync.hpp"
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <atomic>

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
void finishDraw() {
    CompletionGuard guard;
    finishPixels();
    if (auto callback = doneCallback.load(std::memory_order_acquire)) callback();
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
// Native completion currently blocks the render owner. JUT's callback runs only
// after real GPU completion; background/overlapped notification is not claimed.
void GXSetDrawDone() { finishDraw(); }
void GXPixModeSync() {
    CompletionGuard guard;
    finishPixels();
}
void GXInvalidateTexAll() {
    CompletionGuard guard;
    finishPixels();
    if (!dusk::native::platformInvalidateGpuTextures())
        OSPanic(__FILE__, __LINE__, "Native GPU texture invalidation failed");
}
GXDrawDoneCallback GXSetDrawDoneCallback(GXDrawDoneCallback callback) {
    return doneCallback.exchange(callback, std::memory_order_acq_rel);
}
}
