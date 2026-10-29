#include "game_source.hpp"
#include <bit>
#include <cstring>
namespace dusk::pica {
namespace {
struct Hash {
    std::array<std::uint32_t,8> h{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::array<std::uint8_t,64> block{};
    std::uint64_t bytes{};unsigned used{};
    void compress(){
        constexpr std::uint32_t k[]{0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        std::array<std::uint32_t,64> w{};
        for(unsigned i=0;i<16;++i)for(unsigned j=0;j<4;++j)w[i]=(w[i]<<8)|block[4*i+j];
        for(unsigned i=16;i<64;++i){const auto a=w[i-15],b=w[i-2];w[i]=w[i-16]+(std::rotr(a,7)^std::rotr(a,18)^(a>>3))+w[i-7]+(std::rotr(b,17)^std::rotr(b,19)^(b>>10));}
        auto a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],z=h[7];
        for(unsigned i=0;i<64;++i){const auto t=z+(std::rotr(e,6)^std::rotr(e,11)^std::rotr(e,25))+((e&f)^(~e&g))+k[i]+w[i];
            const auto u=(std::rotr(a,2)^std::rotr(a,13)^std::rotr(a,22))+((a&b)^(a&c)^(b&c));z=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;}
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=z;
    }
    void put(std::uint8_t b){block[used++]=b;++bytes;if(used==64){compress();used=0;}}
    void append(std::span<const std::byte> s){for(auto b:s)put(std::to_integer<std::uint8_t>(b));}
    std::array<std::uint8_t,32> finish(){const auto bits=bytes*8;put(0x80);while(used!=56)put(0);for(unsigned i=0;i<8;++i)put(std::uint8_t(bits>>(56-8*i)));
        std::array<std::uint8_t,32> out{};for(unsigned n=0;n<32;++n)out[n]=std::uint8_t(h[n/4]>>(24-8*(n%4)));return out;}
};
}
bool identifySource(std::span<const std::byte> original,SourceIdentity& out){
    out={};
    if(original.size()<32||original.size()>16u<<20)return false;
    if(std::memcmp(original.data(),"J3D2bmd3",8)&&std::memcmp(original.data(),"J3D2bmd2",8)&&
       std::memcmp(original.data(),"J3D2bdl3",8)&&std::memcmp(original.data(),"J3D2bdl4",8))return false;
    std::uint32_t declared=0;for(unsigned n=8;n<12;++n)declared=(declared<<8)|std::to_integer<unsigned>(original[n]);
    if(declared!=original.size())return false; // The archive supplies the actual expanded extent.
    Hash h;h.append(original);out.sha256=h.finish();out.bytes=std::uint32_t(original.size());return true;
}
bool copyGamePalette(const UniformBinding& binding,unsigned count,const float (*position)[3][4],const float (*normal)[3][3],Palette& out){
    out={};out.count=0;
    if(!count||count>65535||!position||!normal||!binding.paletteCount||binding.paletteCount>8)return false;
    Palette result{};result.count=binding.paletteCount;
    for(unsigned n=0;n<binding.paletteCount;++n){const unsigned id=binding.palette[n];if(!id||id>count)return false;
        const auto* p=reinterpret_cast<const std::byte*>(position)+(id-1)*48;
        const auto* q=reinterpret_cast<const std::byte*>(normal)+(id-1)*36;
        for(unsigned row=0;row<3;++row){std::memcpy(result.position[n*3+row].data(),p+row*16,16);
            std::memcpy(result.normal[n*3+row].data(),q+row*12,12);result.normal[n*3+row][3]=0;}
    }
    // Reuse the native numeric contract without requiring unrelated parameters.
    DrawUniforms check;check.palette=result;for(unsigned n=0;n<4;++n)check.viewProjection[n][n]=1;
    if(!validateUniforms(check,binding.paletteCount-1))return false;
    out=result;return true;
}
}
