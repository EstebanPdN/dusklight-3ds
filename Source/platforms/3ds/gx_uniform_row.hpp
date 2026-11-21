#pragma once
#include <3ds/types.h>
extern "C" {
#include <c3d/types.h>
}
#include <array>
#include <cstdint>
#include <cstring>
namespace dusk::native {
// One float uniform row against Citro3D's record of what was last uploaded
// (C3D_FVUnif), compared as 32-bit words instead of four VFP compares (Alpha 6
// console: about 11,800 rows compared per picture, 4,138 of them written).
// The decision is the one the VFP compare made in the renderer thread, which
// runs with flush-to-zero (FPSCR logged at boot): equal words are equal, and
// so are two words that are both zero or denormal, of either sign. The one
// difference is a NaN, never equal as a float: identical NaN words are now
// kept, where the float compare sent the same words again.
// tests/gx_uniform_row_test.cpp checks both claims on every class of word.
inline bool sameUniformWord(std::uint32_t a,std::uint32_t b){return a==b||!((a|b)&0x7f800000u);}
// `row` is x,y,z,w; C3D_FVec holds w,z,y,x (FVec4_New).
inline bool sameUniformRow(const C3D_FVec& have,const std::array<float,4>& row){
    std::uint32_t a[4],b[4];
    std::memcpy(a,row.data(),sizeof(a));std::memcpy(b,have.c,sizeof(b));
    return sameUniformWord(a[0],b[3])&&sameUniformWord(a[1],b[2])&&sameUniformWord(a[2],b[1])&&sameUniformWord(a[3],b[0]);
}
// The bytes FVec4_New(row[0],row[1],row[2],row[3]) stores, moved as words.
inline void storeUniformRow(C3D_FVec& out,const std::array<float,4>& row){
    std::uint32_t a[4];std::memcpy(a,row.data(),sizeof(a));
    const std::uint32_t reversed[4]{a[3],a[2],a[1],a[0]};
    std::memcpy(out.c,reversed,sizeof(reversed));
}
}
