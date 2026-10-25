#include "gx_stream.hpp"
#include "gx_state.hpp"
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <bit>
#include <cmath>

namespace {
using dusk::native::gx::CommandStream;
[[noreturn]] void fail(const char* name){OSPanic(__FILE__,__LINE__,"Native GX pixel command failure: %s",name);}
CommandStream& stream(const char* name){
    auto* s=dusk::native::gx::commandStream();
    if(!s||!s->finish())fail(name);
    return *s;
}
void word(CommandStream& s,u8 reg,u32 value,const char* name){if(!s.writeBp(reg,value))fail(name);}
void masked(CommandStream& s,u8 reg,u32 mask,u32 value,const char* name){
    const auto& r=s.registers();const u32 previous=r.bpValid[reg]?r.bp[reg]:0;
    word(s,reg,(previous&~mask)|(value&mask),name);
}
u32 fogFloat(float value){return (std::bit_cast<u32>(value)>>12)&0xfffff;}
}
extern "C" {
void GXSetFog(GXFogType type,f32 start,f32 end,f32 near,f32 far,GXColor color){
    auto& s=stream("GXSetFog");
    const unsigned mode=unsigned(type),function=mode&7;
    if((mode&~15u)||(function!=0&&function!=2&&function<4)||
       !std::isfinite(start)||!std::isfinite(end)||!std::isfinite(near)||!std::isfinite(far)||
       near<0||far<near)fail("GXSetFog/parameters");
    const bool ortho=mode&8;float a=0,c=0;u32 scale=0,mantissa=0;
    if(ortho){
        if(far!=near&&end!=start){const float inverse=1/(end-start);a=(far-near)*inverse;c=(start-near)*inverse;}
    }else{
        float b=.5f;
        if(far!=near&&end!=start){a=(far*near)/((far-near)*(end-start));b=far/(far-near);c=start/(end-start);}
        // The original GD/GX encoder starts the B scale at one and normalizes
        // its mantissa to [0.5,1]. Near>=0 ensures B>=1 except degenerate cases.
        scale=1;
        if(!std::isfinite(b)||!std::isfinite(a)||!std::isfinite(c))fail("GXSetFog/nonfinite coefficients");
        while(b>1){b*=.5f;if(++scale>31)fail("GXSetFog/B exponent");}
        while(b>0&&b<.5f){if(scale==0)fail("GXSetFog/B exponent");b*=2;--scale;}
        a=std::ldexp(a,-int(scale));mantissa=u32(8388638.0f*b);
    }
    if(!std::isfinite(a)||!std::isfinite(c))fail("GXSetFog/nonfinite coefficients");
    word(s,0xee,fogFloat(a),"GXSetFog/A");
    word(s,0xef,mantissa,"GXSetFog/B");word(s,0xf0,scale,"GXSetFog/scale");
    word(s,0xf1,fogFloat(c)|(u32(ortho)<<20)|(function<<21),"GXSetFog/C");
    word(s,0xf2,(u32(color.r)<<16)|(u32(color.g)<<8)|color.b,"GXSetFog/color");
}
void GXSetFogColor(GXColor color){auto& s=stream("GXSetFogColor");word(s,0xf2,(u32(color.r)<<16)|(u32(color.g)<<8)|color.b,"GXSetFogColor");}
void GXSetFogRangeAdj(GXBool enable,u16 center,GXFogAdjTable* table){
    auto& s=stream("GXSetFogRangeAdj");
    if(enable&&!table)fail("GXSetFogRangeAdj/parameters");
    if(enable){
        for(unsigned i=0;i<10;++i)if(table->r[i]>4095)fail("GXSetFogRangeAdj/table");
        for(unsigned i=0;i<5;++i)word(s,0xe9+i,table->r[2*i]|(u32(table->r[2*i+1])<<12),"GXSetFogRangeAdj/table");
    }
    word(s,0xe8,((u32(center)+342)&1023)|(u32(enable)<<10),"GXSetFogRangeAdj/center");
}
void GXSetColorUpdate(GXBool enable){auto& s=stream("GXSetColorUpdate");masked(s,0x41,8,u32(enable)<<3,"GXSetColorUpdate");}
void GXSetAlphaUpdate(GXBool enable){auto& s=stream("GXSetAlphaUpdate");masked(s,0x41,16,u32(enable)<<4,"GXSetAlphaUpdate");}
void GXSetDstAlpha(GXBool enable,u8 alpha){
    auto& s=stream("GXSetDstAlpha");masked(s,0x42,511,alpha|(u32(enable)<<8),"GXSetDstAlpha");
}
void GXSetPixelFmt(GXPixelFmt format,GXZFmt16 zFormat){
    auto& s=stream("GXSetPixelFmt");
    constexpr u8 hardware[]{0,1,2,3,4,4,4,5};
    if(unsigned(format)>=std::size(hardware)||unsigned(zFormat)>3)fail("GXSetPixelFmt/parameters");
    const unsigned raw=hardware[unsigned(format)];
    masked(s,0x43,63,raw|(unsigned(zFormat)<<3),"GXSetPixelFmt/control");
    masked(s,0,1u<<9,u32(format==GX_PF_RGB565_Z16)<<9,"GXSetPixelFmt/multisample");
    if(raw==4)masked(s,0x42,3u<<9,(unsigned(format)-unsigned(GX_PF_Y8))<<9,"GXSetPixelFmt/YUV");
    // This preserves original pixel-engine state. planRaster still rejects
    // formats whose precision/alpha semantics are not implemented by PICA.
}
void GXSetPointSize(u8 size,GXTexOffset offsets){
    auto& s=stream("GXSetPointSize");
    if(unsigned(offsets)>5)fail("GXSetPointSize/offsets");
    masked(s,0x22,255u<<8|7u<<19,u32(size)<<8|u32(offsets)<<19,"GXSetPointSize");
}
void GXSetDither(GXBool enable){auto& s=stream("GXSetDither");masked(s,0x41,4,u32(enable)<<2,"GXSetDither");}
void GXSetCoPlanar(GXBool enable){
    auto& s=stream("GXSetCoPlanar");
    // Original GX overwrites the pending BP mask with its one-field update.
    if(!s.writeBp(0xfe,1u<<19)||!s.writeBp(0,u32(enable)<<19))fail("GXSetCoPlanar");
}
void GXSetClipMode(GXClipMode mode){
    auto& s=stream("GXSetClipMode");const u32 v=unsigned(mode);
    if(v>1||!s.writeXf(0x1005,{&v,1}))fail("GXSetClipMode");
}
void GXEnableTexOffsets(GXTexCoordID coordinate,GXBool line,GXBool point){
    auto& s=stream("GXEnableTexOffsets");
    if(unsigned(coordinate)>7)fail("GXEnableTexOffsets/value");
    if(s.automaticSuEnabled()){if(!s.automaticSuOffsets(unsigned(coordinate),line,point))fail("GXEnableTexOffsets/automatic SU");return;}
    masked(s,0x30+unsigned(coordinate)*2,3u<<18,(u32(line)<<18)|(u32(point)<<19),"GXEnableTexOffsets");
}
void GXInvalidateVtxCache(){auto& s=stream("GXInvalidateVtxCache");const u8 byte=0x48;if(!s.write({&byte,1}))fail("GXInvalidateVtxCache");}
void GXSetTexCoordScaleManually(GXTexCoordID coord,GXBool enable,u16 ss,u16 ts){
    auto& s=stream("GXSetTexCoordScaleManually");
    if(unsigned(coord)>7||(enable&&(!ss||!ts)))fail("GXSetTexCoordScaleManually/value");
    if(s.automaticSuEnabled()){if(!s.automaticSuManual(unsigned(coord),enable,ss,ts))fail("GXSetTexCoordScaleManually/automatic SU");return;}
    // Like pinned Aurora, disabled manual scaling leaves the last SU register
    // values until the next automatic texture state load. Direct normalized
    // sampling does not apply SU scale twice; indirect stages consume it.
    if(enable){masked(s,0x30+unsigned(coord)*2,65535,u16(ss-1),"GXSetTexCoordScaleManually/S");
        masked(s,0x31+unsigned(coord)*2,65535,u16(ts-1),"GXSetTexCoordScaleManually/T");}
}
void GXSetTexCoordCylWrap(GXTexCoordID coord,GXBool se,GXBool te){
    auto& s=stream("GXSetTexCoordCylWrap");
    if(!s.automaticSuField(unsigned(coord),17,se,te))fail("GXSetTexCoordCylWrap/experiment required");
}
void GXSetTexCoordBias(GXTexCoordID coord,GXBool se,GXBool te){
    auto& s=stream("GXSetTexCoordBias");
    if(!s.automaticSuField(unsigned(coord),16,se,te))fail("GXSetTexCoordBias/experiment required");
}
void GXFlush(){auto& s=stream("GXFlush");if(!s.flushAutomaticSu())fail("GXFlush/automatic SU");} // Complete CPU commands; does not imply GPU completion.
}
