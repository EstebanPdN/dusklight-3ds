// Native static geometry cache: compact-palette batches (Alpha 7 item A5) and
// the stage-change clear (A7). Every cached output is compared byte for byte
// with an uncached decode of the same batch and palette layout, and a compact
// record's replay placement with the slow path's.
#include "../runtime/gx_vertex_decode.hpp"
#include "../runtime/gx_rigid_vertex.hpp"
#include "../runtime/gx_compact_cache.hpp"
#include "../runtime/gx_vertex_warp.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>
#include <algorithm>
#include <bit>
using namespace dusk::native::gx;
using aurora::gx::dl::VtxLayout;
using Bytes=std::vector<u8>;
namespace {
constexpr unsigned vertexCount=96;
struct Fixture {
    Bytes positions,normals,colors,uv0,uv1,packed;
    VertexArrays arrays{};
    VertexDecodePlan plan;
    // Matrix selectors 0,3,6,9 (slots 0..3) and indexed POS/NRM/CLR0/TEX0
    // (and TEX1 for the multi-texture form), all little-endian host arrays.
    explicit Fixture(bool multi){
        VtxLayout layout{};unsigned cursor=0;
        layout.attrs[GX_VA_PNMTXIDX]={u8(cursor++),1,GX_DIRECT};
        std::vector<GXVtxAttrFmtList> formats;
        auto add=[&](GXAttr attr,GXCompCnt cnt,GXCompType type,u8 fraction){
            layout.attrs[attr]={u8(cursor++),1,GX_INDEX8};formats.push_back({attr,cnt,type,fraction});};
        add(GX_VA_POS,GX_POS_XYZ,GX_F32,0);add(GX_VA_NRM,GX_NRM_XYZ,GX_S16,14);
        add(GX_VA_CLR0,GX_CLR_RGBA,GX_RGBA8,0);add(GX_VA_TEX0,GX_TEX_ST,GX_S16,8);
        if(multi)add(GX_VA_TEX1,GX_TEX_ST,GX_S8,4);
        layout.stride=cursor;assert(planVertexDecode(layout,formats,plan));
        for(unsigned i=0;i<23;++i){
            for(unsigned c=0;c<3;++c){const float v=float(int(i*7+c*5)%31-15)*.37f+600.f*float(c==0);
                const auto* b=reinterpret_cast<const u8*>(&v);positions.insert(positions.end(),b,b+4);}
            for(unsigned c=0;c<3;++c){const std::int16_t v=std::int16_t(int(i*1301+c*4099)%30000-15000);
                const auto* b=reinterpret_cast<const u8*>(&v);normals.insert(normals.end(),b,b+2);}
            for(unsigned c=0;c<4;++c)colors.push_back(u8(i*37+c*61));
            for(unsigned c=0;c<2;++c){const std::int16_t v=std::int16_t(int(i*311+c*977)%2000-1000);
                const auto* b=reinterpret_cast<const u8*>(&v);uv0.insert(uv0.end(),b,b+2);}
            for(unsigned c=0;c<2;++c)uv1.push_back(u8(std::int8_t(int(i*13+c*29)%120-60)));
        }
        arrays[GX_VA_POS]={positions,12,true};arrays[GX_VA_NRM]={normals,6,true};
        arrays[GX_VA_CLR0]={colors,4,true};arrays[GX_VA_TEX0]={uv0,4,true};
        if(multi)arrays[GX_VA_TEX1]={uv1,2,true};
        const unsigned selectors[]{0,3,6,9};
        for(unsigned n=0;n<vertexCount;++n){
            packed.push_back(u8(selectors[(n*5+n/7)%4]));
            for(unsigned slot=0;slot<(multi?5u:4u);++slot)packed.push_back(u8((n*(3+2*slot)+slot*5)%23));
        }
    }
};
// The palette rows a planner would place: matrix 0 at row 0, the other three
// from the palette base; shared[m] puts matrix m's normal on its position rows.
void layoutPalette(RigidVertexPlan& rigid,bool compact,std::array<bool,4> shared){
    rigid.matrixOffsets.fill(-1);rigid.normalOffsets.fill(-1);
    rigid.matrixOffsets[0]=0;rigid.normalOffsets[0]=3;
    rigid.compactPalette=compact;
    unsigned row=rigid.paletteBase();
    for(unsigned m=1;m<4;++m){
        const bool share=compact&&shared[m];
        rigid.matrixOffsets[m]=std::int8_t(row);rigid.normalOffsets[m]=std::int8_t(share?row:row+3);
        row+=share?3:6;
    }
    rigid.paletteEnd=row;rigid.matrices=4;
}
RigidVertexPlan basePlan(bool multi){
    RigidVertexPlan rigid;
    rigid.color.material={.2f,.4f,.6f,.8f};rigid.color.lit=1;rigid.color.channels[0].mask=3;
    rigid.lights=2;rigid.indexedMatrices=true;rigid.multiTexture=multi;
    if(multi){multiRole(rigid,0)=1;multiRole(rigid,1)=1;rigid.unitSource={0,1,0};rigid.multiRowUnits=2;}
    return rigid;
}
Bytes bytesOf(const std::vector<GpuVertex>& output,std::size_t recordBytes){
    const auto* b=reinterpret_cast<const u8*>(output.data());return Bytes(b,b+vertexCount*recordBytes);
}
bool decode(const Fixture& f,RigidVertexPlan& rigid,std::vector<GpuVertex>& output,CompactVertexCache* cache=nullptr){
    std::fill(output.begin(),output.end(),GpuVertex{});
    return rigid.multiTexture?decodeCompactRigidBatchMT(f.plan,f.packed,f.arrays,output,rigid):
        decodeCompactRigidBatch(f.plan,f.packed,f.arrays,output,{.7f,.3f},rigid,cache);
}
// The vertex bytes a draw reads: resident pool bytes when handed out,
// otherwise the frame-arena output.
Bytes drawn(const RigidVertexPlan& rigid,const std::vector<GpuVertex>& output,std::size_t recordBytes){
    if(!rigid.staticVertices)return bytesOf(output,recordBytes);
    const auto* b=reinterpret_cast<const u8*>(rigid.staticVertices);return Bytes(b,b+vertexCount*recordBytes);
}
unsigned flushes{};
void countFlush(void*,std::size_t){++flushes;}
const int listToken=0;

void compactPaletteEntries(bool multi){
    const Fixture f(multi);
    const std::size_t recordBytes=multi?sizeof(CompactRigidVertexMT):sizeof(CompactRigidVertex);
    std::vector<GpuVertex> output(vertexCount);
    // Layouts 0 and 2 differ only in the last matrix's normal rows: same
    // offsets, so only the shared-normal bits tell their records apart.
    const std::array<std::array<bool,4>,3> layouts{{{false,true,false,true},{false,false,true,true},{false,true,false,false}}};
    // Uncached references, one per layout (no static hint, no cache).
    setStaticCompactCache(nullptr);staticGeometryHint={};
    std::vector<Bytes> reference;
    for(const auto& shared:layouts){
        auto rigid=basePlan(multi);layoutPalette(rigid,true,shared);
        assert(decode(f,rigid,output)&&rigid.compact&&!rigid.staticVertices);
        reference.push_back(bytesOf(output,recordBytes));
    }
    assert(reference[0]!=reference[1]&&reference[1]!=reference[2]&&reference[0]!=reference[2]);
    // Copied (non-resident) pool, then a GPU-resident one.
    for(const bool resident:{false,true}){
        std::vector<u8> memory(256*1024);auto owner=std::make_unique<StaticCompactCache>();auto& cache=*owner;
        flushes=0;assert(cache.attach(memory,resident,resident?countFlush:nullptr));
        setStaticCompactCache(&cache);staticGeometryHint={41,&listToken,1,false};
        compactPaletteRecords=true; // the patched native shape replay
        for(unsigned turn=0;turn<3;++turn)for(unsigned l=0;l<layouts.size();++l){
            auto rigid=basePlan(multi);layoutPalette(rigid,true,layouts[l]);
            const auto stores=cache.stores,hits=cache.hits;
            assert(decode(f,rigid,output));
            // First sight of a layout decodes and stores; every later one hits.
            if(turn==0)assert(cache.stores==stores+1&&cache.hits==hits&&!rigid.staticVertices);
            else assert(cache.hits==hits+1&&cache.stores==stores&&bool(rigid.staticVertices)==resident);
            assert(drawn(rigid,output,recordBytes)==reference[l]);
        }
        assert(cache.paletteStores==layouts.size()&&cache.paletteHits==2*layouts.size()&&cache.paletteArena==0);
        if(resident)assert(flushes==layouts.size());
        // A later batch of the list never hands out resident vertices: the hit
        // is copied into the frame arena, with the same bytes.
        staticGeometryHint.batch=2;
        for(unsigned turn=0;turn<2;++turn){
            auto rigid=basePlan(multi);layoutPalette(rigid,true,layouts[0]);
            assert(decode(f,rigid,output)&&!rigid.staticVertices);
            assert(bytesOf(output,recordBytes)==reference[0]);
        }
        assert(cache.paletteArena==(resident?1u:0u));
        // A replay that cannot place compact palettes: even the first batch
        // copies its hit into the frame arena, so it never becomes a record.
        compactPaletteRecords=false;staticGeometryHint.batch=1;
        for(unsigned l=0;l<layouts.size();++l){
            auto rigid=basePlan(multi);layoutPalette(rigid,true,layouts[l]);
            const auto hits=cache.hits;
            assert(decode(f,rigid,output)&&cache.hits==hits+1&&!rigid.staticVertices);
            assert(bytesOf(output,recordBytes)==reference[l]);
        }
        assert(cache.paletteArena==(resident?1u+layouts.size():0u));
        // A full palette (normal at +3 everywhere) keeps its own entries and
        // still hands out resident vertices from any batch.
        auto full=basePlan(multi);layoutPalette(full,false,{});
        assert(decode(f,full,output));const auto fullReference=bytesOf(output,recordBytes);
        assert(decode(f,full,output)&&drawn(full,output,recordBytes)==fullReference&&bool(full.staticVertices)==resident);
        setStaticCompactCache(nullptr);staticGeometryHint={};
        auto plain=basePlan(multi);layoutPalette(plain,false,{});
        assert(decode(f,plain,output)&&bytesOf(output,recordBytes)==fullReference);
    }
    std::printf("PASS %s compact-palette static entries: 3 layouts x 3 turns, copied and resident pools, later batches (and every batch without replay support) in the arena; every output equals the uncached decode\n",
        multi?"multi-texture":"single-texture");
}
void validatingCachePalette(){
    const Fixture f(false);
    std::vector<GpuVertex> output(vertexCount);
    std::vector<u8> storage(128*1024);CompactVertexCache cache;assert(cache.attach(storage));
    setStaticCompactCache(nullptr);staticGeometryHint={};
    const std::array<std::array<bool,4>,2> layouts{{{false,true,false,true},{false,true,false,false}}}; // same offsets
    std::vector<Bytes> reference;
    for(const auto& shared:layouts){auto rigid=basePlan(false);layoutPalette(rigid,true,shared);
        assert(decode(f,rigid,output));reference.push_back(bytesOf(output,sizeof(CompactRigidVertex)));}
    for(unsigned turn=0;turn<3;++turn)for(unsigned l=0;l<layouts.size();++l){
        auto rigid=basePlan(false);layoutPalette(rigid,true,layouts[l]);
        const auto hits=cache.hits;
        assert(decode(f,rigid,output,&cache));
        assert(cache.hits==hits+(turn?1:0));
        assert(bytesOf(output,sizeof(CompactRigidVertex))==reference[l]);
    }
    // A placement no key can express is decoded every time, never cached.
    auto odd=basePlan(false);layoutPalette(odd,false,{});odd.normalOffsets[2]=std::int8_t(odd.matrixOffsets[2]+6);
    std::uint16_t mask;assert(!sharedNormalMask(odd.matrixOffsets,odd.normalOffsets,mask));
    const auto stores=cache.stores;
    assert(decode(f,odd,output,&cache)&&decode(f,odd,output,&cache)&&cache.stores==stores);
    std::vector<u8> memory(64*1024);auto owner=std::make_unique<StaticCompactCache>();auto& statics=*owner;assert(statics.attach(memory));
    setStaticCompactCache(&statics);staticGeometryHint={42,&listToken,1,false};
    assert(decode(f,odd,output)&&decode(f,odd,output)&&statics.stores==0&&statics.hits==0);
    setStaticCompactCache(nullptr);staticGeometryHint={};
    std::puts("PASS validating compact cache keys the shared normal rows: compact layouts hit with uncached bytes, inexpressible placements stay uncached");
}
void stageClearProtocol(){
    for(const bool resident:{false,true}){
        std::vector<u8> memory(16*1024);auto owner=std::make_unique<StaticCompactCache>();auto& cache=*owner;
        assert(cache.attach(memory,resident,resident?countFlush:nullptr)&&cache.lastClear==StaticCompactCache::Clear::start);
        StaticCompactCache::Key key{};key.generation=5;key.count=4;
        const std::array<std::byte,128> bytes{};
        assert(cache.store(key,bytes)&&cache.find(key,bytes.size()));
        const auto epoch=cache.epoch;
        // Requested during a frame: entries stay valid until the next frame begins.
        cache.clearAtStageChange();
        assert(cache.find(key,bytes.size())&&cache.epoch==epoch);
        key.generation=6;assert(cache.store(key,bytes));
        assert(cache.frameBegin()&&cache.epoch==epoch+1&&cache.stageClears==1&&cache.resets==0);
        assert(cache.lastClear==StaticCompactCache::Clear::stage&&cache.clearedBytes==256&&cache.bytesUsed()==0);
        assert(!cache.find(key,bytes.size())&&!cache.frameBegin());
        // Different shared-normal layouts are different entries.
        key.sharedNormals=1;assert(cache.store(key,bytes));key.sharedNormals=2;assert(!cache.find(key,bytes.size()));
        key.sharedNormals=1;assert(cache.find(key,bytes.size()));
        // Full pool: a resident pool defers its reset to the frame boundary,
        // a copied pool resets at once. Both count as full resets.
        const std::array<std::byte,4096> large{};
        unsigned stored=0;bool refused=false;
        for(unsigned i=0;i<8&&!refused;++i){key.generation=100+i;if(cache.store(key,large))++stored;else refused=true;}
        if(resident){
            assert(refused&&stored==3&&cache.resets==0&&!cache.find(key,large.size()));
            key.generation=200;assert(!cache.store(key,bytes)); // nothing stored while a reset waits
            assert(!cache.frameBegin()&&cache.resets==1&&cache.lastClear==StaticCompactCache::Clear::full);
        }else assert(!refused&&cache.resets==2&&cache.lastClear==StaticCompactCache::Clear::full);
        // A stage change and a full pool in one frame clear once, as a stage clear.
        if(resident){
            for(unsigned i=0;i<4;++i){key.generation=300+i;cache.store(key,large);}
            cache.clearAtStageChange();
            assert(cache.frameBegin()&&cache.stageClears==2&&cache.resets==1);
        }
        auto detached=std::make_unique<StaticCompactCache>();detached->clearAtStageChange();assert(!detached->frameBegin()&&detached->stageClears==0);
    }
    std::puts("PASS static pool stage clear: deferred to the frame boundary, one epoch bump, counted apart from full-pool resets (copied and resident pools)");
}
// Native shape replay of a compact record (gx_command_draw.cpp nativeReplay
// and its chained form): the record keeps its matrices in palette order; the
// replay restarts the compact palette and adds them in that order. It must
// land on the slow path's rows exactly, and a changed shared-normal decision
// must change a row map the replay compares (it then refuses).
void replayRebuildsCompactLayout(){
    auto registers=std::make_unique<CommandRegisters>();auto& r=*registers;
    // Matrix slot s: position rows at XF 4*3s, normal rows at 0x400+3*3s.
    // Shared: normal = 2 x position. Otherwise each row scaled differently.
    auto load=[&](unsigned slot,bool shared){
        const unsigned index=slot*3;
        for(unsigned row=0;row<3;++row){
            for(unsigned c=0;c<4;++c){
                const float v=float(int(slot*7+row*3+c)%11-5)+(row==c?9.f:0.f);
                r.xf[4*index+4*row+c]=std::bit_cast<u32>(v);r.xfValid[4*index+4*row+c]=true;
            }
            for(unsigned c=0;c<3;++c){
                const float p=std::bit_cast<float>(r.xf[4*index+4*row+c]);
                r.xf[0x400+3*index+3*row+c]=std::bit_cast<u32>(shared?2.f*p:p*float(1+row));r.xfValid[0x400+3*index+3*row+c]=true;
            }
        }
    };
    std::array<bool,10> shared{true,true,false,true,false,true,true,false,true,false};
    for(unsigned s=0;s<10;++s)load(s,shared[s]);
    // Four lights: the full layout fits 1+7 matrices below row 81, so ten
    // overflow and the slow path restarts in compact form.
    RigidVertexPlan plan;plan.multiTexture=true;plan.lights=4;plan.multiRowUnits=3;
    plan.matrixOffsets.fill(-1);plan.normalOffsets.fill(-1);
    const unsigned position=9;assert(addRigidMatrix(r,position,plan)); // the planner's first vertex
    const unsigned encounter[]{9,27,3,0,21,3,6,12,27,15,18,24,0,9}; // vertex order, repeats included
    auto slowPath=[&](){
        auto rigid=plan;bool placed=true;
        for(unsigned m:encounter)if(!addRigidMatrix(r,m,rigid)){placed=false;break;}
        if(!placed){restartCompactPalette(rigid);placed=true;for(unsigned m:encounter)if(!addRigidMatrix(r,m,rigid)){placed=false;break;}}
        assert(placed);return rigid;
    };
    auto slow=slowPath();
    assert(slow.compactPalette&&slow.matrices==10&&slow.paletteEnd<=slow.paletteLimit());
    {auto full=plan;bool fits=true;for(unsigned m:encounter)fits&=addRigidMatrix(r,m,full);assert(!fits);}
    // The record: matrices sorted by their offsets.
    std::array<std::uint8_t,10> order{};unsigned count=0;
    for(unsigned m=0;m<10;++m)if(slow.matrixOffsets[m]>=0)order[count++]=std::uint8_t(m);
    std::sort(order.begin(),order.begin()+count,[&](std::uint8_t x,std::uint8_t y){return slow.matrixOffsets[x]<slow.matrixOffsets[y];});
    auto fullReplay=[&](){
        auto rigid=plan;restartCompactPalette(rigid);
        for(unsigned n=0;n<count;++n)if(!addRigidMatrix(r,order[n]*3u,rigid))return std::optional<RigidVertexPlan>{};
        return std::optional<RigidVertexPlan>{rigid};
    };
    auto chainedReplay=[&](){
        auto rigid=plan;rigid.matrixOffsets.fill(-1);rigid.normalOffsets.fill(-1);rigid.matrices=0;rigid.compactPalette=false;rigid.paletteEnd=0;
        if(!addRigidMatrix(r,position,rigid))return std::optional<RigidVertexPlan>{};
        restartCompactPalette(rigid);
        for(unsigned n=0;n<count;++n)if(!addRigidMatrix(r,order[n]*3u,rigid))return std::optional<RigidVertexPlan>{};
        return std::optional<RigidVertexPlan>{rigid};
    };
    for(auto replay:{fullReplay(),chainedReplay()}){
        assert(replay&&replay->matrixOffsets==slow.matrixOffsets&&replay->normalOffsets==slow.normalOffsets);
        assert(replay->paletteEnd==slow.paletteEnd&&replay->uniformCount()==slow.uniformCount());
        for(unsigned row=0;row<slow.paletteEnd;++row)assert(replay->uniforms[row]==slow.uniforms[row]);
    }
    std::uint16_t recorded;assert(sharedNormalMask(slow.matrixOffsets,slow.normalOffsets,recorded));
    // Live values change: one decision flips. The last placed matrix keeps
    // every offset and moves only its normal rows, so the replay must compare
    // the normal map as well. The slow path lands on the replay's new layout,
    // which keys another static entry.
    for(const unsigned flip:{unsigned(order[count-1]),unsigned(order[2])}){
        shared[flip]=!shared[flip];load(flip,shared[flip]);
        const auto changed=slowPath();
        for(auto replay:{fullReplay(),chainedReplay()}){
            assert(replay&&replay->matrixOffsets==changed.matrixOffsets&&replay->normalOffsets==changed.normalOffsets);
            assert(replay->normalOffsets!=slow.normalOffsets); // refused against the record
            if(flip==order[count-1])assert(replay->matrixOffsets==slow.matrixOffsets);
        }
        std::uint16_t mask;assert(sharedNormalMask(changed.matrixOffsets,changed.normalOffsets,mask)&&mask!=recorded);
        shared[flip]=!shared[flip];load(flip,shared[flip]);
    }
    std::puts("PASS compact record replay: full and chained rebuilds equal the slow path's rows; a flipped shared-normal decision changes the normal map the replay compares");
}
}
// The per-vertex warp pass of the multi-texture decoder (RigidVertexPlan::
// vertexWarp). Every record's second coordinate is what VertexWarpSampler
// stores for the vertex's TEX0 element, the rest of the record is the plain
// decode; a static batch drawn again under a warp it was sampled with copies
// those warps, within the picture, or in later pictures too when the map
// cannot change; another warp, batch or cache epoch samples again; such
// records are never handed out resident.
void vertexWarpPass(){
    const Fixture f(true);
    const std::size_t recordBytes=sizeof(CompactRigidVertexMT);
    std::vector<GpuVertex> output(vertexCount);
    // A 16x16 I8 warp map (GX tiles of 8x4) read through rows that scroll it.
    Bytes map(indirectLevelBytes(1,16,16));
    for(std::size_t i=0;i<map.size();++i)map[i]=u8(i*37+11);
    auto warpPlan=[&](float scroll){
        auto rigid=basePlan(true);layoutPalette(rigid,false,{});
        // One unit, made from TEX0: the record's second coordinate is free.
        multiRole(rigid,1)=0;rigid.unitSource={0,0,0};rigid.multiRowUnits=1;rigid.texcoordAttribute[1]=0xff;
        auto& w=rigid.vertexWarp;w.enabled=true;w.sourceAttribute=GX_VA_TEX0;
        w.rows={{{.75f,.125f,0,scroll},{-.25f,.5f,0,.3f},{0,0,1,0}}};
        w.levels[0]={map.data(),std::uint32_t(map.size()),16,16,true};w.format=1;w.wrapS=w.wrapT=1;
        w.functional={{{1,0,0},{-.5f,0,0}}};w.mean={128,128,128};w.recordScale=64;
        return rigid;
    };
    auto plain=[&]{auto rigid=warpPlan(0);rigid.vertexWarp={};return rigid;};
    setStaticCompactCache(nullptr);staticGeometryHint={};
    // Without a cache: the warps against the sampler, the other bytes
    // against the same decode without a warp.
    auto rigid=warpPlan(.1f);
    assert(decode(f,rigid,output)&&rigid.compact&&!rigid.staticVertices&&!rigid.wideRecord);
    const Bytes warped=bytesOf(output,recordBytes);
    {auto unwarped=plain();assert(decode(f,unwarped,output));
     const Bytes flat=bytesOf(output,recordBytes);
     const VertexWarpSampler sampler(rigid.vertexWarp);
     constexpr std::size_t at=offsetof(CompactRigidVertexMT,texcoord1);
     unsigned moved=0;
     for(unsigned n=0;n<vertexCount;++n){
        for(std::size_t b=0;b<recordBytes;++b)if(b<at||b>=at+4)assert(warped[n*recordBytes+b]==flat[n*recordBytes+b]);
        for(std::size_t b=at;b<at+4;++b)assert(flat[n*recordBytes+b]==0);
        // The vertex's TEX0 element: index at packed byte 4, S16 with 8 fraction bits.
        const unsigned element=f.packed[n*6+4];
        std::int16_t st[2];std::memcpy(st,f.uv0.data()+element*4,4);
        const float in[3]{st[0]/256.f,st[1]/256.f,1};
        std::byte reference[4];sampler.store(in,reference);
        std::int16_t want[2],got[2];std::memcpy(want,reference,4);std::memcpy(got,warped.data()+n*recordBytes+at,4);
        // The decoder's pass has no floats: two of its 1024 steps of a texel
        // may differ, through a map whose neighbours differ by 37.
        for(unsigned i=0;i<2;++i)assert(std::abs(int(got[i])-int(want[i]))<=2+int(.55f*std::abs(rigid.vertexWarp.functional[i][0])*64));
        moved+=got[0]!=0||got[1]!=0;
     }
     assert(moved>vertexCount/2);}
    // A static batch: stored once, copied into the arena each time (never
    // resident), and its warps copied from the first draw of the picture.
    for(const bool resident:{false,true}){
        std::vector<u8> memory(256*1024);auto owner=std::make_unique<StaticCompactCache>();auto& cache=*owner;
        assert(cache.attach(memory,resident,resident?countFlush:nullptr));
        setStaticCompactCache(&cache);staticGeometryHint={77,&listToken,1,false};
        vertexWarpRepeats=true;++textureFrameCounter;
        const auto copied=vertexWarpRepeatDraws;
        rigid=warpPlan(.1f);assert(decode(f,rigid,output)&&!rigid.staticVertices&&bytesOf(output,recordBytes)==warped);
        assert(cache.stores==1&&vertexWarpRepeatDraws==copied);
        rigid=warpPlan(.1f);assert(decode(f,rigid,output)&&!rigid.staticVertices&&bytesOf(output,recordBytes)==warped);
        assert(cache.hits==1&&vertexWarpRepeatDraws==copied+1&&vertexWarpRepeatVertices>=vertexCount);
        // Another warp of the same batch: sampled, and equal to its own uncached decode.
        rigid=warpPlan(.35f);assert(decode(f,rigid,output)&&vertexWarpRepeatDraws==copied+1);
        const Bytes scrolled=bytesOf(output,recordBytes);
        assert(scrolled!=warped);
        setStaticCompactCache(nullptr);staticGeometryHint={};
        rigid=warpPlan(.35f);assert(decode(f,rigid,output)&&bytesOf(output,recordBytes)==scrolled);
        setStaticCompactCache(&cache);staticGeometryHint={77,&listToken,1,false};
        // A mesh has one place: the first warp again is sampled into it (it
        // held the other one), then copied; the next picture samples again,
        // since this map may change.
        rigid=warpPlan(.1f);assert(decode(f,rigid,output)&&vertexWarpRepeatDraws==copied+1&&bytesOf(output,recordBytes)==warped);
        rigid=warpPlan(.1f);assert(decode(f,rigid,output)&&vertexWarpRepeatDraws==copied+2&&bytesOf(output,recordBytes)==warped);
        ++textureFrameCounter;
        rigid=warpPlan(.1f);assert(decode(f,rigid,output)&&vertexWarpRepeatDraws==copied+2&&bytesOf(output,recordBytes)==warped);
        // A map that cannot change: the warps hold in the next picture too,
        // until the static cache starts again.
        auto constant=[&]{auto plan=warpPlan(.1f);plan.vertexWarp.constantMap=true;return plan;};
        rigid=constant();assert(decode(f,rigid,output)&&vertexWarpRepeatDraws==copied+2&&bytesOf(output,recordBytes)==warped);
        ++textureFrameCounter;
        rigid=constant();assert(decode(f,rigid,output)&&vertexWarpRepeatDraws==copied+3&&bytesOf(output,recordBytes)==warped);
        cache.clear();
        rigid=constant();assert(decode(f,rigid,output)&&vertexWarpRepeatDraws==copied+3&&bytesOf(output,recordBytes)==warped);
        rigid=constant();assert(decode(f,rigid,output)&&vertexWarpRepeatDraws==copied+4&&bytesOf(output,recordBytes)==warped);
        // Another batch of the list is another mesh; the switch off copies nothing.
        staticGeometryHint={77,&listToken,2,false};
        rigid=constant();assert(decode(f,rigid,output)&&vertexWarpRepeatDraws==copied+4);
        vertexWarpRepeats=false;
        rigid=constant();assert(decode(f,rigid,output)&&vertexWarpRepeatDraws==copied+4&&bytesOf(output,recordBytes)==warped);
        vertexWarpRepeats=true;
        // The same batch without a warp is still handed out resident.
        auto unwarped=plain();assert(decode(f,unwarped,output)&&bool(unwarped.staticVertices)==resident);
        setStaticCompactCache(nullptr);staticGeometryHint={};
    }
    std::puts("PASS vertex warp pass: each record's second coordinate is the sampler's value for its TEX0 element and nothing else changes; "
        "a static batch drawn again copies its warps (within the picture; across pictures with a map that cannot change), another warp "
        "(into the same place), batch or cache epoch samples; never resident");
}
int main(){
    vertexWarpPass();
    replayRebuildsCompactLayout();
    stageClearProtocol();
    compactPaletteEntries(true);
    compactPaletteEntries(false);
    validatingCachePalette();
}
