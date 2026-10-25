// Explicit host-only backend: actual std::threads/joins and monotonic waits.
// Priority values model the native kernel API contract; host scheduling is not
// a New3DS priority/performance test. Never linked into the game CMake target.
#include "../runtime/thread_backend.hpp"
#if defined(DUSK_TEST_WORKER_SHUTDOWN)
#include "../runtime/os_thread.hpp"
#endif
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>
#include <thread>

namespace {
struct HostThread {
    std::thread worker;
    std::mutex lock;
    std::condition_variable changed;
    bool finished{};
    std::atomic<int> priority{};
};
std::atomic<int> mainPriority{0x30},lastCore{-2};
const auto mainThread = std::this_thread::get_id();
std::atomic<unsigned> liveHandles{}, freedHandles{}, successfulJoins{};
std::atomic<unsigned> activeJoins{};
struct Completion {
    HostThread* thread{};
    ~Completion() {
        // Initialized before entry's TLS. Publish only after its destructors,
        // so bounded joins also cover real thread-local cleanup/I/O.
        { std::lock_guard guard(thread->lock); thread->finished = true; }
        thread->changed.notify_all();
    }
};
}
namespace dusk::native {
bool platformIsMainThread() { return std::this_thread::get_id() == mainThread; }
unsigned testLiveThreadHandles() { return liveHandles.load(); }
unsigned testFreedThreadHandles() { return freedHandles.load(); }
unsigned testSuccessfulThreadJoins() { return successfulJoins.load(); }
unsigned testActiveThreadJoins() { return activeJoins.load(); }
int testLastRequestedCore(){return lastCore.load();}
NativeThreadHandle platformCreateThread(void (*entry)(void*), void* argument, std::size_t bytes, int priority, int core) {
    if(core!=-2&&core!=0&&core!=2)return {};
    lastCore=core;
    if (!entry || bytes < 32768 || priority < 0x18 || priority > 0x3f) return {};
    auto* thread = new(std::nothrow) HostThread;
    if (!thread) return {};
    thread->priority = priority;
    try {
        thread->worker = std::thread([=] {
            thread_local Completion completion{thread};
            entry(argument);
        });
    } catch (...) { delete thread; return {}; }
    ++liveHandles;
    return {thread, 0};
}
bool platformJoinThread(NativeThreadHandle input, std::uint64_t timeout) {
#if defined(DUSK_TEST_WORKER_SHUTDOWN)
    (void)threadStats(); // Deadlocks if production holds the state lock here.
#endif
    struct ActiveJoin { ActiveJoin() { ++activeJoins; } ~ActiveJoin() { --activeJoins; } } active;
    auto* thread = static_cast<HostThread*>(input.owner);
    if (!thread) return false;
    std::unique_lock guard(thread->lock);
    if (timeout == UINT64_MAX) thread->changed.wait(guard, [&] { return thread->finished; });
    else if (!thread->changed.wait_for(guard, std::chrono::nanoseconds(timeout), [&] { return thread->finished; })) return false;
    guard.unlock();
    if (thread->worker.joinable()) thread->worker.join();
    ++successfulJoins;
    return true;
}
void platformFreeThread(NativeThreadHandle input) {
#if defined(DUSK_TEST_WORKER_SHUTDOWN)
    (void)threadStats(); // Free must also be outside the production state lock.
#endif
    auto* thread = static_cast<HostThread*>(input.owner);
    assert(!thread->worker.joinable() && "stack/TLS free before actual thread join");
    delete thread; --liveHandles; ++freedHandles;
}
bool platformSetThreadPriority(NativeThreadHandle input, int priority) {
    if (priority < 0x18 || priority > 0x3f) return false;
    if (input) static_cast<HostThread*>(input.owner)->priority = priority; else mainPriority = priority;
    return true;
}
int platformGetThreadPriority(NativeThreadHandle input) {
    return input ? static_cast<HostThread*>(input.owner)->priority.load() : mainPriority.load();
}
void platformThreadSleep(std::int64_t ns) {
    if (ns) std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
    else std::this_thread::yield();
}
std::uint64_t platformThreadTime() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
