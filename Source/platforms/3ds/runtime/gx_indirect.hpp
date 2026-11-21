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
// GX tile of each admitted indirect image format: texels across, down, bytes.
struct IndirectTile {unsigned width,height,bytes;};
constexpr IndirectTile indirectTile(unsigned format){
    return format==0||format==14?IndirectTile{8,8,32}:format==1||format==2?IndirectTile{8,4,32}:
           format==6?IndirectTile{4,4,64}:IndirectTile{4,4,32};
}
// I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8 and CMPR, with exact GX tiling.
// Palette formats stay refused: their TLUT is not part of the Texture view.
constexpr bool indirectImageFormat(unsigned format){return format<=6||format==14;}
inline std::size_t indirectLevelBytes(unsigned format,unsigned width,unsigned height){
    const auto tile=indirectTile(format);
    return std::size_t((width+tile.width-1)/tile.width)*((height+tile.height-1)/tile.height)*tile.bytes;
}
inline bool validIndirectImage(const Texture& t) {
    if(!t.width||!t.height||t.wrapS>2||t.wrapT>2||!t.mipCount||t.mipCount>8||t.minFilter>5||
       t.magFilter>1||t.minLod>t.maxLod||t.maxLod>=t.mipCount||!std::isfinite(t.lodBias))return false;
    if(!indirectImageFormat(t.format))return false;
    std::size_t bytes=0;
    for(unsigned level=0;level<t.mipCount;++level)
        bytes+=indirectLevelBytes(t.format,std::max(1u,unsigned(t.width)>>level),std::max(1u,unsigned(t.height)>>level));
    return t.image.size()>=bytes;
}
// One texel of one level (x, y inside it) as the pinned Aurora decoder's
// .abg, in expanded byte units: the value an indirect stage reads.
inline std::array<float,3> indirectTexel(const Texture& t,unsigned x,unsigned y) {
    const auto shape=indirectTile(t.format);
    const auto tile=((y/shape.height)*((t.width+shape.width-1)/shape.width)+x/shape.width)*shape.bytes;
    const unsigned at=(y%shape.height)*shape.width+x%shape.width;
    auto half=[&](std::size_t offset){return (unsigned(t.image[offset])<<8)|t.image[offset+1];};
    float a,i;
    if(t.format==0){const auto byte=t.image[tile+at/2];a=i=((at&1)?byte&15:byte>>4)*17;}
    else if(t.format==1){a=i=t.image[tile+at];}
    else if(t.format==2){const auto byte=t.image[tile+at];a=(byte>>4)*17;i=(byte&15)*17;}
    else if(t.format==4){
        // RGB565 expands by bit replication; alpha is opaque.
        const unsigned value=half(tile+at*2),b=value&31,g=(value>>5)&63;
        return {255,float((b<<3)|(b>>2)),float((g<<2)|(g>>4))};
    }
    else if(t.format==5){
        const unsigned value=half(tile+at*2);
        if(value&0x8000){const unsigned b=value&31,g=(value>>5)&31;
            return {255,float((b<<3)|(b>>2)),float((g<<3)|(g>>2))};}
        const unsigned alpha=value>>12;
        return {float((alpha<<5)|(alpha<<2)|(alpha>>1)),float((value&15)*17),float(((value>>4)&15)*17)};
    }
    else if(t.format==6){
        // RGBA8: the AR half of the tile, then the GB half 32 bytes later.
        return {float(t.image[tile+at*2]),float(t.image[tile+at*2+33]),float(t.image[tile+at*2+32])};
    }
    else if(t.format==14){
        // CMPR: four 4x4 blocks per 8x8 tile, decoded as texture_gx.cpp does
        // (the pinned Aurora palette: 5/8-3/8 blends, or halves with a
        // transparent fourth entry when the first endpoint is not larger).
        const std::size_t block=tile+(((y%8)/4)*2+(x%8)/4)*8;
        const unsigned c0=half(block),c1=half(block+2);
        const unsigned index=(t.image[block+4+y%4]>>(6-2*(x%4)))&3;
        auto channels=[](unsigned value){
            const unsigned b=value&31,g=(value>>5)&63;
            return std::array<unsigned,2>{(b<<3)|(b>>2),(g<<2)|(g>>4)};
        };
        const auto p0=channels(c0),p1=channels(c1);
        auto blend=[&](unsigned x0,unsigned x1){return c0>c1?(5*x0+3*x1)/8:(x0+x1)/2;};
        if(index==0)return {255,float(p0[0]),float(p0[1])};
        if(index==1)return {255,float(p1[0]),float(p1[1])};
        if(index==2)return {255,float(blend(p0[0],p1[0])),float(blend(p0[1],p1[1]))};
        return {c0>c1?255.f:0.f,float(blend(p1[0],p0[0])),float(blend(p1[1],p0[1]))};
    }
    else {a=t.image[tile+at*2];i=t.image[tile+at*2+1];}
    return std::array<float,3>{a,i,i};
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
        return indirectTexel(t,unsigned(index(x,t.width,t.wrapS)),unsigned(index(y,t.height,t.wrapT)));
    };
    const double x=axis(u,t.width,t.wrapS),y=axis(v,t.height,t.wrapT);
    if(!t.magFilter){abg=pixel(int(std::floor(x)),int(std::floor(y)));return true;}
    const int ix=int(std::floor(x-.5)),iy=int(std::floor(y-.5));
    const float fx=x-.5-ix,fy=y-.5-iy;
    const auto a=pixel(ix,iy),b=pixel(ix+1,iy),c=pixel(ix,iy+1),d=pixel(ix+1,iy+1);
    for(unsigned n=0;n<3;++n)abg[n]=(a[n]*(1-fx)+b[n]*fx)*(1-fy)+(c[n]*(1-fx)+d[n]*fx)*fy;
    return true;
}
// The vertex sampler's explicit LOD: zero without mipmaps, otherwise the
// original bias inside the original clamps (no derivatives at a vertex).
inline float indirectVertexLod(const Texture& t){
    return t.minFilter<2?0:std::clamp(t.lodBias,float(t.minLod),float(t.maxLod));
}
// One mip level as a Texture view of its own (dimensions and texels).
inline Texture indirectLevel(const Texture& t,unsigned selected){
    auto image=t;std::size_t offset=0;
    for(unsigned n=0;n<selected;++n){offset+=indirectLevelBytes(t.format,image.width,image.height);
        image.width=std::max(1u,unsigned(image.width)/2);image.height=std::max(1u,unsigned(image.height)/2);}
    image.image=image.image.subspan(offset);
    return image;
}
// Vertex evaluation has no fragment derivatives. Use explicit LOD zero plus
// the original bias/clamps and actual original mip levels/filter. The absent
// derivative-dependent minification is part of the counted Tier D path.
inline bool sampleIndirectImage(const Texture& t,float u,float v,std::array<float,3>& abg) {
    const float lod=indirectVertexLod(t);
    const bool blend=t.minFilter>=4;
    const unsigned low=blend?unsigned(lod):unsigned(std::floor(lod+.5f));
    auto level=[&](unsigned selected,std::array<float,3>& result){
        auto image=indirectLevel(t,selected);
        image.magFilter=lod<=0?t.magFilter:(t.minFilter&1);
        return sampleIndirectLevel(image,u,v,result);
    };
    if(!level(low,abg))return false;
    if(blend&&lod>low){std::array<float,3> high;if(!level(low+1,high))return false;
        const float fraction=lod-low;for(unsigned c=0;c<3;++c)abg[c]+=(high[c]-abg[c])*fraction;}
    return true;
}
// Warp maps in the formats admitted since Alpha 7 (RGB565, RGBA8, CMPR;
// Alpha 6 refused them, e.g. the CMPR map of the Snowpeak iron ball
// aa_Yironball_v): submitted draws per format and the last map (see
// reportIndirectConstant). Counted at submission, so a draw translated
// twice (a restart) counts once.
struct IndirectFormatAdmissions {
    std::uint64_t rgb565{},rgba8{},cmpr{};
    std::uint16_t lastWidth{},lastHeight{};std::uint8_t lastFormat{};
};
inline IndirectFormatAdmissions indirectFormatAdmissions{};
// The first warp map of a plan in one of those formats (format 0: none).
struct IndirectFormatNote {std::uint16_t width{},height{};std::uint8_t format{};};
inline void countIndirectFormat(const IndirectFormatNote& note){
    if(!note.format)return;
    auto& a=indirectFormatAdmissions;++(note.format==4?a.rgb565:note.format==6?a.rgba8:a.cmpr);
    a.lastFormat=note.format;a.lastWidth=note.width;a.lastHeight=note.height;
}
inline IndirectFormatNote indirectFormatNote(const VertexIndirectPlan& p){
    if(p.enabled)for(const auto& e:p.stages)if(e.enabled){
        const auto f=e.source.format;
        if(f==4||f==6||f==14)return {e.source.width,e.source.height,std::uint8_t(f)};
    }
    return {};
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
        // Texel scale of both coordinates: the size of the texture each one
        // samples, which is what the texture coordinate scale registers
        // (BP 0x30-0x3F) hold on the original (the SDK writes them from the
        // bound textures at every GXBegin, J3D display lists write the same
        // sizes). This port leaves the SDK update out (gx-auto-su77.txt, off
        // by default), so for typed GX draws (J2D) the registers are stale:
        // Alpha 13 console dump 007, file select, 304x224 on coordinate 0
        // while the selected-slot light (mat_w_bk_l00_v) warps its 72x96
        // mask there. The mask shrank into the pane's corner and the rest of
        // the quad sampled past the image (a whitish rectangle with bands).
        // The plain texture route never reads these registers either.
        const unsigned destination=(stage.texture/4)*32+stage.texture%4;
        if(stage.texture>7||!r.bpValid[0x88+destination])return fail("GX vertex indirect destination size missing");
        const auto image=r.bp[0x88+destination];
        const auto scale=(r.bp[0x25+ind/2]>>(8*(ind&1)))&255;
        for(unsigned axis=0;axis<2;++axis) {
            const unsigned exponent=(scale>>(4*axis))&15;
            if(exponent>8)return fail("GX vertex indirect coordinate scale invalid");
            // Source: the warp map's own size divided by GXSetIndTexCoordScale.
            effect.sourceScale[axis]=std::ldexp(1.f,-int(exponent));
            effect.baseScale[axis]=float(((image>>(10*axis))&1023)+1);
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
