#include "model_plan.hpp"
#include <bit>
#include <cstring>
#include <bitset>
namespace dusk::pica {
namespace {
unsigned byte(std::span<const std::byte> d,unsigned at){return std::to_integer<unsigned>(d[at]);}
std::uint32_t word(std::span<const std::byte> d,unsigned at){std::uint32_t value=0;for(unsigned n=0;n<4;++n)value|=byte(d,at+n)<<(8*n);return value;}
}
PackError ModelPlan::open(std::span<const std::byte> data,ModelPlan& out){
    out={};
    if(data.size()<48||data.size()>48+maxModelParts*64+maxModelParts*80||std::memcmp(data.data(),"DUSKML01",8)||word(data,8)!=4)return PackError::header;
    const auto count=word(data,16),slots=word(data,20),requirements=word(data,24),offset=word(data,32);
    if(!count||count>maxModelParts||!slots||slots>maxModelParts||requirements>63||word(data,12)!=data.size()||word(data,28)!=48||
       offset!=48+count*64||word(data,36)!=slots*80||data.size()!=offset+slots*80||word(data,40))return PackError::layout;
    if(crc32(data.first(44),data.subspan(48))!=word(data,44))return PackError::checksum;
    out.parts=allocateStorage<PartBinding>(count);out.uniforms=allocateStorage<UniformBinding>(slots);out.passes=allocateStorage<std::uint8_t>(count);
    if(!out.parts||!out.uniforms||!out.passes){out={};return PackError::allocation;}
    std::bitset<maxModelParts> used;
    for(unsigned n=0;n<count;++n){const auto at=48+n*64;auto& p=out.parts[n];p={};
        p.mesh=word(data,at);p.material=word(data,at+4);p.uniformSlot=word(data,at+8);const auto mask=word(data,at+12),pass=word(data,at+16);
        if(!p.mesh||!p.material||p.uniformSlot>=slots||mask>7||pass>1||word(data,at+20)!=n||word(data,at+60))return PackError::layout;
        p.samplerMask=std::uint8_t(mask);out.passes[n]=std::uint8_t(pass);used.set(p.uniformSlot);
        for(unsigned unit=0;unit<3;++unit){const auto start=at+24+unit*12;auto& s=p.samplers[unit];
            if(byte(data,start+7)||byte(data,start+10)||byte(data,start+11))return PackError::layout;
            s.wrapS=Wrap(byte(data,start));s.wrapT=Wrap(byte(data,start+1));s.magnify=Filter(byte(data,start+2));s.minify=Filter(byte(data,start+3));s.mipmap=Filter(byte(data,start+4));
            s.firstLevel=std::uint8_t(byte(data,start+5));s.lastLevel=std::uint8_t(byte(data,start+6));
            s.lodBias=std::bit_cast<std::int16_t>(std::uint16_t(byte(data,start+8)|(byte(data,start+9)<<8)));
            if(!validateSampler(s,8))return PackError::layout;
        }
    }
    if(used.count()!=slots)return PackError::layout;
    for(unsigned n=0;n<slots;++n){const auto at=offset+n*80;auto& u=out.uniforms[n];u={};
        u.material=word(data,at);u.anchor=word(data,at+4);u.paletteCount=word(data,at+8);
        if(!u.material||!u.anchor||u.anchor>4096||!u.paletteCount||u.paletteCount>4||byte(data,at+34)||byte(data,at+35)||
           byte(data,at+62)||byte(data,at+63)||byte(data,at+74)||byte(data,at+75)||word(data,at+76))return PackError::layout;
        for(unsigned p=0;p<4;++p){u.palette[p]=word(data,at+12+p*4);if(p<u.paletteCount?(!u.palette[p]||u.palette[p]>65535):u.palette[p]!=0)return PackError::layout;}
        for(unsigned unit=0;unit<3;++unit){
            const auto slot=byte(data,at+28+unit),generator=byte(data,at+31+unit);
            if((slot>7&&slot!=255)||(generator>7&&generator!=255)||(slot==255)!=(generator==255))return PackError::layout;
            u.textureSlots[unit]=std::uint8_t(slot);u.textureGenerators[unit]=std::uint8_t(generator);
        }
        u.sourceAttributes=word(data,at+36);
        u.channelReadMask=std::uint8_t(byte(data,at+40));
        if(u.channelReadMask>15)return PackError::layout;
        for(unsigned p=0;p<7;++p){
            const auto read=byte(data,at+41+p),wrap=byte(data,at+48+p),sign=byte(data,at+55+p);
            if(read>15||wrap>15||sign>15||(wrap&~read)||(sign&~read)||(wrap&sign)||(p>=3&&(wrap||sign)))return PackError::layout;
            u.parameterReadMasks[p]=std::uint8_t(read);u.parameterWrapMasks[p]=std::uint8_t(wrap);u.parameterSignedMasks[p]=std::uint8_t(sign);
        }
        u.sourceShape=word(data,at+64);u.billboardMode=word(data,at+68);
        if(u.sourceShape>4095||u.billboardMode>2)return PackError::layout;
        const auto stages=byte(data,at+72),texgens=byte(data,at+73);
        if(stages>16||texgens>8||(!stages&&texgens))return PackError::layout;
        u.sourceStageCount=std::uint8_t(stages);u.sourceTexGenCount=std::uint8_t(texgens);
    }
    for(unsigned n=0;n<count;++n)if(out.parts[n].material!=out.uniforms[out.parts[n].uniformSlot].material)return PackError::layout;
    out.count=count;out.uniformsCount=slots;out.requirements=requirements;return PackError::none;
}
bool ModelPlan::entry(unsigned index,PartBinding& part,unsigned& pass)const{
    part={};pass=0;if(index>=count)return false;part=parts[index];pass=passes[index];return true;
}
}
