#pragma once
#include "gx_i4_quarter_bake67.hpp"

namespace dusk::native::gx::bake84 {
// Separate contract: bake67's strictNonzeroI4 policy and IA8 bytes are unchanged.
enum class Policy { disabled, exactNearestI4 };
using Error=bake67::Error;
inline constexpr unsigned maxAxis=16,maxBtiBytes=32+maxAxis*maxAxis*4;
using Color=std::array<std::uint8_t,4>;
using Palette=std::array<Color,16>;
struct Result {
    std::array<std::uint8_t,maxBtiBytes> bti{};
    unsigned bytes{};
    std::uint16_t intensities{}; // Includes zero when present; alpha is never dropped.
    PicaMaterialPlan material{};
};
// GX ADD: ZERO + KONST(1/4)*TEXC + C1, clamped, then
// R8_GT(PREV,C2,PREV,ZERO). C2 is an A/B input (low eight bits).
// Restrict consumed D.rgb/T.red to unsigned bytes rather than general signed TEV.
inline Palette palette(const MaterialState& state){
    Palette result{};
    for(unsigned n=0;n<16;++n){
        const unsigned intensity=17*n,q=bake67::quarter(std::uint8_t(intensity));
        const bool pass=std::min(255u,unsigned(state.colors[2][0])+q)>unsigned(state.colors[3][0]);
        for(unsigned c=0;c<3;++c)result[n][c]=pass?std::uint8_t(std::min(255u,unsigned(state.colors[2][c])+q)):0;
        result[n][3]=std::uint8_t(intensity);
    }
    return result;
}
// Provisional only. This material plan cannot publish a draw until bake() has
// validated the freshly resolved immutable source header, sampler and extent.
inline bool plan(const MaterialState& state,PicaMaterialPlan& out,
                 Policy policy=Policy::disabled,Error* error=nullptr){
    if(error)*error=Error::none;
    auto fail=[&](Error why){if(error)*error=why;return false;};
    if(policy!=Policy::exactNearestI4)return fail(Error::disabled);
    if(bake67::overlaps({reinterpret_cast<const std::uint8_t*>(&state),sizeof(state)},&out,sizeof(out)))return fail(Error::alias);
    if(state.stageCount!=2||!state.texgenCount||state.texgenCount>8||state.channelCount>2||state.indirectCount)return fail(Error::material);
    const auto& a=state.stages[0];const auto& b=state.stages[1];
    if(a.indirect||b.indirect||a.color!=std::array<std::uint8_t,4>{15,14,8,4}||
       a.alpha!=std::array<std::uint8_t,4>{7,7,7,7}||b.color!=std::array<std::uint8_t,4>{0,6,0,15}||
       b.alpha!=std::array<std::uint8_t,4>{7,7,7,4}||!bake67::unitPrev(a.colorOp,0)||!bake67::unitPrev(a.alphaOp,0)||
       !bake67::unitPrev(b.colorOp,8)||!bake67::unitPrev(b.alphaOp,0)||a.konstColor!=6)return fail(Error::material);
    if((state.colorValid&12)!=12)return fail(Error::constant);
    for(unsigned c=0;c<3;++c)if(state.colors[2][c]<0||state.colors[2][c]>255)return fail(Error::constant);
    if(state.colors[3][0]<0||state.colors[3][0]>255)return fail(Error::constant);
    if(!a.textureEnabled||!b.textureEnabled||a.texture>=8||a.coordinate>=state.texgenCount||
       a.texture!=b.texture||a.coordinate!=b.coordinate)return fail(Error::binding);
    // Only texture swaps are consumed by these equations. Both must be valid
    // identity; raster swaps and unconsumed konst registers have no dependency.
    for(unsigned i=0;i<2;++i){const auto sw=state.stages[i].textureSwap;
        if(sw>=4||!(state.swapValid&(1u<<sw))||state.swaps[sw]!=std::array<std::uint8_t,4>{0,1,2,3})return fail(Error::swap);}
    PicaMaterialPlan result{};result.stageCount=1;result.textureCount=1;result.textures[0]={a.texture,a.coordinate};
    result.stages[0].rgb.inputs[0]={MaterialSource::texture0,MaterialOperand::color,false};
    result.stages[0].alpha.inputs[0]={MaterialSource::texture0,MaterialOperand::alpha,false};
    out=result;return true;
}
// Pure, allocation-free and transactional. Nearest/clamp/single-level means
// every sample selects one nibble: the 16-entry complete RGBA table is exact
// independently of source content, including zero and compare-false low alpha.
inline bool bake(const MaterialState& state,const Texture& texture,Result& out,
                 Policy policy=Policy::disabled,Error* error=nullptr){
    if(error)*error=Error::none;
    auto fail=[&](Error why){if(error)*error=why;return false;};
    if(policy!=Policy::exactNearestI4)return fail(Error::disabled);
    if(bake67::overlaps({reinterpret_cast<const std::uint8_t*>(&state),sizeof(state)},&out,sizeof(out))||
       bake67::overlaps({reinterpret_cast<const std::uint8_t*>(&texture),sizeof(texture)},&out,sizeof(out))||
       bake67::overlaps(texture.bti,&out,sizeof(out))||bake67::overlaps(texture.image,&out,sizeof(out)))return fail(Error::alias);
    PicaMaterialPlan matched;
    if(!plan(state,matched,policy,error))return false;
    if(!texture.handle||!texture.immutablePixels||texture.format!=0||texture.bti.size()<32)return fail(Error::texture);
    const auto* header=texture.bti.data();
    if(texture.wrapS||texture.wrapT||texture.minFilter||texture.magFilter||texture.mipCount!=1||
       texture.minLod||texture.maxLod||texture.lodBias!=0||header[6]||header[7]||header[16]||
       header[17]||header[18]||header[19]||header[20]||header[21]||header[22]||header[23]||
       header[24]!=1||header[26]||header[27])return fail(Error::sampler);
    const unsigned width=bake67::be16(header+2),height=bake67::be16(header+4);
    if(header[0]!=0||header[8]||bake67::be16(header+10)||!width||width>maxAxis||!height||height>maxAxis||
       texture.width!=width||texture.height!=height)return fail(Error::texture);
    const std::size_t offset=bake67::be32(header+28),sourceBytes=((width+7)/8)*((height+7)/8)*32;
    if(offset<32||offset>texture.bti.size()||sourceBytes>texture.bti.size()-offset||
       texture.image.data()!=header+offset||texture.image.size()!=sourceBytes)return fail(Error::extent);
    const auto table=palette(state);
    const auto* pixels=texture.image.data();
    auto nibble=[&](unsigned x,unsigned y){const unsigned at=((y/8)*((width+7)/8)+x/8)*32+(y%8)*4+(x%8)/2;
        return unsigned((pixels[at]>>((x&1)?0:4))&15);};
    Result result{};result.bytes=32+((width+3)/4)*((height+3)/4)*64;
    // Canonical GX RGBA8: 4x4 tiles, first A/R pairs then G/B pairs.
    result.bti[0]=6;result.bti[1]=1;result.bti[2]=std::uint8_t(width>>8);result.bti[3]=std::uint8_t(width);
    result.bti[4]=std::uint8_t(height>>8);result.bti[5]=std::uint8_t(height);result.bti[24]=1;result.bti[31]=32;
    for(unsigned y=0;y<((height+3)/4)*4;++y)for(unsigned x=0;x<((width+3)/4)*4;++x){
        const auto n=nibble(std::min(x,width-1),std::min(y,height-1));const auto& rgba=table[n];
        if(x<width&&y<height)result.intensities|=std::uint16_t(1u<<n);
        const auto at=32+((y/4)*((width+3)/4)+x/4)*64+((y%4)*4+x%4)*2;
        result.bti[at]=rgba[3];result.bti[at+1]=rgba[0];result.bti[at+32]=rgba[1];result.bti[at+33]=rgba[2];
    }
    result.material=matched;out=result;return true;
}
}
