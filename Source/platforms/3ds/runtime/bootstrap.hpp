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
// Main-thread teardown could not prove ownership safe. Preserve live storage.
[[noreturn, gnu::format(printf,1,2)]] void containShutdownFailure(const char* format,...);
// A thread that must not end the program itself hands a fatal error to the
// main thread: the hook returns true when it took the text (and the caller
// then never runs again). Set by the second-core worker (gx_worker_ctru.cpp):
// only the main thread can show the message and restart an automatic test.
inline bool (*fatalRelayHook)(const char* text)=nullptr;
// The same for restartAfterGpuHang (the GPU guard met a stopped PICA on that
// thread): the main thread shows the message and stops the game.
inline bool (*gpuHangRelayHook)(const char* text,bool restart)=nullptr;
// Unattended automatic test: a fatal error shows its message briefly and asks
// the system to start the game again, so the test continues with the next zone.
void setFatalUnattended(bool unattended);
// Console GPU guard (gpu_canary.hpp): message, then restart or wait for START.
[[noreturn]] void restartAfterGpuHang(const char* text,bool restart);
// Per-thread ARM11 exception report into the SD log (call at thread entry).
void installExceptionLogger();
// Write pending native log lines to sdmc:/3ds/DuskLight/log.txt now (any
// thread; one writer at a time, the SD writes run outside the ring's lock).
void flushNativeLog();
// The frame loop's flush: at most every 5 seconds, sooner once a quarter of
// the ring is waiting (main.cpp). It only wakes the SD writer thread: the
// writes never run on the game thread (console Alpha 10: 27-110 ms per flush
// on average, up to 5 s, all of it a frozen picture).
void flushNativeLogPeriodic();
// Wake the SD writer thread (log flush and the GPU guard's session record).
void kickNativeSdWriter();
// Wake the writer without falling back to synchronous I/O.
bool kickNativeSdWriterAsync();
// One render-side waiter rechecks its slot after each completion notification.
void waitForNativeCaptureProgress();
void signalNativeCaptureProgress();
void flushCache(void* memory, std::uint32_t size);
// Development switch: forward the game's own cache flushes to the kernel again.
void setGameCacheFlush(bool forward);
void invalidateCache(void* memory, std::uint32_t size);
void waitForRetrace();
}
