#include "os_arena.hpp"
#include "primitives.hpp"
#include <dolphin/os.h>
#include <cstdint>

namespace {
uintptr_t begin, end, low, high;
s32 arenaLock;
bool configured;
struct Guard {
    Guard() { dusk::native::lock(&arenaLock); }
    ~Guard() { dusk::native::unlock(&arenaLock); }
};
bool validAlignment(u32 alignment) { return alignment && !(alignment & (alignment - 1)); }
}

bool dusk::native::configureGameArena(void* memory, std::size_t size) {
    const auto address = reinterpret_cast<uintptr_t>(memory);
    if (configured || !memory || !size || size > UINT32_MAX || address > UINTPTR_MAX - size)
        return false;
    lockInit(&arenaLock);
    begin = low = address;
    end = high = address + size;
    configured = true;
    return true;
}

extern "C" {
void* OSGetArenaLo() {
    if (!configured) return nullptr;
    Guard guard;
    return reinterpret_cast<void*>(low);
}
void* OSGetArenaHi() {
    if (!configured) return nullptr;
    Guard guard;
    return reinterpret_cast<void*>(high);
}
void OSSetArenaLo(void* value) {
    if (!configured) OSPanic(__FILE__, __LINE__, "Game arena is not configured");
    Guard guard;
    const auto address = reinterpret_cast<uintptr_t>(value);
    if (address < begin || address > high) OSPanic(__FILE__, __LINE__, "Invalid arena lower bound");
    low = address;
}
void OSSetArenaHi(void* value) {
    if (!configured) OSPanic(__FILE__, __LINE__, "Game arena is not configured");
    Guard guard;
    const auto address = reinterpret_cast<uintptr_t>(value);
    if (address < low || address > end) OSPanic(__FILE__, __LINE__, "Invalid arena upper bound");
    high = address;
}
void* OSAllocFromArenaLo(u32 size, u32 alignment) {
    if (!configured || !validAlignment(alignment)) return nullptr;
    Guard guard;
    const uintptr_t mask = alignment - 1;
    if (low > UINTPTR_MAX - mask) return nullptr;
    const uintptr_t start = (low + mask) & ~mask;
    if (start > high || size > high - start) return nullptr;
    const uintptr_t finish = start + size;
    if (finish > UINTPTR_MAX - mask) return nullptr;
    const uintptr_t next = (finish + mask) & ~mask;
    if (next > high) return nullptr;
    low = next;
    return reinterpret_cast<void*>(start);
}
void* OSAllocFromArenaHi(u32 size, u32 alignment) {
    if (!configured || !validAlignment(alignment)) return nullptr;
    Guard guard;
    const uintptr_t mask = alignment - 1;
    const uintptr_t finish = high & ~mask;
    if (finish < low || size > finish - low) return nullptr;
    const uintptr_t start = (finish - size) & ~mask;
    if (start < low) return nullptr;
    high = start;
    return reinterpret_cast<void*>(start);
}
u32 OSGetPhysicalMemSize() {
    // Dolphin's admitted game memory, not a fabricated native RAM measurement.
    return configured ? u32(end - begin) : 0;
}
}
