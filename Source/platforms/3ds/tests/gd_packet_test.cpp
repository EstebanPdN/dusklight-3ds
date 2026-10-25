#include "JSystem/JSystem.h"
#include "JSystem/J3DGraphBase/J3DPacket.h"
#include "JSystem/J3DGraphBase/J3DGD.h"
#include <array>
#include <atomic>
#include <cassert>
#include <thread>

void testOriginalPackets() {
    assert(!__GDCurrentDL);
    alignas(32) std::array<u8,64> bytes{};
    J3DDisplayListObj packet;
    packet.setSingleDisplayList(bytes.data(),bytes.size());
    packet.beginDL();
    J3DGDSetZMode(GX_TRUE,GX_LEQUAL,GX_TRUE);
    assert(packet.endDL()==32 && !__GDCurrentDL);
    assert(bytes[0]==0x61 && bytes[1]==0x40 && bytes[4]==0x17);
    packet.beginPatch();GDSetCurrOffset(4);J3DGDWrite_u8(0x07);
    assert(packet.endPatch()==32 && bytes[4]==0x07 && !__GDCurrentDL);
    // Distinct model/list ownership per thread; neither thread disables interrupts.
    std::atomic<int> ready{0};
    auto worker=[&]{
        alignas(32) std::array<u8,64> own{};
        J3DDisplayListObj list;list.setSingleDisplayList(own.data(),own.size());list.beginDL();
        ready.fetch_add(1);while(ready.load()<2)std::this_thread::yield();
        J3DGDSetZMode(GX_TRUE,GX_GEQUAL,GX_FALSE);
        assert(list.endDL()==32 && own[4]==0x0d && !__GDCurrentDL);
    };
    std::thread a(worker),b(worker);a.join();b.join();
    assert(!__GDCurrentDL && bytes[4]==0x07);
}
