#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>

namespace dusk::native::gx {
// Original grass VAT: direct XYZ F32, indexed normal, direct RGBA8, indexed UV.
// Only packet serialization changes. The normal/color/UV indices, vertex order,
// values and regular command processor validation remain unchanged.
inline std::array<std::uint8_t,18> grassVertexWire(float x,float y,float z,
    std::uint8_t normal,const std::array<std::uint8_t,4>& color,std::uint8_t uv){
    std::array<std::uint8_t,18> bytes;
    const float position[]{x,y,z};
    for(unsigned c=0;c<3;++c){
        auto bits=std::bit_cast<std::uint32_t>(position[c]);
        if constexpr(std::endian::native==std::endian::little)bits=__builtin_bswap32(bits);
        std::memcpy(bytes.data()+4*c,&bits,4);
    }
    bytes[12]=normal;
    std::memcpy(bytes.data()+13,color.data(),4);
    bytes[17]=uv;
    return bytes;
}
// 3D line vertex (mDoExt_3DlineMat0_c and mDoExt_3DlineMat1_c with every
// attribute direct): XYZ F32, normal S8 x3 and, with `st`, ST F32. 15 or 23
// bytes at `out`, the bytes the GX vertex calls write; returns their count.
inline unsigned lineVertexWire(std::uint8_t* out,const float* position,const std::int8_t* normal,const float* st){
    auto put=[&](unsigned at,float value){
        auto bits=std::bit_cast<std::uint32_t>(value);
        if constexpr(std::endian::native==std::endian::little)bits=__builtin_bswap32(bits);
        std::memcpy(out+at,&bits,4);
    };
    for(unsigned c=0;c<3;++c)put(4*c,position[c]);
    for(unsigned c=0;c<3;++c)out[12+c]=std::uint8_t(normal[c]);
    if(!st)return 15;
    put(15,st[0]);put(19,st[1]);
    return 23;
}
}
