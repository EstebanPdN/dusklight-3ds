#include "thread_backend.hpp"
#include "clock.hpp"
#include "bootstrap.hpp"
#include <3ds.h>

namespace dusk::native {
bool platformIsMainThread() { return threadGetCurrent() == nullptr; }
namespace { struct Trampoline{void(*entry)(void*);void* arg;}; void trampoline(void* p){
    const Trampoline t=*static_cast<Trampoline*>(p);delete static_cast<Trampoline*>(p);
    installExceptionLogger();t.entry(t.arg);} }
NativeThreadHandle platformCreateThread(void (*entry)(void*), void* arg, std::size_t size, int priority, int core) {
    // Keep all native handles joinable until the actual worker is stopped.
    // libctru owns the real stack/TLS allocation; no private Thread layout access.
    auto* boxed=new Trampoline{entry,arg};
    auto* thread = threadCreate(trampoline, boxed, size, priority, core, false);
    if(!thread)delete boxed;
    if (!thread) return {};
    // OSCreateThread holds its state lock; the new worker cannot pass its
    // suspended-start checkpoint and exit before this public handle capture.
    return {thread, threadGetHandle(thread)};
}
bool platformJoinThread(NativeThreadHandle input, std::uint64_t timeout) {
    // libctru's threadJoin may return immediately after its finished flag is
    // set, before svcExitThread. Wait on the captured actual kernel handle so
    // the real stack/TLS cannot be freed while the thread is still exiting.
    return input && R_SUCCEEDED(svcWaitSynchronization(input.kernel, timeout));
}
void platformFreeThread(NativeThreadHandle input) { threadFree(static_cast<Thread>(input.owner)); }
bool platformSetThreadPriority(NativeThreadHandle input, int priority) {
    const auto handle = input ? input.kernel : CUR_THREAD_HANDLE;
    return R_SUCCEEDED(svcSetThreadPriority(handle, priority));
}
int platformGetThreadPriority(NativeThreadHandle input) {
    const auto handle = input ? input.kernel : CUR_THREAD_HANDLE;
    s32 priority;
    return R_SUCCEEDED(svcGetThreadPriority(&priority, handle)) ? priority : -1;
}
void platformThreadSleep(std::int64_t nanoseconds) { svcSleepThread(nanoseconds); }
std::uint64_t platformThreadTime() {
    const auto ticks = std::uint64_t(svcGetSystemTick());
    return ticks / nativeTickFrequency * 1000000000 + ticks % nativeTickFrequency * 1000000000 / nativeTickFrequency;
}
}
