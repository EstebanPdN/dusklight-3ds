#include <dolphin/gd/GDBase.h>
#include <dolphin/os.h>
#include <cstdint>
#include <limits>

// GD creates CPU-resident command lists. Each native thread owns its current
// builder and callback; no GameCube interrupt/FIFO semantics are simulated here.
extern "C" { thread_local GDLObj* __GDCurrentDL = nullptr; }
namespace {
thread_local GDOverflowCb overflowCallback = nullptr;
[[noreturn]] void fail(const char* message) { OSPanic(__FILE__, __LINE__, "%s", message); }
u32 available(const GDLObj* dl) {
    if (!dl) fail("GD has no current display list");
    const auto start = reinterpret_cast<std::uintptr_t>(dl->start);
    const auto top = reinterpret_cast<std::uintptr_t>(dl->top);
    const auto ptr = reinterpret_cast<std::uintptr_t>(dl->ptr);
    if (!start || start % 32 || dl->length % 32 ||
        dl->length > std::numeric_limits<std::uintptr_t>::max() - start ||
        top != start + dl->length || ptr < start || ptr > top)
        fail("GD display-list bounds are invalid");
    return static_cast<u32>(top - ptr);
}
}
extern "C" {
void GDInitGDLObj(GDLObj* dl, void* start, u32 length) {
    const auto address = reinterpret_cast<std::uintptr_t>(start);
    if (!dl || !address || address % 32 || length % 32 ||
        length > std::numeric_limits<std::uintptr_t>::max() - address)
        fail("GDInitGDLObj requires aligned, bounded storage");
    dl->start = static_cast<u8*>(start);
    dl->length = length;
    dl->ptr = dl->start;
    dl->top = dl->start + length;
}
void GDSetCurrent(GDLObj* dl) {
    if (dl) (void)available(dl);
    __GDCurrentDL = dl;
}
void GDOverflowed() {
    if (!overflowCallback) fail("GD display list overflowed");
    overflowCallback();
}
void GDNativeCheck(u32 size) {
    if (size <= available(__GDCurrentDL)) return;
    GDOverflowed();
    // A callback must actually supply sufficient valid storage. Never keep
    // writing past the buffer merely because a callback returned.
    if (size > available(__GDCurrentDL)) fail("GD overflow callback did not provide enough storage");
}
void GDPadCurr32() {
    const u32 padding = (32 - (GDGetCurrOffset() & 31)) & 31;
    GDNativeCheck(padding);
    for (u32 i = 0; i < padding; ++i) *__GDCurrentDL->ptr++ = 0;
}
void GDFlushCurrToMem() {
    (void)available(__GDCurrentDL);
    DCFlushRange(__GDCurrentDL->start, __GDCurrentDL->length);
}
void GDSetOverflowCallback(GDOverflowCb callback) { overflowCallback = callback; }
GDOverflowCb GDGetOverflowCallback() { return overflowCallback; }
u32 GDNativeGetOffset(const GDLObj* dl) {
    const u32 remaining = available(dl);
    return dl->length - remaining;
}
void GDNativeSetOffset(u32 offset) {
    (void)available(__GDCurrentDL);
    if (offset > __GDCurrentDL->length) fail("GD patch offset exceeds display-list storage");
    __GDCurrentDL->ptr = __GDCurrentDL->start + offset;
}
}
