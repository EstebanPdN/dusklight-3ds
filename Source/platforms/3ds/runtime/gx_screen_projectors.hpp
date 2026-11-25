#pragma once

#include "gx_pica_material.hpp"
#include "gx_texture_commands.hpp"
#include "gx_vertex_transform.hpp"
#include <bit>
#include <cmath>

namespace dusk::native::gx {

// The paired GX projectors use the same view-space position as clip, with
// q = clip.w = -view.z. Their normalized UVs are affine in screen x and y.
// Carry the two I4 samples in a single screen texture (RGB and alpha) so the
// mesh can still use perspective-correct raster color, base UVs and depth.
struct ScreenProjectors {
    using Corners = std::array<std::array<std::array<float,2>,4>,2>;
    Corners corners{};
    std::array<unsigned,2> units{};
    unsigned baseUnit{};
};

inline bool planScreenProjectors(const CommandRegisters& r,const VertexTransformPlan& transform,
    const std::array<Texture,3>& textures,PicaMaterialPlan& material,ScreenProjectors& out){
    if(!material.stageCount||material.stageCount>material.stages.size()||
       material.sourceTextures()!=3||material.textureCount!=3||material.rasterCarrier!=RasterCarrier::none||
       !r.matrixIndexValid[0])return false;
    const auto& clip=transform.clipProjection();
    // Rotated perspective viewport. No view-space translations; the two
    // nonzero XY coefficients span the same plane as the projectors.
    if(clip[0]!=0||clip[5]!=0||clip[3]!=0||clip[7]!=0||
       !clip[1]||!clip[4]||clip[12]!=0||clip[13]!=0||clip[14]!=-1||clip[15]!=0)return false;
    ScreenProjectors plan;
    unsigned count=0,bases=0;
    for(unsigned unit=0;unit<3;++unit){
        const unsigned c=material.textures[unit].coordinate;
        if(c>=4)return false;
        if(!(transform.projectiveMask()&(1u<<c))){
            plan.baseUnit=unit;++bases;continue;
        }
        if(count>=2)return false;
        const auto& tex=textures[unit];
        // With equal min/mag filters and one level, the screen pass retains
        // the sampler for every fragment. RGBA8 carries the two TEV input bytes.
        if(!tex.immutablePixels||tex.format!=0||tex.mipCount!=1||tex.minFilter>1||tex.minFilter!=tex.magFilter||
           !std::has_single_bit(tex.width)||!std::has_single_bit(tex.height))return false;
        const unsigned post=r.xf[0x1050+c];
        if((r.xf[0x1040+c]&0xfff)!=6||post>57||post%3)return false;
        const unsigned at=0x500+post*4;
        float rows[12];
        for(unsigned i=0;i<12;++i){
            if(!r.xfValid[at+i])return false;
            rows[i]=std::bit_cast<float>(r.xf[at+i]);
            if(!std::isfinite(rows[i]))return false;
        }
        if(rows[3]!=0||rows[7]!=0||rows[8]!=0||rows[9]!=0||rows[10]!=-1||rows[11]!=0)return false;
        // v.y/w = (ndc.x + clip[2])/clip[1], and similarly for v.x/w.
        constexpr float xy[4][2]{{-1,-1},{1,-1},{1,1},{-1,1}};
        for(unsigned i=0;i<4;++i){
            const double y=(double(xy[i][0])+clip[2])/clip[1];
            const double x=(double(xy[i][1])+clip[6])/clip[4];
            for(unsigned a=0;a<2;++a){
                const double uv=rows[4*a]*x+rows[4*a+1]*y-rows[4*a+2];
                if(!std::isfinite(uv)||std::abs(uv)>32)return false;
                plan.corners[count][i][a]=float(uv);
            }
        }
        plan.units[count++]=unit;
    }
    if(count!=2||bases!=1||material.textures[plan.units[0]].coordinate==material.textures[plan.units[1]].coordinate)
        return false;
    // I4 expands the same value into R, G, B and A. Redirect every operand
    // that read either sample. Keep its inversion and all TEV equations.
    for(unsigned i=0;i<material.stageCount;++i)for(auto* channel:{&material.stages[i].rgb,&material.stages[i].alpha})
        for(auto& input:channel->inputs){
            const int unit=int(input.source)-int(MaterialSource::texture0);
            if(unit<0||unit>2)continue;
            if(unsigned(unit)==plan.baseUnit)input.source=MaterialSource::texture1;
            else{
                input.source=MaterialSource::texture0;
                input.operand=unsigned(unit)==plan.units[0]?MaterialOperand::red:MaterialOperand::alpha;
            }
        }
    material.textures[1]=material.textures[plan.baseUnit];
    material.textures[0]={255,255}; // Adapter-owned screen texture.
    material.textures[2]={};
    material.textureCount=2;
    material.projectiveTexture0=true;
    out=plan;
    return true;
}

inline bool screenProjectorsMatchPosition(const CommandRegisters& r,std::uint8_t projected,
    const DecodedVertex& v){
    const unsigned position=v.present&1?v.matrix[0]:r.matrixIndex[0]&63;
    for(unsigned c=0;c<4;++c)if(projected&(1u<<c)){
        const unsigned matrix=v.present&(1u<<(GX_VA_TEX0MTXIDX+c))?
            v.matrix[1+c]:(r.matrixIndex[0]>>(6*(c+1)))&63;
        if(matrix!=position)return false;
    }
    return true;
}

inline std::array<float,2> screenProjectorUV(const std::array<float,4>& clip,const std::array<float,2>& extent){
    return {(clip[0]+clip[3])*(extent[0]*.5f),(clip[1]+clip[3])*(extent[1]*.5f)};
}

}
