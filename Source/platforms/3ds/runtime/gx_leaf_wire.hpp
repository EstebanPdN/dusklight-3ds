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
}
