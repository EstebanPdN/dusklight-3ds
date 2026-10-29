#include "pack.hpp"
#include <cstring>
namespace dusk::pica {
namespace {
constexpr auto crcTable=[](){std::array<std::uint32_t,256> result{};for(unsigned i=0;i<256;++i){auto c=i;for(unsigned n=0;n<8;++n)c=(c>>1)^((c&1)?0xedb88320u:0);result[i]=c;}return result;}();
std::uint32_t word(std::span<const std::byte> data,std::size_t offset){
    std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t(std::to_integer<unsigned>(data[offset+i]))<<(8*i);return value;
}
bool zero(std::span<const std::byte> bytes){for(auto b:bytes)if(b!=std::byte{})return false;return true;}
}
std::uint32_t crc32(std::span<const std::byte> first,std::span<const std::byte> second){
    std::uint32_t c=0xffffffffu;for(auto span:{first,second})for(auto b:span)c=crcTable[(c^std::to_integer<unsigned>(b))&255]^(c>>8);return c^0xffffffffu;
}
PackError MeshPack::open(std::span<const std::byte> data,MeshPack& output){
    output={};
    if(data.size()<32||data.size()>64u<<20||std::memcmp(data.data(),"DUSKPN01",8)||word(data,8)!=1)return PackError::header;
    const auto count=word(data,16),tableBytes=word(data,24);
    if(word(data,12)!=data.size()||!count||count>64||word(data,20)!=32||tableBytes!=count*32||data.size()<32+tableBytes)return PackError::bounds;
    if(crc32(data.first(28),data.subspan(32,tableBytes))!=word(data,28))return PackError::checksum;
    std::size_t cursor=32+tableBytes;std::uint32_t previous=0;
    for(unsigned i=0;i<count;++i){const unsigned entry=32+i*32;
        const auto id=word(data,entry),vertices=word(data,entry+4),indices=word(data,entry+8);
        const auto vOffset=word(data,entry+12),iOffset=word(data,entry+16),vBytes=word(data,entry+20),iBytes=word(data,entry+24);
        if(id<=previous||!vertices||vertices>65536||!indices||indices>65535||indices%3||vBytes!=vertices*60||iBytes!=indices*2)return PackError::layout;
        if(vOffset!=cursor||iOffset!=align16(cursor+vBytes))return PackError::layout;
        const auto next=align16(std::size_t(iOffset)+iBytes);if(next>data.size())return PackError::bounds;
        if(!zero(data.subspan(cursor+vBytes,iOffset-cursor-vBytes))||!zero(data.subspan(iOffset+iBytes,next-iOffset-iBytes)))return PackError::layout;
        output.entries[i]={id,vOffset,iOffset,vBytes,iBytes,word(data,entry+28)};
        cursor=next;previous=id;
    }
    if(cursor!=data.size())return PackError::layout;
    output.bytes=data;output.count=count;return PackError::none;
}
PackError MeshPack::mesh(std::uint32_t id,MeshView& output)const{
    output={};if(!count)return PackError::header;
    unsigned low=0,high=count;
    while(low<high){auto middle=low+(high-low)/2;if(entries[middle].id<id)low=middle+1;else high=middle;}
    if(low==count||entries[low].id!=id)return PackError::missing;
    const auto& entry=entries[low];
    auto vertices=bytes.subspan(entry.vertexOffset,entry.vertexBytes);
    auto indices=bytes.subspan(entry.indexOffset,entry.indexBytes);
    if(crc32(vertices,indices)!=entry.checksum)return PackError::checksum;
    output={id,vertices,indices};return PackError::none;
}
}
