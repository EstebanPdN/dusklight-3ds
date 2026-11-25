// The C++ runtime's exception state, one block per thread.
//
// libstdc++ keeps, per thread, the exceptions being handled (catch blocks
// entered) and, on the ARM EH ABI, the exceptions running cleanups
// (destructors during an unwind). __cxa_begin_catch, __cxa_rethrow,
// __cxa_begin_cleanup and __cxa_end_cleanup push and pop those two lists, and
// `throw;` rethrows whatever heads the first one. libstdc++ finds the block
// through a pthread key; devkitARM's pthread_key_create returns ENOSYS
// (libctru provides no TLS-key syscalls), so libstdc++ falls back to ONE
// static block for the whole process (_S_init stays 0: Alpha 15 console dump,
// address 0x014efd48). Two threads unwinding at once then pop or rethrow each
// other's exceptions. The worker barrier at exit cancels every game worker at
// once (ThreadExit, os_thread.cpp), the DVD thread unwinds on core 2 while the
// JKR, memory-card and JAS threads unwind on core 0, and phase 2 of one of
// those unwinds ran off its own stack: abort() from unwind_phase2 after
// "Native shutdown phase=worker barrier" (Alpha 13 and 15 on the console).
//
// The image links every libstdc++ caller of __cxa_get_globals(_fast) to these
// two functions instead (-Wl,--wrap, CMakeLists.txt); every one of those
// callers is in another object of libstdc++ than the static block, so --wrap
// reaches all of them. Tools/3ds/check_eh_globals.py proves it on the linked
// image after each build. The block is thread_local, as os_thread.cpp's
// `current` is: libctru gives the main thread and every threadCreate thread
// their own zeroed TLS.
#include <cstdint>

namespace {
// __cxa_eh_globals for __ARM_EABI_UNWINDER__ (unwind-cxx.h): caught
// exceptions, uncaught count, propagating (cleanup) exceptions. libstdc++'s
// own allocation of it is 12 bytes, zeroed.
struct EhGlobals {
    void* caughtExceptions;
    unsigned int uncaughtExceptions;
    void* propagatingExceptions;
};
static_assert(sizeof(EhGlobals) == 3 * sizeof(void*), "ARM EH ABI exception globals are three words");
thread_local EhGlobals threadGlobals{};
}

extern "C" {
void* __wrap___cxa_get_globals() noexcept { return &threadGlobals; }
void* __wrap___cxa_get_globals_fast() noexcept { return &threadGlobals; }
}
