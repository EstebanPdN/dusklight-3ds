#pragma once
#include <array>
#include <cstdint>

namespace dusk::pica {
// Two 8x8 mathematical textures transport an independently interpolated RGBA
// color through two spare UV pairs. Only the first 2x2 texel centers are used:
// bilinear interpolation reconstructs RG in one sample and BA in the other.
// Multiplication remains in the fragment combiner, never at the vertices.
constexpr std::array<std::uint8_t,4> colorLookupTexel(unsigned plane,unsigned x,unsigned y){
    const auto a=std::uint8_t(x?255:0),b=std::uint8_t(y?255:0);
    return plane?std::array<std::uint8_t,4>{0,0,a,b}:std::array<std::uint8_t,4>{a,b,0,0};
}
inline std::array<float,2> colorLookupUv(float x,float y){
    return {(.5f+x)/8.f,1.f-(.5f+y)/8.f};
}
constexpr auto colorLookupBti(unsigned plane){
    std::array<std::uint8_t,288> bytes{};
    bytes[0]=6;bytes[3]=bytes[5]=8;bytes[20]=bytes[21]=1;bytes[24]=1;bytes[31]=32;
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){
        const auto color=colorLookupTexel(plane,x,y);
        const auto at=32+((y/4)*2+x/4)*64+((y%4)*4+x%4)*2;
        bytes[at]=color[3];bytes[at+1]=color[0];bytes[at+32]=color[1];bytes[at+33]=color[2];
    }
    return bytes;
}
constexpr auto colorLookupImage(unsigned plane){
    std::array<std::uint8_t,256> bytes{};
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){
        const auto color=colorLookupTexel(plane,x,y);
        unsigned texel=0;for(unsigned bit=0;bit<3;++bit)texel|=((x>>bit)&1)<<(2*bit)|((y>>bit)&1)<<(2*bit+1);
        for(unsigned component=0;component<4;++component)bytes[4*texel+component]=color[3-component];
    }
    return bytes;
}
}
