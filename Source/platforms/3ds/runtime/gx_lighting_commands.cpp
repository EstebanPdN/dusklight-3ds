#include "gx_lighting_commands.hpp"
#include "gx_stream.hpp"
#include <dolphin/os.h>

namespace dusk::native::gx {
namespace {
[[noreturn]] void fail(const char* call){OSPanic(__FILE__,__LINE__,"Native GX lighting-state failure: %s",call);}
void xf(CommandStream& stream,u16 address,u32 value,const char* call){
    if(!stream.finish())fail(call);
    const auto& regs=stream.registers();
    if((!regs.xfValid[address]||regs.xf[address]!=value)&&!stream.writeXf(address,{&value,1}))fail(call);
}
void genMode(CommandStream& stream,u32 mask,u32 value,const char* call){
    if(!stream.finish())fail(call);
    const auto& regs=stream.registers();
    // Do not skip a BP write: even an unchanged value consumes a pending mask.
    if(!stream.writeBp(0,(regs.bp[0]&~mask)|(value&mask)))fail(call);
}
}
bool commandNumTexGens(unsigned count){
    auto* stream=commandStream();if(!stream)return false;
    if(count>8)fail("GXSetNumTexGens");
    genMode(*stream,15,count,"GXSetNumTexGens");xf(*stream,0x103f,count,"GXSetNumTexGens");
    return true;
}
bool commandNumChannels(unsigned count){
    auto* stream=commandStream();if(!stream)return false;
    if(count>2)fail("GXSetNumChans");
    genMode(*stream,7u<<4,count<<4,"GXSetNumChans");xf(*stream,0x1009,count,"GXSetNumChans");
    return true;
}
bool commandTexCoordGen(unsigned destination,unsigned function,unsigned source,u32 matrix,unsigned normalize,u32 postMatrix){
    auto* stream=commandStream();if(!stream)return false;
    const bool matrixValid=matrix<=60&&matrix%3==0;
    const bool postValid=(postMatrix>=64&&postMatrix<=121&&(postMatrix-64)%3==0)||postMatrix==125;
    if(destination>=8||function>10||source>=GX_MAX_TEXGENSRC||normalize>1||!matrixValid||!postValid)fail("GXSetTexCoordGen2");
    u32 row=5,form=0,type=0,projection=0,embossSource=0,light=0;
    if(source<=GX_TG_TANGENT){const u32 rows[]{0,1,3,4};row=rows[source];form=1;}
    else if(source<=GX_TG_TEX7)row=source-GX_TG_TEX0+5;
    else if(source==GX_TG_COLOR0||source==GX_TG_COLOR1)row=2;
    if(function==GX_TG_MTX3x4||function==GX_TG_MTX2x4){
        if(source>=GX_TG_TEXCOORD0&&source<=GX_TG_TEXCOORD6)fail("GXSetTexCoordGen2/source");
        projection=function==GX_TG_MTX3x4;
    }else if(function==GX_TG_SRTG){
        if(source!=GX_TG_COLOR0&&source!=GX_TG_COLOR1)fail("GXSetTexCoordGen2/color-source");
        type=source==GX_TG_COLOR0?2:3;row=2;
    }else{
        if(source<GX_TG_TEXCOORD0||source>GX_TG_TEXCOORD6)fail("GXSetTexCoordGen2/bump-source");
        type=1;embossSource=source-GX_TG_TEXCOORD0;light=function-GX_TG_BUMP0;
    }
    const u32 config=(projection<<1)|(form<<2)|(type<<4)|(row<<7)|(embossSource<<12)|(light<<15);
    xf(*stream,0x1040+destination,config,"GXSetTexCoordGen2");
    xf(*stream,0x1050+destination,(postMatrix-64)|(normalize<<8),"GXSetTexCoordGen2");
    const unsigned slot=destination/4,shift=slot?6*(destination-4):6*(destination+1);
    const auto& regs=stream->registers();
    const u32 selected=(regs.matrixIndex[slot]&~(63u<<shift))|(matrix<<shift);
    const unsigned cp=slot?0x40:0x30;
    if((!regs.cpValid[cp]||regs.cp[cp]!=selected)&&!stream->writeCp(cp,selected))fail("GXSetTexCoordGen2");
    // XF/CP alias order still matters if either raw shadow differs from the canonical selector.
    xf(*stream,0x1018+slot,selected,"GXSetTexCoordGen2");
    return true;
}
bool commandChannelControl(unsigned channel,unsigned enabled,unsigned ambient,unsigned material,u32 lights,unsigned diffuse,unsigned attenuation){
    auto* stream=commandStream();if(!stream)return false;
    if(channel>5||enabled>1||ambient>1||material>1||lights>255||diffuse>2||attenuation>2)fail("GXSetChanCtrl");
    const u32 value=material|(enabled<<1)|((lights&15)<<2)|(ambient<<6)|
        ((attenuation==GX_AF_SPEC?0:diffuse)<<7)|(u32(attenuation!=GX_AF_NONE)<<9)|
        (u32(attenuation!=GX_AF_SPEC)<<10)|((lights>>4)<<11);
    const unsigned first=channel<4?channel:channel-4;
    xf(*stream,0x100e + first,value,"GXSetChanCtrl");
    if(channel>=4)xf(*stream,0x1010+first,value,"GXSetChanCtrl");
    return true;
}
bool commandChannelColor(unsigned channel,u32 rgba,bool material){
    auto* stream=commandStream();if(!stream)return false;
    if(channel>5)fail("GXSetChanColor");
    // The pinned GX/GD implementation writes a full RGBA word for color/alpha aliases.
    xf(*stream,(material?0x100c:0x100a)+(channel&1),rgba,"GXSetChanColor");
    return true;
}
}
