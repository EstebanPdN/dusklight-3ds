#pragma once
#include "gx_alpha_interval.hpp"
#include <array>
#include <cstddef>
#include <limits>
#include <string_view>
namespace dusk::native {
// Exact, bounded SD config grammar. Missing file is handled as disabled by the
// caller; an empty file, extra token, NUL, or whitespace is an error.
inline bool parseAlphaIntervalConfig(std::string_view text,bool& enabled){
    enabled=false;
    if(text=="enabled=0\n"||text=="enabled=0\r\n")return true;
    if(text=="enabled=1\n"||text=="enabled=1\r\n"){enabled=true;return true;}
    return false;
}
// Render-owner only, fixed main + eight offscreen slots. Storage generations
// never reset on release (including identical-address reuse); no per-draw heap.
// Native snapshots additionally validate C3D allocation ownership/formats/masks.
struct AlphaIntervalStorage {
    gx::AlphaIntervalTarget target{};
    std::uintptr_t colorPhysical{},depthPhysical{};
    bool operator==(const AlphaIntervalStorage&) const = default;
};
class AlphaIntervalTargetLedger {
public:
    struct Record {
        void* handle{};AlphaIntervalStorage storage{};
        std::uint64_t generation{},clearSerial{};
        bool wholeZero{},canonical{};
    };
    bool enabled{};
    const void* owner{};
    bool canRegister() const {
        if(nextGeneration==std::numeric_limits<std::uint64_t>::max())return false;
        for(const auto& r:records)if(!r.handle)return true;
        return false;
    }
    const Record* find(void* handle) const {
        if(!handle)return nullptr;
        for(const auto& r:records)if(r.handle==handle)return &r;
        return nullptr;
    }
    bool matches(void* handle,const AlphaIntervalStorage& storage) const {
        const auto* r=find(handle);return r&&r->storage==storage;
    }
    bool add(void* handle,const AlphaIntervalStorage& storage){
        if(!handle||find(handle)||!canRegister()||!valid(storage))return false;
        for(const auto& r:records)if(r.handle&&!disjoint(r.storage,storage))return false;
        for(auto& r:records)if(!r.handle){r={handle,storage,++nextGeneration,0,false,false};return true;}
        return false;
    }
    // Invoke only AFTER the existing successful queue/bind transaction. A bind
    // cannot establish whole-target zero evidence; a queued full D24S8 clear
    // establishes ordering evidence, not CPU/GPU completion or a new fence.
    void didBind(void* handle){
        auto* r=mutableFind(handle);if(!r)return;
        r->canonical=true;bound=handle;boundGeneration=r->generation;
    }
    void didClear(void* handle,std::uint32_t depth){
        auto* r=mutableFind(handle);if(!r)return;
        if(r->clearSerial==std::numeric_limits<std::uint64_t>::max()){r->wholeZero=false;return;}
        ++r->clearSerial;
        // D24S8: depth is LOW 24 bits, stencil is the HIGH byte. Preserve all
        // requested depth/upper stencil bits; never silently repair a clear.
        r->wholeZero=(depth&0x01000000u)==0;
    }
    bool owns(void* handle,const AlphaIntervalStorage& storage,const void* caller) const {
        const auto* r=find(handle);
        return enabled&&owner&&caller==owner&&r&&r->storage==storage&&
            bound==handle&&boundGeneration==r->generation&&r->wholeZero&&r->clearSerial&&r->canonical;
    }
    void release(){
        records={};bound=nullptr;boundGeneration=0;enabled=false;owner=nullptr;
        // nextGeneration intentionally survives; renderer poison is separate.
    }
private:
    std::array<Record,9> records{};
    void* bound{};std::uint64_t boundGeneration{},nextGeneration{};
    Record* mutableFind(void* handle){return const_cast<Record*>(find(handle));}
    static bool apart(std::uintptr_t a,std::size_t n,std::uintptr_t b,std::size_t m){
        return a&&b&&(a<=b?n<=b-a:m<=a-b);
    }
    static bool valid(const AlphaIntervalStorage& s){
        const auto& t=s.target;
        if(!t.color||!t.depth||!t.width||!t.height||t.width>1024||t.height>1024||
           (t.width&7)||(t.height&7))return false;
        const auto n=std::size_t(t.width)*t.height*4;
        const auto c=reinterpret_cast<std::uintptr_t>(t.color),d=reinterpret_cast<std::uintptr_t>(t.depth);
        return !(c&7)&&!(d&7)&&!(s.colorPhysical&7)&&!(s.depthPhysical&7)&&c<=UINTPTR_MAX-n&&d<=UINTPTR_MAX-n&&s.colorPhysical<=UINTPTR_MAX-n&&
            s.depthPhysical<=UINTPTR_MAX-n&&apart(c,n,d,n)&&apart(s.colorPhysical,n,s.depthPhysical,n);
    }
    static bool disjoint(const AlphaIntervalStorage& a,const AlphaIntervalStorage& b){
        const auto n=std::size_t(a.target.width)*a.target.height*4,m=std::size_t(b.target.width)*b.target.height*4;
        const std::array<std::uintptr_t,2> av{reinterpret_cast<std::uintptr_t>(a.target.color),reinterpret_cast<std::uintptr_t>(a.target.depth)};
        const std::array<std::uintptr_t,2> bv{reinterpret_cast<std::uintptr_t>(b.target.color),reinterpret_cast<std::uintptr_t>(b.target.depth)};
        const std::array<std::uintptr_t,2> ap{a.colorPhysical,a.depthPhysical},bp{b.colorPhysical,b.depthPhysical};
        for(unsigned i=0;i<2;++i)for(unsigned j=0;j<2;++j)if(!apart(av[i],n,bv[j],m)||!apart(ap[i],n,bp[j],m))return false;
        return true;
    }
};
inline AlphaIntervalTargetLedger alphaIntervalTargets;
// Startup only, before any framebuffer frame/offscreen allocation. Disabled
// yields an empty policy. Enabled installs the actual pure native callback;
// allocation/bind without a subsequent existing whole clear still rejects.
bool configureFramebufferAlphaIntervals(bool enabled,gx::AlphaIntervalPolicy&);
}
