#include "../runtime/gpu_sync.hpp"
#include "../runtime/gx_stream.hpp"
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <stdexcept>

namespace {
dusk::native::GpuCompletion simulatedResult = dusk::native::GpuCompletion::notReady;
unsigned waits, callbacks, invalidations;
bool completed;
bool allowInvalidation = true;
OSThread* simulatedOwner;
template<class F> void rejected(F f) {
    bool caught = false;
    try { f(); } catch (const std::runtime_error&) { caught = true; }
    assert(caught);
}
void callback() { assert(completed); ++callbacks; }
void reentrantCallback() {
    assert(completed);
    ++callbacks;
    const auto previousWaits = waits;
    rejected([] { GXDrawDone(); });
    rejected([] { GXPixModeSync(); });
    rejected([] { GXInvalidateTexAll(); });
    assert(waits == previousWaits);
    assert(GXSetDrawDoneCallback(callback) == reentrantCallback);
}
}
// Host-only identity fixture for this isolated GX test. Native bootstrap owns
// the actual TLS identity and is exercised separately by the kernel tests.
extern "C" OSThread* OSGetCurrentThread() {
    static thread_local OSThread identity{};
    return &identity;
}
namespace dusk::native {
// Explicit host-only completion fixture. This file is never a game provider or
// part of CMake; production waits on the actual libctru/Citro3D command queue.
GpuCompletion platformCompleteGpu(std::uint64_t timeout) {
    assert(timeout == 500000000);
    ++waits;
    completed = simulatedResult == GpuCompletion::complete;
    return simulatedResult;
}
OSThread* platformGpuOwner() { return simulatedOwner; }
bool platformInvalidateGpuTextures() {
    assert(completed);
    if (!allowInvalidation) return false;
    ++invalidations;
    return true;
}
}
void testNativeGpuSync() {
    using namespace dusk::native::gx;
    using dusk::native::GpuCompletion;
    assert(GXGetCurrentGXThread() == nullptr);
    rejected([] { GXSetCurrentGXThread(); });
    OSThread foreign{};
    simulatedOwner = &foreign;
    assert(GXGetCurrentGXThread() == &foreign);
    rejected([] { GXSetCurrentGXThread(); });
    simulatedOwner = OSGetCurrentThread();
    assert(simulatedOwner && GXGetCurrentGXThread() == simulatedOwner);
    assert(GXSetCurrentGXThread() == simulatedOwner);
    assert(!commandStream());
    assert(GXSetDrawDoneCallback(callback) == nullptr);
    rejected([] { GXDrawDone(); });
    assert(waits == 0 && callbacks == 0);
    CommandMemory memory; CommandProcessor processor(memory);
    std::array<u8, 32> scratch{}; CommandStream stream(processor, scratch);
    assert(setCommandStream(&stream));
    for (auto result : {GpuCompletion::notReady, GpuCompletion::wrongThread,
                        GpuCompletion::timedOut, GpuCompletion::exhausted}) {
        simulatedResult = result;
        rejected([] { GXSetDrawDone(); });
        assert(callbacks == 0);
    }
    simulatedResult = GpuCompletion::complete;
    GXDrawDone(); GXSetDrawDone();
    assert(callbacks == 2 && waits == 6);
    assert(GXSetDrawDoneCallback(nullptr) == callback);
    simulatedOwner = nullptr;
    assert(GXGetCurrentGXThread() == nullptr);
    GXDrawDone(); assert(callbacks == 2 && waits == 7);
    assert(GXSetDrawDoneCallback(reentrantCallback) == nullptr);
    GXSetDrawDone(); assert(callbacks == 3 && waits == 8);
    GXDrawDone(); assert(callbacks == 4 && waits == 9);
    const u8 incomplete = 0x61;
    assert(stream.write({&incomplete, 1}));
    rejected([] { GXDrawDone(); });
    assert(callbacks == 4 && waits == 9);
    assert(stream.reset() && setCommandStream(nullptr));
    assert(GXSetDrawDoneCallback(nullptr) == callback);
    simulatedOwner = OSGetCurrentThread();
    assert(setCommandStream(&stream));
    const auto callbackCount = callbacks;
    GXPixModeSync();
    assert(waits == 10 && callbacks == callbackCount && invalidations == 0);
    GXInvalidateTexAll();
    assert(waits == 11 && callbacks == callbackCount && invalidations == 1);
    for (auto result : {GpuCompletion::notReady, GpuCompletion::wrongThread,
                        GpuCompletion::timedOut, GpuCompletion::exhausted}) {
        simulatedResult = result;
        const auto before = waits;
        rejected([] { GXPixModeSync(); });
        rejected([] { GXInvalidateTexAll(); });
        assert(waits == before + 2 && callbacks == callbackCount && invalidations == 1);
    }
    simulatedResult = GpuCompletion::complete;
    allowInvalidation = false;
    rejected([] { GXInvalidateTexAll(); });
    assert(waits == 20 && callbacks == callbackCount && invalidations == 1);
    allowInvalidation = true;
    assert(stream.write({&incomplete, 1}));
    rejected([] { GXPixModeSync(); });
    rejected([] { GXInvalidateTexAll(); });
    assert(waits == 20 && callbacks == callbackCount && invalidations == 1);
    assert(stream.reset() && setCommandStream(nullptr));
    // No command context means there is nothing valid to synchronize.
    rejected([] { GXPixModeSync(); });
    rejected([] { GXInvalidateTexAll(); });
    assert(waits == 20 && invalidations == 1);
    std::puts("PASS: GPU completion, pixel synchronization and texture invalidation reject incomplete commands/missing queue/foreign owner/timeout/exhaustion/backend failure; no draw-done callbacks for pixel/cache operations; callback exchange/reentry guards (host completion fixture)");
}
