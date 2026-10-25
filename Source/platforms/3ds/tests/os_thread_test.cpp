#include "../runtime/bootstrap.hpp"
#include "../runtime/os_thread.hpp"
#include "../runtime/os_sync.hpp"
#include "../runtime/clock.hpp"
#include "../runtime/thread_backend.hpp"
#include "JSystem/JKernel/JKRThread.h"
#include "JSystem/JKernel/JKRExpHeap.h"
#include "JSystem/JKernel/JKRSolidHeap.h"
#include "JSystem/JAudio2/JASTaskThread.h"
#include "helpers/native_thread.h"
#include <dolphin/os.h>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace dusk::native {
int testLastRequestedCore();
void logText(const char* text) { std::fputs(text, stderr); }
[[noreturn]] void fatal(const char* text) { logText(text); std::abort(); }
void waitForRetrace() { std::abort(); }
void invalidateCache(void*, std::uint32_t) { std::atomic_thread_fence(std::memory_order_seq_cst); }
void flushCache(void*, std::uint32_t) { std::atomic_thread_fence(std::memory_order_seq_cst); }
std::uint64_t counterTicks() {
    const auto ns = platformThreadTime();
    return ns / 1000000000 * nativeTickFrequency + ns % 1000000000 * nativeTickFrequency / 1000000000;
}
std::uint64_t wallMilliseconds() { return epoch1900To2000Ms; }
}
namespace {
template<class F> void eventually(F predicate) {
    const auto start = std::chrono::steady_clock::now();
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(3));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
struct Work {
    OSThread* expected{};
    OSMessageQueue queue{};
    OSMessage storage[1]{};
    std::atomic<unsigned> calls{}, unwound{};
    std::atomic<bool> locked{};
    bool exit{}, sender{}, mutex{}, condition{};
    OSMutex lock{};
    OSCond cond{};
    JKRHeap* heap{};
};
void init(Work& work) {
    OSInitMessageQueue(&work.queue, work.storage, 1);
    OSInitMutex(&work.lock); OSInitCond(&work.cond);
    work.heap = JKRHeap::getCurrentHeap();
}
void* workEntry(void* argument) {
    auto& work = *static_cast<Work*>(argument);
    assert(OSGetCurrentThread() == work.expected);
    struct Unwind {
        Work& work;
        ~Unwind() {
            // Real cleanup must acquire/free the original heap after cancellation
            // without raising a second exit exception during C++ unwinding.
            auto* memory = work.heap->alloc(32, 8); assert(memory);
            work.heap->free(memory); ++work.unwound;
        }
    } unwind{work};
    OSSetThreadSpecific(0, &work); assert(OSGetThreadSpecific(0) == &work && OSGetThreadSpecific(1) == nullptr);
    ++work.calls;
    if (work.exit) OSExitThread(reinterpret_cast<void*>(0x12345678));
    if (work.mutex) { OSLockMutex(&work.lock); OSLockMutex(&work.lock); work.locked = true; }
    if (work.condition) {
        OSLockMutex(&work.lock); work.locked = true;
        while (work.condition) OSWaitCond(&work.cond, &work.lock);
    }
    for (;;) {
        if (work.sender) OSSendMessage(&work.queue, &work, OS_MESSAGE_BLOCK);
        else {
            OSMessage message;
            if (!OSReceiveMessage(&work.queue, &message, OS_MESSAGE_BLOCK)) return &work;
            if (message == reinterpret_cast<void*>(1)) return &work;
        }
        ++work.calls;
    }
}
struct SleepWork { OSThreadQueue queue{}; std::atomic<unsigned> calls{}; bool paused{true}; bool conditional{}; };
void* sleeping(void* argument) {
    auto& work = *static_cast<SleepWork*>(argument);
    ++work.calls;
    if (work.conditional) dusk::native::sleepThreadWhile(&work.queue, &work.paused);
    else OSSleepThread(&work.queue);
    ++work.calls;
    return argument;
}
struct OriginalWorker : JKRThread {
    std::atomic<bool> entered{};
    explicit OriginalWorker(JKRHeap* heap) : JKRThread(heap, 3072, 1, 16) {}
    ~OriginalWorker() override { OSCancelThread(getThreadRecord()); }
    void* run() override {
        assert(OSGetCurrentThread() == getThreadRecord());
        assert(JKRHeap::getCurrentHeap());
        entered = true;
        BOOL received;
        waitMessageBlock(&received);
        return this;
    }
};
struct CopiedCommand { std::atomic<unsigned>* calls; std::uint64_t marker; };
void countCommand(void* value) { ++*static_cast<std::atomic<unsigned>*>(value); }
void copiedCommand(void* value) {
    auto& command = *static_cast<CopiedCommand*>(value);
    assert(command.marker == 0x0123456789abcdef);
    ++*command.calls;
}
void waitingCommand(void* value) {
    auto& work = *static_cast<Work*>(value);
    ++work.calls;
    OSReceiveMessage(&work.queue, nullptr, OS_MESSAGE_BLOCK);
}
struct ReturningIO {
    std::mutex lock;
    std::condition_variable changed;
    std::atomic<bool> entered{};
    bool released{};
};
void returningIOCommand(void* value) {
    auto& io = *static_cast<ReturningIO*>(value);
    std::unique_lock guard(io.lock);
    io.entered = true;
    io.changed.wait(guard, [&] { return io.released; });
    // Return normally after real foreign I/O. Original PendingCommand cleanup
    // must free through its actual heap/mutex before the next OS wait cancels.
}
struct JoinWork { OSThread* child; std::atomic<bool> entered{}; };
void* joining(void* value) {
    auto& work = *static_cast<JoinWork*>(value);
    work.entered = true;
    void* result{};
    assert(OSJoinThread(work.child, &result));
    return result;
}
}
int main() {
    using namespace dusk::native;
    alignas(32) static std::array<unsigned char, 512 * 1024> memory;
    assert(initializeKernel(memory.data(), memory.size()));
    auto* heap = JKRHeap::getCurrentHeap();
    auto* main = OSGetCurrentThread();
    const auto before = kernelStats();
    assert(main && !OSIsThreadTerminated(main) && threadStats().workers == 0);
    OSSetThreadSpecific(1, heap);
    assert(!OSCreateThread(nullptr, workEntry, nullptr, nullptr, 3072, 16, 0));
    OSThread invalid{};
    assert(!createThreadOnCore(&invalid,workEntry,nullptr,nullptr,3072,16,0,3));
    assert(!OSCreateThread(&invalid, nullptr, nullptr, nullptr, 3072, 16, 0));
    assert(!OSCreateThread(&invalid, workEntry, nullptr, nullptr, 3072, 32, 0));
    assert(!OSCreateThread(&invalid, workEntry, nullptr, nullptr, 0, 16, 0));
    assert(!OSCreateThread(&invalid, workEntry, nullptr, nullptr, 3072, 16, 2));
    OSMessageQueue empty{}; OSInitMessageQueue(&empty, nullptr, 0);
    assert(!OSSendMessage(&empty, nullptr, OS_MESSAGE_NOBLOCK));
    assert(!OSReceiveMessage(&empty, nullptr, OS_MESSAGE_NOBLOCK));
    for (unsigned iteration = 0; iteration < 32; ++iteration) {
        OSThread record{}; Work work{}; init(work); work.expected = &record;
        if(iteration%2){assert(createThreadOnCore(&record,workEntry,&work,nullptr,3072,16,0,2));assert(testLastRequestedCore()==2);}
        else {assert(OSCreateThread(&record, workEntry, &work, nullptr, 3072, 16, 0));assert(testLastRequestedCore()==-2);}
        assert(!OSCreateThread(&record, workEntry, &work, nullptr, 3072, 16, 0));
        assert(OSIsThreadSuspended(&record) && !work.calls);
        assert(OSResumeThread(&record) == 1);
        eventually([&] { return work.calls.load() == 1; });
        assert(OSSuspendThread(&record) == 0);
        assert(OSSuspendThread(&record) == 1);
        assert(OSSendMessage(&work.queue, reinterpret_cast<void*>(1), OS_MESSAGE_NOBLOCK));
        assert(OSResumeThread(&record) == 2 && OSIsThreadSuspended(&record));
        assert(work.calls == 1 && !OSIsThreadTerminated(&record));
        assert(OSResumeThread(&record) == 1);
        void* result{};
        assert(OSJoinThread(&record, &result) && result == &work);
        assert(OSIsThreadTerminated(&record) && work.unwound == 1 && !OSJoinThread(&record, nullptr));
    }
    for (unsigned kind = 0; kind < 5; ++kind) {
        OSThread record{}; Work work{}; init(work); work.expected = &record;
        work.sender = kind == 1; work.mutex = kind == 2; work.condition = kind == 3; work.exit = kind == 4;
        if (work.sender) assert(OSSendMessage(&work.queue, &work, OS_MESSAGE_NOBLOCK));
        assert(OSCreateThread(&record, workEntry, &work, nullptr, 4096, 0, 0));
        for (int priority = 0; priority <= 31; ++priority) {
            assert(OSSetThreadPriority(&record, priority) && OSGetThreadPriority(&record) == priority);
        }
        assert(OSResumeThread(&record) == 1);
        eventually([&] { return work.calls.load() == 1; });
        if (work.mutex || work.condition) eventually([&] { return work.locked.load(); });
        if (work.condition) {
            // Confirm the real condition wait releases this mutex and allows
            // another original OSThread to link/unlink it before cancellation.
            eventually([&] { return OSTryLockMutex(&work.lock) != FALSE; });
            assert(main->queueMutex.head == &work.lock);
            OSUnlockMutex(&work.lock);
        }
        if (work.exit) {
            void* result{}; assert(OSJoinThread(&record, &result) && result == reinterpret_cast<void*>(0x12345678));
        } else {
            OSDetachThread(&record); assert(!OSJoinThread(&record, nullptr));
            OSCancelThread(&record);
        }
        assert(work.unwound == 1 && OSIsThreadTerminated(&record));
        assert(OSTryLockMutex(&work.lock)); OSUnlockMutex(&work.lock);
        assert(!record.queueMutex.head && !record.queue);
    }
    SleepWork sleep{}; OSInitThreadQueue(&sleep.queue); OSThread record{};
    assert(OSCreateThread(&record, sleeping, &sleep, nullptr, 4096, 16, 0));
    OSResumeThread(&record); eventually([&] { return threadStats().waitingQueues == 1; });
    assert(OSSuspendThread(&record) == 0);
    OSWakeupThread(&sleep.queue); assert(OSResumeThread(&record) == 1);
    void* result{}; assert(OSJoinThread(&record, &result) && result == &sleep && sleep.calls == 2);
    for (unsigned kind = 0; kind < 3; ++kind) {
        SleepWork conditional{}; conditional.conditional = true;
        OSInitThreadQueue(&conditional.queue);
        assert(OSCreateThread(&record, sleeping, &conditional, nullptr, 4096, 16, 0));
        if (!kind) {
            // Resume-before-enqueue: a real wake on an empty queue must still
            // leave the cleared predicate observable at the eventual sleep.
            std::atomic_ref<bool>(conditional.paused).store(false, std::memory_order_release);
            OSWakeupThread(&conditional.queue);
        }
        OSResumeThread(&record);
        if (kind) {
            eventually([&] { return threadStats().waitingQueues == 1; });
            if (kind == 2) {
                OSCancelThread(&record);
                assert(conditional.calls == 1 && threadStats().waitingQueues == 0);
                continue;
            }
            std::atomic_ref<bool>(conditional.paused).store(false, std::memory_order_release);
            OSWakeupThread(&conditional.queue);
        }
        assert(OSJoinThread(&record, &result) && result == &conditional && conditional.calls == 2);
    }
    std::array<OSThread, 25> records{};
    for (unsigned i = 0; i < 24; ++i) assert(OSCreateThread(&records[i], workEntry, nullptr, nullptr, 3072, 16, 0));
    assert(!OSCreateThread(&records[24], workEntry, nullptr, nullptr, 3072, 16, 0));
    for (unsigned i = 0; i < 24; ++i) OSCancelThread(&records[i]); // Cancel before original callbacks execute.
    for (unsigned i = 0; i < 4; ++i) assert(OSCreateThread(&records[i], workEntry, nullptr, nullptr, 512 * 1024, 16, 0));
    assert(threadStats().stackBytes == 2 * 1024 * 1024);
    assert(!OSCreateThread(&records[4], workEntry, nullptr, nullptr, 3072, 16, 0));
    for (unsigned i = 0; i < 4; ++i) OSCancelThread(&records[i]);
    OSThread child{}, parent{}; Work childWork{}; init(childWork); childWork.expected = &child;
    assert(OSCreateThread(&child, workEntry, &childWork, nullptr, 4096, 16, 0));
    OSResumeThread(&child); eventually([&] { return childWork.calls.load() == 1; });
    JoinWork join{&child};
    assert(OSCreateThread(&parent, joining, &join, nullptr, 4096, 16, 0));
    OSResumeThread(&parent); eventually([&] { return join.entered.load(); });
    OSCancelThread(&parent);
    assert(OSIsThreadTerminated(&parent) && !OSIsThreadTerminated(&child));
    assert(OSSendMessage(&childWork.queue, reinterpret_cast<void*>(1), OS_MESSAGE_NOBLOCK));
    assert(OSJoinThread(&child, &result) && result == &childWork);
    OSThread blockedRecord{}; Work blockedWork{}; init(blockedWork);
    blockedWork.expected = &blockedRecord; blockedWork.mutex = true;
    OSLockMutex(&blockedWork.lock);
    assert(OSCreateThread(&blockedRecord, workEntry, &blockedWork, nullptr, 4096, 16, 0));
    OSResumeThread(&blockedRecord); eventually([&] { return blockedWork.calls.load() == 1; });
    OSCancelThread(&blockedRecord); // Exit while contending for another owner's lock.
    assert(blockedWork.unwound == 1 && !blockedWork.locked && blockedWork.lock.thread == main);
    OSUnlockMutex(&blockedWork.lock);
    for (unsigned i = 0; i < 8; ++i) {
        auto* poisoned = heap->alloc(sizeof(OriginalWorker), 8);
        assert(poisoned); std::memset(poisoned, 0xa5, sizeof(OriginalWorker)); heap->free(poisoned);
        auto* worker = JKR_NEW_ARGS(heap, 0) OriginalWorker(heap);
        assert(worker && !worker->getStack());
        worker->resume(); eventually([&] { return worker->entered.load(); });
        JKR_DELETE(worker);
        assert(kernelStats().free == before.free && kernelStats().healthy && threadStats().workers == 0);
    }
    // Exercise the actual JAS command pool, JKR heap binding, task worker and
    // critical section. No replacement JAS worker or successful audio flag.
    auto* audioRoot = JKRSolidHeap::create(384 * 1024, heap, false); assert(audioRoot);
    JASKernel::setupRootHeap(audioRoot, 128 * 1024);
    const auto commandFree = JASKernel::getSystemHeap()->getFreeSize();
    std::atomic<unsigned> commands{};
    for (unsigned kind = 0; kind < 4; ++kind) {
        auto* task = JKR_NEW_ARGS(heap, 0) JASTaskThread(16, 4, 4096); assert(task);
        task->pause(true);
        assert(task->sendCmdMsg(countCommand, &commands));
        if (!kind) task->pause(false); // Unpause before the task starts.
        task->resume();
        if (kind) eventually([&] { return threadStats().waitingQueues == 1; });
        if (kind < 2) {
            task->pause(false);
            eventually([&] { return commands.load() == kind + 1; });
            CopiedCommand copy{&commands, 0x0123456789abcdef};
            assert(task->sendCmdMsg(copiedCommand, &copy, sizeof(copy)));
            copy.marker = 0; // The actual copy is independent of caller storage.
            eventually([&] { return commands.load() == kind + 2; });
            commands = kind + 1;
        } else if (kind == 2) {
            // A paused in-flight command plus queued commands must all be
            // freed by real cancellation unwinding and the derived destructor.
            assert(task->sendCmdMsg(countCommand, &commands));
            assert(task->sendCmdMsg(countCommand, &commands));
        } else {
            Work blocked{}; init(blocked);
            task->pause(false);
            eventually([&] { return commands.load() == 3; });
            assert(task->sendCmdMsg(waitingCommand, &blocked));
            eventually([&] { return blocked.calls.load() == 1; });
            JKR_DELETE(task); task = nullptr;
            assert(blocked.calls == 1);
        }
        if (task) JKR_DELETE(task);
        assert(threadStats().workers == 0 && threadStats().waitingQueues == 0);
        assert(JASKernel::getSystemHeap()->getFreeSize() == commandFree);
    }
    assert(commands == 3);
    ReturningIO io;
    auto* task = JKR_NEW_ARGS(heap, 0) JASTaskThread(16, 4, 4096); assert(task);
    task->resume(); assert(task->sendCmdMsg(returningIOCommand, &io));
    eventually([&] { return io.entered.load(); });
    const auto incomplete = shutdownWorkers(60000000);
    assert(incomplete.status == WorkerShutdownStatus::timedOut && incomplete.remaining.workers == 1);
    assert(incomplete.remaining.stackBytes && threadStats().workers == 1);
    { std::lock_guard guard(io.lock); io.released = true; io.changed.notify_all(); }
    const auto stopped = shutdownWorkers();
    assert(stopped.status == WorkerShutdownStatus::complete && !stopped.remaining.workers && !stopped.remaining.stackBytes && !stopped.remaining.waitingQueues);
    JKR_DELETE(task);
    assert(JASKernel::getSystemHeap()->getFreeSize() == commandFree);
    assert(shutdownWorkers(0).status == WorkerShutdownStatus::complete);
    audioRoot->destroy();
    JASDram = nullptr; JASKernel::sSystemHeap = nullptr; JASKernel::sCommandHeap = nullptr;
    assert(kernelStats().free == before.free && kernelStats().healthy);
    assert(OSGetCurrentThread() == main && OSGetThreadSpecific(1) == heap);
    assert(threadStats().workers == 0 && !threadStats().stackBytes);
    std::puts("PASS: actual host workers, original TLS, suspended start/nesting, full-width join/exit, cancellation/cleanup, predicate wake races, bounded capacity/stacks, original JKR destruction and real JAS command copy/pause/cancellation/heap restoration");
}
