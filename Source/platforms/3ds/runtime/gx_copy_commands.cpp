#include "gx_stream.hpp"
#include <dolphin/os.h>
#include <cmath>
#include <bit>
#include <type_traits>

namespace {
using dusk::native::gx::CommandStream;
[[noreturn]] void fail(const char* name){OSPanic(__FILE__,__LINE__,"Native GX copy setup failure: %s",name);}
CommandStream& stream(const char* name){auto* value=dusk::native::gx::commandStream();if(!value||!value->finish())fail(name);return *value;}
void word(CommandStream& value,u8 reg,u32 data,const char* name){if(!value.writeBp(reg,data))fail(name);}
void source(bool texture,u16 left,u16 top,u16 width,u16 height,const char* name){
    if(!stream(name).setCopySource(texture,left,top,width,height))fail(name);
}
}
extern "C" {
void GXSetDispCopySrc(u16 left,u16 top,u16 width,u16 height){source(false,left,top,width,height,"GXSetDispCopySrc");}
void GXSetTexCopySrc(u16 left,u16 top,u16 width,u16 height){source(true,left,top,width,height,"GXSetTexCopySrc");}
void GXSetDispCopyDst(u16 width,u16 height){
    if(!stream("GXSetDispCopyDst").setCopyDestination(false,width,height,GX_TF_RGBA8,false))fail("GXSetDispCopyDst");
}
void GXSetTexCopyDst(u16 width,u16 height,GXTexFmt format,GXBool mipmap){
    const auto raw=std::bit_cast<std::underlying_type_t<GXTexFmt>>(format);
    if(!stream("GXSetTexCopyDst").setCopyDestination(true,width,height,u32(raw),mipmap))fail("GXSetTexCopyDst");
}
void GXSetCopyClamp(GXFBClamp clamp){
    auto& value=stream("GXSetCopyClamp");
    const auto raw=std::bit_cast<std::underlying_type_t<GXFBClamp>>(clamp);
    if(u32(raw)>3||!value.setCopyControl(false,3,u32(raw))||!value.setCopyControl(true,3,u32(raw)))fail("GXSetCopyClamp");
}
void GXSetDispCopyGamma(GXGamma gamma){
    auto& value=stream("GXSetDispCopyGamma");
    const auto raw=std::bit_cast<std::underlying_type_t<GXGamma>>(gamma);
    if(u32(raw)>2||!value.setCopyControl(false,3u<<7,u32(raw)<<7))fail("GXSetDispCopyGamma");
}
u32 GXSetDispCopyYScale(f32 scale){
    auto& value=stream("GXSetDispCopyYScale");
    if(!std::isfinite(scale)||scale<1||scale>256||!value.registers().copy.display.sourceReady)fail("GXSetDispCopyYScale");
    const u32 quantized=u32(256.f/scale)&511;
    if(!quantized)fail("GXSetDispCopyYScale/zero divisor");
    word(value,0x4e,quantized,"GXSetDispCopyYScale/scale");
    if(!value.setCopyControl(false,1u<<10,u32(quantized!=256)<<10))fail("GXSetDispCopyYScale/control");
    return GXGetNumXfbLines(value.registers().copy.display.source[3],scale);
}
void GXSetCopyClear(GXColor color,u32 depth){
    auto& value=stream("GXSetCopyClear");if(depth>0xffffff)fail("GXSetCopyClear/depth");
    word(value,0x4f,color.r|(u32(color.a)<<8),"GXSetCopyClear/RA");
    word(value,0x50,color.b|(u32(color.g)<<8),"GXSetCopyClear/BG");
    word(value,0x51,depth,"GXSetCopyClear/Z");
}
void GXSetCopyFilter(GXBool aa,u8 pattern[12][2],GXBool vertical,u8 filter[7]){
    auto& value=stream("GXSetCopyFilter");
    if((aa&&!pattern)||(vertical&&!filter))fail("GXSetCopyFilter/null");
    if(aa)for(unsigned i=0;i<12;++i)for(unsigned j=0;j<2;++j)if(pattern[i][j]>15)fail("GXSetCopyFilter/sample");
    if(vertical)for(unsigned i=0;i<7;++i)if(filter[i]>63)fail("GXSetCopyFilter/coefficient");
    for(unsigned group=0;group<4;++group){
        u32 samples=0x666666;
        if(aa){samples=0;for(unsigned i=0;i<3;++i)samples|=u32(pattern[group*3+i][0])<<(i*8)|u32(pattern[group*3+i][1])<<(i*8+4);}
        word(value,1+group,samples,"GXSetCopyFilter/sample registers");
    }
    constexpr u8 defaults[]{0,0,21,22,21,0,0};const u8* coefficients=vertical?filter:defaults;
    word(value,0x53,coefficients[0]|u32(coefficients[1])<<6|u32(coefficients[2])<<12|u32(coefficients[3])<<18,"GXSetCopyFilter/coeff0");
    word(value,0x54,coefficients[4]|u32(coefficients[5])<<6|u32(coefficients[6])<<12,"GXSetCopyFilter/coeff1");
}
}
