#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace dusk::native::gx {
// Bounded caller-owned snapshots of primitive packets and their exact grouped
// topology. A pointer is only a lookup hint: every hit compares all source
// bytes and the current packed stride. No array, matrix or texture is retained.
class PrimitiveBatchCache {
public:
    struct Result {
        std::span<const std::uint8_t> vertices{},indices{};
        unsigned vertexCount{};
        explicit operator bool()const{return vertexCount!=0;}
    };
    bool attach(std::span<std::uint8_t> bytes){
        if(bytes.empty()||bytes.size()>4*1024*1024)return false;
        storage=bytes;clear();return true;
    }
    void clear(){entries={};cursor=next=0;}
    std::span<const std::uint8_t> memory()const{return storage;}
    // generation: nonzero for immutable static J3D display lists (native engine);
    // such entries are trusted by identity without comparing source bytes.
    Result find(std::span<const std::uint8_t> source,unsigned stride,std::uint32_t generation=0)const{
        const auto address=reinterpret_cast<std::uintptr_t>(source.data());
        for(const auto& e:entries)if(e.count&&e.address==address&&e.source==source.size()&&e.stride==stride&&
            (generation&&e.generation==generation?true:!std::memcmp(storage.data()+e.offset,source.data(),source.size()))){
            return {storage.subspan(e.offset+e.source,e.vertices),
                storage.subspan(e.offset+e.source+e.vertices,e.indices),e.count};
        }
        return {};
    }
    bool store(std::span<const std::uint8_t> source,unsigned stride,const Result& result,std::uint32_t generation=0){
        if(source.empty()||!stride||result.vertexCount<3||result.vertexCount>65535||
           result.vertices.size()!=std::size_t(result.vertexCount)*stride||result.indices.empty()||
           result.indices.size()%6||result.indices.size()>196605*2)return false;
        auto overlaps=[](auto a,auto b){
            if(a.empty()||b.empty())return false;
            const auto x=reinterpret_cast<std::uintptr_t>(a.data()),y=reinterpret_cast<std::uintptr_t>(b.data());
            return x<=y?y-x<a.size():x-y<b.size();
        };
        if(overlaps(storage,source)||overlaps(storage,result.vertices)||overlaps(storage,result.indices))return false;
        if(source.size()>storage.size()||result.vertices.size()>storage.size()-source.size()||
           result.indices.size()>storage.size()-source.size()-result.vertices.size())return false;
        const auto total=source.size()+result.vertices.size()+result.indices.size();
        if(total>storage.size()-cursor)cursor=0;
        unsigned slot=entries.size();
        for(unsigned n=0;n<entries.size();++n){auto& e=entries[n];
            if(e.count&&e.offset<cursor+total&&cursor<e.offset+e.source+e.vertices+e.indices)e={};
            if(!e.count&&slot==entries.size())slot=n;
        }
        if(slot==entries.size())slot=next++%entries.size();
        std::memcpy(storage.data()+cursor,source.data(),source.size());
        std::memcpy(storage.data()+cursor+source.size(),result.vertices.data(),result.vertices.size());
        std::memcpy(storage.data()+cursor+source.size()+result.vertices.size(),result.indices.data(),result.indices.size());
        entries[slot]={reinterpret_cast<std::uintptr_t>(source.data()),cursor,source.size(),
            result.vertices.size(),result.indices.size(),stride,result.vertexCount,generation};
        cursor+=total;return true;
    }
private:
    struct Entry {std::uintptr_t address{};std::size_t offset{},source{},vertices{},indices{};unsigned stride{},count{};std::uint32_t generation{};};
    std::array<Entry,256> entries{};
    std::span<std::uint8_t> storage{};
    std::size_t cursor{};unsigned next{};
};
}
