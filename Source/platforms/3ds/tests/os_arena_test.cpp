#include "../runtime/os_arena.hpp"
#include <dolphin/os.h>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>

int main() {
    assert(OSGetArenaLo() == nullptr && OSGetArenaHi() == nullptr);
    assert(OSAllocFromArenaLo(1, 4) == nullptr);
    assert(!dusk::native::configureGameArena(nullptr, 4096));
    assert(!dusk::native::configureGameArena(reinterpret_cast<void*>(UINTPTR_MAX - 31), 64));
    alignas(32) std::array<unsigned char, 4096> memory;
    assert(dusk::native::configureGameArena(memory.data(), memory.size()));
    assert(!dusk::native::configureGameArena(memory.data(), memory.size()));
    assert(OSGetPhysicalMemSize() == memory.size());
    auto* a = OSAllocFromArenaLo(3, 32);
    auto* b = OSAllocFromArenaHi(3, 32);
    assert(a == memory.data() && b == memory.data() + memory.size() - 32);
    assert(OSGetArenaLo() == memory.data() + 32);
    assert(OSGetArenaHi() == b);
    for (u32 alignment : {0u, 3u}) {
        assert(OSAllocFromArenaLo(1, alignment) == nullptr);
        assert(OSAllocFromArenaHi(1, alignment) == nullptr);
    }
    assert(OSAllocFromArenaLo(UINT32_MAX, 32) == nullptr);
    assert(OSAllocFromArenaHi(UINT32_MAX, 32) == nullptr);
    assert(OSGetArenaLo() == memory.data() + 32 && OSGetArenaHi() == b);
    assert(OSAllocFromArenaLo(4096 - 64, 32) == memory.data() + 32);
    assert(OSGetArenaLo() == OSGetArenaHi());
    assert(!OSAllocFromArenaLo(1, 32) && !OSAllocFromArenaHi(1, 32));
    std::puts("PASS: bounded arena ownership, alignment, overflow, head/tail and exhaustion");
}
