#pragma once
// PRIVATE fatal-only borrowed source observer. No allocation, pointer retention,
// destination reads, renderer selection, GPU access, or pixel coherence promise.
#include "gx_indirect.hpp"
#include <bit>
#include <cstdio>
#include <initializer_list>

namespace dusk::native::gx::payload108 {
inline constexpr unsigned maxBytes=8192,maxUnits=3,rowBytes=16;
inline constexpr std::uint32_t basis=2166136261u,prime=16777619u;
using Sink=void(*)(const char*); // Synchronous borrowed row, never retained.
inline void row(Sink sink,const char* tag,std::initializer_list<std::uint32_t> words){
    char text[256];unsigned used=unsigned(std::snprintf(text,sizeof(text),"IPAY108 %s",tag));
    for(auto word:words){
        const int n=std::snprintf(text+used,sizeof(text)-used," %08lx",static_cast<unsigned long>(word));
        if(n!=9||unsigned(n)>=sizeof(text)-used)return;
        used+=unsigned(n);
    }
    text[used++]='\n';text[used]=0;sink(text);
}
// The caller is the existing first-fatal IC105 latch. All plans/sources have
// already passed normal admission. Logging samples each byte exactly once:
// LE32 packing and FNV32 both use that same local byte, including a short tail.
inline void emit(const std::array<const VertexIndirectEffect*,3>& units,unsigned count,
                 const VertexIndirectPlan& plan,const CommandRegisters& registers,
                 unsigned incidentVertex,Sink sink){
    if(!sink)return;
    unsigned selected=0,total=0,complete=0;
    for(unsigned unit=0;unit<std::min(count,maxUnits);++unit)selected+=units[unit]!=nullptr;
    row(sink,"START",{1,incidentVertex,selected,maxBytes});
    for(unsigned unit=0;unit<std::min(count,maxUnits);++unit){
        const auto* e=units[unit];if(!e)continue;
        const auto& t=e->source;const unsigned stage=unsigned(e-plan.stages.data());
        const unsigned map=(registers.bp[0x27]>>(6*(e->state&3)))&7,offset=(map/4)*32+map%4;
        const auto size=std::uint64_t(t.image.size()),bti=std::uint64_t(t.bti.size());
        row(sink,"META",{unit,stage,map,e->coordinate,std::uint32_t(t.handle),std::uint32_t(t.handle>>32),
            t.width,t.height,t.format,t.wrapS,t.wrapT,t.minFilter,t.magFilter,t.mipCount,t.minLod,t.maxLod,
            std::bit_cast<std::uint32_t>(t.lodBias),unsigned(t.immutablePixels),std::uint32_t(size),std::uint32_t(size>>32),
            std::uint32_t(bti),std::uint32_t(bti>>32),registers.bp[0x94+offset]});
        if(size>maxBytes-total||!size){row(sink,"SKIP",{unit,size?1u:2u});continue;}
        std::uint32_t hash=basis;unsigned observed=0;
        for(unsigned at=0;at<size;at+=rowBytes){
            const unsigned n=std::min(rowBytes,unsigned(size)-at);
            std::uint32_t words[4]{};
            for(unsigned j=0;j<n;++j){const auto byte=t.image[at+j];
                words[j/4]|=std::uint32_t(byte)<<(8*(j%4));hash=(hash^byte)*prime;++observed;}
            row(sink,"DATA",{unit,at,n,words[0],words[1],words[2],words[3]});
        }
        row(sink,"END",{unit,observed,hash});total+=observed;++complete;
    }
    row(sink,"FINISH",{1,selected,complete,total,unsigned(complete==selected&&count<=maxUnits)});
}
}
