#include "gx_stream.hpp"
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>

namespace {
// Match the pinned Aurora light-object ABI: color, then twelve native floats.
// The object is opaque storage, so memcpy avoids aliasing/lifetime assumptions.
static_assert(sizeof(GXLightObj) == 64 && sizeof(GXColor) == 4);
void floats(GXLightObj* object, unsigned word, float x, float y, float z) {
    const float values[]{x,y,z};
    std::memcpy(reinterpret_cast<unsigned char*>(object)+word*4,values,sizeof(values));
}
[[noreturn]] void fail(const char* reason) {
    OSPanic(__FILE__,__LINE__,"Native GX light object: %s",reason);
}
}
extern "C" {
void GXInitLightColor(GXLightObj* object,GXColor color) { std::memcpy(object,&color,sizeof(color)); }
void GXInitLightAttnA(GXLightObj* object,float a0,float a1,float a2) { floats(object,1,a0,a1,a2); }
void GXInitLightAttnK(GXLightObj* object,float k0,float k1,float k2) { floats(object,4,k0,k1,k2); }
void GXInitLightAttn(GXLightObj* object,float a0,float a1,float a2,float k0,float k1,float k2) {
    GXInitLightAttnA(object,a0,a1,a2);GXInitLightAttnK(object,k0,k1,k2);
}
void GXInitLightPos(GXLightObj* object,float x,float y,float z) { floats(object,7,x,y,z); }
void GXInitLightDir(GXLightObj* object,float x,float y,float z) { floats(object,10,-x,-y,-z); }
void GXInitLightSpot(GXLightObj* object,float cutoff,GXSpotFn function) {
    if(cutoff<=0 || cutoff>90)function=GX_SP_OFF;
    const float c=std::cos(std::numbers::pi_v<float>*cutoff/180.f),one=1-c,d=one*one;
    float a0=1,a1=0,a2=0;
    // Use the pinned original GDLight equations, including SHARP/RING1 terms
    // which differ from the desktop GXLighting implementation.
    switch(function) {
    case GX_SP_FLAT:a0=-1000*c;a1=1000;break;
    case GX_SP_COS:a0=-c/one;a1=1/one;break;
    case GX_SP_COS2:a0=0;a1=-c/one;a2=1/one;break;
    case GX_SP_SHARP:a0=c*(c-2)/d;a1=2/d;a2=-1/d;break;
    case GX_SP_RING1:a0=-4*c/d;a1=4*(1+c)/d;a2=-4/d;break;
    case GX_SP_RING2:a0=1-2*c*c/d;a1=4*c/d;a2=-2/d;break;
    default:break;
    }
    GXInitLightAttnA(object,a0,a1,a2);
}
void GXInitLightDistAttn(GXLightObj* object,float distance,float brightness,GXDistAttnFn function) {
    if(distance<0 || brightness<=0 || brightness>=1)function=GX_DA_OFF;
    float k1=0,k2=0;
    switch(function) {
    case GX_DA_GENTLE:k1=(1-brightness)/(brightness*distance);break;
    case GX_DA_MEDIUM:k1=(.5f*(1-brightness))/(brightness*distance);k2=(.5f*(1-brightness))/(distance*(brightness*distance));break;
    case GX_DA_STEEP:k2=(1-brightness)/(distance*(brightness*distance));break;
    default:break;
    }
    GXInitLightAttnK(object,1,k1,k2);
}
void GXInitSpecularDir(GXLightObj* object,float x,float y,float z) {
    const float hx=-x,hy=-y,hz=1-z,length2=hx*hx+hy*hy+hz*hz;
    const float inv=length2!=0?1/std::sqrt(length2):0;
    // This target runs the GameCube game; use its pinned GX large-number value.
    GXInitLightPos(object,-1048576.f*x,-1048576.f*y,-1048576.f*z);
    floats(object,10,hx*inv,hy*inv,hz*inv);
}
void GXLoadLightObjImm(GXLightObj* object,GXLightID id) {
    const auto mask=static_cast<u32>(id);
    if(!object || !mask || mask>128 || (mask&(mask-1)))fail("invalid immediate light");
    auto* stream=dusk::native::gx::commandStream();
    if(!stream || !stream->finish())fail("command stream unavailable or incomplete");
    GXColor color;std::memcpy(&color,object,sizeof(color));
    u32 words[16]{};
    words[3]=(u32(color.r)<<24)|(u32(color.g)<<16)|(u32(color.b)<<8)|color.a;
    std::memcpy(words+4,reinterpret_cast<const unsigned char*>(object)+4,12*sizeof(u32));
    const unsigned address=0x600+16*std::countr_zero(mask);
    const auto& regs=stream->registers();bool changed=false;
    for(unsigned i=0;i<16;++i)changed|=!regs.xfValid[address+i] || regs.xf[address+i]!=words[i];
    if(changed && !stream->writeXf(address,words))fail("XF light upload failed");
}
}
