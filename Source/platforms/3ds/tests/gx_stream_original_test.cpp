#include "JSystem/JSystem.h"
#include "JSystem/J3DGraphBase/J3DPacket.h"
#include "JSystem/J3DGraphBase/J3DGD.h"
#include "JSystem/J3DGraphBase/J3DFifo.h"
#include "../runtime/gx_stream.hpp"
#include <bit>
#include <cassert>

void testOriginalCommandAbi(dusk::native::gx::CommandProcessor& processor) {
    alignas(32) u8 storage[64]{};
    J3DDisplayListObj packet;packet.setSingleDisplayList(storage,sizeof(storage));
    packet.beginDL();J3DGDSetZMode(GX_TRUE,GX_LEQUAL,GX_TRUE);assert(packet.endDL()==32);
    packet.callDL();assert(processor.registers().bp[0x40]==0x17);
    J3DFifoLoadCPCmd(0x30,0x12345678);assert(processor.registers().cp[0x30]==0x12345678);
    J3DFifoLoadBPCmd(0x410000ff);assert(processor.registers().bp[0x41]==0xff);
    Mtx matrix{{1.25f,2.5f,3.75f,4},{5,6,7,8},{9,10,11,12}};
    J3DFifoLoadPosMtxImm(matrix,3);
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)
        assert(processor.registers().xf[12+r*4+c]==std::bit_cast<u32>(matrix[r][c]));
    J3DFifoLoadNrmMtxImm(matrix,6);
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)
        assert(processor.registers().xf[0x400+18+r*3+c]==std::bit_cast<u32>(matrix[r][c]));
}
