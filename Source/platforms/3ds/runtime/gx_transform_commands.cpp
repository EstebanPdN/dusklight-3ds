#include "gx_transform_commands.hpp"
#include "gx_stream.hpp"
#include "gx_state.hpp"
#include <dolphin/os.h>
#include <bit>
#include <cmath>

namespace dusk::native::gx {
namespace {
[[noreturn]] void fail(const char* call){OSPanic(__FILE__,__LINE__,"Native GX command-state failure: %s",call);}
bool finite(const float* values,unsigned count){
    if(!values)return false;
    for(unsigned i=0;i<count;++i)if(!std::isfinite(values[i]))return false;
    return true;
}
void xf(CommandStream& stream,u16 address,std::span<const u32> values,const char* call){
    if(!stream.writeXf(address,values))fail(call);
}
void floats(CommandStream& stream,u16 address,const float* values,unsigned count,const char* call){
    if(count>12||!finite(values,count))fail(call);
    u32 bits[12];for(unsigned i=0;i<count;++i)bits[i]=std::bit_cast<u32>(values[i]);
    xf(stream,address,{bits,count},call);
}
bool positionId(u32 id){return id<=27&&id%3==0;}
void bp(CommandStream& stream,u8 reg,u32 mask,u32 value,const char* call){
    if(!stream.finish())fail(call);
    const auto& regs=stream.registers();
    const u32 previous=regs.bpValid[reg]?regs.bp[reg]:0;
    if(!stream.writeBp(reg,(previous&~mask)|(value&mask)))fail(call);
}
}
void requireImmediateMode(const char* call){if(commandStream())fail(call);}
void finishCommandBoundary(const char* call){if(auto* stream=commandStream();stream&&!stream->finish())fail(call);}
void commandTextureBinding(unsigned unit){
    auto* stream=commandStream();if(!stream)return;
    Texture texture;u32 token;
    if(!textureBinding(unit,texture)||!textureCommandToken(texture.image.data(),token))fail("GXLoadTexObj/resource");
    const unsigned offset=(unit/4)*32+unit%4;
    constexpr u32 filters[]{0,4,1,5,2,6};
    if(texture.minFilter>5)fail("GXLoadTexObj/filter");
    const u32 mode=texture.wrapS|(texture.wrapT<<2)|(texture.magFilter<<4)|(filters[texture.minFilter]<<5)|
        (u32(static_cast<u8>(static_cast<int>(texture.lodBias*32)))<<9);
    const u32 lod=(texture.minLod*16)|((texture.maxLod*16)<<8);
    const u32 attr=(texture.width-1)|((texture.height-1)<<10)|(texture.format<<20);
    // Typed state is a complete snapshot, so later partial GD sampler updates
    // preserve its image identity. Reset BP write mask exactly at this boundary.
    if(!stream->writeBp(0xfe,0xffffff)||!stream->writeBp(0x80+offset,mode)||
       !stream->writeBp(0x84+offset,lod)||!stream->writeBp(0x88+offset,attr)||
       !stream->writeBp(0x94+offset,token))fail("GXLoadTexObj/registers");
    if(!stream->automaticSuTexture(unit,attr,mode))fail("GXLoadTexObj/automatic SU");
    if(texture.bti[8]){
        const u32 colors=(u32(texture.bti[10])<<8)|texture.bti[11];
        const u32 tmem=(0xf0000+(unit<<13)-0x80000)>>9;
        if(!stream->writeBp(0x64,token)||!stream->writeBp(0x65,tmem|(((colors+15)/16)<<10))||
           !stream->writeBp(0x98+offset,tmem|(u32(texture.bti[9])<<10)))fail("GXLoadTexObj/palette-registers");
    }
}
bool commandProjection(const float* matrix,unsigned type){
    auto* stream=commandStream();if(!stream)return false;
    if(type>1||!finite(matrix,16))fail("GXSetProjection");
    const u32 words[]{std::bit_cast<u32>(matrix[0]),std::bit_cast<u32>(matrix[type?3:2]),
        std::bit_cast<u32>(matrix[5]),std::bit_cast<u32>(matrix[type?7:6]),
        std::bit_cast<u32>(matrix[10]),std::bit_cast<u32>(matrix[11]),type};
    xf(*stream,0x1020,words,"GXSetProjection");return true;
}
bool commandViewport(float x,float y,float width,float height,float nearZ,float farZ){
    auto* stream=commandStream();if(!stream)return false;
    const float input[]{x,y,width,height,nearZ,farZ};
    if(!finite(input,6)||width<=0||height<=0||nearZ<0||nearZ>1||farZ<0||farZ>1)fail("GXSetViewport");
    const float z0=16777215.0f*nearZ,z1=16777215.0f*farZ;
    const float values[]{width/2,-height/2,z1-z0,340+(x+width/2),340+(y+height/2),z1};
    floats(*stream,0x101a,values,6,"GXSetViewport");return true;
}
bool commandPositionMatrix(const float* matrix,u32 id){
    auto* stream=commandStream();if(!stream)return false;
    if(!positionId(id))fail("GXLoadPosMtxImm");
    floats(*stream,id*4,matrix,12,"GXLoadPosMtxImm");return true;
}
bool commandNormalMatrix(const float* matrix,u32 id){
    auto* stream=commandStream();if(!stream)return false;
    if(!positionId(id)||!matrix)fail("GXLoadNrmMtxImm");
    // Source is a 3x4 matrix; its translation column is not a normal component.
    const float values[]{matrix[0],matrix[1],matrix[2],matrix[4],matrix[5],matrix[6],matrix[8],matrix[9],matrix[10]};
    floats(*stream,0x400+id*3,values,9,"GXLoadNrmMtxImm");return true;
}
bool commandTextureMatrix(const float* matrix,u32 id,unsigned type){
    auto* stream=commandStream();if(!stream)return false;
    u32 address;
    if(id>=30&&id<=60&&id%3==0){address=id*4;}
    else if(((id>=64&&id<=121&&(id-64)%3==0)||id==GX_PTIDENTITY)&&type==0){address=0x500+(id-64)*4;}
    else fail("GXLoadTexMtxImm/index");
    if(type>1)fail("GXLoadTexMtxImm/type");
    floats(*stream,address,matrix,type==0?12:8,"GXLoadTexMtxImm");return true;
}
bool commandCurrentMatrix(u32 id){
    auto* stream=commandStream();if(!stream)return false;
    if(!positionId(id)||!stream->finish())fail("GXSetCurrentMtx");
    const auto& regs=stream->registers();
    const u32 value=(regs.matrixIndexValid[0]?regs.matrixIndex[0]&~63u:0)|id;
    if(!stream->writeCp(0x30,value))fail("GXSetCurrentMtx");
    xf(*stream,0x1018,{&value,1},"GXSetCurrentMtx");return true;
}
bool commandScissor(u32 x,u32 y,u32 width,u32 height){
    auto* stream=commandStream();if(!stream)return false;
    if(x>1705||y>1705||width>1706-x||height>1706-y)fail("GXSetScissor");
    bp(*stream,0x20,0x7ff7ff,((x+342)<<12)|(y+342),"GXSetScissor");
    bp(*stream,0x21,0x7ff7ff,((x+342+width-1)<<12)|(y+342+height-1),"GXSetScissor");return true;
}
bool commandBlend(unsigned mode,unsigned source,unsigned destination,unsigned logic){
    auto* stream=commandStream();if(!stream)return false;
    if(mode>3||source>7||destination>7||logic>15)fail("GXSetBlendMode");
    const u32 value=(mode==1||mode==3)|(u32(mode==2)<<1)|(destination<<5)|(source<<8)|(u32(mode==3)<<11)|(logic<<12);
    bp(*stream,0x41,0xffe3,value,"GXSetBlendMode");return true;
}
bool commandDepth(unsigned enabled,unsigned compare,unsigned write){
    auto* stream=commandStream();if(!stream)return false;
    if(enabled>1||compare>7||write>1)fail("GXSetZMode");
    bp(*stream,0x40,0x1f,enabled|(compare<<1)|(write<<4),"GXSetZMode");return true;
}
bool commandAlpha(unsigned compare0,u8 ref0,unsigned op,unsigned compare1,u8 ref1){
    auto* stream=commandStream();if(!stream)return false;
    if(compare0>7||compare1>7||op>3)fail("GXSetAlphaCompare");
    bp(*stream,0xf3,0xffffff,ref0|(u32(ref1)<<8)|(compare0<<16)|(compare1<<19)|(op<<22),"GXSetAlphaCompare");return true;
}
bool commandCull(unsigned mode){
    auto* stream=commandStream();if(!stream)return false;
    if(mode>3)fail("GXSetCullMode");
    const unsigned hardware=mode==1?2:mode==2?1:mode;
    bp(*stream,0,0xc000,hardware<<14,"GXSetCullMode");return true;
}
bool commandZLocation(unsigned beforeTexture){
    auto* stream=commandStream();if(!stream)return false;
    if(beforeTexture>1)fail("GXSetZCompLoc");
    bp(*stream,0x43,0x40,beforeTexture<<6,"GXSetZCompLoc");return true;
}
bool commandLineWidth(u8 width,unsigned offsets){
    auto* stream=commandStream();if(!stream)return false;
    if(offsets>7)fail("GXSetLineWidth");
    bp(*stream,0x22,0x700ff,width|(offsets<<16),"GXSetLineWidth");return true;
}
}
