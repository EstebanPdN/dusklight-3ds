#include "os_thread.hpp"
#include "thread_backend.hpp"
#include "bootstrap.hpp"
#include "primitives.hpp"
#include "os_sync.hpp"
#include <dolphin/os.h>
#include <array>
#include <atomic>
#include <climits>
#include <cstring>

namespace native = dusk::native;
namespace {
constexpr unsigned capacity = 24;
constexpr std::uint32_t stackBudget = 2 * 1024 * 1024;
constexpr std::uint64_t stopTimeout = 2000000000;
constexpr std::int64_t waitSlice = 25000000;
struct Control {
    OSThread* record{};
    native::NativeThreadHandle handle{};
    void* (*entry)(void*){};
    void* argument{};
    std::uint32_t stackBytes{};
    bool cancelled{}, paused{}, done{}, joining{}, exiting{};
    std::int32_t* waitingCondition{};
    std::array<char, 64> name{};
};
std::array<Control, capacity> controls{};
std::uint32_t stackBytes;
bool stopping;
thread_local Control* current;
struct ThreadExit { void* value; };
struct State {
    std::int32_t lock{}, changed{};
    State() { native::lockInit(&lock); native::conditionInit(&changed); }
};
State& state() { static State value; return value; }
struct Guard {
    Guard() { native::lock(&state().lock); }
    ~Guard() { native::unlock(&state().lock); }
};
void notify() { native::conditionBroadcast(&state().changed); }
void waitChanged() { native::conditionWaitTimeout(&state().changed, &state().lock, waitSlice); }
[[noreturn]] void reject(const char* message) { OSPanic(__FILE__, __LINE__, "%s", message); }
Control* find(OSThread* record) {
    for (auto& slot : controls) if (slot.record == record) return &slot;
    return nullptr;
}
int nativePriority(OSPriority priority) { return 0x18 + (priority * 39 + 15) / 31; }
void dequeue(OSThread* thread) {
    auto* queue = thread->queue;
    if (!queue) return;
    if (thread->link.prev) thread->link.prev->link.next = thread->link.next;
    else queue->head = thread->link.next;
    if (thread->link.next) thread->link.next->link.prev = thread->link.prev;
    else queue->tail = thread->link.prev;
    thread->queue = nullptr; thread->link = {};
}
bool freeJoined(Control* slot) {
    // A successful kernel join precedes validation and free. The claim keeps
    // the slot/record stable while the backend frees stack/TLS OUTSIDE lock.
    {
        Guard guard;
        if (!slot->done || slot->record->queue || slot->record->queueMutex.head || slot->waitingCondition)
            return false;
    }
    native::platformFreeThread(slot->handle);
    Guard guard;
    slot->record->state = 0;
    stackBytes -= slot->stackBytes;
    *slot = {};
    notify();
    return true;
}
struct JoinClaim {
    Control* slot;
    ~JoinClaim() {
        if (!slot) return;
        Guard guard; slot->joining = false; notify();
    }
};
Control* awaitShutdownClaim(OSThread* record) {
    // State lock held. Cleanup may encounter a done child already claimed by
    // the main barrier; keep its caller-owned record alive until collection.
    auto* slot = find(record);
    while (stopping && slot && slot->joining) {
        waitChanged();
        slot = find(record);
    }
    return slot;
}
void collect(Control* slot, std::uint64_t timeout) {
    JoinClaim claim{slot};
    const auto start = native::platformThreadTime();
    for (;;) {
        bool aggregate;
        { Guard guard; aggregate = stopping && current; }
        const auto elapsed = native::platformThreadTime() - start;
        const auto remaining = elapsed < timeout ? timeout - elapsed : 0;
        const auto slice = aggregate || (current && remaining > std::uint64_t(waitSlice)) ? std::uint64_t(waitSlice) : remaining;
        if (native::platformJoinThread(slot->handle, slice)) break;
        // Cancellation/destructor collection may itself be noexcept cleanup.
        // During aggregate stop the main barrier owns the only deadline and
        // preserves this claim on failure; never throw/fatal from that cleanup.
        { Guard guard; aggregate = stopping && current; }
        if (aggregate) continue;
        if (current) native::threadCheckpoint();
        if (!remaining || native::platformThreadTime() - start >= timeout)
            reject("Native worker did not terminate within its join deadline");
    }
    if (!freeJoined(slot)) reject("Native thread finished with retained wait/mutex ownership");
    claim.slot = nullptr;
}
void reapDetached() {
    for (unsigned i = 0; i < capacity; ++i) {
        Control* slot = nullptr;
        {
            Guard guard; auto& candidate = controls[i];
            if (!stopping && candidate.record && candidate.done && !candidate.joining &&
                (candidate.record->attr & OS_THREAD_ATTR_DETACH)) {
                candidate.joining = true; slot = &candidate;
            }
        }
        if (slot) collect(slot, stopTimeout);
    }
}
void worker(void* argument) {
    auto* slot = static_cast<Control*>(argument);
    current = slot;
    native::bindCurrentThread(slot->record);
    void* result = nullptr;
    try {
        native::threadCheckpoint(); // OSCreateThread starts logically suspended.
        result = slot->entry(slot->argument);
    } catch (const ThreadExit& exit) {
        result = exit.value;
    } catch (...) {
        reject("Unhandled exception escaped an original native thread");
    }
    // Original OS exit releases owned mutexes; do this on their actual owner.
    native::releaseCurrentThreadMutexes();
    {
        Guard guard;
        auto* record = slot->record;
        dequeue(record);
        record->val = result;
        record->state = OS_THREAD_STATE_MORIBUND;
        slot->done = true;
        notify();
    }
    native::bindCurrentThread(nullptr);
    current = nullptr;
    // Return through libctru's own TLS/stack exit path, never svcExitThread.
}
}

namespace dusk::native {
namespace {
void checkpoint(bool mutexAcquisition) {
    if (!current) return;
    Guard guard;
    // Normal-return destructors (not only exception unwinding) can acquire
    // heap/command locks. Aggregate stop must not throw through noexcept
    // cleanup. Defer lock-only cancellation to the next explicit wait/yield;
    // a stuck mutex then fails closed at the owner's aggregate deadline.
    if (current->exiting || (mutexAcquisition && stopping)) return;
    while (current->record->suspend && !current->cancelled) {
        current->paused = true;
        current->record->state = OS_THREAD_STATE_WAITING;
        notify();
        conditionWait(&state().changed, &state().lock);
    }
    current->paused = false;
    if (current->cancelled) { current->exiting = true; throw ThreadExit{nullptr}; }
    current->record->state = OS_THREAD_STATE_RUNNING;
}
}
void threadCheckpoint() { checkpoint(false); }
void threadLock(std::int32_t* value) {
    if (!current) { lock(value); return; }
    for (;;) {
        checkpoint(true);
        if (tryLock(value)) return;
        platformThreadSleep(1000000);
    }
}
void threadConditionWait(std::int32_t* condition, std::int32_t* value) {
    if (!current) { conditionWait(condition, value); return; }
    bool wait;
    {
        Guard guard; current->waitingCondition = condition; wait = !current->cancelled || current->exiting;
    }
    if (wait) conditionWaitTimeout(condition, value, waitSlice);
    { Guard guard; current->waitingCondition = nullptr; }
    // Never park a suspended worker while holding another queue's wait lock.
    // Reacquire before propagating exit so the caller's guard can unlock it.
    unlock(value);
    try { threadCheckpoint(); }
    catch (...) { lock(value); throw; }
    lock(value);
}
void releaseTerminatedThread(OSThread* record) {
    Control* slot;
    {
        Guard guard; slot = awaitShutdownClaim(record);
        if (!slot) return;
        if (!slot->done || slot->joining) reject("Native thread storage released before termination");
        slot->joining = true;
    }
    collect(slot, stopTimeout);
}
ThreadStats threadStats() {
    Guard guard;
    ThreadStats stats{0, stackBytes, 0};
    for (auto& slot : controls) {
        stats.workers += slot.record != nullptr;
        stats.waitingQueues += slot.record && slot.record->queue;
    }
    return stats;
}
const char* workerShutdownStatusName(WorkerShutdownStatus status) {
    switch (status) {
    case WorkerShutdownStatus::complete: return "complete";
    case WorkerShutdownStatus::wrongOwner: return "wrong-owner";
    case WorkerShutdownStatus::timedOut: return "deadline-expired";
    case WorkerShutdownStatus::retainedOwnership: return "retained-wait-or-mutex-ownership";
    }
    return "unknown";
}
WorkerShutdownResult shutdownWorkers(std::uint64_t timeout) {
    const auto start = platformThreadTime();
    if (timeout > stopTimeout) timeout = stopTimeout;
    const auto remainingTime = [&] {
        const auto elapsed = platformThreadTime() - start;
        return elapsed < timeout ? timeout - elapsed : std::uint64_t(0);
    };
    unsigned joined = 0;
    const auto result = [&](WorkerShutdownStatus status) {
        Guard guard;
        WorkerShutdownResult value{status, {0, stackBytes, 0}, joined, 0};
        for (auto& slot : controls) {
            value.remaining.workers += slot.record != nullptr;
            value.remaining.waitingQueues += slot.record && slot.record->queue;
            value.joining += slot.record && slot.joining;
        }
        return value;
    };
    if (current || !platformIsMainThread()) return result(WorkerShutdownStatus::wrongOwner);
    {
        Guard guard;
        stopping = true; // Serialized with the last creation admission/handle capture.
        for (auto& slot : controls) if (slot.record) {
            slot.cancelled = true;
            if (slot.waitingCondition) conditionBroadcast(slot.waitingCondition);
        }
        notify(); // Suspended start, OS sleep, suspension and claim waiters.
    }
    for (;;) {
        Control* ready = nullptr;
        bool any = false;
        {
            Guard guard;
            for (auto& slot : controls) if (slot.record) {
                any = true;
                // Do not claim a live child needed by a parent's cleanup. Let
                // existing nested join claims finish or unwind cooperatively.
                if (!ready && slot.done && !slot.joining) ready = &slot;
            }
            if (ready) ready->joining = true;
            else if (any) {
                const auto remaining = remainingTime();
                if (remaining) conditionWaitTimeout(&state().changed, &state().lock,
                    remaining < std::uint64_t(waitSlice) ? remaining : waitSlice);
            }
        }
        if (!any) return result(WorkerShutdownStatus::complete);
        if (ready) {
            JoinClaim claim{ready};
            // done is only a logical exit marker. TLS/destructors/kernel exit
            // must finish before any stack storage is freed.
            if (!platformJoinThread(ready->handle, remainingTime())) {
                // Release our claim before reporting the retained ownership.
                { Guard guard; ready->joining = false; notify(); }
                claim.slot = nullptr;
                return result(WorkerShutdownStatus::timedOut);
            }
            if (!freeJoined(ready)) {
                { Guard guard; ready->joining = false; notify(); }
                claim.slot = nullptr;
                return result(WorkerShutdownStatus::retainedOwnership);
            }
            claim.slot = nullptr;
            ++joined;
        }
        if (!remainingTime()) return result(WorkerShutdownStatus::timedOut);
    }
}
void sleepThreadWhile(OSThreadQueue* queue, const bool* paused) {
    if (!queue || !current) reject("Native thread sleep requires a managed worker and queue");
    threadCheckpoint();
    {
        Guard guard;
        if (paused && !std::atomic_ref<bool>(*const_cast<bool*>(paused)).load(std::memory_order_acquire)) return;
        auto* record = current->record;
        if (record->queue) reject("Native thread already belongs to a wait queue");
        record->queue = queue; record->link.prev = queue->tail;
        if (queue->tail) queue->tail->link.next = record; else queue->head = record;
        queue->tail = record;
        record->state = OS_THREAD_STATE_WAITING;
        while (record->queue && !current->cancelled) {
            current->paused = record->suspend > 0;
            notify();
            waitChanged();
        }
        dequeue(record);
    }
    threadCheckpoint();
}
}

namespace dusk::native {
bool createThreadOnCore(OSThread* record, void* (*entry)(void*), void* argument,
                    void* stackTop, std::uint32_t size, int priority, std::uint16_t attr, int core) {
    if(core!=-2&&core!=0&&core!=2)return false;
    if (!record || !entry || priority < 0 || priority > 31 || (attr & ~OS_THREAD_ATTR_DETACH) ||
        !size || size > 512 * 1024 ||
        (stackTop && (reinterpret_cast<std::uintptr_t>(stackTop) < size || reinterpret_cast<std::uintptr_t>(stackTop) % 8))) return FALSE;
    { Guard guard; if (stopping) return FALSE; }
    reapDetached();
    Guard guard;
    if (stopping || find(record)) return FALSE;
    Control* slot = nullptr;
    for (auto& candidate : controls) if (!candidate.record) { slot = &candidate; break; }
    const std::uint32_t bytes = ((size < 32768 ? 32768 : size) + 7) & ~7u;
    if (!slot || bytes > stackBudget - stackBytes) return FALSE;
    *record = {};
    record->state = OS_THREAD_STATE_READY; record->attr = attr;
    record->suspend = 1; record->base = record->priority = priority;
    // These are caller compatibility bounds, not libctru's private stack/TLS.
    // Null means backend-owned stack with unavailable public stack addresses.
    record->stackBase = static_cast<u8*>(stackTop);
    record->stackEnd = stackTop ? reinterpret_cast<u8*>(reinterpret_cast<std::uintptr_t>(stackTop) - size) : nullptr;
    *slot = {}; slot->record = record; slot->entry = entry; slot->argument = argument; slot->stackBytes = bytes;
    slot->handle = native::platformCreateThread(worker, slot, bytes, nativePriority(priority), core);
    if (!slot->handle) { *slot = {}; *record = {}; return FALSE; }
    stackBytes += bytes;
    return TRUE;
}
}
extern "C" {
BOOL OSCreateThread(OSThread* record, void* (*entry)(void*), void* argument,
                    void* stackTop, u32 size, OSPriority priority, u16 attr) {
    return native::createThreadOnCore(record,entry,argument,stackTop,size,priority,attr,-2);
}
s32 OSResumeThread(OSThread* record) {
    if (!record) return 0;
    Guard guard; auto* slot = find(record);
    if (!slot || slot->done) return 0;
    const auto previous = record->suspend;
    if (previous) --record->suspend;
    notify();
    return previous;
}
s32 OSSuspendThread(OSThread* record) {
    if (!record) return 0;
    s32 previous;
    {
        Guard guard; auto* slot = find(record);
        if (!slot) reject("Cannot suspend an unmanaged native thread");
        if (slot->done) return 0;
        if (record->suspend == INT32_MAX) reject("Native suspend count overflow");
        previous = record->suspend++;
        notify();
        if (slot != current) {
            const auto start = native::platformThreadTime();
            while (!slot->paused && !slot->done) {
                if (native::platformThreadTime() - start >= stopTimeout)
                    reject("Native suspension was not acknowledged at a cooperative boundary");
                waitChanged();
            }
            return previous;
        }
    }
    native::threadCheckpoint();
    return previous;
}
void OSCancelThread(OSThread* record) {
    if (!record) return;
    Control* slot;
    {
        Guard guard; slot = awaitShutdownClaim(record);
        if (!slot) {
            if (!record->state) return;
            reject("Cannot cancel an unmanaged native thread");
        }
        if (slot->joining) reject("Concurrent native thread join/cancellation");
        slot->cancelled = true;
        notify();
        if (slot == current) { slot->exiting = true; throw ThreadExit{nullptr}; }
        slot->joining = true;
    }
    collect(slot, stopTimeout);
}
void OSDetachThread(OSThread* record) {
    if (!record) return;
    bool collectNow = false;
    {
        Guard guard; auto* slot = awaitShutdownClaim(record);
        if (!slot) {
            if (!record->state) return;
            reject("Cannot detach an unmanaged native thread");
        }
        if (slot->joining) reject("Native detach raced an existing join");
        record->attr |= OS_THREAD_ATTR_DETACH;
        collectNow = slot->done;
    }
    if (collectNow) native::releaseTerminatedThread(record);
}
BOOL OSJoinThread(OSThread* record, void** result) {
    if (!record) return FALSE;
    Control* slot;
    {
        Guard guard; slot = find(record);
        if (!slot || slot == current || slot->joining || (record->attr & OS_THREAD_ATTR_DETACH)) return FALSE;
        slot->joining = true;
    }
    JoinClaim claim{slot};
    if (current) {
        // A managed caller must remain cancellable while waiting for another
        // real worker. Unwinding releases only its claim; the child stays live.
        while (!native::platformJoinThread(slot->handle, waitSlice)) native::threadCheckpoint();
    } else if (!native::platformJoinThread(slot->handle, UINT64_MAX)) reject("Native join failed");
    { Guard guard; if (result) *result = record->val; }
    if (!freeJoined(slot)) reject("Native thread finished with retained wait/mutex ownership");
    claim.slot = nullptr;
    // A child may exit at the same time cancellation arrives. Observe it here
    // even after a successful join, before caller cleanup takes mutexes.
    native::threadCheckpoint();
    return TRUE;
}
void OSExitThread(void* result) {
    if (!current) reject("Cannot exit an unmanaged native thread");
    { Guard guard; current->exiting = true; }
    throw ThreadExit{result};
}
BOOL OSIsThreadSuspended(OSThread* record) {
    if (!record) return FALSE;
    Guard guard; return record->suspend > 0;
}
BOOL OSIsThreadTerminated(OSThread* record) {
    if (!record) return TRUE;
    Guard guard; auto* slot = find(record);
    return slot ? slot->done : record->state == 0;
}
void OSYieldThread() { native::threadCheckpoint(); native::platformThreadSleep(0); }
BOOL OSSetThreadPriority(OSThread* record, OSPriority priority) {
    if (!record || priority < 0 || priority > 31) return FALSE;
    Guard guard; auto* slot = find(record);
    if (slot ? slot->done : record != OSGetCurrentThread()) return FALSE;
    if (!native::platformSetThreadPriority(slot ? slot->handle : native::NativeThreadHandle{}, nativePriority(priority))) return FALSE;
    record->base = record->priority = priority;
    return TRUE;
}
s32 OSGetThreadPriority(OSThread* record) {
    if (!record) reject("Null native priority owner");
    Guard guard; auto* slot = find(record);
    if (slot) return record->base;
    if (record != OSGetCurrentThread()) reject("Unknown native priority owner");
    const int priority = native::platformGetThreadPriority({});
    if (priority < 0x18 || priority > 0x3f) reject("Native kernel priority query failed");
    return ((priority - 0x18) * 31 + 19) / 39;
}
void OSSetThreadSpecific(s32 index, void* value) {
    if (index < 0 || index >= OS_THREAD_SPECIFIC_MAX) reject("Invalid native TLS index");
    OSGetCurrentThread()->specific[index] = value;
}
void* OSGetThreadSpecific(s32 index) {
    if (index < 0 || index >= OS_THREAD_SPECIFIC_MAX) reject("Invalid native TLS index");
    return OSGetCurrentThread()->specific[index];
}
void OSInitThreadQueue(OSThreadQueue* queue) {
    if (!queue) reject("Null native thread queue");
    Guard guard; queue->head = queue->tail = nullptr;
}
void OSSleepThread(OSThreadQueue* queue) {
    native::sleepThreadWhile(queue, nullptr);
}
void OSWakeupThread(OSThreadQueue* queue) {
    if (!queue) reject("Null native wake queue");
    Guard guard;
    while (queue->head) {
        auto* record = queue->head;
        if (!find(record)) reject("Foreign worker in native wait queue");
        dequeue(record); record->state = OS_THREAD_STATE_READY;
    }
    notify();
}
void OSSetCurrentThreadName(const char* name) {
    if (!name) reject("Null native thread name");
    std::array<char, 64> label{};
    std::size_t size = 0;
    for (; size + 1 < label.size() && name[size]; ++size) label[size] = name[size];
    {
        Guard guard;
        if (current) current->name = label;
    }
    // Actual bounded diagnostic label; no unsupported kernel rename is claimed.
    OSReport("Native worker %p: %s\n", OSGetCurrentThread(), label.data());
}
}
