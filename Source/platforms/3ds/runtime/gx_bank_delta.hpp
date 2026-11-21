#pragma once
// Register bank deltas for the second-core worker (gx_worker.hpp).
//
// The worker draws shapes on another core and reads the GX register bank as
// it stood when the game thread issued each shape. It keeps a copy of the
// bank (the mirror) that follows the game thread's bank word for word,
// including the valid bits, epochs and state hashes the draw code keys on,
// so it runs that code unchanged.
//
// A bank is seen as the 32-bit words of its memory. A delta is a list of
// runs: one word (index of the first bank word << 16 | words), then the
// values. The mirror is fed from three sources:
// - the CP, BP and XF arrays (4,696 words): the processor's log of the
//   stores that changed a word (BankLog, gx_commands.hpp), which already is
//   a delta;
// - everything behind the arrays, as one run with every shape: the valid
//   bits of the arrays (147 words) only after the processor made a word
//   valid (BankLog::grewValid; a word can become valid without changing its
//   value), and always the words behind them (colour latches, palette loads,
//   matrix indices, masks, epochs, copy setup, state hashes: about 150
//   words). Comparing those words with what was sent last cost more on each
//   shape than copying all of them.
// When the log lost a store the whole bank is sent instead.
#include "gx_commands.hpp"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace dusk::native::gx {
namespace bank_layout {
static_assert(std::is_trivially_copyable_v<CommandRegisters>&&sizeof(CommandRegisters)%4==0&&alignof(CommandRegisters)>=4);
inline constexpr std::size_t words=sizeof(CommandRegisters)/4;
inline constexpr std::size_t cpAt=offsetof(CommandRegisters,cp)/4,bpAt=offsetof(CommandRegisters,bp)/4,xfAt=offsetof(CommandRegisters,xf)/4;
inline constexpr std::size_t cpWords=256,bpWords=256,xfWords=0x1058;
// A bitset keeps bit i in byte i/8 of its storage (little-endian words of any
// size), so bit i is bit i%32 of the 32-bit word i/32 of its memory.
inline constexpr std::size_t cpValidAt=offsetof(CommandRegisters,cpValid)/4,bpValidAt=offsetof(CommandRegisters,bpValid)/4,
    xfValidAt=offsetof(CommandRegisters,xfValid)/4;
// The first word behind the valid bits.
inline constexpr std::size_t tailAt=offsetof(CommandRegisters,tevColor)/4;
static_assert(offsetof(CommandRegisters,cp)%4==0&&offsetof(CommandRegisters,bp)%4==0&&offsetof(CommandRegisters,xf)%4==0&&
              offsetof(CommandRegisters,cpValid)%4==0&&offsetof(CommandRegisters,bpValid)%4==0&&offsetof(CommandRegisters,xfValid)%4==0&&
              offsetof(CommandRegisters,tevColor)%4==0);
// The order both sources rely on: the three arrays back to back, then the
// valid bits, then the tail.
static_assert(cpAt==0&&bpAt==cpAt+cpWords&&xfAt==bpAt+bpWords&&xfAt+xfWords<=cpValidAt&&cpValidAt<bpValidAt&&bpValidAt<xfValidAt&&
              xfValidAt<tailAt&&tailAt<words);
static_assert(sizeof(CommandRegisters::cpValid)>=cpWords/8&&sizeof(CommandRegisters::bpValid)>=bpWords/8&&
              sizeof(CommandRegisters::xfValid)>=(xfWords+7)/8);
static_assert(words<=0xffff);
inline constexpr std::size_t tailWords=words-tailAt;
}
inline const std::uint32_t* bankWordsOf(const CommandRegisters& bank){return reinterpret_cast<const std::uint32_t*>(&bank);}
inline std::uint32_t* bankWordsOf(CommandRegisters& bank){return reinterpret_cast<std::uint32_t*>(&bank);}

// The most words the run of the valid bits and the tail takes.
inline constexpr std::size_t bankTailRunCapacity=1+bank_layout::words-bank_layout::cpValidAt;
// The words behind the arrays as one run: the tail, and the arrays' valid
// bits in front of it when `validBits`. `out` holds bankTailRunCapacity
// words; the result is the words used.
inline std::size_t copyBankTail(const CommandRegisters& bank,bool validBits,std::uint32_t* out){
    const std::size_t first=validBits?bank_layout::cpValidAt:bank_layout::tailAt,count=bank_layout::words-first;
    out[0]=std::uint32_t(first<<16|count);std::memcpy(out+1,bankWordsOf(bank)+first,count*4);
    return 1+count;
}
// Applies a delta to a bank. False when a run does not lie inside a bank (a
// damaged record): nothing is trusted from there on.
inline bool applyBankDelta(CommandRegisters& bank,const std::uint32_t* delta,std::size_t count){
    auto* out=bankWordsOf(bank);
    for(std::size_t at=0;at<count;){
        const std::uint32_t head=delta[at];
        // Most runs are one register.
        if((head&0xffff)==1&&at+1<count&&(head>>16)<bank_layout::words){out[head>>16]=delta[at+1];at+=2;continue;}
        const std::size_t first=head>>16;std::size_t run=head&0xffff;
        if(!run||first+run>bank_layout::words||run>count-at-1)return false;
        const std::uint32_t* from=delta+at+1;std::uint32_t* to=out+first;
        at+=1+run;
        if(run<=8)do *to++=*from++;while(--run);
        else std::memcpy(to,from,run*4);
    }
    return true;
}
// The first word where two banks differ, or `words` when they are equal.
inline std::size_t firstBankDifference(const CommandRegisters& a,const CommandRegisters& b){
    const auto* x=bankWordsOf(a);const auto* y=bankWordsOf(b);
    for(std::size_t i=0;i<bank_layout::words;++i)if(x[i]!=y[i])return i;
    return bank_layout::words;
}
}
