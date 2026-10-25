#pragma once
#include "gx_state.hpp"
#include <cstdint>
namespace dusk::native::gx {
struct AlphaInterval { bool enabled{}; std::uint8_t lower{},upper{}; };
// Physical identity supplied by the backend after verifying active D24S8,
// full depth/stencil access, dimensions, and non-block32 storage.
struct AlphaIntervalTarget {
    const void* color{}; const void* depth{};
    unsigned width{},height{};
    bool operator==(const AlphaIntervalTarget&) const = default;
};
struct AlphaIntervalPolicy {
    bool enabled{};
    // Pure synchronous render-owner check. Must not submit, flush, rebind,
    // mutate, allocate, or retain borrowed geometry/state. True certifies:
    // bit 0 is exclusively engine scratch, initially zero across the entire
    // target (except masks currently controlled by this synchronous backend);
    // upper bits are unobserved by GX and preserved; no other stencil user;
    // target/storage generation stays live through submission/completion;
    // every copy/target reuse checks alphaIntervalCopySafe(), and a false
    // result stops rendering/copying for the rest of this process lifetime.
    // Existing framebuffer/startup code does NOT yet provide this hook.
    bool (*ownsTarget)(const AlphaIntervalTarget&,void*){};
    void* user{};
};
constexpr unsigned alphaIntervalStencilBit=1;
enum class AlphaIntervalPass : unsigned { clear, lower, upper, cleanup };
// Bounded exact interior interval only. Single comparisons and OR tails keep
// their existing planners. No TEV alpha replacement, constant-alpha inference,
// or reference-only heuristic is used.
inline bool planAlphaInterval(const AlphaCompare& c,AlphaInterval& out){
    out={};
    if(c.compare0>7||c.compare1>7||c.op>3)return false;
    auto test=[](unsigned op,unsigned a,unsigned b){
        switch(op){case 0:return false;case 1:return a<b;case 2:return a==b;
        case 3:return a<=b;case 4:return a>b;case 5:return a!=b;
        case 6:return a>=b;default:return true;}
    };
    unsigned first=256,last=0,count=0;
    for(unsigned a=0;a<256;++a){
        const bool x=test(c.compare0,a,c.reference0),y=test(c.compare1,a,c.reference1);
        const bool accept=c.op==0?(x&&y):c.op==1?(x||y):c.op==2?(x!=y):(x==y);
        if(accept){if(first==256)first=a;last=a;++count;}
    }
    if(first==0||first==256||last==255||count!=last-first+1)return false;
    out={true,std::uint8_t(first),std::uint8_t(last)};return true;
}
}
namespace dusk::native {
// Parent must guard copies/target reuse with this query before opting in.
// Failure after any mask pass poisons the renderer; all backend draw entry
// points reject thereafter, even after renderer teardown/reinitialization.
// No recovery/reset API can certify cleared pixels; restart the process.
bool alphaIntervalCopySafe();
}
