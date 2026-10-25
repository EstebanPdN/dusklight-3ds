#pragma once
#include "gx_commands.hpp"
#include "gx_material.hpp"
#include "gx_texture_commands.hpp"
#include <algorithm>
#include <cmath>

namespace dusk::native::gx {
// Tier D: sample the original tiled indirect image at original vertices and
// interpolate the resulting UV. The pinned Aurora fragment equations define
// the values at those vertices; nonlinear distortion between them is approximate.
struct VertexIndirectEffect {
    Texture source{};
    std::array<std::array<float,3>,2> matrix{};
    std::array<float,2> sourceScale{},baseScale{};
    std::uint32_t state{};
    unsigned coordinate{},bias{};
    bool enabled{};
};
struct VertexIndirectPlan {
    std::array<VertexIndirectEffect,16> stages{};
    std::uint8_t coordinates{};
    bool enabled{};
};
inline bool validIndirectImage(const Texture& t) {
    if(!t.width||!t.height||t.wrapS>2||t.wrapT>2||!t.mipCount||t.mipCount>8||t.minFilter>5||
       t.magFilter>1||t.minLod>t.maxLod||t.maxLod>=t.mipCount||!std::isfinite(t.lodBias))return false;
    if(t.format>3&&t.format!=5)return false; // I4, I8, IA4, IA8, RGB5A3, with exact GX tiling.
    const unsigned tw=(t.format==3||t.format==5)?4:8,th=t.format==0?8:4;
    std::size_t bytes=0;
    for(unsigned level=0;level<t.mipCount;++level)
        bytes+=std::size_t((std::max(1u,unsigned(t.width)>>level)+tw-1)/tw)*((std::max(1u,unsigned(t.height)>>level)+th-1)/th)*32;
    return t.image.size()>=bytes;
}
inline bool sampleIndirectLevel(const Texture& t,float u,float v,std::array<float,3>& abg) {
    if(!std::isfinite(u)||!std::isfinite(v))return false;
    auto axis=[](double value,unsigned size,unsigned wrap){
        if(!wrap)return std::clamp(value,0.0,1.0)*size;
        const double period=wrap==2?2:1;
        value=std::fmod(value,period);if(value<0)value+=period;
        return value*size;
    };
    auto index=[](int value,unsigned size,unsigned wrap){
        if(!wrap)return std::clamp(value,0,int(size)-1);
        const int period=int(size)*(wrap==2?2:1);
        value%=period;if(value<0)value+=period;
        return wrap==2&&value>=int(size)?period-1-value:value;
    };
    auto pixel=[&](int x,int y){
        x=index(x,t.width,t.wrapS);y=index(y,t.height,t.wrapT);
        const unsigned tw=(t.format==3||t.format==5)?4:8,th=t.format==0?8:4;
        const auto tile=((unsigned(y)/th)*((t.width+tw-1)/tw)+unsigned(x)/tw)*32;
        const unsigned at=(unsigned(y)%th)*tw+unsigned(x)%tw;
        float a,i;
        if(t.format==0){const auto byte=t.image[tile+at/2];a=i=((at&1)?byte&15:byte>>4)*17;}
        else if(t.format==1){a=i=t.image[tile+at];}
        else if(t.format==2){const auto byte=t.image[tile+at];a=(byte>>4)*17;i=(byte&15)*17;}
        else if(t.format==5){
            const unsigned value=(unsigned(t.image[tile+at*2])<<8)|t.image[tile+at*2+1];
            // Pinned Aurora indirect samples use .abg, in expanded byte units.
            if(value&0x8000){const unsigned b=value&31,g=(value>>5)&31;
                return std::array<float,3>{255,float((b<<3)|(b>>2)),float((g<<3)|(g>>2))};}
            const unsigned alpha=value>>12;
            return std::array<float,3>{float((alpha<<5)|(alpha<<2)|(alpha>>1)),float((value&15)*17),float(((value>>4)&15)*17)};
        }
        else {a=t.image[tile+at*2];i=t.image[tile+at*2+1];}
        return std::array<float,3>{a,i,i};
    };
    const double x=axis(u,t.width,t.wrapS),y=axis(v,t.height,t.wrapT);
    if(!t.magFilter){abg=pixel(int(std::floor(x)),int(std::floor(y)));return true;}
    const int ix=int(std::floor(x-.5)),iy=int(std::floor(y-.5));
    const float fx=x-.5-ix,fy=y-.5-iy;
    const auto a=pixel(ix,iy),b=pixel(ix+1,iy),c=pixel(ix,iy+1),d=pixel(ix+1,iy+1);
    for(unsigned n=0;n<3;++n)abg[n]=(a[n]*(1-fx)+b[n]*fx)*(1-fy)+(c[n]*(1-fx)+d[n]*fx)*fy;
    return true;
}
// Vertex evaluation has no fragment derivatives. Use explicit LOD zero plus
// the original bias/clamps and actual original mip levels/filter. The absent
// derivative-dependent minification is part of the counted Tier D path.
inline bool sampleIndirectImage(const Texture& t,float u,float v,std::array<float,3>& abg) {
    const float lod=t.minFilter<2?0:std::clamp(t.lodBias,float(t.minLod),float(t.maxLod));
    const bool blend=t.minFilter>=4;
    const unsigned low=blend?unsigned(lod):unsigned(std::floor(lod+.5f));
    auto level=[&](unsigned selected,std::array<float,3>& result){
        auto image=t;std::size_t offset=0;const unsigned tw=(t.format==3||t.format==5)?4:8,th=t.format==0?8:4;
        for(unsigned n=0;n<selected;++n){offset+=std::size_t((image.width+tw-1)/tw)*((image.height+th-1)/th)*32;
            image.width=std::max(1u,unsigned(image.width)/2);image.height=std::max(1u,unsigned(image.height)/2);}
        image.image=image.image.subspan(offset);
        image.magFilter=lod<=0?t.magFilter:(t.minFilter&1);
        return sampleIndirectLevel(image,u,v,result);
    };
    if(!level(low,abg))return false;
    if(blend&&lod>low){std::array<float,3> high;if(!level(low+1,high))return false;
        const float fraction=lod-low;for(unsigned c=0;c<3;++c)abg[c]+=(high[c]-abg[c])*fraction;}
    return true;
}
inline bool planVertexIndirect(const CommandRegisters& r,const MaterialState& m,VertexIndirectPlan& p,const char*& failure) {
    p={};failure="none";
    auto fail=[&](const char* why){p={};failure=why;return false;};
    for(unsigned i=0;i<m.stageCount;++i) {
        const auto& stage=m.stages[i];const auto bits=stage.indirect;
        if(!bits)continue;
        // Eight-bit samples, static matrix, no bump alpha, wrap, LOD override
        // or previous-coordinate accumulation. Other forms remain explicit.
        const unsigned ind=bits&3,matrix=(bits>>9)&15;
        if((bits&~0x1e73u)||matrix<1||matrix>3||ind>=m.indirectCount||
           !stage.textureEnabled||stage.coordinate>=m.texgenCount)
            return fail("GX vertex indirect operation pending");
        if(!r.bpValid[0x27]||!r.bpValid[0x25+ind/2])return fail("GX vertex indirect order/scale missing");
        auto& effect=p.stages[i];effect.state=bits;effect.enabled=true;effect.bias=(bits>>4)&7;
        const unsigned order=(r.bp[0x27]>>(6*ind))&63;
        effect.coordinate=order>>3;
        if(effect.coordinate>=m.texgenCount)return fail("GX vertex indirect source coordinate invalid");
        if(!resolveCommandTexture(r,order&7,effect.source,failure)){p={};return false;}
        if(!validIndirectImage(effect.source))return fail("GX vertex indirect image/sampler pending");
        const auto scale=(r.bp[0x25+ind/2]>>(8*(ind&1)))&255;
        for(unsigned axis=0;axis<2;++axis) {
            const unsigned exponent=(scale>>(4*axis))&15;
            const auto sourceReg=0x30+2*effect.coordinate+axis,baseReg=0x30+2*stage.coordinate+axis;
            if(exponent>8||!r.bpValid[sourceReg]||!r.bpValid[baseReg])return fail("GX vertex indirect coordinate scale missing");
            effect.sourceScale[axis]=std::ldexp(float((r.bp[sourceReg]&65535)+1),-int(exponent))/(axis?effect.source.height:effect.source.width);
            effect.baseScale[axis]=(r.bp[baseReg]&65535)+1;
        }
        unsigned exponent=0;
        for(unsigned column=0;column<3;++column) {
            const unsigned reg=6+3*(matrix-1)+column;
            if(!r.bpValid[reg])return fail("GX vertex indirect matrix missing");
            const auto value=r.bp[reg];exponent|=((value>>22)&(column==2?1:3))<<(column*2);
            for(unsigned row=0;row<2;++row){int coefficient=(value>>(row*11))&2047;if(coefficient&1024)coefficient-=2048;
                effect.matrix[row][column]=float(coefficient)/1024;}
        }
        for(auto& row:effect.matrix)for(auto& coefficient:row)coefficient=std::ldexp(coefficient,int(exponent)-17);
        p.coordinates|=1u<<effect.coordinate;p.enabled=true;
    }
    return true;
}
// Negative base q is private/default-off; the adapter must prove whole-source
// camera alignment before opting in. Effective source q remains strictly positive.
inline bool evaluateVertexIndirect(const VertexIndirectEffect& effect,const std::array<float,3>& source,
                                   const std::array<float,3>& base,bool sourceProjected,bool baseProjected,
                                   const Texture& destination,std::array<float,2>& output,bool allowNegativeBase=false) {
    const float sourceQ=sourceProjected?source[2]:1,baseQ=baseProjected?base[2]:1;
    if(!std::isfinite(sourceQ)||sourceQ<=0||!std::isfinite(baseQ)||baseQ==0||(baseQ<0&&!allowNegativeBase))return false;
    std::array<float,3> sample;
    if(!sampleIndirectImage(effect.source,source[0]/sourceQ*effect.sourceScale[0],source[1]/sourceQ*effect.sourceScale[1],sample))return false;
    for(unsigned c=0;c<3;++c)if(effect.bias&(1u<<c))sample[c]-=128;
    for(unsigned axis=0;axis<2;++axis) {
        float texel=base[axis]/baseQ*effect.baseScale[axis];
        for(unsigned c=0;c<3;++c)texel+=effect.matrix[axis][c]*sample[c];
        output[axis]=texel/(axis?destination.height:destination.width)*baseQ;
        if(!std::isfinite(output[axis]))return false;
    }
    return true;
}
}
