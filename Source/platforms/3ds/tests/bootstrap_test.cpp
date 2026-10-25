#include "../runtime/bootstrap.hpp"
#include "../runtime/clock.hpp"
#include "../runtime/os_sync.hpp"
#include "JSystem/JKernel/JKRExpHeap.h"
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {
void* invalidated;
std::uint32_t invalidatedSize;
unsigned retraces;
}
extern "C" void VIWaitForRetrace();
extern "C" int stricmp(const char*,const char*);
extern "C" int strnicmp(const char*,const char*,int);
namespace dusk::native {
void logText(const char* text) { std::fputs(text, stderr); }
[[noreturn]] void fatal(const char* text) { logText(text); std::abort(); }
void waitForRetrace() { ++retraces; }
void invalidateCache(void* memory, std::uint32_t size) { invalidated=memory; invalidatedSize=size; std::atomic_thread_fence(std::memory_order_seq_cst); }
void flushCache(void*, std::uint32_t) { std::atomic_thread_fence(std::memory_order_seq_cst); }
std::uint64_t counterTicks() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() * (nativeTickFrequency / 1000);
}
std::uint64_t wallMilliseconds() { return epoch1900To2000Ms; }
}
int main() {
    using namespace dusk::native;
    alignas(32) static std::array<unsigned char, 256 * 1024> memory;
    assert(initializeKernel(memory.data(), memory.size()));
    DCInvalidateRange(memory.data(),32);
    assert(invalidated==memory.data() && invalidatedSize==32);
    VIWaitForRetrace(); assert(retraces==1);
    assert(stricmp("Stage","sTaGe")==0 && stricmp("A","b")<0 && stricmp("C","b")>0);
    assert(strnicmp("PACKroom","packXXXX",4)==0&&strnicmp("PACKroom","packXXXX",5)<0);
    assert(strnicmp("ab","ABc",3)<0&&strnicmp("abC","AB",3)>0);
    assert(strnicmp(nullptr,nullptr,0)==0&&strnicmp(nullptr,nullptr,-1)==0);
    const auto before = kernelStats();
    assert(before.healthy && before.free && before.budget <= memory.size());
    auto* mainIdentity = OSGetCurrentThread();
    auto* mainHeap = JKRHeap::getCurrentHeap();
    assert(mainHeap && mainIdentity);
    // Byte-sized path buffers must not misalign the next block's pointer fields.
    for (int alignment : {0, 1, 2, 4, 8, -1, -2, -4, -8}) {
        std::array<void*, 31> blocks{};
        for (unsigned size = 1; size <= blocks.size(); ++size) {
            blocks[size - 1] = mainHeap->alloc(size, alignment);
            assert(blocks[size - 1] && uintptr_t(blocks[size - 1]) % alignof(JKRExpHeap::CMemBlock) == 0);
        }
        for (auto* block : blocks) mainHeap->free(block);
        assert(kernelStats().free == before.free && kernelStats().healthy);
    }
    OSMessageQueue queue{}; OSMessage storage[4]; OSInitMessageQueue(&queue, storage, 4);
    std::array<std::thread, 4> workers;
    for (auto& thread : workers) thread = std::thread([&] {
        assert(OSGetCurrentThread() != mainIdentity);
        assert(kernelWorkerCheck());
        assert(OSSendMessage(&queue, reinterpret_cast<OSMessage>(1), OS_MESSAGE_BLOCK));
    });
    for (unsigned i = 0; i < workers.size(); ++i) {
        OSMessage result; assert(OSReceiveMessage(&queue, &result, OS_MESSAGE_BLOCK));
        assert(result == reinterpret_cast<OSMessage>(1));
    }
    for (auto& thread : workers) thread.join();
    closeMessageQueue(&queue);
    assert(JKRHeap::getCurrentHeap() == mainHeap);
    assert(OSGetCurrentThread() == mainIdentity);
    assert(kernelStats().free == before.free && kernelStats().healthy);
    assert(OSGetArenaHi() == OSGetArenaLo());
    assert(!initializeKernel(memory.data(), memory.size()));
    std::puts("PASS: real JKR + arena + recursive mutex/queue + TLS; four concurrent workers, no leaked blocks");
}
