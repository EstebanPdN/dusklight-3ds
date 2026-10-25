// Unit-test-only host arena, synchronization and diagnostic sinks.
// This file must never be included by the New3DS application target.
#include "JSystem/JKernel/JKRHeap.h"
#include "JSystem/JUtility/JUTException.h"
#include "JSystem/JUtility/JUTConsole.h"
#include "os_report.h"
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace {
alignas(32) std::array<unsigned char, 256 * 1024> arena{};
size_t availableArena = arena.size();
size_t arenaLow = 0;
struct LockEntry { OSMutex* key = nullptr; std::recursive_mutex lock; };
std::array<LockEntry, 16> locks;
std::mutex registry;
std::recursive_mutex& lookup(OSMutex* key) {
    std::lock_guard guard(registry);
    for (auto& e : locks) if (e.key == key) return e.lock;
    std::abort();
}
void log_args(const char* format, va_list args) { std::vfprintf(stderr, format, args); }
}
void dusk_test_arena_size(size_t size) { availableArena = size; arenaLow = 0; }
extern "C" {
void* OSGetArenaLo() { return arena.data() + arenaLow; }
void* OSGetArenaHi() { return arena.data() + availableArena; }
void OSSetArenaLo(void* value) { arenaLow = static_cast<unsigned char*>(value) - arena.data(); }
void OSSetArenaHi(void* value) { availableArena = static_cast<unsigned char*>(value) - arena.data(); }
void OSInitMutex(OSMutex* key) {
    std::lock_guard guard(registry);
    for (auto& e : locks) if (e.key == key) return;
    for (auto& e : locks) if (!e.key) { e.key = key; return; }
    std::abort();
}
void OSLockMutex(OSMutex* key) { lookup(key).lock(); }
void OSUnlockMutex(OSMutex* key) { lookup(key).unlock(); }
BOOL OSTryLockMutex(OSMutex* key) { return lookup(key).try_lock(); }
void DCFlushRange(void*, u32) { std::atomic_thread_fence(std::memory_order_seq_cst); }
void OSReport(const char* format, ...) {
    va_list args; va_start(args, format); log_args(format, args); va_end(args);
}
[[noreturn]] void OSPanic(const char*, int, const char* format, ...) {
    va_list args; va_start(args, format); log_args(format, args); va_end(args);
    std::abort();
}
}
void JUTException::panic_f(const char*, int, const char* format, ...) {
    va_list args; va_start(args, format); log_args(format, args); va_end(args);
    std::abort();
}
void OSReport_Error(const char* format, ...) {
    va_list args; va_start(args, format); log_args(format, args); va_end(args);
}
void JUTReportConsole(const char* text) { std::fputs(text, stderr); }
void JUTWarningConsole(const char* text) { std::fputs(text, stderr); }
void JUTReportConsole_f(const char* format, ...) {
    va_list args; va_start(args, format); log_args(format, args); va_end(args);
}
void JUTWarningConsole_f(const char* format, ...) {
    va_list args; va_start(args, format); log_args(format, args); va_end(args);
}
