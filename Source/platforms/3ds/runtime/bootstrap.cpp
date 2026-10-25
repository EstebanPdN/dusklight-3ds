#include "bootstrap.hpp"
#include "clock.hpp"
#include "os_arena.hpp"
#include "JSystem/JKernel/JKRExpHeap.h"
#include "JSystem/JKernel/JKRSolidHeap.h"
#include "JSystem/JUtility/JUTException.h"
#include "JSystem/JUtility/JUTConsole.h"
#include "os_report.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace {
JKRExpHeap* root;
std::uint32_t peak;
thread_local OSThread* boundThread;
void report(const char* format, va_list args) {
    char text[256];
    std::vsnprintf(text, sizeof(text), format, args);
    dusk::native::logText(text);
}
}
// libctru initializes static-executable TLS for main and threadCreate workers.
// Managed native workers bind their original record before entering game code.
extern "C" OSThread* OSGetCurrentThread() {
    static thread_local OSThread identity = [] {
        OSThread value{}; value.state = OS_THREAD_STATE_RUNNING;
        return value;
    }();
    return boundThread ? boundThread : &identity;
}
void dusk::native::bindCurrentThread(OSThread* record) { boundThread = record; }
extern "C" void DCInvalidateRange(void* memory, u32 size) { dusk::native::invalidateCache(memory, size); }
extern "C" void DCFlushRange(void* memory, u32 size) { dusk::native::flushCache(memory, size); }
extern "C" void DCStoreRange(void* memory, u32 size) { dusk::native::flushCache(memory, size); }
extern "C" void DCStoreRangeNoSync(void* memory, u32 size) { dusk::native::flushCache(memory, size); }
extern "C" void VIWaitForRetrace() { dusk::native::waitForRetrace(); }
extern "C" int stricmp(const char* first, const char* second) { return ::strcasecmp(first, second); }
extern "C" int strnicmp(const char* first, const char* second, int count) {
    // Original callers use an int length; non-positive lengths compare no bytes.
    return count>0 ? ::strncasecmp(first,second,static_cast<std::size_t>(count)) : 0;
}
extern "C" void PPCSync() { __sync_synchronize(); }
extern "C" void OSReport(const char* format, ...) {
    va_list args; va_start(args, format); report(format, args); va_end(args);
}
extern "C" [[noreturn]] void OSPanic(const char*, int, const char* format, ...) {
    char text[256]; va_list args; va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args); va_end(args);
    dusk::native::fatal(text);
}
#if !defined(DUSK_NATIVE_ORIGINAL_JUT)
void JUTException::panic_f(const char*, int, const char* format, ...) {
    char text[256]; va_list args; va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args); va_end(args);
    dusk::native::fatal(text);
}
#endif
void OSReport_Error(const char* format, ...) {
    va_list args; va_start(args, format); report(format, args); va_end(args);
}
#if !defined(DUSK_NATIVE_ORIGINAL_JUT)
void JUTReportConsole(const char* text) { dusk::native::logText(text); }
void JUTWarningConsole(const char* text) { dusk::native::logText(text); }
void JUTReportConsole_f(const char* format, ...) {
    va_list args; va_start(args, format); report(format, args); va_end(args);
}
void JUTWarningConsole_f(const char* format, ...) {
    va_list args; va_start(args, format); report(format, args); va_end(args);
}

#endif

bool dusk::native::initializeKernel(void* arena, std::size_t size) {
    if (root || !initializeClock() || !configureGameArena(arena, size)) return false;
    root = JKRExpHeap::createRoot(1, false);
    if (!root || !root->check()) return false;
    root->setName("native-bootstrap");
    auto* child = JKRSolidHeap::create(4096, root, false);
    if (!child) return false;
    void* head = child->alloc(64, 32), *tail = child->alloc(64, -32);
    bool okay = head && tail && head != tail && child->check();
    child->destroy();
    root->becomeCurrentHeap();
    root->becomeSystemHeap();
    return okay && root->check() && OSGetArenaLo() == OSGetArenaHi();
}
dusk::native::HeapStats dusk::native::kernelStats() {
    if (!root) return {};
    const auto free = std::uint32_t(root->getTotalFreeSize());
    const auto used = root->getSize() - free;
    if (used > peak) peak = used;
    return {root->getSize(), used, peak, free, root->check()};
}
bool dusk::native::kernelWorkerCheck() {
    if (!root || JKRHeap::getCurrentHeap()) return false;
    JKRHeap::setCurrentHeap(root);
    for (unsigned i = 0; i < 1024; ++i) {
        void* allocation = root->alloc(128, 32);
        if (!allocation) return false;
        std::memset(allocation, 0x5a, 128);
        root->free(allocation);
    }
    JKRHeap::setCurrentHeap(nullptr);
    return root->check();
}
bool dusk::native::attachKernelWorker() {
    if (!root || JKRHeap::getCurrentHeap()) return false;
    JKRHeap::setCurrentHeap(root);
    return true;
}
void dusk::native::detachKernelWorker() {
    if (JKRHeap::getCurrentHeap() != root) fatal("Unexpected loading-worker heap\n");
    JKRHeap::setCurrentHeap(nullptr);
}
