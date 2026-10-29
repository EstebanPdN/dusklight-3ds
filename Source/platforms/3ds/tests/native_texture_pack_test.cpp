#include "native/texture_pack.hpp"
#include <cassert>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include <cstdio>
using namespace dusk::pica;
namespace {
std::uint32_t read(const std::vector<std::byte>& b,unsigned at){std::uint32_t v=0;for(unsigned n=0;n<4;++n)v|=std::uint32_t(std::to_integer<unsigned>(b[at+n]))<<(8*n);return v;}
void write(std::vector<std::byte>& b,unsigned at,std::uint32_t v){for(unsigned n=0;n<4;++n)b[at+n]=std::byte(v>>(8*n));}
void repair(std::vector<std::byte>& b){write(b,28,crc32(std::span(b).first(28),std::span(b).subspan(32,read(b,24))));}
}
int main(int argc,char** argv){
    assert(argc==2);std::ifstream file(argv[1],std::ios::binary);assert(file.good());
    std::vector<char> raw((std::istreambuf_iterator<char>(file)),{});std::vector<std::byte> bytes(raw.size());std::memcpy(bytes.data(),raw.data(),raw.size());
    TexturePack pack;assert(TexturePack::open(bytes,pack)==PackError::none&&pack.size()==4);
    TextureView image;
    for(unsigned id=1;id<=4;++id){assert(pack.image(id,image)==PackError::none);assert(unsigned(image.image.format)==id-1);
        assert(image.image.width==16&&image.image.height==16&&image.image.mipLevels==2);
        TextureInfo info{16,16,TextureFormat(id-1)};info.mipLevels=2;assert(image.payload.size()==textureBytes(info));
    }
    assert(pack.image(9,image)==PackError::missing&&image.payload.empty());
    std::vector<std::byte> unaligned(bytes.size()+1);std::memcpy(unaligned.data()+1,bytes.data(),bytes.size());
    assert(TexturePack::open(std::span(unaligned).subspan(1),pack)==PackError::none&&pack.image(1,image)==PackError::none);
    unsigned rejected=0;
    for(unsigned n=0;n<bytes.size();++n){TexturePack bad;assert(TexturePack::open(std::span(bytes).first(n),bad)!=PackError::none&&bad.size()==0);++rejected;}
    for(unsigned n=0;n<160;++n){auto bad=bytes;bad[n]^=std::byte{1};TexturePack p;assert(TexturePack::open(bad,p)!=PackError::none);++rejected;}
    for(auto [offset,value]:std::vector<std::pair<unsigned,std::uint32_t>>{{32,0},{64,1},{36,65544},{40,0},{44,256},{48,3},{52,0xffffffff},{56,256},{68,1025}}){
        auto bad=bytes;write(bad,offset,value);repair(bad);TexturePack p;assert(TexturePack::open(bad,p)!=PackError::none&&p.size()==0);++rejected;
    }
    {auto edited=bytes;TexturePack p;assert(TexturePack::open(edited,p)==PackError::none);write(edited,52,0xffffffff);write(edited,36,0xffffffff);
        assert(p.image(1,image)==PackError::none&&image.image.width==16&&image.payload.size()==1280);}
    {auto bad=bytes;bad[read(bad,52)]^=std::byte{1};TexturePack p;assert(TexturePack::open(bad,p)==PackError::none);
        assert(p.image(1,image)==PackError::checksum&&image.payload.empty());++rejected;}
    std::printf("PASS four native image formats, mip chains, owned metadata, unaligned input and %u malformed/truncated rejections\n",rejected);
}
