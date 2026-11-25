#pragma once
// worldMips (perf_features.hpp): which texture units of a draw are plain
// world-model samples that may read generated mip levels (texture_gx.hpp).
// Read from the registers of the draw itself, at every bind, so that the same
// image keeps its base level wherever the game samples it another way.
// Never: 2D (J2D, HUD, fonts) and particles, which are immediate draws, not
// shapes of a static display list; orthographic draws; colour or emboss ramps
// (texgen from a colour channel or a bump row); environment, projected and
// shadow lookups (texgen from position, normal or binormal, or a projective
// 3x4 texgen); indirect maps and every texture of a draw with an indirect
// stage or command (a warp reads coordinates, not a picture). Images the game
// can rewrite (copies, captures) never get levels (gx_texture_pica.cpp).
#include "gx_commands.hpp"
#include "gx_world_mip_use.hpp"
#include <cstdint>

namespace dusk::native::gx {
// The draw: a shape of a static display list (J3D room and actor models), in
// perspective, with no indirect stage and no indirect command on its stages.
inline bool worldMipDraw(const CommandRegisters& r,bool staticList){
    if(!staticList||!r.xfValid[0x1026]||r.xf[0x1026]!=0||!r.bpValid[0x00])return false;
    const auto gen=r.bp[0x00];
    if((gen>>16)&7)return false;
    const unsigned stages=((gen>>10)&15)+1;
    for(unsigned s=0;s<stages;++s)if(!r.bpValid[0x10+s]||(r.bp[0x10+s]&0x1fffff))return false;
    return true;
}
// One coordinate: a regular 2x4 (ST) texgen of a vertex texture coordinate
// (rows 5..12) with the identity post matrix, not normalized.
inline bool worldMipCoordinate(const CommandRegisters& r,unsigned coordinate){
    if(coordinate>7||!r.xfValid[0x1040+coordinate]||!r.xfValid[0x1050+coordinate])return false;
    const auto config=r.xf[0x1040+coordinate],post=r.xf[0x1050+coordinate];
    const unsigned type=(config>>4)&7,row=(config>>7)&31,projective=(config>>1)&1;
    return type==0&&!projective&&row>=5&&row<=12&&(post&63)==61&&!((post>>8)&1);
}
// The alpha test (BP 0xF3) as one lower threshold: true when the alphas that
// pass are exactly [reference, 255] (reference 0: every alpha passes).
inline bool alphaThreshold(std::uint32_t compare,std::uint8_t& reference){
    auto passes=[](unsigned function,unsigned ref,unsigned a){
        switch(function){
        case 0:return false;      case 1:return a<ref;  case 2:return a==ref; case 3:return a<=ref;
        case 4:return a>ref;      case 5:return a!=ref; case 6:return a>=ref; default:return true;
        }
    };
    const unsigned ref0=compare&255,ref1=(compare>>8)&255,f0=(compare>>16)&7,f1=(compare>>19)&7,op=(compare>>22)&3;
    int first=-1;
    for(unsigned a=0;a<256;++a){
        const bool p0=passes(f0,ref0,a),p1=passes(f1,ref1,a);
        const bool pass=op==0?(p0&&p1):op==1?(p0||p1):op==2?(p0!=p1):(p0==p1);
        if(pass&&first<0)first=int(a);
        if(!pass&&first>=0)return false;
    }
    if(first<0)return false;
    reference=std::uint8_t(first);return true;
}
// The use of the unit sampling through `coordinate` in a draw that passed
// worldMipDraw (false for any other draw).
inline GxWorldMipUse worldMipUse(const CommandRegisters& r,bool worldDraw,unsigned coordinate){
    GxWorldMipUse use{};
    if(!worldDraw||!worldMipCoordinate(r,coordinate))return use;
    use.world=true;
    use.thresholdKnown=r.bpValid[0xf3]&&alphaThreshold(r.bp[0xf3],use.reference);
    return use;
}
}
