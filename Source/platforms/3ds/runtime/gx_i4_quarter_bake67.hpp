#pragma once
#include "gx_pica_material.hpp"
#include "gx_state.hpp"
#include <algorithm>

namespace dusk::native::gx::bake67 {
enum class Policy { disabled, strictNonzeroI4 };
enum class Error { none, disabled, material, constant, binding, swap, texture, sampler, extent, zero, alias };
inline constexpr unsigned maxAxis=16,maxBtiBytes=32+maxAxis*maxAxis*2;
struct Result {
    std::array<std::uint8_t,maxBtiBytes> bti{};
    unsigned bytes{};
    std::uint16_t intensities{}; // Nibble-domain proof mask; bit0 always clear.
    PicaMaterialPlan material{};
};
inline constexpr std::uint8_t quarter(std::uint8_t intensity) {
    return std::uint8_t((unsigned(intensity)+(intensity>>7)+2)>>2);
}
inline bool unitPrev(const MaterialOperation& op,unsigned kind){
    return op.operation==kind&&!op.bias&&!op.scale&&op.clamp==1&&!op.output;
}
inline unsigned be16(const std::uint8_t* p){return (unsigned(p[0])<<8)|p[1];}
inline std::uint32_t be32(const std::uint8_t* p){return (std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|(std::uint32_t(p[2])<<8)|p[3];}
inline bool overlaps(std::span<const std::uint8_t> input,const void* output,std::size_t size){
    const auto a=reinterpret_cast<std::uintptr_t>(input.data()),b=reinterpret_cast<std::uintptr_t>(output);
    return a<=b?b-a<input.size():a-b<size;
}
// Provisional material-shape recognition only. The nonzero-image proof in bake
// remains mandatory before substitution; this function alone admits no draw.
inline bool plan(const MaterialState& state,PicaMaterialPlan& out,
                 Policy policy=Policy::disabled,Error* error=nullptr){
    if(error)*error=Error::none;
    auto fail=[&](Error why){if(error)*error=why;return false;};
    if(policy!=Policy::strictNonzeroI4)return fail(Error::disabled);
    if(overlaps({reinterpret_cast<const std::uint8_t*>(&state),sizeof(state)},&out,sizeof(out)))return fail(Error::alias);
    if(state.stageCount!=2||!state.texgenCount||state.texgenCount>8||state.channelCount>2||state.indirectCount)
        return fail(Error::material);
    const auto& a=state.stages[0];const auto& b=state.stages[1];
    if(a.indirect||b.indirect||a.color!=std::array<std::uint8_t,4>{15,14,8,4}||
       a.alpha!=std::array<std::uint8_t,4>{7,7,7,7}||b.color!=std::array<std::uint8_t,4>{0,6,0,15}||
       b.alpha!=std::array<std::uint8_t,4>{7,7,7,4}||!unitPrev(a.colorOp,0)||!unitPrev(a.alphaOp,0)||
       !unitPrev(b.colorOp,8)||!unitPrev(b.alphaOp,0)||a.konstColor!=6)return fail(Error::material);
    if((state.colorValid&12)!=12)return fail(Error::constant);
    const auto& d=state.colors[2];
    if(d[0]<0||d[0]>254||state.colors[3][0]!=d[0]||d[1]<0||d[1]>255||d[2]<0||d[2]>255)
        return fail(Error::constant);
    if(!a.textureEnabled||!b.textureEnabled||a.texture>=8||a.coordinate>=state.texgenCount||
       a.texture!=b.texture||a.coordinate!=b.coordinate)return fail(Error::binding);
    for(unsigned i=0;i<2;++i){const auto sw=state.stages[i].textureSwap;
        if(sw>=4||!(state.swapValid&(1u<<sw))||state.swaps[sw]!=std::array<std::uint8_t,4>{0,1,2,3})
            return fail(Error::swap);}
    PicaMaterialPlan result{};result.stageCount=1;result.textureCount=1;result.textures[0]={a.texture,a.coordinate};
    auto& stage=result.stages[0];stage.rgb.function=MaterialFunction::add;
    stage.rgb.inputs[0]={MaterialSource::texture0,MaterialOperand::color,false};
    stage.rgb.inputs[1]={MaterialSource::constant,MaterialOperand::color,false};
    for(unsigned c=0;c<3;++c)stage.constant[c]=std::uint8_t(d[c]);
    stage.alpha.inputs[0]={MaterialSource::texture0,MaterialOperand::alpha,false};
    out=result;return true;
}
// Pure, allocation-free, transactional helper. No registration/binding/cache/GPU
// calls. All failures leave output and input untouched. The caller still owns
// original material/draw/Q/raster validation and lifetime-safe publication.
inline bool bake(const MaterialState& state,const Texture& texture,Result& out,
                 Policy policy=Policy::disabled,Error* error=nullptr){
    if(error)*error=Error::none;
    auto fail=[&](Error why){if(error)*error=why;return false;};
    if(policy!=Policy::strictNonzeroI4)return fail(Error::disabled);
    if(overlaps({reinterpret_cast<const std::uint8_t*>(&state),sizeof(state)},&out,sizeof(out))||
       overlaps(texture.bti,&out,sizeof(out))||overlaps(texture.image,&out,sizeof(out)))return fail(Error::alias);
    PicaMaterialPlan matched;
    if(!plan(state,matched,policy,error))return false;
    if(!texture.handle||!texture.immutablePixels||texture.format!=0||texture.bti.size()<32)
        return fail(Error::texture);
    const auto* header=texture.bti.data();
    if(texture.wrapS||texture.wrapT||texture.minFilter||texture.magFilter||texture.mipCount!=1||
       texture.minLod||texture.maxLod||texture.lodBias!=0||header[6]||header[7]||header[16]||
       header[17]||header[18]||header[19]||header[20]||header[21]||header[22]||header[23]||
       header[24]!=1||header[26]||header[27])return fail(Error::sampler);
    const unsigned width=be16(header+2),height=be16(header+4);
    if(header[0]!=0||header[8]||be16(header+10)||(width!=8&&width!=16)||(height!=8&&height!=16)||
       texture.width!=width||texture.height!=height)return fail(Error::texture);
    const std::size_t offset=be32(header+28),sourceBytes=((width+7)/8)*((height+7)/8)*32;
    if(offset<32||offset>texture.bti.size()||sourceBytes>texture.bti.size()-offset||
       texture.image.data()!=header+offset||texture.image.size()!=sourceBytes)return fail(Error::extent);
    const auto* pixels=texture.image.data();
    auto nibble=[&](unsigned x,unsigned y){const unsigned at=((y/8)*((width+7)/8)+x/8)*32+(y%8)*4+(x%8)/2;
        return unsigned((pixels[at]>>((x&1)?0:4))&15);};
    std::uint16_t seen=0;
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){const auto n=nibble(x,y);
        if(!n)return fail(Error::zero);
        seen|=std::uint16_t(1u<<n);}
    // Every logical nibble is nonzero: Q>=4. min(255,D.r+Q)>D.r for D.r<=254.
    // Original R8_GT is therefore true for every reachable clamped sample.
    Result result{};result.intensities=seen;
    result.bytes=32+((width+3)/4)*((height+3)/4)*32;
    std::copy_n(header,32,result.bti.begin());
    result.bti[0]=3;result.bti[1]=1; // GX IA8: bytes A,I; decoded PICA LA8.
    for(unsigned i=8;i<16;++i)result.bti[i]=0;
    result.bti[28]=result.bti[29]=result.bti[30]=0;result.bti[31]=32;
    for(unsigned y=0;y<((height+3)/4)*4;++y)for(unsigned x=0;x<((width+3)/4)*4;++x){
        const auto intensity=std::uint8_t(nibble(std::min(x,width-1),std::min(y,height-1))*17);
        const auto at=32+((y/4)*((width+3)/4)+x/4)*32+((y%4)*4+x%4)*2;
        result.bti[at]=intensity;result.bti[at+1]=quarter(intensity);
    }
    result.material=matched;
    out=result;return true;
}
}
