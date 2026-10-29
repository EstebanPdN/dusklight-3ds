#include "asset_pack.hpp"
#include <cstring>
namespace dusk::pica {
namespace {
std::uint32_t word(std::span<const std::byte> b,std::size_t at){std::uint32_t v=0;for(unsigned n=0;n<4;++n)v|=std::to_integer<unsigned>(b[at+n])<<(8*n);return v;}
}
PackError AssetPack::open(std::span<const std::byte> b,const SourceIdentity& expected,AssetPack& out){
    out={};
    if(b.size()<128||b.size()>maxBytes||std::memcmp(b.data(),"DUSKNA01",8)||word(b,8)!=1)return PackError::header;
    const unsigned count=word(b,20);
    if(word(b,12)!=b.size()||!expected.bytes||word(b,16)!=expected.bytes||!count||count>64||std::memcmp(b.data()+24,expected.sha256.data(),32))return PackError::layout;
    if(word(b,96)>1)return PackError::layout;
    for(auto v:b.subspan(100,24))if(v!=std::byte{})return PackError::layout;
    if(crc32(b.first(124),b.subspan(128))!=word(b,124))return PackError::checksum;
    std::array<std::span<const std::byte>,5> parts{};std::size_t cursor=128;
    for(unsigned n=0;n<5;++n){const auto offset=word(b,56+n*8),bytes=word(b,60+n*8);
        if(offset!=cursor||bytes>b.size()-cursor||(!bytes&&n!=1))return PackError::layout;
        parts[n]=b.subspan(cursor,bytes);const auto next=align16(cursor+bytes);if(next>b.size())return PackError::bounds;
        for(auto v:b.subspan(cursor+bytes,next-cursor-bytes))if(v!=std::byte{})return PackError::layout;
        cursor=next;
    }
    if(cursor!=b.size()||parts[4].size()!=count*4)return PackError::layout;
    if(auto e=MeshPack::open(parts[0],out.meshes);e!=PackError::none)return e;
    if(!parts[1].empty())if(auto e=TexturePack::open(parts[1],out.images);e!=PackError::none)return e;
    if(auto e=MaterialPack::open(parts[2],out.materials);e!=PackError::none)return e;
    if(auto e=ModelPlan::open(parts[3],out.plan);e!=PackError::none)return e;
    if(out.plan.size()!=count){out.plan={};return PackError::layout;}
    for(unsigned n=0;n<count;++n){out.shapes[n]=word(parts[4],n*4);if(out.shapes[n]>=4096){out.plan={};return PackError::layout;}}
    out.requiresTextureContext=word(b,96)!=0;return PackError::none;
}
}
