#pragma once
#include <cstddef>
#include <cstdint>
struct OSThread;

namespace dusk::native {
struct HeapStats { std::uint32_t budget, used, peak, free; bool healthy; };
// Only JKernel initialization, not game_main or gameplay. Caller retains arena.
bool initializeKernel(void* arena, std::size_t size);
HeapStats kernelStats();
// Called from a real libctru worker to exercise TLS + the production JKR/OS adapters.
bool kernelWorkerCheck();
// Attach the current loading worker after its isolated TLS self-check. Detach
// only after its original JKR objects have been destroyed.
bool attachKernelWorker();
void detachKernelWorker();
// Native OS worker trampoline binds the original caller-owned thread record.
// Null restores the unbound current thread's own TLS identity.
void bindCurrentThread(OSThread* record);
void logText(const char* text);
[[noreturn]] void fatal(const char* text);
// Per-thread ARM11 exception report into the SD log (call at thread entry).
void installExceptionLogger();
// Main thread only: write pending native log lines to sdmc:/3ds/DuskLight/log.txt.
void flushNativeLog();
void flushCache(void* memory, std::uint32_t size);
void invalidateCache(void* memory, std::uint32_t size);
void waitForRetrace();
}
