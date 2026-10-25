// Test-only implementation of the OS adapter's synchronization primitives.
#include "../runtime/primitives.hpp"
#include <dolphin/os.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace {
constexpr unsigned capacity = 256;
std::array<std::mutex, capacity> locks;
std::array<std::condition_variable, capacity> conditions;
std::atomic<unsigned> nextLock{}, nextCondition{};
unsigned reserve(std::atomic<unsigned>& next) {
    const auto index = next.fetch_add(1);
    if (index >= capacity) std::abort();
    return index;
}
}
namespace dusk::native {
void lockInit(std::int32_t* value) { *value = reserve(nextLock); }
void lock(std::int32_t* value) { locks.at(*value).lock(); }
bool tryLock(std::int32_t* value) { return locks.at(*value).try_lock(); }
void unlock(std::int32_t* value) { locks.at(*value).unlock(); }
void conditionInit(std::int32_t* value) { *value = reserve(nextCondition); }
void conditionWait(std::int32_t* condition, std::int32_t* lock) {
    std::unique_lock held(locks.at(*lock), std::adopt_lock);
    conditions.at(*condition).wait(held);
    held.release();
}
void conditionBroadcast(std::int32_t* value) { conditions.at(*value).notify_all(); }
void conditionWaitTimeout(std::int32_t* condition, std::int32_t* lock, std::int64_t nanoseconds) {
    std::unique_lock held(locks.at(*lock), std::adopt_lock);
    conditions.at(*condition).wait_for(held, std::chrono::nanoseconds(nanoseconds));
    held.release();
}
}
// The integration test uses bootstrap.cpp's production TLS identity and panic
// routing; the isolated adapter tests retain their standalone environment.
#if !defined(DUSK_TEST_KERNEL)
extern "C" {
OSThread* OSGetCurrentThread() {
    static thread_local OSThread record{};
    return &record;
}
[[noreturn]] void OSPanic(const char*, int, const char* format, ...) {
    va_list args; va_start(args, format); std::vfprintf(stderr, format, args); va_end(args);
    std::abort();
}
}
#endif
