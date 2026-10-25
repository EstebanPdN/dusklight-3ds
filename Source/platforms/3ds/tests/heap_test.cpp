// Host contract test of the real JKR allocator with the New3DS allocation policy.
// The separate environment file supplies host OS primitives, not a game runtime.
#include "JSystem/JKernel/JKRExpHeap.h"
#include "JSystem/JKernel/JKRSolidHeap.h"
#include "helpers/gx_helper.h"
#include "m_Do/m_Do_graphic.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <thread>

void dusk_test_arena_size(size_t size);

struct alignas(32) ArrayElement {
    static inline int live = 0;
    int value = ++live;
    ~ArrayElement() { assert(value == live); --live; }
};

int main() {
    assert(JKRHeap::getCurrentHeap() == nullptr);
    assert(operator new(32, JKRHeapToken::Dummy) == nullptr);
    assert(JKRExpHeap::create(1024, nullptr, false) == nullptr);
    assert(JKRSolidHeap::create(1024, nullptr, false) == nullptr);
    dusk_test_arena_size(0);
    assert(JKRExpHeap::createRoot(1, false) == nullptr);
    dusk_test_arena_size(64);
    assert(JKRExpHeap::createRoot(1, false) == nullptr);
    dusk_test_arena_size(256 * 1024);
    auto* root = JKRExpHeap::createRoot(1, false);
    assert(root && root->check());
    assert(JKRExpHeap::createRoot(1, false) == root);
    const auto initialFree = root->getTotalFreeSize();
    auto* small = JKRExpHeap::create(4096, root, false);
    assert(small);
    small->becomeCurrentHeap();
    std::array<void*, 128> allocations{};
    size_t count = 0;
    for (; count < allocations.size(); ++count) {
        auto* p = operator new(128, JKRHeapToken::Dummy, 32);
        if (!p) break;
        assert(reinterpret_cast<uintptr_t>(p) % 32 == 0);
        assert(JKRHeap::findFromRoot(p) == small);
        std::memset(p, 0xA5, 128);
        allocations[count] = p;
    }
    assert(count > 0 && count < allocations.size());
    assert(operator new(8192, JKRHeapToken::Dummy, small, 32) == nullptr);
    assert(operator new(UINT32_MAX, JKRHeapToken::Dummy, small, 32) == nullptr);
    assert(operator new(32, JKRHeapToken::Dummy, small, INT32_MIN) == nullptr);
    assert(operator new(32, JKRHeapToken::Dummy, small, 3) == nullptr);
    assert(small->check());
    for (size_t i = 0; i < count; ++i) {
        auto* p = static_cast<unsigned char*>(allocations[i]);
        for (size_t j = 0; j < 128; ++j) assert(p[j] == 0xA5);
        operator delete(p, JKRHeapToken::Dummy);
    }
    assert(small->check());
    auto* tail = operator new(64, JKRHeapToken::Dummy, -32);
    assert(tail && reinterpret_cast<uintptr_t>(tail) % 32 == 0);
    const auto tailBytes=small->getSize(tail);
    assert(tailBytes>=64);
    for(unsigned n=0;n<unsigned(tailBytes);++n)
        assert(small->getRemainingAllocationSize(static_cast<unsigned char*>(tail)+n)==tailBytes-s32(n));
    assert(small->getRemainingAllocationSize(nullptr)==-1);
    assert(small->getRemainingAllocationSize(static_cast<unsigned char*>(tail)+tailBytes)==-1);
    assert(root->getRemainingAllocationSize(root->getStartAddr())==-1);
    operator delete(tail, JKRHeapToken::Dummy);
    assert(small->getRemainingAllocationSize(tail)==-1);

    // Repeated interior queries across more blocks than the bounded cache,
    // then free/reuse, in-place resize, tail disposal and whole-heap reset.
    auto* ranges=JKRExpHeap::create(32768,root,false);assert(ranges);
    std::array<void*,32> live{};
    for(unsigned round=0;round<256;++round){
        const unsigned slot=(round*13)%live.size();
        if(live[slot]){auto* old=live[slot];ranges->free(old);live[slot]=nullptr;
            assert(ranges->getRemainingAllocationSize(old)==-1);}
        live[slot]=ranges->alloc(64+(round%7)*32,round&1?-32:32);assert(live[slot]);
        if(round%3==0){const auto result=ranges->resize(live[slot],32);assert(result>=32);}
        for(auto* allocation:live)if(allocation){
            const auto size=ranges->getSize(allocation);assert(size>0);
            for(unsigned n:{0u,unsigned(size/2),unsigned(size-1)})
                assert(ranges->getRemainingAllocationSize(static_cast<unsigned char*>(allocation)+n)==size-s32(n));
            assert(ranges->getRemainingAllocationSize(static_cast<unsigned char*>(allocation)+size)==-1);
        }
    }
    ranges->freeAll();for(auto* p:live)if(p)assert(ranges->getRemainingAllocationSize(p)==-1);
    auto* rangeHead=ranges->alloc(128,32);auto* rangeTail=ranges->alloc(128,-32);assert(rangeHead&&rangeTail);
    assert(ranges->getRemainingAllocationSize(rangeHead)>=128&&ranges->getRemainingAllocationSize(rangeTail)>=128);
    ranges->freeTail();assert(ranges->getRemainingAllocationSize(rangeHead)>=128&&ranges->getRemainingAllocationSize(rangeTail)==-1);
    ranges->destroy();assert(root->check());

    assert(JKR_NEW_ARRAY(u32, SIZE_MAX / sizeof(u32) + 1) == nullptr);
    assert(JKR_NEW_ARRAY(ArrayElement, UINT32_MAX / sizeof(ArrayElement)) == nullptr);
    for (int alignment : {0, 32, -32, 64, -64}) {
        auto* elements = JKR_NEW_ARRAY_ARGS(ArrayElement, 3, small, alignment);
        assert(elements && ArrayElement::live == 3);
        const auto required = alignment == 64 || alignment == -64 ? 64 : 32;
        assert(reinterpret_cast<uintptr_t>(elements) % required == 0);
        JKR_DELETE_ARRAY(elements);
        assert(ArrayElement::live == 0);
    }
    assert(JKR_NEW_ARRAY_ARGS(ArrayElement, 3, small, INT32_MIN) == nullptr);
    assert(JKR_NEW_ARRAY_ARGS(ArrayElement, 3, small, 3) == nullptr);
    alignas(32) u32 placement[4];
    assert(JKR_NEW_ARRAY_ARGS(u32, 4, placement) == placement);
    assert(JKR_NEW_ARRAY_ARGS(u32, 3, placement+1) == placement+1);
    alignas(32) unsigned char bytePlacement[4];
    assert(JKR_NEW_ARRAY_ARGS(unsigned char, 3, bytePlacement+1) == bytePlacement+1);
    std::array<void*,3> pointerPlacement{};
    assert(JKR_NEW_ARRAY_ARGS(void*, 1, pointerPlacement.data()+1)==pointerPlacement.data()+1);
    auto* solid = JKRSolidHeap::create(4096, root, false);
    assert(solid);
    assert(solid->alloc(UINT32_MAX, 4) == nullptr);
    assert(solid->alloc(32, INT32_MIN) == nullptr);
    assert(solid->alloc(32, 3) == nullptr);
    assert(JKRSolidHeap::create(8192, small, false) == nullptr);
    assert(JKRExpHeap::create(8192, small, false) == nullptr);
    assert(solid->alloc(64, 32));
    assert(solid->alloc(64, -32));
    assert(solid->check());
    solid->freeAll();
    solid->destroy();

    auto* other = JKRExpHeap::create(4096, root, false);
    assert(other);
    auto exercise = [](JKRExpHeap* heap) {
        assert(JKRHeap::getCurrentHeap() == nullptr);
        JKRHeap::setCurrentHeap(heap);
        for (int i = 0; i < 1000; ++i) {
            auto* p = operator new(80, JKRHeapToken::Dummy);
            assert(p && JKRHeap::findFromRoot(p) == heap);
            const auto bytes=heap->getSize(p);
            assert(bytes>=80);
            for(unsigned offset:{0u,31u,79u})
                assert(heap->getRemainingAllocationSize(static_cast<unsigned char*>(p)+offset)==bytes-s32(offset));
            if(i%3==0){
                assert(heap->resize(p,32)>=32);
                const auto resized=heap->getSize(p);
                assert(heap->getRemainingAllocationSize(p)==resized);
                assert(heap->getRemainingAllocationSize(static_cast<unsigned char*>(p)+resized)==-1);
            }
            operator delete(p, JKRHeapToken::Dummy);
        }
        assert(JKRHeap::getCurrentHeap() == heap);
    };
    std::thread first(exercise, small), second(exercise, other),
        third(exercise, small), fourth(exercise, small);
    first.join(); second.join(); third.join(); fourth.join();
    assert(JKRHeap::getCurrentHeap() == small);
    assert(small->check() && other->check());
    root->becomeCurrentHeap();
    other->destroy(); small->destroy();
    assert(root->check() && root->getTotalFreeSize() == initialFree);

    // Logical HUD coordinates remain independent of the physical framebuffer.
    assert(mDoGph_gInf_c::getSafeMinXF() == 0);
    assert(mDoGph_gInf_c::getSafeMinYF() == 0);
    assert(mDoGph_gInf_c::getSafeMaxXF() == FB_WIDTH_BASE);
    assert(mDoGph_gInf_c::getSafeMaxYF() == FB_HEIGHT_BASE);
    assert(mDoGph_gInf_c::getSafeWidthF() == FB_WIDTH_BASE);
    assert(mDoGph_gInf_c::getSafeHeightF() == FB_HEIGHT_BASE);
}
