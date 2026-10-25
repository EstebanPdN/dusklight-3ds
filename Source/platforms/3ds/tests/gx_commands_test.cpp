#include "../runtime/gx_commands.hpp"
#include <dolphin/gd.h>
#include <dolphin/os.h>
#include <dolphin/gx/GXAurora.h>
#include <cassert>
#include <bit>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace dusk::native::gx;
extern "C" void OSPanic(const char*,int,const char* message,...){throw std::runtime_error(message);}
extern "C" void DCFlushRange(void*,u32){}
struct Capture { std::vector<DecodedVertex> vertices;std::vector<u16> indices; };
static bool capture(const CommandDraw& draw,void* context) {
    auto& out=*static_cast<Capture*>(context);std::vector<DecodedVertex> next;
    for(u32 i=0;i<draw.vertexCount;++i){DecodedVertex v;if(!draw.vertex(i,v))return false;next.push_back(v);}
    out.vertices=next;out.indices.clear();
    for(u32 i=0;i<draw.indexCount;++i){u16 index;if(!draw.triangleIndex(i,index))return false;out.indices.push_back(index);}
    return true;
}
static void close(float a,float b){assert(std::fabs(a-b)<1e-6f);}
int main(){
    // Full registry, slot reuse and stale handles, including the last slot.
    CommandMemory full;std::array<u8,CommandMemory::capacity+1> regions{};
    std::array<MemoryHandle,CommandMemory::capacity> handles{};std::span<const u8> resolved;
    for(unsigned i=0;i<full.capacity;++i)assert(full.add({&regions[i],1},handles[i]));
    MemoryHandle extra;assert(!full.add({&regions.back(),1},extra)&&extra==0);
    for(unsigned i=0;i<full.capacity;++i){
        assert(full.resolve(handles[i],0,1,resolved)&&resolved.data()==&regions[i]);
        assert(!full.resolve(handles[i],1,1,resolved)&&resolved.empty());
    }
    const auto expired=handles.back();assert(full.remove(expired));
    assert(full.add({&regions.back(),1},extra)&&extra!=expired);
    assert(!full.resolve(expired,0,1,resolved)&&!full.remove(expired));
    assert(full.resolve(extra,0,1,resolved)&&resolved.data()==&regions.back());
    assert(!full.resolve(0,0,0,resolved)&&!full.remove(0));
    CommandMemory memory;CommandProcessor processor(memory);Capture result;processor.setSink(capture,&result);
    std::array<float,9> positions{1,2,3,4,5,6,7,8,9};std::array<s16,6> uv{0,256,128,64,256,0};
    auto bytes=[](const auto& a){return std::span<const u8>(reinterpret_cast<const u8*>(a.data()),sizeof(a));};
    MemoryHandle posHandle,uvHandle,duplicate;
    assert(memory.add(bytes(positions),posHandle)&&memory.add(bytes(uv),uvHandle));
    assert(!memory.add(bytes(positions).subspan(4),duplicate)&&duplicate==0);
    alignas(32) u8 storage[4096]{};GDLObj gd;GDInitGDLObj(&gd,storage,sizeof(storage));GDSetCurrent(&gd);
    const GXVtxDescList desc[]{{GX_VA_POS,GX_INDEX16},{GX_VA_TEX0,GX_INDEX8},{GX_VA_NULL,GX_NONE}};
    const GXVtxAttrFmtList formats[]{{GX_VA_POS,GX_POS_XYZ,GX_F32,0},{GX_VA_TEX0,GX_TEX_ST,GX_S16,8},{GX_VA_NULL,GX_POS_XY,GX_U8,0}};
    GDSetVtxDescv(desc);GDSetVtxAttrFmtv(GX_VTXFMT0,formats);
    GDSetArraySized(GX_VA_POS,positions.data(),sizeof(positions),12,true);
    GDSetArraySized(GX_VA_TEX0,uv.data(),sizeof(uv),4,true);
    GDWriteBPCmd(0x410000ff);GDWriteBPCmd(0xfe00000f);GDWriteBPCmd(0x41001230);GDWriteBPCmd(0x42000123);
    const u32 geometryOffset=GDGetCurrOffset();
    GDWrite_u8(GX_TRIANGLES);GDWrite_u16(3);
    for(u16 i=0;i<3;++i){GDWrite_u16(i);GDWrite_u8(2-i);}
    const auto all=std::span<const u8>(storage,GDGetCurrOffset()),geometry=all.subspan(geometryOffset);
    const std::vector<u8> original(all.begin(),all.end());
    assert(processor.execute(all));assert(result.vertices.size()==3);close(result.vertices[2].position[1],8);
    close(result.vertices[0].texcoord[0][0],1);close(result.vertices[1].texcoord[0][1],0.25f);
    assert(processor.registers().bp[0x41]==0xf0&&processor.registers().bp[0x42]==0x123);
    assert(processor.statistics().plans==1&&processor.statistics().draws==1);
    assert(processor.execute(all)&&processor.execute(geometry));assert(processor.statistics().plans==1&&processor.statistics().draws==3);
    // All prefixes shorter than a complete geometry packet fail, without a draw.
    for(size_t i=1;i<geometry.size();++i){CommandProcessor p(memory);p.setSink(capture,&result);assert(p.execute(all.first(geometryOffset)));
      assert(!p.execute(geometry.first(i))&&p.error()==CommandError::truncated&&p.statistics().draws==0);}
    assert(memory.remove(posHandle));MemoryHandle replacement;assert(memory.add(bytes(positions),replacement)&&replacement!=posHandle);
    assert(!processor.execute(geometry)&&processor.error()==CommandError::staleMemory);
    processor.reset();assert(processor.execute(all));
    // An oversized vertex index reaches the bounded decoder, never a raw pointer read.
    std::vector<u8> bad(geometry.begin(),geometry.end());bad[3]=0xff;bad[4]=0xff;
    assert(!processor.execute(bad)&&processor.error()==CommandError::sinkRejected);
    processor.reset();assert(processor.execute(all));
    // Strips optimized by the original Aurora routine preserve prebuilt indices.
    std::vector<u8> strip{GX_TRIANGLESTRIP,0,4,0,0,0,0,1,1,0,2,2,0,0,0};
    auto optimized=aurora::gx::dl::optimize(strip.data(),strip.size(),desc);assert(optimized);
    assert(processor.execute(*optimized));assert((result.indices==std::vector<u16>{0,1,2,2,1,3}));
    auto wrong=*optimized;wrong[10]=0xff;wrong[11]=0xff;assert(!processor.execute(wrong)&&processor.error()==CommandError::invalidIndex);
    // Every texture VAT field, including TEX4 spanning VAT B/C.
    processor.reset();GDInitGDLObj(&gd,storage,sizeof(storage));GDSetCurrent(&gd);
    std::vector<GXVtxDescList> direct{{GX_VA_POS,GX_DIRECT}};std::vector<GXVtxAttrFmtList> fmt{{GX_VA_POS,GX_POS_XYZ,GX_F32,0}};
    for(unsigned i=0;i<8;++i){direct.push_back({static_cast<GXAttr>(GX_VA_TEX0+i),GX_DIRECT});fmt.push_back({static_cast<GXAttr>(GX_VA_TEX0+i),GX_TEX_ST,GX_S16,static_cast<u8>(i+1)});}
    direct.push_back({GX_VA_NULL,GX_NONE});fmt.push_back({GX_VA_NULL,GX_POS_XY,GX_U8,0});
    GDSetVtxDescv(direct.data());GDSetVtxAttrFmtv(GX_VTXFMT3,fmt.data());
    const u32 directOffset=GDGetCurrOffset();
    GDWrite_u8(GX_POINTS|3);GDWrite_u16(1);GDWrite_f32(-1);GDWrite_f32(2);GDWrite_f32(0);
    for(unsigned i=0;i<8;++i){GDWrite_u16(2u<<(i+1));GDWrite_u16(static_cast<u16>(-(3<<(i+1))));}
    assert(processor.execute({storage,GDGetCurrOffset()}));assert(processor.statistics().plans==1);
    for(unsigned i=0;i<8;++i){close(result.vertices[0].texcoord[i][0],2);close(result.vertices[0].texcoord[i][1],-3);}
    // Rewriting a used VAT invalidates exactly its plan; the new fraction is observed.
    const std::vector<u8> directGeometry(storage+directOffset,storage+GDGetCurrOffset());
    GDSetCurrOffset(0);fmt[1].frac=2;GDSetVtxAttrFmtv(GX_VTXFMT3,fmt.data());
    assert(processor.execute({storage,GDGetCurrOffset()}));assert(processor.execute(directGeometry));
    assert(processor.statistics().plans==2);close(result.vertices[0].texcoord[0][0],1);
    close(result.vertices[0].texcoord[1][0],2);
    // Indexed position-matrix upload follows the registered byte bound/stride/endian.
    std::array<float,24> matrices{};for(unsigned i=0;i<24;++i)matrices[i]=static_cast<float>(i)+0.25f;
    MemoryHandle matrixHandle;assert(memory.add(bytes(matrices),matrixHandle));
    GDInitGDLObj(&gd,storage,sizeof(storage));GDSetCurrent(&gd);
    GDSetArraySized(GX_POS_MTX_ARRAY,matrices.data(),sizeof(matrices),48,true);
    GDWriteXFIndxACmd(0,12,1);assert(processor.execute({storage,GDGetCurrOffset()}));
    for(unsigned i=0;i<12;++i)assert(processor.registers().xf[i]==std::bit_cast<u32>(matrices[12+i]));
    GDSetCurrOffset(0);GDWriteXFIndxACmd(0,12,2);
    assert(!processor.execute({storage,GDGetCurrOffset()})&&processor.error()==CommandError::arrayBounds);
    // Direct XF writes are transactional per packet, including every truncated prefix.
    const u8 xf[]{GX_LOAD_XF_REG,0,1,0x10,0x56,0x12,0x34,0x56,0x78,0x90,0xab,0xcd,0xef};
    for(size_t n=1;n<sizeof(xf);++n){processor.reset();assert(!processor.execute({xf,n}));
        assert(processor.error()==CommandError::truncated&&!processor.registers().xfValid.any());}
    processor.reset();assert(processor.execute(xf));
    assert(processor.registers().xf[0x1056]==0x12345678&&processor.registers().xf[0x1057]==0x90abcdef);
    auto invalidXF=std::vector<u8>(std::begin(xf),std::end(xf));invalidXF[4]=0x57;
    assert(!processor.execute(invalidXF)&&processor.error()==CommandError::unsupported);
    // Array records reject unregistered memory and malformed flags before exposing it.
    processor.reset();GDSetCurrOffset(0);
    GDSetArraySized(GX_VA_POS,positions.data(),sizeof(positions),12,true);
    assert(memory.remove(replacement));
    assert(!processor.execute({storage,GDGetCurrOffset()})&&processor.error()==CommandError::unregisteredMemory);
    assert(memory.add(bytes(positions),replacement));processor.reset();storage[15]=2;
    assert(!processor.execute({storage,GDGetCurrOffset()})&&processor.error()==CommandError::format);
    // Reentrancy cannot invalidate a draw's borrowed register/plan state.
    processor.reset();processor.setSink([](const CommandDraw&,void* p){return !static_cast<CommandProcessor*>(p)->execute({});},&processor);
    assert(!processor.execute(original)&&processor.error()==CommandError::reentrant);
    // Reset leaves no stale format/binding admission.
    processor.reset();processor.setSink(capture,&result);const u8 unconfigured[]{GX_TRIANGLES,0,3};
    assert(!processor.execute(unconfigured)&&processor.error()==CommandError::format);
    processor.reset();const u8 nested[]{GX_CMD_CALL_DL,0,0,0,0,0,0,0,0};assert(!processor.execute(nested)&&processor.error()==CommandError::unsupported);
    // Compare actual ordered triangle/UV/position output with and without
    // batching, including mixed primitives, state barriers and storage splits.
    struct Triangles {std::vector<std::array<float,6>> vertices;unsigned calls{};};
    auto triangles=[](const CommandDraw& d,void* context){
        auto& out=*static_cast<Triangles*>(context);++out.calls;
        auto vertex=[&](unsigned i){DecodedVertex v;assert(d.vertex(i,v));
            out.vertices.push_back({v.position[0],v.position[1],v.position[2],v.texcoord[0][0],v.texcoord[0][1],float(d.registers.bp[0x41])});};
        if(d.indexCount)for(unsigned i=0;i<d.indexCount;++i){u16 index;assert(d.triangleIndex(i,index));vertex(index);}
        else assert(aurora::gx::dl::expand_triangles(d.primitive,d.vertexCount,[&](u16 a,u16 b,u16 c){vertex(a);vertex(b);vertex(c);}));
        return true;
    };
    std::vector<u8> mixed;
    for(unsigned i=0;i<3000;++i){
        const unsigned n=i%4==0?4:i%4==1?5:i%4==2?3:4;
        const u8 op=i%4==0?GX_TRIANGLESTRIP:i%4==1?GX_TRIANGLEFAN:i%4==2?GX_TRIANGLES:GX_QUADS;
        mixed.insert(mixed.end(),{op,0,u8(n)});
        for(unsigned v=0;v<n;++v)mixed.insert(mixed.end(),{0,u8(v%3),u8((v+1)%3)});
        if(i%127==126)mixed.insert(mixed.end(),{GX_LOAD_BP_REG,0x41,0,0,u8(i)});
    }
    CommandProcessor plain(memory),batched(memory);Triangles reference,grouped;
    plain.setSink(triangles,&reference);batched.setSink(triangles,&grouped);
    std::array<u8,1024> batchVertices;std::array<u16,384> batchIndices;
    batched.setPrimitiveBatchScratch({batchVertices,batchIndices,128});
    assert(plain.execute(std::span(original).first(geometryOffset))&&batched.execute(std::span(original).first(geometryOffset)));
    assert(plain.execute(mixed)&&batched.execute(mixed));
    assert(reference.vertices==grouped.vertices&&grouped.calls<reference.calls/10);
    assert(plain.statistics().commands==batched.statistics().commands&&batched.statistics().mergedPrimitives>2900);
    // Deduplication compares every packed byte, preserving ordered original
    // triangles/UVs/state even across hash collisions and bounded batch splits.
    CommandProcessor deduplicated(memory);Triangles compact;
    std::array<u16,256> hashSlots;std::array<u16,128> remap;
    deduplicated.setPrimitiveBatchScratch({batchVertices,batchIndices,128,hashSlots,remap});
    deduplicated.setSink(triangles,&compact);
    assert(deduplicated.execute(std::span(original).first(geometryOffset))&&deduplicated.execute(mixed));
    assert(compact.vertices==reference.vertices&&compact.calls==grouped.calls);
    assert(deduplicated.statistics().reusedVertices>11000);

    // Snapshot reuse still resolves live arrays and consumes current state.
    // Mutating a packet at the same address must miss, not reuse old topology.
    CommandMemory cacheMemory;MemoryHandle cachedPosition,cachedUv;
    assert(cacheMemory.add(bytes(positions),cachedPosition)&&cacheMemory.add(bytes(uv),cachedUv));
    CommandProcessor cached(cacheMemory);Triangles retained;
    std::array<u8,128*1024> cacheStorage{};PrimitiveBatchCache topology;
    assert(topology.attach(cacheStorage));cached.setPrimitiveBatchCache(&topology);
    cached.setPrimitiveBatchScratch({batchVertices,batchIndices,128,hashSlots,remap});
    cached.setSink(triangles,&retained);
    assert(cached.execute(std::span(original).first(geometryOffset)));
    auto compareCached=[&]{
        reference={};retained={};assert(plain.writeBp(0x41,0)&&cached.writeBp(0x41,0));
        assert(plain.execute(mixed)&&cached.execute(mixed));assert(reference.vertices==retained.vertices);
    };
    compareCached();assert(cached.statistics().batchCacheMisses>0&&cached.statistics().batchCacheHits==0);
    compareCached();assert(cached.statistics().batchCacheHits>0);
    const auto cachedHits=cached.statistics().batchCacheHits;
    positions[0]+=12;uv[0]+=32;compareCached();assert(cached.statistics().batchCacheHits>cachedHits);
    positions[0]-=12;uv[0]-=32;
    const auto oldIndex=mixed[4];mixed[4]=(oldIndex+1)%3;
    const auto misses=cached.statistics().batchCacheMisses;compareCached();
    assert(cached.statistics().batchCacheMisses>misses);mixed[4]=oldIndex;
    // Same bytes under a different decode fraction must use the new plan.
    const auto vatA=plain.registers().cp[0x70];
    const auto fraction=(vatA&~(31u<<25))|(9u<<25);
    assert(plain.writeCp(0x70,fraction)&&cached.writeCp(0x70,fraction));compareCached();
    assert(plain.writeCp(0x70,vatA)&&cached.writeCp(0x70,vatA));
    // Force many ring wraps and entry evictions with a small bounded pool.
    assert(topology.attach(std::span(cacheStorage).first(4096)));
    for(unsigned pass=0;pass<3;++pass)compareCached();
    assert(topology.attach(cacheStorage));compareCached();compareCached();
    assert(cacheMemory.remove(cachedPosition));
    assert(!cached.execute(mixed)&&cached.error()==CommandError::staleMemory);
    assert(cacheMemory.add(bytes(positions),cachedPosition));
    cached.reset();assert(cached.execute(std::span(original).first(geometryOffset)));
    compareCached();assert(cached.statistics().batchCacheHits==0);
    // Alias/extent rejection must leave an existing valid snapshot untouched.
    PrimitiveBatchCache::Result fixture{{mixed.data()+3,9},{reinterpret_cast<const u8*>(batchIndices.data()),6},3};
    assert(topology.store(std::span(mixed).first(12),3,fixture));
    assert(topology.find(std::span(mixed).first(12),3)&&!topology.find(std::span(mixed).first(12),4));
    assert(!topology.store(std::span(cacheStorage).first(12),3,fixture));
    auto invalidFixture=fixture;invalidFixture.vertexCount=65536;
    assert(!topology.store(std::span(mixed).first(12),3,invalidFixture));
    puts("PASS packet snapshot cache: exact ordered triangles, same-address mutations, current arrays/VAT/state, stale lifetime rejection, bounded eviction, reset and alias/extent refusal");
    deduplicated.reset();assert(deduplicated.execute(std::span(original).first(geometryOffset)));
    deduplicated.setPrimitiveBatchScratch({batchVertices,batchIndices,128,hashSlots,{hashSlots.data(),128}});
    assert(!deduplicated.execute(mixed)&&deduplicated.error()==CommandError::format);
    deduplicated.reset();assert(deduplicated.execute(std::span(original).first(geometryOffset)));
    deduplicated.setPrimitiveBatchScratch({batchVertices,batchIndices,128,{hashSlots.data(),16},remap});
    assert(!deduplicated.execute(mixed)&&deduplicated.error()==CommandError::format);
    deduplicated.reset();assert(deduplicated.execute(std::span(original).first(geometryOffset)));
    deduplicated.setPrimitiveBatchScratch({batchVertices,batchIndices,128,hashSlots,remap});
    deduplicated.setSink([](const CommandDraw& d,void*){assert(d.vertexCount==3&&d.indexCount==6);
        for(unsigned n=0;n<6;++n){u16 index;assert(d.triangleIndex(n,index)&&index==0);}return true;});
    const u8 degenerate[]{GX_TRIANGLES,0,3,0,0,0,0,0,0,0,0,0,GX_TRIANGLES,0,3,0,0,0,0,0,0,0,0,0};
    assert(deduplicated.execute(degenerate));
    // Malformed tails still fail, and an aliased scratch rejects before copy.
    mixed.push_back(GX_TRIANGLES);assert(!batched.execute(mixed)&&batched.error()==CommandError::truncated);
    batched.reset();assert(batched.execute(std::span(original).first(geometryOffset)));
    batched.setPrimitiveBatchScratch({{mixed.data(),1024},batchIndices,128});
    assert(!batched.execute(mixed)&&batched.error()==CommandError::format);
    GDSetCurrent(nullptr);
    puts("PASS: original GD-to-command execution, CP/VAT caching, registered arrays, generation expiry, BP mask, indexed XF, all UV formats and draw decoding");
}
