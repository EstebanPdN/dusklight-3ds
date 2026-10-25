#pragma once
// Narrow endian reader for the existing THP codecs on native targets. Desktop
// internal.hpp also owns logging, containers and GPU-facing helpers; codecs
// require only these big-endian reads, without fmt or a desktop application.
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>
template<class T> T read_bits(const void* address) noexcept {
    static_assert(std::is_integral_v<T>);
    using U=std::make_unsigned_t<T>;
    const auto* bytes=static_cast<const std::uint8_t*>(address);U value=0;
    for(unsigned i=0;i<sizeof(T);++i)value=U((value<<8)|bytes[i]);
    return std::bit_cast<T>(value);
}
namespace aurora {
class ByteReader {
    const std::uint8_t* bytes;std::size_t length,position{};
public:
    static ByteReader unbounded(const void* data) noexcept {return {static_cast<const std::uint8_t*>(data),std::numeric_limits<std::size_t>::max()};}
    ByteReader(const std::uint8_t* data,std::size_t size):bytes(data),length(size){}
    std::size_t offset()const{return position;}
    bool try_take(std::size_t size,std::span<const std::uint8_t>& output)noexcept{
        if(position>length||size>length-position)return false;
        output={bytes+position,size};position+=size;return true;
    }
    template<class T>bool try_read(T& output)noexcept{
        std::span<const std::uint8_t> data;if(!try_take(sizeof(T),data))return false;
        output=read_bits<T>(data.data());return true;
    }
};
}
