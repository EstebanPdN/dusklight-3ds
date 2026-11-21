// Uniform rows compared as words (gx_uniform_row.hpp) against the float
// compare they replace. The renderer thread runs with flush-to-zero (FPSCR
// logged at boot), so the float compare saw zero and denormal words as zero;
// that compare is modelled here explicitly, the host FPU does not flush.
#include "../gx_uniform_row.hpp"
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
extern "C" {
#include <c3d/maths.h>
}
using namespace dusk::native;
namespace {
float flushed(float v){return std::fpclassify(v)==FP_SUBNORMAL?std::copysign(0.0f,v):v;}
bool floatEqual(std::uint32_t a,std::uint32_t b){return flushed(std::bit_cast<float>(a))==flushed(std::bit_cast<float>(b));}
bool isNan(std::uint32_t a){return (a&0x7f800000u)==0x7f800000u&&(a&0x007fffffu);}
// The old row filter of gx_pica.cpp, with the flush-to-zero compare.
bool oldSame(const C3D_FVec& c,const std::array<float,4>& v){
    auto bits=[](float f){return std::bit_cast<std::uint32_t>(f);};
    return floatEqual(bits(c.x),bits(v[0]))&&floatEqual(bits(c.y),bits(v[1]))&&floatEqual(bits(c.z),bits(v[2]))&&floatEqual(bits(c.w),bits(v[3]));
}
}
int main(){
    // Every class of word: zeros, denormals (smallest, largest), normals at the
    // edges, ordinary values, infinities, quiet and signalling NaNs.
    std::vector<std::uint32_t> words{0x00000000,0x80000000,0x00000001,0x80000001,0x007fffff,0x807fffff,0x00400000,
        0x00800000,0x80800000,0x00800001,0x3f800000,0xbf800000,0x3f800001,0x3e800000,0x7f7fffff,0xff7fffff,
        0x7f800000,0xff800000,0x7fc00000,0xffc00000,0x7fc00001,0x7f800001,0xff800001,0x7fffffff,0x42c80000};
    std::uint32_t random=0x12345678;
    auto next=[&]{random^=random<<13;random^=random>>17;random^=random<<5;return random;};
    for(unsigned n=0;n<4000;++n)words.push_back(next());
    std::uint64_t pairs=0,nanKept=0;
    for(const auto a:words)for(const auto b:words){
        const bool now=sameUniformWord(a,b),before=floatEqual(a,b);
        // The only allowed difference: identical NaN words, kept now.
        if(now!=before){assert(now&&a==b&&isNan(a));++nanKept;}
        // Never keep a row whose value differs as a float.
        if(now)assert(before||a==b);
        ++pairs;
    }
    // Whole rows, component order included: C3D_FVec holds w,z,y,x.
    std::uint64_t rows=0;
    for(unsigned n=0;n<200000;++n){
        std::array<float,4> v;C3D_FVec c;
        for(unsigned k=0;k<4;++k){
            const auto w=words[next()%words.size()];
            v[k]=std::bit_cast<float>(w);
        }
        // Half the rows start from the same value with one component changed.
        if(n&1)c=FVec4_New(v[0],v[1],v[2],v[3]);
        else for(unsigned k=0;k<4;++k)c.c[k]=std::bit_cast<float>(words[next()%words.size()]);
        if(n%4==1){const unsigned k=next()%4;c.c[k]=std::bit_cast<float>(words[next()%words.size()]);}
        const bool now=sameUniformRow(c,v),before=oldSame(c,v);
        if(now!=before){
            assert(now);
            // Every differing component is an identical NaN word.
            const std::uint32_t mine[4]{std::bit_cast<std::uint32_t>(v[3]),std::bit_cast<std::uint32_t>(v[2]),std::bit_cast<std::uint32_t>(v[1]),std::bit_cast<std::uint32_t>(v[0])};
            for(unsigned k=0;k<4;++k){const auto have=std::bit_cast<std::uint32_t>(c.c[k]);if(!floatEqual(have,mine[k]))assert(have==mine[k]&&isNan(have));}
        }
        C3D_FVec stored;storeUniformRow(stored,v);
        const C3D_FVec expected=FVec4_New(v[0],v[1],v[2],v[3]);
        assert(!std::memcmp(&stored,&expected,sizeof(expected)));
        assert(sameUniformRow(stored,v));
        ++rows;
    }
    std::printf("PASS uniform rows as words: %llu word pairs and %llu rows match the flush-to-zero float compare "
                "(only identical NaN words differ: %llu kept instead of re-sent); stored bytes equal FVec4_New\n",
                static_cast<unsigned long long>(pairs),static_cast<unsigned long long>(rows),static_cast<unsigned long long>(nanKept));
}
