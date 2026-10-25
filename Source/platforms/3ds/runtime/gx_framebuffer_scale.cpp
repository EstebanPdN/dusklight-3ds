#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <cmath>

namespace {
[[noreturn]] void invalid(const char* call){OSPanic(__FILE__,__LINE__,"Invalid native GX copy scale: %s",call);}
// Preserve the original GX integer quantization, including its extra-line
// rule for scales between1 and2. These are layout calculations, not a copy
// implementation or evidence of GPU completion.
u32 lines(u32 height,u32 scale){
    if(!height||height>1024||!scale||scale>511)invalid("height/quantization");
    u32 result=((height-1)*256)/scale+1;
    if(scale>128&&scale<256){
        u32 odd=scale;while(!(odd&1))odd/=2;
        if(height%odd==0)++result;
    }
    return result>1024?1024:result;
}
u32 quantize(float scale){
    if(!std::isfinite(scale)||scale<=0||scale>256)invalid("scale");
    const u32 value=u32(256.f/scale)&511;
    if(!value)invalid("zero divisor");
    return value;
}
}
extern "C" u16 GXGetNumXfbLines(u16 height,f32 scale){
    if(scale<1)invalid("GXGetNumXfbLines");
    return static_cast<u16>(lines(height,quantize(scale)));
}
extern "C" f32 GXGetYScaleFactor(u16 efbHeight,u16 xfbHeight){
    if(!efbHeight||!xfbHeight||xfbHeight>1024||efbHeight>xfbHeight||
       lines(efbHeight,1)<xfbHeight)invalid("GXGetYScaleFactor/heights");
    u32 target=xfbHeight;
    float scale=float(target)/efbHeight;
    u32 actual=lines(efbHeight,quantize(scale));
    while(actual>xfbHeight){
        if(target<=1)invalid("GXGetYScaleFactor/lower bound");
        scale=float(--target)/efbHeight;actual=lines(efbHeight,quantize(scale));
    }
    float result=scale;
    while(actual<xfbHeight){
        result=scale;
        if(target>=u32(efbHeight)*256)invalid("GXGetYScaleFactor/upper bound");
        scale=float(++target)/efbHeight;actual=lines(efbHeight,quantize(scale));
    }
    return result;
}
