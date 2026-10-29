#include "material_pack.hpp"
#include <bit>
#include <cstring>
namespace dusk::pica {
namespace {
std::uint32_t word(std::span<const std::byte> data,std::size_t at){
    std::uint32_t v=0;for(unsigned n=0;n<4;++n)v|=std::uint32_t(std::to_integer<unsigned>(data[at+n]))<<(8*n);return v;
}
unsigned byte(std::span<const std::byte> data,unsigned at){return std::to_integer<unsigned>(data[at]);}
bool zero(std::span<const std::byte> data){for(auto b:data)if(b!=std::byte{})return false;return true;}
}
PackError MaterialPack::open(std::span<const std::byte> data,MaterialPack& out){
    out={};if(data.size()<32||data.size()>32+64*(32+1248)||std::memcmp(data.data(),"DUSKMT01",8)||(word(data,8)!=1&&word(data,8)!=2))return PackError::header;
    const auto count=word(data,16),tableBytes=word(data,24);
    if(word(data,12)!=data.size()||!count||count>64||word(data,20)!=32||tableBytes!=count*32||data.size()<32+tableBytes)return PackError::bounds;
    if(crc32(data.first(28),data.subspan(32,tableBytes))!=word(data,28))return PackError::checksum;
    std::size_t cursor=32+tableBytes;std::uint32_t previous=0;
    for(unsigned n=0;n<count;++n){const auto at=32+n*32;
        const auto id=word(data,at),offset=word(data,at+4),size=word(data,at+8);
        if(id<=previous||offset!=cursor||size<64||size>1248||size%32||word(data,at+28)>3)return PackError::layout;
        if(cursor>data.size()||size>data.size()-cursor)return PackError::bounds;
        const auto requirements=word(data,at+28);
        out.entries[n]={id,offset,size,word(data,at+12),{word(data,at+16),word(data,at+20),word(data,at+24)},bool(requirements&1),bool(requirements&2)};
        cursor+=size;previous=id;
    }
    if(cursor!=data.size())return PackError::layout;
    out.bytes=data;out.count=count;out.version=word(data,8);return PackError::none;
}
PackError MaterialPack::material(std::uint32_t id,MaterialView& out)const{
    out={};if(!count)return PackError::header;
    unsigned lo=0,hi=count;
    while(lo<hi){const auto mid=lo+(hi-lo)/2;if(entries[mid].id<id)lo=mid+1;else hi=mid;}
    if(lo==count||entries[lo].id!=id)return PackError::missing;
    const auto& e=entries[lo];auto data=bytes.subspan(e.offset,e.size);
    if(crc32(data)!=e.checksum)return PackError::checksum;
    const unsigned stages=byte(data,0),flags=byte(data,6);
    if(!stages||stages>6||flags>63||!zero(data.subspan(10,2))||!zero(data.subspan(26,6)))return PackError::layout;
    const bool fog=flags&16;
    if(data.size()!=32+stages*32+(fog?1024:0))return PackError::layout;
    // Read into a local validated native object; never cast file bytes into
    // compiler-layout structs or expose a partially decoded output.
    Material m;m.stageCount=stages;m.textureMask=byte(data,1);m.blend=Blend(byte(data,2));m.cull=Cull(byte(data,3));
    m.depth=Compare(byte(data,4));m.alpha=Compare(byte(data,5));m.alphaReference=byte(data,7);
    m.depthTest=flags&1;m.depthWrite=flags&2;m.colorWrite=flags&4;m.alphaWrite=flags&8;m.fog=fog;m.fogDepthFlip=flags&32;
    m.bufferRgbMask=byte(data,8);m.bufferAlphaMask=byte(data,9);m.bufferColor=word(data,12);m.fogColor=word(data,16);
    m.customBlend={BlendEquation(byte(data,20)),BlendEquation(byte(data,21)),BlendFactor(byte(data,22)),BlendFactor(byte(data,23)),BlendFactor(byte(data,24)),BlendFactor(byte(data,25))};
    for(unsigned n=0;n<stages;++n){const auto at=32+n*32;auto& s=m.stages[n];
        if(!zero(data.subspan(at+(version==1?20:24),version==1?12:8)))return PackError::layout;
        if(version==2)for(unsigned c=0;c<4;++c)s.constantBindings[c]=std::uint8_t(byte(data,at+20+c));
        for(unsigned c=0;c<3;++c){s.rgb[c]=Source(byte(data,at+c));s.alpha[c]=Source(byte(data,at+3+c));s.rgbOperand[c]=ColorOperand(byte(data,at+6+c));s.alphaOperand[c]=AlphaOperand(byte(data,at+9+c));}
        s.rgbFunction=Combine(byte(data,at+12));s.alphaFunction=Combine(byte(data,at+13));s.rgbScale=byte(data,at+14);s.alphaScale=byte(data,at+15);s.constant=word(data,at+16);
    }
    if(fog)for(unsigned n=0;n<256;++n)m.fogValues[n]=std::bit_cast<float>(word(data,32+stages*32+n*4));
    if(!validateMaterial(m))return PackError::layout;
    for(unsigned n=0;n<3;++n)if(bool(m.textureMask&(1u<<n))!=bool(e.images[n]))return PackError::layout;
    out={id,m,e.images,e.requiresFog,e.initialSnapshot};return PackError::none;
}
bool instantiateMaterial(const MaterialView& view,const FogParameters* fog,Material& out,bool allowInitialSnapshot){
    out={};if(!view.id||(view.requiresFog&&!fog)||(view.initialSnapshotOnly&&!allowInitialSnapshot))return false;
    auto m=view.state;
    for(unsigned n=0;n<3;++n)if(bool(m.textureMask&(1u<<n))!=bool(view.imageIds[n]))return false;
    if(fog){m.fog=fog->enabled;m.fogDepthFlip=fog->depthFlip;m.fogColor=fog->color;m.fogValues=fog->values;}
    if(!validateMaterial(m))return false;
    out=m;return true;
}
}
