#include "texture_pack.hpp"
#include <cstring>
namespace dusk::pica {
namespace {
std::uint32_t word(std::span<const std::byte> data,std::size_t at){
    std::uint32_t v=0;for(unsigned i=0;i<4;++i)v|=std::uint32_t(std::to_integer<unsigned>(data[at+i]))<<(8*i);return v;
}
}
PackError TexturePack::open(std::span<const std::byte> data,TexturePack& out){
    out={};
    if(data.size()<32||data.size()>64u<<20||std::memcmp(data.data(),"DUSKTX01",8)||word(data,8)!=1)return PackError::header;
    const auto count=word(data,16),tableBytes=word(data,24);
    if(word(data,12)!=data.size()||!count||count>64||word(data,20)!=32||tableBytes!=count*32||data.size()<32+tableBytes)return PackError::bounds;
    if(crc32(data.first(28),data.subspan(32,tableBytes))!=word(data,28))return PackError::checksum;
    std::size_t cursor=32+tableBytes;std::uint32_t previous=0;
    for(unsigned n=0;n<count;++n){
        const auto at=32+n*32;
        const auto id=word(data,at),width=word(data,at+4),height=word(data,at+8);
        const auto format=word(data,at+12),levels=word(data,at+16),offset=word(data,at+20),size=word(data,at+24);
        if(id<=previous||width>1024||height>1024||format>unsigned(TextureFormat::etc1a4))return PackError::layout;
        TextureInfo info{std::uint16_t(width),std::uint16_t(height),TextureFormat(format)};info.mipLevels=levels;
        const auto expected=textureBytes(info);
        if(!expected||size!=expected||offset!=cursor)return PackError::layout;
        // Check subtraction before forming a span or aligned next offset.
        if(cursor>data.size()||size>data.size()-cursor)return PackError::bounds;
        const auto next=align16(cursor+size);if(next>data.size())return PackError::bounds;
        for(auto b:data.subspan(cursor+size,next-cursor-size))if(b!=std::byte{})return PackError::layout;
        out.entries[n]={id,offset,size,word(data,at+28),{info.width,info.height,info.format,levels}};
        cursor=next;previous=id;
    }
    if(cursor!=data.size())return PackError::layout;
    out.bytes=data;out.count=count;return PackError::none;
}
PackError TexturePack::image(std::uint32_t id,TextureView& out)const{
    out={};if(!count)return PackError::header;
    unsigned lo=0,hi=count;
    while(lo<hi){const auto mid=lo+(hi-lo)/2;if(entries[mid].id<id)lo=mid+1;else hi=mid;}
    if(lo==count||entries[lo].id!=id)return PackError::missing;
    const auto& e=entries[lo];const auto payload=bytes.subspan(e.offset,e.size);
    if(crc32(payload)!=e.checksum)return PackError::checksum;
    out={id,e.image,payload};return PackError::none;
}
}
