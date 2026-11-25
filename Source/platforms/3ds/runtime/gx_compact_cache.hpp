#pragma once
#include "gx_bytes_equal.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
namespace dusk::native::gx {
// Raw compact geometry reuse with shared array snapshots. Every hit compares
// every byte in the consumed live intervals, not a resource identity/frame stamp.
// Arrays shared by many shapes occupy one snapshot; output records retain only
// generation tokens. Mutation, eviction, extent changes and pointer reuse
// cannot make a stale token valid. No transform/light/material state is cached.
class CompactVertexCache {
public:
    using Key=std::array<std::uint32_t,32>;
    using Arrays=std::array<std::span<const std::uint8_t>,4>;
    struct Range {std::size_t first{},bytes{};};
    using Ranges=std::array<Range,4>;
    bool attach(std::span<std::uint8_t> bytes,std::size_t inputCapacity=0){
        if(bytes.size()<2||bytes.size()>4*1024*1024)return false;
        const auto capacity=inputCapacity?inputCapacity:bytes.size()/2;
        if(capacity>=bytes.size())return false;
        storage=bytes;inputSize=capacity;clear();return true;
    }
    void clear(){entries={};snapshots={};cursor=arrayCursor=next=arrayNext=0;generation=0;
        hits=misses=stores=evictions=storeBytes=rangeBytes=outputBytes=packedChanges=arrayChanges=0;
        arrayHits=arrayStores=arrayBytes=arrayEvictions=comparedBytes=0;
    }
    std::uint64_t hits{},misses{},stores{},evictions{},storeBytes{},rangeBytes{},outputBytes{},packedChanges{},arrayChanges{};
    std::uint64_t arrayHits{},arrayStores{},arrayBytes{},arrayEvictions{},comparedBytes{};
    bool find(const Key& key,std::span<const std::uint8_t> packed,const Arrays& arrays,std::span<std::byte> output){
        ++misses;
        if(!independent(packed,arrays,output))return false;
        for(const auto& e:entries)if(e.total&&e.address==reinterpret_cast<std::uintptr_t>(packed.data())&&
            e.packed==packed.size()&&e.output==output.size()&&e.key==key){
            const auto* saved=outputs().data()+e.offset;
            if(!equalBytes(saved,packed.data(),packed.size())){++packedChanges;continue;}
            bool same=true;
            for(unsigned i=0;i<4;++i){
                if(!e.tokens[i]){if(!arrays[i].empty())same=false;continue;}
                const auto& snap=snapshots[e.slots[i]];const auto range=e.ranges[i];
                if(snap.token!=e.tokens[i]||snap.address!=reinterpret_cast<std::uintptr_t>(arrays[i].data())||
                   snap.size!=arrays[i].size()||range.first>arrays[i].size()||range.bytes>arrays[i].size()-range.first){same=false;break;}
                comparedBytes+=range.bytes;
                if(range.bytes&&!equalBytes(inputs().data()+snap.offset+range.first,arrays[i].data()+range.first,range.bytes)){same=false;break;}
                ++arrayHits;
            }
            if(!same){++arrayChanges;continue;}
            std::memcpy(output.data(),saved+e.payload,output.size());++hits;--misses;return true;
        }
        return false;
    }
    bool store(const Key& key,std::span<const std::uint8_t> packed,const Arrays& arrays,const Ranges& ranges,std::span<const std::byte> output){
        if(packed.empty()||output.empty()||!independent(packed,arrays,output))return false;
        for(unsigned i=0;i<4;++i)
            if(ranges[i].first>arrays[i].size()||ranges[i].bytes>arrays[i].size()-ranges[i].first)return false;
        auto pool=outputs();
        if(packed.size()>pool.size()||output.size()>pool.size()-packed.size())return false;
        const auto payload=(packed.size()+15)&~std::size_t(15);
        if(payload>pool.size()||output.size()>pool.size()-payload)return false;
        Tokens tokens{};Slots slots{};if(!observe(arrays,tokens,slots))return false;
        const auto total=payload+output.size();
        cursor=aligned(pool,cursor);
        if(cursor>pool.size()||total>pool.size()-cursor)cursor=aligned(pool,0);
        if(cursor>pool.size()||total>pool.size()-cursor)return false;
        unsigned slot=entries.size();
        const auto address=reinterpret_cast<std::uintptr_t>(packed.data());
        for(unsigned i=0;i<entries.size();++i){auto& e=entries[i];
            if(e.total&&((e.offset<cursor+total&&cursor<e.offset+e.total)||
               (e.address==address&&e.packed==packed.size()&&e.output==output.size()&&e.key==key))){e={};++evictions;}
            if(!e.total&&slot==entries.size())slot=i;
        }
        if(slot==entries.size()){slot=next++%entries.size();++evictions;}
        std::memcpy(pool.data()+cursor,packed.data(),packed.size());
        std::memcpy(pool.data()+cursor+payload,output.data(),output.size());
        entries[slot]={key,address,cursor,total,packed.size(),output.size(),payload,tokens,slots,ranges};
        cursor+=total;++stores;storeBytes+=total;outputBytes+=output.size();
        return true;
    }
private:
    using Tokens=std::array<std::uint32_t,4>;
    using Slots=std::array<std::uint8_t,4>;
    static std::size_t aligned(std::span<std::uint8_t> pool,std::size_t at){
        return at+((16-((reinterpret_cast<std::uintptr_t>(pool.data())+at)&15))&15);
    }
    std::span<std::uint8_t> inputs()const{return storage.first(inputSize);}
    std::span<std::uint8_t> outputs()const{return storage.subspan(inputSize);}
    bool observe(const Arrays& arrays,Tokens& tokens,Slots& slots){
        for(unsigned i=0;i<4;++i){
            if(arrays[i].empty()){tokens[i]=0;continue;}
            if(!(tokens[i]=observeArray(arrays[i])))return false;
            for(unsigned n=0;n<snapshots.size();++n)if(snapshots[n].token==tokens[i]){slots[i]=n;break;}
        }
        return true;
    }
    std::uint32_t observeArray(std::span<const std::uint8_t> bytes){
        auto pool=inputs();if(bytes.size()>pool.size())return 0;
        const auto address=reinterpret_cast<std::uintptr_t>(bytes.data());
        for(auto& s:snapshots)if(s.token&&s.address==address&&s.size==bytes.size()){
            comparedBytes+=bytes.size();
            if(equalBytes(pool.data()+s.offset,bytes.data(),bytes.size())){++arrayHits;return s.token;}
            if(generation==std::numeric_limits<std::uint32_t>::max())return 0;
            // Same extent can replace its own input snapshot in place; cached
            // outputs carrying the previous token will fail on their next use.
            std::memcpy(pool.data()+s.offset,bytes.data(),bytes.size());
            s.token=++generation;++arrayStores;arrayBytes+=bytes.size();return s.token;
        }
        if(generation==std::numeric_limits<std::uint32_t>::max())return 0;
        arrayCursor=aligned(pool,arrayCursor)+(address&3);
        if(arrayCursor>pool.size()||bytes.size()>pool.size()-arrayCursor)arrayCursor=aligned(pool,0)+(address&3);
        if(arrayCursor>pool.size()||bytes.size()>pool.size()-arrayCursor)return 0;
        unsigned slot=snapshots.size();
        for(unsigned i=0;i<snapshots.size();++i){auto& s=snapshots[i];
            if(s.token&&s.offset<arrayCursor+bytes.size()&&arrayCursor<s.offset+s.size){s={};++arrayEvictions;}
            if(!s.token&&slot==snapshots.size())slot=i;
        }
        if(slot==snapshots.size()){slot=arrayNext++%snapshots.size();++arrayEvictions;}
        std::memcpy(pool.data()+arrayCursor,bytes.data(),bytes.size());
        snapshots[slot]={address,arrayCursor,bytes.size(),++generation};
        arrayCursor+=bytes.size();++arrayStores;arrayBytes+=bytes.size();return generation;
    }
    template<class A,class B>static bool overlaps(A a,B b){
        if(a.empty()||b.empty())return false;
        const auto x=reinterpret_cast<std::uintptr_t>(a.data()),y=reinterpret_cast<std::uintptr_t>(b.data());
        return x<=y?y-x<a.size_bytes():x-y<b.size_bytes();
    }
    bool independent(std::span<const std::uint8_t> packed,const Arrays& arrays,std::span<const std::byte> output)const{
        if(storage.empty()||overlaps(storage,packed)||overlaps(storage,output)||overlaps(packed,output))return false;
        for(auto array:arrays)if(overlaps(storage,array)||overlaps(output,array))return false;
        return true;
    }
    struct Entry {Key key{};std::uintptr_t address{};std::size_t offset{},total{},packed{},output{},payload{};Tokens tokens{};Slots slots{};Ranges ranges{};};
    struct Snapshot {std::uintptr_t address{};std::size_t offset{},size{};std::uint32_t token{};};
    std::array<Entry,256> entries{};
    std::array<Snapshot,128> snapshots{};
    std::span<std::uint8_t> storage{};
    std::size_t inputSize{},cursor{},arrayCursor{};unsigned next{},arrayNext{};std::uint32_t generation{};
};
// Native static geometry: decoded compact vertices for display lists of J3D
// models that draw their original archive arrays. The owner (J3DShape) marks
// such draws with a vertex data generation that is never reused, so entries
// are trusted without byte comparison. The pool empties at each new play
// scene (clearAtStageChange); a pool that fills up within a scene resets.
// Per thread: with the second-core worker on (gx_worker.hpp) the worker
// draws a queued shape's list while the game thread marks the next one.
struct StaticGeometryHint {std::uint32_t generation{};const void* list{};std::uint32_t batch{};bool trustedIndices{};};
extern thread_local StaticGeometryHint staticGeometryHint;
// Native shape records: notified when a static display list call ends.
inline void (*staticListEnded)(const void* list,std::uint32_t batches)=nullptr;
// Set by native shape replay once it rebuilds compact palettes and compares
// their normal rows (gx_command_draw.cpp). Until then a compact-palette batch
// only reuses its decoded entry through the frame arena: a record the replay
// cannot place would be refused every frame after its plans and textures.
inline bool compactPaletteRecords=false;
// Normal rows of each placed matrix (offset >= 0) as one bit: set when they
// are the position rows (compact palette, proportional normal matrix), clear
// when they follow at +3. With the offsets this fixes every compact record's
// v4 exactly. False for any other placement, which a key cannot express.
inline bool sharedNormalMask(const std::array<std::int8_t,10>& offsets,const std::array<std::int8_t,10>& normals,std::uint16_t& mask){
    mask=0;
    for(unsigned m=0;m<10;++m){
        if(offsets[m]<0)continue; // no record can select it
        if(normals[m]==offsets[m])mask|=std::uint16_t(1u<<m);
        else if(normals[m]!=offsets[m]+3)return false;
    }
    return true;
}
class StaticCompactCache {
public:
    struct Key {
        std::uint32_t generation{},batch{},count{},stride{},layout{};
        std::uintptr_t list{};
        std::array<std::int8_t,10> offsets{};
        // sharedNormalMask: a compact palette's layout depends on live matrix
        // values, so each layout is its own entry. Fills the former padding.
        std::uint16_t sharedNormals{};
        bool operator==(const Key&)const=default;
    };
    static_assert(sizeof(void*)!=4||sizeof(Key)==36,"the 8192 entries stay in the measured BSS budget");
    enum class Clear:std::uint8_t {start,full,stage};
    // gpuResident: the pool is GPU-readable linear memory. Entries are flushed
    // once on store and stay immutable until a reset at a frame boundary.
    bool attach(std::span<std::uint8_t> bytes,bool resident=false,void(*flushRange)(void*,std::size_t)=nullptr){
        if(bytes.size()<4096||(resident&&!flushRange))return false;
        pool=bytes;gpuResident=resident;flush=flushRange;clear();lastClear=Clear::start;return true;
    }
    bool resident()const{return gpuResident;}
    // A new play scene: the deleted scene's models never draw again (their
    // generations are not reused); models kept across scenes decode once
    // more. Emptied at the next frameBegin, behind the loading fade, instead
    // of by a full-pool reset (all records invalid) during play.
    void clearAtStageChange(){if(!pool.empty())stagePending=true;}
    // Call only after the previous frame's GPU work completed. True when the
    // pool emptied for a stage change (the caller drops resident indices).
    bool frameBegin(){
        if(!resetPending&&!stagePending)return false;
        const bool stage=stagePending;
        clearedBytes=cursor;clear();
        if(stage)++stageClears;else ++resets;
        lastClear=stage?Clear::stage:Clear::full;
        resetPending=stagePending=false;
        return stage;
    }
    bool attached()const{return !pool.empty();}
    void clear(){entries.fill({});cursor=0;used=0;++epoch;}
    std::uint32_t epoch=1; // bumps on every reset; native records check it
    const std::byte* find(const Key& key,std::size_t bytes){
        if(pool.empty())return nullptr;
        for(std::size_t probe=0,slot=hash(key);probe<maxProbe;++probe,slot=(slot+1)&(entries.size()-1)){
            const auto& e=entries[slot];
            if(!e.bytes)break;
            if(e.key==key&&e.bytes==bytes){++hits;return reinterpret_cast<const std::byte*>(pool.data()+e.offset);}
        }
        ++misses;return nullptr;
    }
    // True when the output is now an entry.
    bool store(const Key& key,std::span<const std::byte> output){
        if(pool.empty()||output.empty()||output.size()>pool.size()/4||resetPending)return false;
        if(used>=entries.size()*3/4||output.size()>pool.size()-cursor){
            // Resident entries may still be read by queued GPU work this frame.
            if(gpuResident){resetPending=true;return false;}
            clearedBytes=cursor;clear();++resets;lastClear=Clear::full;
        }
        std::size_t slot=hash(key);
        for(std::size_t probe=0;entries[slot].bytes;++probe,slot=(slot+1)&(entries.size()-1))
            if(probe>=maxProbe||entries[slot].key==key)return false;
        std::memcpy(pool.data()+cursor,output.data(),output.size());
        if(flush)flush(pool.data()+cursor,output.size());
        entries[slot]={key,cursor,output.size()};
        cursor=(cursor+output.size()+15)&~std::size_t(15);++used;++stores;
        return true;
    }
    // resets: full pool during a scene; stageClears: clearAtStageChange.
    std::uint64_t hits{},misses{},stores{},resets{},stageClears{};
    // Compact-palette batches (decoders): hits, stores, and hits copied into
    // the frame arena because the batch is not its list's first. Native shape
    // replay (gx_command_draw.cpp): compact records replayed, and refused
    // because the live layout differs from the recorded one.
    std::uint64_t paletteHits{},paletteStores{},paletteArena{},paletteReplays{},paletteRefusals{};
    std::size_t clearedBytes{}; // bytes in use when the pool last emptied
    Clear lastClear{Clear::start};
    std::size_t bytesUsed()const{return cursor;}
    std::size_t capacity()const{return pool.size();}
private:
    static constexpr std::size_t maxProbe=32;
    struct Entry {Key key{};std::size_t offset{},bytes{};};
    static std::size_t hash(const Key& k){
        std::uint32_t h=2166136261u;
        auto mix=[&](std::uint32_t v){h=(h^v)*16777619u;};
        mix(k.generation);mix(k.batch);mix(k.count);mix(k.stride);mix(k.layout);mix(std::uint32_t(k.list));
        for(auto o:k.offsets)mix(std::uint8_t(o));
        mix(k.sharedNormals);
        return h&(8192-1);
    }
    std::array<Entry,8192> entries{};
    std::span<std::uint8_t> pool{};
    std::size_t cursor{},used{};
    bool gpuResident{},resetPending{},stagePending{};
    void(*flush)(void*,std::size_t){};
};
StaticCompactCache* staticCompactCache();
void setStaticCompactCache(StaticCompactCache*);
}
