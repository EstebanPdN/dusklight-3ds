#include "gx_vertex_commands.hpp"
#include "gx_stream.hpp"
#include <dolphin/os.h>

namespace dusk::native::gx {
namespace {
[[noreturn]] void fail(const char* call){OSPanic(__FILE__,__LINE__,"Native GX vertex-state failure: %s",call);}
void field(u32& word,unsigned shift,unsigned bits,unsigned value){
    const u32 mask=((1u<<bits)-1)<<shift;word=(word&~mask)|((value<<shift)&mask);
}
}
bool commandClearDescriptors(){
    auto* stream=commandStream();if(!stream)return false;
    if(!stream->clearDescriptors())fail("GXClearVtxDesc");
    return true;
}
bool commandDescriptor(unsigned attribute,unsigned type){
    auto* stream=commandStream();if(!stream)return false;
    if(!stream->setDescriptor(attribute,type))fail("GXSetVtxDesc");
    return true;
}
bool commandVertexFormat(unsigned format,unsigned attribute,unsigned count,unsigned type,u8 fraction){
    auto* stream=commandStream();if(!stream)return false;
    if(format>=8||!stream->finish())fail("GXSetVtxAttrFmt");
    if(attribute==GX_VA_NBT)attribute=GX_VA_NRM;
    if(attribute<GX_VA_POS||attribute>GX_VA_TEX7)fail("GXSetVtxAttrFmt/attribute");
    const bool normal=attribute==GX_VA_NRM,color=attribute==GX_VA_CLR0||attribute==GX_VA_CLR1;
    if(count>(normal?2u:1u)||type>(color?5u:4u)||(!normal&&!color&&fraction>31))fail("GXSetVtxAttrFmt/format");
    const auto& regs=stream->registers();
    u32 a=regs.cpValid[0x70+format]?regs.cp[0x70+format]:1u<<30;
    u32 b=regs.cpValid[0x80+format]?regs.cp[0x80+format]:1u<<31;
    u32 c=regs.cpValid[0x90+format]?regs.cp[0x90+format]:0;
    if(attribute==GX_VA_POS){field(a,0,1,count);field(a,1,3,type);field(a,4,5,fraction);}
    else if(normal){field(a,9,1,count!=0);field(a,10,3,type);field(a,31,1,count==2);}
    else if(color){const unsigned shift=attribute==GX_VA_CLR0?13:17;field(a,shift,1,count);field(a,shift+1,3,type);}
    else if(attribute==GX_VA_TEX0){field(a,21,1,count);field(a,22,3,type);field(a,25,5,fraction);}
    else if(attribute<=GX_VA_TEX3){const unsigned shift=(attribute-GX_VA_TEX1)*9;field(b,shift,1,count);field(b,shift+1,3,type);field(b,shift+4,5,fraction);}
    else if(attribute==GX_VA_TEX4){field(b,27,1,count);field(b,28,3,type);field(c,0,5,fraction);}
    else {const unsigned shift=5+(attribute-GX_VA_TEX5)*9;field(c,shift,1,count);field(c,shift+1,3,type);field(c,shift+4,5,fraction);}
    const u32 words[]{a,b,c};
    for(unsigned group=0;group<3;++group){const unsigned reg=0x70+16*group+format;
        if((!regs.cpValid[reg]||regs.cp[reg]!=words[group])&&!stream->writeCp(reg,words[group]))fail("GXSetVtxAttrFmt");}
    return true;
}
bool commandArray(unsigned attribute,const void* data,u32 size,u8 stride,bool little){
    auto* stream=commandStream();if(!stream)return false;
    if(attribute==GX_VA_NBT)attribute=GX_VA_NRM;
    if(attribute<GX_VA_POS||attribute>=GX_VA_POS+16)fail("GXSetArray/attribute");
    if(!stream->bindArray(attribute-GX_VA_POS,data,size,stride,little))
        OSPanic(__FILE__,__LINE__,"Native GXSetArray attr=%u data=%p bytes=%lu stride=%u stream=%u command=%u offset=%lu",
            attribute,data,static_cast<unsigned long>(size),unsigned(stride),unsigned(stream->error()),
            unsigned(stream->commandError()),static_cast<unsigned long>(stream->commandErrorOffset()));
    return true;
}
}
