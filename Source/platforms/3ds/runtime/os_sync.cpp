#include "os_sync.hpp"
#include "primitives.hpp"
#if defined(DUSK_NATIVE_THREAD_RUNTIME)
#include "os_thread.hpp"
#endif
#include <dolphin/os.h>
#include <atomic>
#include <climits>

namespace native = dusk::native;
namespace {
void lockSync(s32* value) {
#if defined(DUSK_NATIVE_THREAD_RUNTIME)
    native::threadLock(value);
#else
    native::lock(value);
#endif
}
void waitSync(s32* condition, s32* value) {
#if defined(DUSK_NATIVE_THREAD_RUNTIME)
    native::threadConditionWait(condition, value);
#else
    native::conditionWait(condition, value);
#endif
}
std::atomic_ref<OSThread*> owner(OSMutex* mutex) {
    return std::atomic_ref<OSThread*>(mutex->thread);
}
void linkMutex(OSMutex* mutex, OSThread* current) {
    mutex->link.prev = current->queueMutex.tail;
    mutex->link.next = nullptr;
    if (current->queueMutex.tail) current->queueMutex.tail->link.next = mutex;
    else current->queueMutex.head = mutex;
    current->queueMutex.tail = mutex;
}
void unlinkMutex(OSMutex* mutex, OSThread* current) {
    if (mutex->link.prev) mutex->link.prev->link.next = mutex->link.next;
    else current->queueMutex.head = mutex->link.next;
    if (mutex->link.next) mutex->link.next->link.prev = mutex->link.prev;
    else current->queueMutex.tail = mutex->link.prev;
    mutex->link = {};
}
void enter(OSMutex* mutex, OSThread* current) {
    if (mutex->count == INT32_MAX) OSPanic(__FILE__, __LINE__, "Mutex recursion overflow");
    if (!mutex->count) linkMutex(mutex, current);
    ++mutex->count;
    owner(mutex).store(current, std::memory_order_relaxed);
}
struct Guard {
    s32* value;
    explicit Guard(s32* value) : value(value) { lockSync(value); }
    ~Guard() { native::unlock(value); }
};
BOOL send(OSMessageQueue* queue, OSMessage message, s32 flags, bool jam) {
    if (!queue) return FALSE;
    Guard guard(&queue->nativeLock);
    while (!queue->nativeClosed && queue->usedCount == queue->msgCount) {
        if (!(flags & OS_MESSAGE_BLOCK)) return FALSE;
        waitSync(&queue->nativeSendCondition, &queue->nativeLock);
    }
    if (queue->nativeClosed) return FALSE;
    s32 index;
    if (jam) {
        queue->firstIndex = queue->firstIndex == 0 ? queue->msgCount - 1 : queue->firstIndex - 1;
        index = queue->firstIndex;
    } else {
        index = s32((u32(queue->firstIndex) + u32(queue->usedCount)) % u32(queue->msgCount));
    }
    queue->msgArray[index] = message;
    ++queue->usedCount;
    native::conditionBroadcast(&queue->nativeReceiveCondition);
    return TRUE;
}
}

extern "C" {
void OSInitMutex(OSMutex* mutex) {
    if (!mutex) return;
    mutex->queue = {};
    mutex->thread = nullptr;
    mutex->count = 0;
    mutex->link = {};
    native::lockInit(&mutex->nativeLock);
}
void OSLockMutex(OSMutex* mutex) {
    if (!mutex) return;
    auto* current = OSGetCurrentThread();
    if (!current) OSPanic(__FILE__, __LINE__, "Mutex caller has no thread record");
    if (owner(mutex).load(std::memory_order_relaxed) != current) lockSync(&mutex->nativeLock);
    enter(mutex, current);
}
BOOL OSTryLockMutex(OSMutex* mutex) {
    if (!mutex) return FALSE;
    auto* current = OSGetCurrentThread();
    if (!current) OSPanic(__FILE__, __LINE__, "Mutex caller has no thread record");
    if (owner(mutex).load(std::memory_order_relaxed) != current &&
        !native::tryLock(&mutex->nativeLock)) return FALSE;
    enter(mutex, current);
    return TRUE;
}
void OSUnlockMutex(OSMutex* mutex) {
    if (!mutex || owner(mutex).load(std::memory_order_relaxed) != OSGetCurrentThread()) return;
    if (--mutex->count == 0) {
        unlinkMutex(mutex, OSGetCurrentThread());
        owner(mutex).store(nullptr, std::memory_order_relaxed);
        native::unlock(&mutex->nativeLock);
    }
}
void OSInitCond(OSCond* condition) {
    if (!condition) return;
    condition->queue = {};
    native::conditionInit(&condition->nativeCondition);
}
void OSWaitCond(OSCond* condition, OSMutex* mutex) {
    if (!condition || !mutex) return;
    auto* current = OSGetCurrentThread();
    if (!current || owner(mutex).load(std::memory_order_relaxed) != current || mutex->count < 1)
        OSPanic(__FILE__, __LINE__, "Condition wait requires an owned mutex");
    const s32 depth = mutex->count;
    unlinkMutex(mutex, current);
    mutex->count = 0;
    owner(mutex).store(nullptr, std::memory_order_relaxed);
    try { waitSync(&condition->nativeCondition, &mutex->nativeLock); }
    catch (...) {
        linkMutex(mutex, current);
        mutex->count = depth;
        owner(mutex).store(current, std::memory_order_relaxed);
        throw;
    }
    linkMutex(mutex, current);
    mutex->count = depth;
    owner(mutex).store(current, std::memory_order_relaxed);
}
void OSSignalCond(OSCond* condition) {
    if (condition) native::conditionBroadcast(&condition->nativeCondition);
}
void OSInitMessageQueue(OSMessageQueue* queue, OSMessage* messages, s32 count) {
    if (!queue || count < 0 || (count && !messages))
        OSPanic(__FILE__, __LINE__, "Invalid message queue storage");
    queue->queueSend = {};
    queue->queueReceive = {};
    queue->msgArray = messages;
    queue->msgCount = count;
    queue->firstIndex = queue->usedCount = 0;
    queue->nativeClosed = FALSE;
    native::lockInit(&queue->nativeLock);
    native::conditionInit(&queue->nativeSendCondition);
    native::conditionInit(&queue->nativeReceiveCondition);
}
BOOL OSSendMessage(OSMessageQueue* queue, OSMessage message, s32 flags) {
    return send(queue, message, flags, false);
}
BOOL OSJamMessage(OSMessageQueue* queue, OSMessage message, s32 flags) {
    return send(queue, message, flags, true);
}
BOOL OSReceiveMessage(OSMessageQueue* queue, OSMessage* message, s32 flags) {
    if (!queue) return FALSE;
    Guard guard(&queue->nativeLock);
    while (queue->usedCount == 0) {
        if (queue->nativeClosed || !(flags & OS_MESSAGE_BLOCK)) return FALSE;
        waitSync(&queue->nativeReceiveCondition, &queue->nativeLock);
    }
    if (message) *message = queue->msgArray[queue->firstIndex];
    queue->firstIndex = queue->firstIndex == queue->msgCount - 1 ? 0 : queue->firstIndex + 1;
    --queue->usedCount;
    native::conditionBroadcast(&queue->nativeSendCondition);
    return TRUE;
}
}

void dusk::native::closeMessageQueue(OSMessageQueue* queue) {
    if (!queue) return;
    Guard guard(&queue->nativeLock);
    queue->nativeClosed = TRUE;
    conditionBroadcast(&queue->nativeSendCondition);
    conditionBroadcast(&queue->nativeReceiveCondition);
}
void dusk::native::releaseCurrentThreadMutexes() {
    auto* current = OSGetCurrentThread();
    while (auto* mutex = current->queueMutex.head) {
        if (owner(mutex).load(std::memory_order_relaxed) != current || mutex->count < 1)
            OSPanic(__FILE__, __LINE__, "Corrupt native thread mutex ownership");
        mutex->count = 1;
        OSUnlockMutex(mutex);
    }
}
