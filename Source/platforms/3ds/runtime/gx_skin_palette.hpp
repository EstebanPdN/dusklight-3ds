#pragma once
// Drawing side of the skin palette (runtime/skin_palette.hpp has the whole
// picture): a merged list's palette, as the game thread wrote it into the
// bank's XF words, placed into the rows of a multi-texture plan.
#include "gx_rigid_vertex.hpp"
#include "skin_palette.hpp"
#include <bit>
#include <cmath>

namespace dusk::native::gx {
// Why a merged draw was not planned (the plan of the drawing side; the
// group-by-group fallback then draws the list's groups).
enum class SkinRefusal : unsigned {header,program,lights,rows,matrix,decode,count};
// Places the palette of the merged list `generation` into `p`, a plan the
// multi-texture planner made with slot 0 as its first matrix: entry k at
// skin::row(k), position rows only (the shader's normal rows are the same
// rows, see skin_palette.hpp). False, `p` unusable for this draw: the bank
// holds no palette for this list, or the plan has rows the palette may not
// take (more than skin::maxLights lights, the units' rows, a projected unit,
// per-bone texgens, a warp, a third coordinate).
inline bool placeSkinPalette(const CommandRegisters& r,std::uint32_t generation,RigidVertexPlan& p,SkinRefusal* why=nullptr){
    auto refuse=[&](SkinRefusal reason){if(why)*why=reason;return false;};
    if(!r.xfValid[skin::xfHeader]||!r.xfValid[skin::xfList]||(r.xf[skin::xfHeader]>>16)!=skin::headerMagic||r.xf[skin::xfList]!=generation)
        return refuse(SkinRefusal::header);
    const unsigned n=r.xf[skin::xfHeader]&0xffff;
    if(!n||n>skin::maxMatrices)return refuse(SkinRefusal::header);
    if(!p.multiTexture||p.projectedTexture||p.multiProjected||p.pairedUnits||p.wideRecord||p.vertexWarp.enabled)
        return refuse(SkinRefusal::program);
    if(p.paletteBase()>skin::paletteRow)return refuse(SkinRefusal::lights);
    if(skin::rowsReached(n)>p.paletteLimit()||skin::rowsReached(n)>p.uniforms.size())return refuse(SkinRefusal::rows);
    for(unsigned k=0;k<n;++k){
        const unsigned at=skin::xfMatrices+12*k,row=skin::row(k);
        for(unsigned i=0;i<12;++i){
            if(!r.xfValid[at+i])return refuse(SkinRefusal::matrix);
            const float value=std::bit_cast<float>(r.xf[at+i]);
            if(!std::isfinite(value)||std::abs(value)>1e8f)return refuse(SkinRefusal::matrix);
            p.uniforms[row+i/4][i%4]=value;
        }
    }
    p.matrixOffsets.fill(-1);p.normalOffsets.fill(-1);
    p.matrices=n;p.compactPalette=true;p.paletteEnd=std::max(skin::rowsReached(n),6u);
    p.skinEntries=std::uint8_t(n);
    return true;
}
// The matrix row byte a record of a merged draw carries for the vertex whose
// list byte is `index` (3k for entry k); -1 when it names no entry.
inline int skinRecordRow(const RigidVertexPlan& p,unsigned index){
    return index%3||index/3>=p.skinEntries?-1:int(skin::row(index/3));
}
}
