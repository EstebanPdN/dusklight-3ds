#pragma once
#include <cstddef>
#include <cstdint>

namespace dusk::native {
// Production implements these with the public libctru thread API. Host tests
// use a separate backend with actual std::threads, never linked into the game.
struct NativeThreadHandle {
    void* owner{};
    std::uint32_t kernel{};
    explicit operator bool() const { return owner != nullptr; }
};
NativeThreadHandle platformCreateThread(void (*entry)(void*), void* arg, std::size_t stackBytes, int priority, int core);
bool platformJoinThread(NativeThreadHandle handle, std::uint64_t timeoutNanoseconds);
void platformFreeThread(NativeThreadHandle handle);
bool platformSetThreadPriority(NativeThreadHandle handle, int priority);
int platformGetThreadPriority(NativeThreadHandle handle);
void platformThreadSleep(std::int64_t nanoseconds);
std::uint64_t platformThreadTime(); // Monotonic nanoseconds, not wall time.
bool platformIsMainThread(); // Public libctru identity; includes foreign-worker rejection.
}
