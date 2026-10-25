#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace dusk::native::gx {
// Exact equality only (not lexical ordering). The installed ARM libc memcmp
// stays byte-wise for an entire interval if either starting address is odd.
// Equal-alignment spans can peel a short prefix, then compare bounded words.
// memcpy + proven alignment avoid typed aliasing and unaligned ARM loads.
inline bool equalBytes(const void* left,const void* right,std::size_t bytes){
    auto* a=static_cast<const std::uint8_t*>(left);
    auto* b=static_cast<const std::uint8_t*>(right);
    if(((reinterpret_cast<std::uintptr_t>(a)^reinterpret_cast<std::uintptr_t>(b))&3)==0){
        while(bytes&&(reinterpret_cast<std::uintptr_t>(a)&3)){
            if(*a++!=*b++)return false;
            --bytes;
        }
        while(bytes>=16){
            std::uint32_t x[4],y[4];
            std::memcpy(x,__builtin_assume_aligned(a,4),16);
            std::memcpy(y,__builtin_assume_aligned(b,4),16);
            if((x[0]^y[0])|(x[1]^y[1])|(x[2]^y[2])|(x[3]^y[3]))return false;
            a+=16;b+=16;bytes-=16;
        }
        while(bytes>=4){
            std::uint32_t x,y;
            std::memcpy(&x,__builtin_assume_aligned(a,4),4);
            std::memcpy(&y,__builtin_assume_aligned(b,4),4);
            if(x!=y)return false;
            a+=4;b+=4;bytes-=4;
        }
    }
    return !bytes||!std::memcmp(a,b,bytes);
}
}
