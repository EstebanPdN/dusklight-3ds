#pragma once
#include "gx_commands.hpp"
#include "gx_pica_material.hpp"
#include <algorithm>
#include <bit>
#include <cmath>

namespace dusk::native::gx {
// Tier D: evaluate the pinned GX depth/range equations at original vertices,
// then interpolate a ramp coordinate. This is not per-fragment GX fog. The
// final TEV stage preserves material alpha; original images are unmodified.
struct VertexFogPlan {
    double a{},b{},c{},rangeCenter{},viewportWidth{};
    std::array<double,10> rangeK{};
    std::array<std::uint8_t,4> color{};
    unsigned function{};
    bool orthographic{},rangeEnabled{},enabled{};
};
inline bool planVertexFog(const CommandRegisters& r,VertexFogPlan& p) {
    p={};
    if(!r.bpValid[0xf1])return false;
    p.function=(r.bp[0xf1]>>21)&7;
    if(!p.function)return true;
    if(p.function==1||p.function==3)return false;
    for(unsigned reg:{0xe8u,0xeeu,0xefu,0xf0u,0xf2u})if(!r.bpValid[reg])return false;
    auto decode=[](std::uint32_t value){return std::bit_cast<float>((value&0xfffffu)<<12);};
    p.enabled=true;p.orthographic=(r.bp[0xf1]>>20)&1;
    const int shift=r.bp[0xf0]&31;
    p.a=std::ldexp(double(decode(r.bp[0xee])),p.orthographic?0:shift);
    // Match the pinned Aurora register decoder, including its mantissa scale.
    p.b=std::ldexp(double(r.bp[0xef]&0xffffff)/8388638.0,shift-1);
    p.c=decode(r.bp[0xf1]);
    p.color={std::uint8_t(r.bp[0xf2]>>16),std::uint8_t(r.bp[0xf2]>>8),std::uint8_t(r.bp[0xf2]),255};
    if(!std::isfinite(p.a)||!std::isfinite(p.b)||!std::isfinite(p.c))return false;
    p.rangeEnabled=r.bp[0xe8]&1024;
    if(p.rangeEnabled) {
        if(!r.xfValid[0x101a])return false;
        p.viewportWidth=2*double(std::bit_cast<float>(r.xf[0x101a]));
        if(!std::isfinite(p.viewportWidth)||p.viewportWidth<=0)return false;
        p.rangeCenter=int(r.bp[0xe8]&1023)-342;
        for(unsigned i=0;i<10;++i){
            const auto reg=0xe9+i/2;
            if(!r.bpValid[reg])return false;
            p.rangeK[i]=double((r.bp[reg]>>((i&1)?0:12))&4095)/64;
        }
    }
    // These curves are identically zero when A=0 and C>=0, including range
    // adjustment. Preserve validation above but avoid a redundant fog sampler.
    if(p.a==0&&p.c>=0&&(p.function==2||p.function==4||p.function==5))p.enabled=false;
    return true;
}
inline float evaluateVertexFog(const VertexFogPlan& p,const std::array<float,4>& clip,unsigned logicalWidth) {
    if(!p.enabled)return 0;
    // PICA clips camera-crossing triangles itself. Their non-visible original
    // vertices use near-plane fog; interpolation across clipped edges is part
    // of this explicitly approximate vertex path.
    const double depth=clip[3]>0?std::clamp(1+double(clip[2])/clip[3],0.0,1.0):0;
    const double denominator=p.b-depth;
    double base=p.orthographic?p.a*depth:denominator!=0?p.a/denominator:(p.a>0?1e30:0);
    if(p.rangeEnabled&&clip[3]>0) {
        const double x=(1-double(clip[1])/clip[3])*logicalWidth*.5;
        const double offset=2*(x-p.rangeCenter)/p.viewportWidth;
        const double index=std::clamp(9-std::abs(offset)*9,0.0,9.0);
        const unsigned low=unsigned(index),high=std::min(low+1,9u);
        const double fraction=index-low;
        const double k=std::max(p.rangeK[low]*(1-fraction)+p.rangeK[high]*fraction,1e-6);
        base*=std::hypot(offset,k)/k;
    }
    const double f=std::clamp(base-p.c,0.0,1.0);
    switch(p.function) {
    case 2:return f;
    case 4:return 1-std::exp2(-8*f);
    case 5:return 1-std::exp2(-8*f*f);
    case 6:return std::exp2(-8*(1-f));
    case 7:return std::exp2(-8*(1-f)*(1-f));
    default:return 0;
    }
}
// Primary alpha/red may carry fog only after the caller proves the original
// material does not read that component (required bit1/bit0 respectively).
inline bool appendVertexFog(PicaMaterialPlan& m,const VertexFogPlan& p,bool primaryAlpha=false,bool primaryRed=false) {
    if(!p.enabled)return true;
    if(m.stageCount>=m.stages.size()||(!(primaryAlpha||primaryRed)&&m.textureCount>=m.textures.size()))return false;
    const auto texture=(primaryAlpha||primaryRed)?MaterialSource::primary:static_cast<MaterialSource>(unsigned(MaterialSource::texture0)+m.textureCount);
    auto& stage=m.stages[m.stageCount++];stage={};stage.constant=p.color;
    stage.rgb.function=MaterialFunction::interpolate;
    stage.rgb.inputs={MaterialInput{MaterialSource::constant,MaterialOperand::color},
                      MaterialInput{MaterialSource::previous,MaterialOperand::color},
                      MaterialInput{texture,primaryRed?MaterialOperand::red:MaterialOperand::alpha}};
    stage.alpha.function=MaterialFunction::replace;
    stage.alpha.inputs[0]={MaterialSource::previous,MaterialOperand::alpha};
    if(!(primaryAlpha||primaryRed))m.textures[m.textureCount++]={255,255}; // Utility coordinate supplied by caller.
    return true;
}
}
