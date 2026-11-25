// Execute the actual PICA arena adapter with a shadow GPU memory model. Draws
// observe only successfully flushed bytes when the simulated queue executes.
#include "../runtime/gx_compact_cache.hpp"
#include "../gx_geometry_pica.hpp"
#include "../runtime/gpu_sync.hpp"
#include "../runtime/gx_fog_pass.hpp"
#include <cmath>
#include <algorithm>
#include <3ds/types.h>
#include <3ds/gpu/enums.h>
extern "C" {
#include <3ds/gpu/gpu.h>
#include <3ds/gpu/registers.h>
}
#include <3ds/gpu/shaderProgram.h>
extern "C" {
#include <3ds/allocator/linear.h>
#include <3ds/services/gspgpu.h>
#include <c3d/attribs.h>
#include <c3d/buffers.h>
#include <c3d/base.h>
}
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <limits>
using namespace dusk::native;
// Linear allocations get fake physical addresses (Citro3D encodes buffer
// offsets as 32-bit physical addresses; host pointers do not fit).
struct Allocation {std::size_t size;std::vector<unsigned char> gpu;u32 physical;};
static std::map<void*,Allocation> allocations;
static u32 nextPhysical=0x18100000;
struct Queued {const void* vertex;std::size_t vertexBytes;const void* index;std::size_t indexBytes;};
static std::vector<Queued> queued;
// Citro3D 1.7.1 context copies (attribs.c, buffers.c) with their dirty flags,
// and the PICA registers the command words write. Each draw reads its vertex
// address, stride and loaders back from the REGISTERS, so a configuration that
// was not sent, or sent wrong, fails here.
static C3D_AttrInfo contextAttributes;
static C3D_BufInfo contextBuffers;
static bool attributesDirty,buffersDirty;
static std::array<u32,0x300> registers{};
static const void* vertices;
static unsigned stride;
static unsigned compactDraws,attributeChanges,bufferBlocks,bufferWords;
static unsigned flushes,draws,completions;
static std::size_t allocationLimit=SIZE_MAX;
static bool refuseFlush;
static void writeRegisters(u32 header,const u32* param,u32 count){
    const unsigned reg=header&0x3ff,mask=(header>>16)&0xf;const bool incremental=header>>31;
    u32 bits=0;for(unsigned b=0;b<4;++b)if(mask>>b&1)bits|=0xffu<<(8*b);
    for(u32 i=0;i<count;++i){auto& r=registers.at(incremental?reg+i:reg);r=(r&~bits)|(param[i]&bits);}
}
static const void* virtualAt(u32 physical){
    for(auto& [base,a]:allocations)if(physical>=a.physical&&physical-a.physical<a.size)return static_cast<unsigned char*>(base)+(physical-a.physical);
    return nullptr;
}
extern "C" {
void* linearMemAlign(size_t n,size_t alignment){
    if(n>allocationLimit)return nullptr;
    void* p=nullptr;assert(!posix_memalign(&p,alignment,n));
    allocations.emplace(p,Allocation{n,std::vector<unsigned char>(n,0xa5),nextPhysical});
    nextPhysical+=u32((n+0xfff)&~std::size_t(0xfff))+0x1000;return p;
}
void linearFree(void* p){assert(queued.empty());assert(allocations.erase(p)==1);std::free(p);}
u32 osConvertVirtToPhys(const void* p){
    const auto at=reinterpret_cast<std::uintptr_t>(p);
    for(auto& [base,a]:allocations){const auto start=reinterpret_cast<std::uintptr_t>(base);if(at>=start&&at-start<a.size)return a.physical+u32(at-start);}
    return 0;
}
Result GSPGPU_FlushDataCache(const void* p,u32 bytes){
    ++flushes;if(refuseFlush)return -1;
    for(auto& [base,a]:allocations){const auto start=reinterpret_cast<std::uintptr_t>(base),at=reinterpret_cast<std::uintptr_t>(p);
        if(at>=start&&at-start<a.size){assert(bytes<=a.size-(at-start));std::memcpy(a.gpu.data()+at-start,p,bytes);return 0;}}
    assert(false);return -1;
}
// Verbatim Citro3D 1.7.1 logic.
void AttrInfo_Init(C3D_AttrInfo* info){std::memset(info,0,sizeof(*info));info->flags[1]=0xFFF<<16;}
int AttrInfo_AddLoader(C3D_AttrInfo* info,int regId,GPU_FORMATS format,int count){
    if(info->attrCount==12)return -1;
    int id=info->attrCount++;
    if(regId<0)regId=id;
    if(id<8)info->flags[0]|=GPU_ATTRIBFMT(id,count,format);
    else info->flags[1]|=GPU_ATTRIBFMT(id-8,count,format);
    info->flags[1]=(info->flags[1]&~(0xF0000000|BIT(id+16)))|(id<<28);
    info->permutation|=u64(regId)<<(id*4);
    return id;
}
void BufInfo_Init(C3D_BufInfo* info){std::memset(info,0,sizeof(*info));info->base_paddr=0x18000000;}
int BufInfo_Add(C3D_BufInfo* info,const void* data,ptrdiff_t bytes,int attribCount,u64 permutation){
    if(info->bufCount==12)return -1;
    int id=info->bufCount++;
    u32 pa=osConvertVirtToPhys(data);
    if(pa<info->base_paddr)return -2;
    C3D_BufCfg* buf=&info->buffers[id];
    buf->offset=pa-info->base_paddr;
    buf->flags[0]=permutation&0xFFFFFFFF;
    buf->flags[1]=(permutation>>32)|(bytes<<16)|(attribCount<<28);
    return id;
}
C3D_AttrInfo* C3D_GetAttrInfo(){attributesDirty=true;return &contextAttributes;}
C3D_BufInfo* C3D_GetBufInfo(){buffersDirty=true;return &contextBuffers;}
void GPUCMD_Add(u32 header,const u32* param,u32 count){
    writeRegisters(header,param,count);
    const unsigned reg=header&0x3ff;
    // Words of the buffer block (location and the twelve buffers), padding included.
    if(reg==GPUREG_ATTRIBBUFFERS_LOC||(reg>=GPUREG_ATTRIBBUFFER0_OFFSET&&reg<=GPUREG_ATTRIBBUFFER0_OFFSET+35))bufferWords+=1+count+((1+count)&1);
}
void C3D_DrawElements(GPU_Primitive_t mode,int count,int type,const void* data){
    assert(mode==GPU_TRIANGLES&&type==C3D_UNSIGNED_SHORT);
    // C3Di_UpdateContext: C3Di_AttrInfoBind, then C3Di_BufInfoBind.
    if(attributesDirty){
        attributesDirty=false;++attributeChanges;const auto& a=contextAttributes;
        GPUCMD_AddIncrementalWrites(GPUREG_ATTRIBBUFFERS_FORMAT_LOW,a.flags,2);
        GPUCMD_AddMaskedWrite(GPUREG_VSH_INPUTBUFFER_CONFIG,0xB,0xA0000000|(a.attrCount-1));
        GPUCMD_AddWrite(GPUREG_VSH_NUM_ATTR,a.attrCount-1);
        GPUCMD_AddIncrementalWrites(GPUREG_VSH_ATTRIBUTES_PERMUTATION_LOW,reinterpret_cast<const u32*>(&a.permutation),2);
    }
    if(buffersDirty){
        buffersDirty=false;++bufferBlocks;
        GPUCMD_AddWrite(GPUREG_ATTRIBBUFFERS_LOC,contextBuffers.base_paddr>>3);
        GPUCMD_AddIncrementalWrites(GPUREG_ATTRIBBUFFER0_OFFSET,reinterpret_cast<const u32*>(contextBuffers.buffers),36);
    }
    // The registers Citro3D's record claims must be the registers sent.
    const auto* sent=reinterpret_cast<const u32*>(contextBuffers.buffers);
    for(unsigned i=0;i<36;++i)assert(registers[GPUREG_ATTRIBBUFFER0_OFFSET+i]==sent[i]);
    assert(registers[GPUREG_ATTRIBBUFFERS_LOC]==contextBuffers.base_paddr>>3);
    assert(registers[GPUREG_ATTRIBBUFFERS_FORMAT_LOW]==contextAttributes.flags[0]&&registers[GPUREG_ATTRIBBUFFERS_FORMAT_HIGH]==contextAttributes.flags[1]);
    // Decode the draw from the registers: one buffer, six loaders, as before.
    const u32 base=registers[GPUREG_ATTRIBBUFFERS_LOC]<<3,config1=registers[GPUREG_ATTRIBBUFFER0_CONFIG1],config2=registers[GPUREG_ATTRIBBUFFER0_CONFIG2];
    vertices=virtualAt(base+registers[GPUREG_ATTRIBBUFFER0_OFFSET]);stride=(config2>>16)&0xfff;
    assert(vertices&&config1==0x543210&&(config2&0xffff)==0&&config2>>28==6);
    for(unsigned b=1;b<12;++b)for(unsigned w=0;w<3;++w)assert(!registers[GPUREG_ATTRIBBUFFER0_OFFSET+3*b+w]);
    const u32 formats=registers[GPUREG_ATTRIBBUFFERS_FORMAT_LOW],high=registers[GPUREG_ATTRIBBUFFERS_FORMAT_HIGH];
    const u64 order=registers[GPUREG_VSH_ATTRIBUTES_PERMUTATION_LOW]|u64(registers[GPUREG_VSH_ATTRIBUTES_PERMUTATION_LOW+1])<<32;
    assert(high>>28==5&&registers[GPUREG_VSH_NUM_ATTR]==5&&(registers[GPUREG_VSH_INPUTBUFFER_CONFIG]&0xf)==5);
    struct Attribute {unsigned reg;GPU_FORMATS type;unsigned count;};
    const bool compact=stride==sizeof(gx::CompactRigidVertex)||stride==sizeof(gx::CompactRigidVertexMT);
    static_assert(sizeof(gx::CompactRigidVertex)!=sizeof(gx::GpuVertex)&&sizeof(gx::CompactRigidVertexMT)!=sizeof(gx::GpuVertex));
    assert(compact||stride==sizeof(gx::GpuVertex));
    // Six attributes, the unused v5 of the single compact layout included.
    const Attribute compactLayout[]{{0,GPU_FLOAT,3},{1,GPU_SHORT,3},{2,GPU_SHORT,2},{5,GPU_SHORT,2},{4,GPU_BYTE,2},{3,GPU_UNSIGNED_BYTE,4}};
    const Attribute clipLayout[]{{0,GPU_FLOAT,4},{1,GPU_FLOAT,4},{2,GPU_FLOAT,2},{3,GPU_FLOAT,2},{4,GPU_FLOAT,2},{5,GPU_FLOAT,1}};
    for(unsigned i=0;i<6;++i){
        const auto& e=(compact?compactLayout:clipLayout)[i];
        assert(((formats>>(4*i))&3)==unsigned(e.type)&&((formats>>(4*i+2))&3)+1==e.count&&((order>>(4*i))&15)==e.reg);
    }
    if(compact)++compactDraws;
    const auto* indices=static_cast<const u16*>(data);unsigned maximum=0;
    for(int i=0;i<count;++i)if(indices[i]>maximum)maximum=indices[i];
    queued.push_back({vertices,(maximum+1)*stride,data,std::size_t(count)*2});++draws;
}
void OSReport(const char*,...){}
}
static void assertPublished(const void* p,std::size_t bytes){
    for(auto& [base,a]:allocations){const auto start=reinterpret_cast<std::uintptr_t>(base),at=reinterpret_cast<std::uintptr_t>(p);
        if(at>=start&&at-start<a.size){assert(bytes<=a.size-(at-start));assert(!std::memcmp(a.gpu.data()+at-start,p,bytes));return;}}
    assert(false);
}
static void executeQueue(){for(auto q:queued){assertPublished(q.vertex,q.vertexBytes);assertPublished(q.index,q.indexBytes);}queued.clear();}
namespace dusk::native {
// Frame-begin hooks of other modules (copy execution, PICA state filter,
// compact cache); this fixture has none of them.
void dropPendingCapture(){}
// As in gx_pica.cpp: a new frame sends the attribute and buffer blocks again.
void invalidatePicaCommandState(){invalidateGxGeometryBindings();}
namespace gx {StaticCompactCache* staticCompactCache(){return nullptr;}}
bool platformBeginGpuCommands(){assert(queued.empty());return true;}
GpuCompletion platformReserveGpuEntries(unsigned n,std::uint64_t){assert(n==2);return GpuCompletion::complete;}
GpuCompletion platformCompleteGpu(std::uint64_t){++completions;if(!flushGxGeometry())return GpuCompletion::exhausted;executeQueue();return GpuCompletion::complete;}
}
static gx::GpuGeometryBatch make(unsigned count,float value){
    gx::GpuGeometryBatch b;assert(reserveGxGeometry(count,3,b));
    // w=64 keeps every vertex inside the hardware clip guard (|x|,|y| <= 8w).
    for(auto& v:b.vertices){v={};v.position={value,2,3,64};v.color={.2f,.4f,.6f,1};v.texcoord[0]={value,.5f};}
    b.indices[0]=0;b.indices[1]=1;b.indices[2]=count-1;return b;
}
static void casterSnapshots(){
    constexpr unsigned count=1501;
    using Record=gx::CompactRigidVertex;
    static_assert(offsetof(Record,matrix)==offsetof(gx::CompactRigidVertexMT,matrix));
    std::vector<Record> expected(count);
    for(unsigned i=0;i<count;++i){
        expected[i].position={float(i),float(i%7),-float(i)};
        expected[i].matrix={std::int8_t(i%2?20:0),3};
    }
    const std::array<u16,3> expectedIndices{1500,2,57};
    const auto prepare=[&]{
        auto batch=make(count,1);
        std::memcpy(static_cast<void*>(batch.vertices.data()),expected.data(),count*sizeof(Record));
        std::copy(expectedIndices.begin(),expectedIndices.end(),batch.indices.begin());
        const auto compactSlots=(count*sizeof(Record)+sizeof(gx::GpuVertex)-1)/sizeof(gx::GpuVertex);
        assert(shrinkGxGeometry(batch,compactSlots,batch.indices.size(),true));
        assert(submitGxCompactRigidGeometry(batch,nullptr,true));
        return batch;
    };

    const auto batch=prepare();
    const void* vertices=batch.vertices.data();
    const u16* indices=batch.indices.data();
    assert(!retainGxCasterGeometry(vertices,sizeof(Record),indices,3));
    assert(vertices==batch.vertices.data()&&indices==batch.indices.data());
    allocationLimit=0;
    assert(!retainGxCasterGeometry(vertices,batch.vertices.size_bytes(),indices,3));
    assert(vertices==batch.vertices.data()&&indices==batch.indices.data());
    allocationLimit=60*1024; // refuse the preferred page, accept the exact-size spill
    assert(retainGxCasterGeometry(vertices,batch.vertices.size_bytes(),indices,3));
    allocationLimit=SIZE_MAX;
    assert(vertices!=batch.vertices.data()&&indices!=batch.indices.data());

    const auto other=prepare();
    const void* otherVertices=other.vertices.data();
    const u16* otherIndices=other.indices.data();
    const auto pagesBefore=allocations.size();
    assert(retainGxCasterGeometry(otherVertices,other.vertices.size_bytes(),otherIndices,3));
    assert(allocations.size()==pagesBefore+1);

    // Borrow stable vertices while retaining transient indices independently.
    const void* stable=vertices;
    const u16* copiedIndices=other.indices.data();
    assert(retainGxCasterGeometry(stable,0,copiedIndices,3));
    assert(stable==vertices&&copiedIndices!=other.indices.data());

    const auto generation=gxGeometryArenaGeneration();
    for(unsigned n=0;n<30;++n){
        auto overwrite=make(count,float(n));
        assert(submitGxGeometry(overwrite));
    }
    assert(gxGeometryArenaGeneration()!=generation);
    const auto beforeOversized=gxGeometryArenaGeneration();
    gx::GpuGeometryBatch oversized;
    assert(!reserveGxGeometry(1024*1024,3,oversized));
    assert(gxGeometryArenaGeneration()!=beforeOversized);
    assert(std::memcmp(vertices,expected.data(),count*sizeof(Record))==0);
    assert(std::memcmp(otherVertices,expected.data(),count*sizeof(Record))==0);
    assert(std::memcmp(indices,expectedIndices.data(),sizeof(expectedIndices))==0);
    assert(std::memcmp(copiedIndices,expectedIndices.data(),sizeof(expectedIndices))==0);
    refuseFlush=true;
    assert(!publishGxCasterGeometry());
    refuseFlush=false;
    assert(publishGxCasterGeometry());
    const auto published=flushes;
    assert(publishGxCasterGeometry()&&flushes==published);
    assert(drawResidentRigid(vertices,indices,3,true));
    assert(drawResidentRigid(otherVertices,otherIndices,3,false));
    assert(endGxGeometryFrame());
    executeQueue();

    // A successful fence makes the pages reusable at the next frame begin.
    assert(beginGxGeometryFrame());
    const auto held=allocations.size();
    const auto next=prepare();
    const void* nextVertices=next.vertices.data();
    const u16* nextIndices=next.indices.data();
    assert(retainGxCasterGeometry(nextVertices,next.vertices.size_bytes(),nextIndices,3));
    assert(allocations.size()==held);
    assert(publishGxCasterGeometry()&&drawResidentRigid(nextVertices,nextIndices,3,true));
    assert(endGxGeometryFrame());
    executeQueue();
    std::puts("PASS caster snapshots: unchanged indexed positions and palette rows across arena reclaims, page spill, allocation/flush failure and fenced reuse");
}
int main(){
    assert(flushGxGeometry());assert(initializeGxGeometry()&&beginGxGeometryFrame());
    const auto before=flushes;
    auto a=make(3,17);assert(submitGxGeometry(a));auto b=make(4,29);assert(submitGxGeometry(b));
    assert(flushes==before&&queued.size()==2);assert(flushGxGeometry());executeQueue();assert(flushes==before+1);
    assert(flushGxGeometry()&&flushes==before+1);
    auto pending=make(3,41);assert(flushGxGeometry()&&flushes==before+1);assert(!endGxGeometryFrame());
    assert(submitGxGeometry(pending));refuseFlush=true;assert(!flushGxGeometry()&&queued.size()==1);
    refuseFlush=false;assert(flushGxGeometry());executeQueue();assert(flushes==before+3);
    auto cancelled=make(3,53);assert(cancelGxGeometry(cancelled));assert(flushGxGeometry()&&flushes==before+3);
    auto invalid=make(3,61);invalid.indices[2]=3;assert(!submitGxGeometry(invalid)&&queued.empty());assert(cancelGxGeometry(invalid));

    // Execute the actual ordinary geometry backend with signed vertex bytes.
    auto signedBatch=make(3,1);signedBatch.qPolicy=gx::GeometryQPolicy::directTexture0Diagnostic;
    signedBatch.vertices[0].textureQ=-2;signedBatch.vertices[1].textureQ=4;signedBatch.vertices[2].textureQ=-8;
    auto strictSigned=signedBatch;strictSigned.qPolicy=gx::GeometryQPolicy::positiveOnly;
    const auto sentSigned=draws;assert(!submitGxGeometry(strictSigned)&&draws==sentSigned&&queued.empty());
    unsigned handle=99;assert(!uploadStaticGxGeometry(signedBatch,handle)&&!handle&&draws==sentSigned);
    assert(submitGxGeometry(signedBatch)&&draws==sentSigned+1);assert(flushGxGeometry());executeQueue();
    for(float q:{0.f,-0.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}){
        auto bad=make(3,1);bad.qPolicy=gx::GeometryQPolicy::directTexture0Diagnostic;bad.vertices[2].textureQ=q;
        const auto sent=draws;assert(!submitGxGeometry(bad)&&draws==sent&&queued.empty());assert(cancelGxGeometry(bad));
    }
    std::puts("PASS signed-q real arena backend: strict refusal, static refusal, exact GPU shadow byte publication, zero/nonfinite fail before draws");
    // CPU scratch tails must never become GPU-visible vertices or flush bytes.
    const auto beforeShrinkBytes=gxGeometryBytes();const auto beforeShrinkFlush=flushes,beforeShrinkDraw=draws,beforeShrinkWait=completions;
    gx::GpuGeometryBatch grown;assert(reserveGxGeometry(8,9,grown));
    for(unsigned i=0;i<8;++i){grown.vertices[i]={};grown.vertices[i].position={float(i),0,-.5f,1};grown.vertices[i].textureQ=i<3?1.f:-1.f;}
    const u16 scratchIndices[]{0,1,2,0,6,7,1,6,7};std::copy(std::begin(scratchIndices),std::end(scratchIndices),grown.indices.begin());
    const auto stale=grown;
    assert(flushGxGeometry()&&flushes==beforeShrinkFlush); // pending scratch excluded
    assert(shrinkGxGeometry(grown,3,3)&&grown.qPolicy==gx::GeometryQPolicy::positiveOnly&&gxGeometryBytes()==beforeShrinkBytes);
    assert(flushes==beforeShrinkFlush&&draws==beforeShrinkDraw&&completions==beforeShrinkWait);
    assert(!cancelGxGeometry(stale)&&!submitGxGeometry(stale)&&queued.empty());
    assert(submitGxGeometry(grown)&&gxGeometryBytes()==((beforeShrinkBytes+15)&~std::size_t(15))+198);
    assert(flushGxGeometry());executeQueue();
    std::puts("PASS scratch shrink publication: relocated indices, positive-only vertex prefix, pending tails excluded, no shrink wait/flush/draw, stale handle rejected");
    // Compact attributes must reach the queue with the actual register layout
    // and published bytes, including a nonzero palette selector.
    auto compact=make(3,0);
    for(unsigned i=0;i<3;++i){
        gx::CompactRigidVertex v{{float(i),2,3},{-16384,0,16384},{-8192,4096},{std::int16_t(i*39)}};
        std::memcpy(std::as_writable_bytes(compact.vertices).data()+i*sizeof(v),&v,sizeof(v));
    }
    assert(submitGxCompactRigidGeometry(compact));assert(flushGxGeometry());executeQueue();assert(compactDraws==1);
    auto broken=make(3,0);gx::CompactRigidVertex bad{};bad.position[0]=std::numeric_limits<float>::quiet_NaN();
    std::memcpy(broken.vertices.data(),&bad,sizeof(bad));
    assert(!submitGxCompactRigidGeometry(broken)&&queued.empty());assert(cancelGxGeometry(broken));
    // Actual backend ordering is original0,fog0,original1,fog1. Simulate a
    // command-buffer flush before EVERY pass: pending bytes must be committed.
    gx::GpuGeometryBatch pair;assert(reserveGxGeometry(8,6,pair));
    for(unsigned i=0;i<8;++i){auto& v=pair.vertices[i];v={};v.position={float(i%4),0,-.5,1};v.color={.2,.3,.4,i<4?.75f:.5f};}
    const u16 pairIndices[]{0,1,2,0,2,3};std::copy(std::begin(pairIndices),std::end(pairIndices),pair.indices.begin());
    unsigned passes=0;const auto pairAttributeChanges=attributeChanges;
    auto pass=[](bool fog,void* data){auto& count=*static_cast<unsigned*>(data);assert(fog==bool(count&1));
        assert(flushGxGeometry());executeQueue();++count;return true;};
    assert(submitGxTrianglePairs(pair,pass,&passes)&&passes==4);assert(flushGxGeometry());executeQueue();
    assert(attributeChanges==pairAttributeChanges+1);
    gx::GpuGeometryBatch additive;assert(reserveGxGeometry(8,12,additive));
    for(unsigned i=0;i<8;++i){additive.vertices[i]={};additive.vertices[i].position={float(i%4),0,-.5,1};}
    std::copy_n(pairIndices,6,additive.indices.begin());
    std::fill(additive.indices.begin()+6,additive.indices.end(),UINT16_MAX);
    passes=0;
    auto additivePass=[](unsigned pass,void* data){
        auto& count=*static_cast<unsigned*>(data);assert(pass==count);
        assert(flushGxGeometry());
        if(pass)assert(queued.size()==1);
        executeQueue();++count;return true;
    };
    const auto beforeAdditive=draws;
    assert(submitGxTrianglePasses(additive,true,true,additivePass,&passes)&&passes==3&&draws==beforeAdditive+3);
    const u16 reversed[]{0,2,3,0,1,2};
    assert(std::equal(std::begin(reversed),std::end(reversed),additive.indices.begin()+6));
    assert(vertices==additive.vertices.data()+4&&flushGxGeometry());executeQueue();
    // Two alpha ranges reuse the SAME vertices and preserve primitive order.
    // Force queue execution before every range, including the first one.
    gx::GpuGeometryBatch alphaPair;assert(reserveGxGeometry(4,6,alphaPair));
    for(unsigned i=0;i<4;++i){alphaPair.vertices[i]={};alphaPair.vertices[i].position={float(i),0,-.5,1};}
    std::copy(std::begin(pairIndices),std::end(pairIndices),alphaPair.indices.begin());
    passes=0;const auto beforeAlpha=draws;
    assert(submitGxTriangleAlphaPairs(alphaPair,pass,&passes)&&passes==4&&draws==beforeAlpha+4);
    assert(vertices==alphaPair.vertices.data());assert(flushGxGeometry());executeQueue();
    auto badPair=make(8,0);badPair.indices[2]=4;
    assert(!submitGxTrianglePairs(badPair,pass,&passes)&&passes==4);assert(cancelGxGeometry(badPair));
    auto unaligned=make(6,0);unaligned.indices[2]=2;
    assert(!submitGxTrianglePairs(unaligned,pass,&passes));assert(cancelGxGeometry(unaligned));
    // Exhaustive raster-state admission: only opaque RGB, untouched alpha,
    // always-alpha, depth-write LEQUAL. The second pass is EQUAL/no depth write.
    gx::RasterPlan original;original.writeRgb=true;original.alpha.comparison=7;original.depth={true,3,true};
    gx::RasterPlan overlay;gx::PicaMaterialPlan fogMaterial;
    assert(gx::planTriangleFogPass(original,overlay,fogMaterial));
    assert(overlay.depth.compare==2&&!overlay.depth.write&&overlay.blend.src==4&&overlay.blend.dst==5&&!overlay.writeAlpha);
    for(unsigned reason=0;reason<9;++reason){auto invalid=original;
        if(reason==0)invalid.blend.mode=1;else if(reason==1)invalid.subtractBlend=true;
        else if(reason==2)invalid.writeRgb=false;else if(reason==3)invalid.writeAlpha=true;
        else if(reason==4)invalid.alpha.comparison=4;else if(reason==5)invalid.depth.enabled=false;
        else if(reason==6)invalid.depth.write=false;else if(reason==7)invalid.depth.compare=1;else invalid.earlyDepthAlpha=true;
        assert(!gx::planTriangleFogPass(invalid,overlay,fogMaterial));}
    // Independent per-pixel occlusion model includes closer, farther and
    // coplanar overlapping triangles. Compare combined fog with actual extra
    // UNORM8 store. RGB differs by at most one byte; depth/alpha are identical.
    auto quant=[](double v){return std::round(std::clamp(v,0.0,1.0)*255)/255;};
    std::uint32_t random=17;auto randomWord=[&](){random=1664525*random+1013904223;return random;};
    for(unsigned sample=0;sample<100000;++sample){
        double expected=.25,actual=.25,depth=1,actualDepth=1;
        for(unsigned triangle=0;triangle<8;++triangle){
            const double z=double(randomWord()%5)/4,source=double(randomWord()&65535)/65535,
                fog=double(randomWord()&255)/255,factor=double(randomWord()&65535)/65535;
            const bool covered=randomWord()&1;
            if(covered&&z<=depth){expected=quant(source*(1-factor)+fog*factor);depth=z;}
            if(covered&&z<=actualDepth){actual=quant(source);actualDepth=z;}
            if(covered&&z==actualDepth)actual=quant(actual*(1-factor)+fog*factor);
            assert(depth==actualDepth&&std::abs(expected-actual)<=1.000001/255);
        }
    }
    // Whole-mesh fog is observably wrong: two coplanar triangles cover the same
    // pixel. Interleaving overwrites the first fog result before the next one.
    const double correct=.5,wholeMesh=.75;assert(wholeMesh-correct>.2);
    std::puts("PASS triangle-pair queue ordering/publication/alignment and 800000 occlusion/color fragments; <=1 RGB byte extra-store error in host model");
    // Alpha-union layers as two whole-mesh passes: the first test over every
    // triangle, then the second; one draw each, same vertices and indices.
    gx::GpuGeometryBatch unionMesh;assert(reserveGxGeometry(4,6,unionMesh));
    for(unsigned i=0;i<4;++i){unionMesh.vertices[i]={};unionMesh.vertices[i].position={float(i),0,-.5,1};}
    std::copy(std::begin(pairIndices),std::end(pairIndices),unionMesh.indices.begin());
    passes=0;const auto beforeUnion=draws;
    auto unionPass=[](bool second,void* data){auto& count=*static_cast<unsigned*>(data);assert(second==(count==1));
        assert(flushGxGeometry());executeQueue();++count;return true;};
    assert(submitGxAlphaUnionMesh(unionMesh,unionPass,&passes)&&passes==2&&draws==beforeUnion+2);
    assert(vertices==unionMesh.vertices.data()&&queued.size()==1&&queued.back().indexBytes==12);
    assert(flushGxGeometry());executeQueue();
    auto badUnion=make(3,0);badUnion.indices[2]=3;
    assert(!submitGxAlphaUnionMesh(badUnion,unionPass,&passes)&&passes==2&&queued.empty());assert(cancelGxGeometry(badUnion));
    // Why the order of the passes may change for these blends: the integer
    // blend of the PICA (and Azahar), out = min(255, (src*sf + dst*df)/255),
    // with disjoint alpha tails and a depth buffer the mesh does not write.
    // Per triangle (both tests, triangle by triangle) and whole-mesh (all
    // first-test fragments, then all second-test ones) give the same bytes
    // for every admitted factor pair; an alpha-mixing blend does not.
    {
        std::uint32_t seed=29;auto next=[&]{seed=1664525*seed+1013904223;return seed>>8;};
        struct Fragment {unsigned alpha,color,z;bool covers;};
        struct Factors {int src,dst;bool admitted;}; // -1 = source alpha, -2 = 255 - source alpha
        const Factors factors[]{{-1,255,true},{255,255,true},{0,255,true},{-2,255,true},{-1,-2,false}};
        auto factor=[](int f,unsigned alpha){return f==-1?alpha:f==-2?255-alpha:unsigned(f);};
        unsigned differing[5]{};
        for(unsigned sample=0;sample<200000;++sample){
            const unsigned low=next()%200,high=low+1+next()%(255-low);
            std::array<Fragment,8> fragments;
            for(auto& f:fragments)f={next()&255,next()&255,next()%5,(next()&3)!=0};
            const unsigned depth=next()%5,start=next()&255;
            for(unsigned k=0;k<5;++k){
                const auto& b=factors[k];
                auto blend=[&](unsigned dst,const Fragment& f){
                    return std::min(255u,(f.color*factor(b.src,f.alpha)+dst*factor(b.dst,f.alpha))/255);};
                auto passes=[&](const Fragment& f,bool second){
                    return f.covers&&f.z<=depth&&(second?f.alpha>=high:f.alpha<=low);};
                unsigned perTriangle=start,wholeMesh=start;
                for(const auto& f:fragments)for(bool second:{false,true})if(passes(f,second))perTriangle=blend(perTriangle,f);
                for(bool second:{false,true})for(const auto& f:fragments)if(passes(f,second))wholeMesh=blend(wholeMesh,f);
                if(b.admitted)assert(perTriangle==wholeMesh);
                differing[k]+=perTriangle!=wholeMesh;
            }
        }
        assert(differing[4]>1000);
        std::printf("PASS alpha-union whole-mesh passes: 2 draws, same vertices; 800000 admitted blend samples identical to per-triangle order (alpha-mixing differs in %u of 200000)\n",differing[4]);
    }
    // Attribute and buffer configuration against Citro3D's copy (the
    // register checks in C3D_DrawElements are the oracle). Distinct resident
    // records: the layout is sent once, then each new vertex address as the
    // first buffer's three registers (4 command words instead of 40).
    {
        constexpr unsigned records=64;
        constexpr std::size_t recordBytes=4*sizeof(gx::CompactRigidVertexMT);
        auto* resident=static_cast<unsigned char*>(linearMemAlign(records*recordBytes,16));std::memset(resident,0,records*recordBytes);
        auto* residentIndices=static_cast<u16*>(linearMemAlign(64,16));for(unsigned i=0;i<3;++i)residentIndices[i]=u16(i);
        assert(!GSPGPU_FlushDataCache(resident,records*recordBytes)&&!GSPGPU_FlushDataCache(residentIndices,64));
        invalidateGxGeometryBindings();
        const auto before=gxBindingStatistics();const auto blocks=bufferBlocks,changes=attributeChanges,words=bufferWords;
        for(unsigned n=0;n<records;++n){
            const void* v=resident+n*recordBytes;
            assert(drawResidentRigid(v,residentIndices,3,n&1)&&vertices==v);
            // The same record again (a second pass): nothing is sent.
            assert(drawResidentRigid(v,residentIndices,3,n&1)&&vertices==v);
        }
        executeQueue();
        const auto after=gxBindingStatistics();
        assert(bufferBlocks==blocks+1&&attributeChanges==changes+1&&bufferWords-words==40+(records-1)*4);
        assert(after.buffersSent==before.buffersSent+1&&after.buffersFirstOnly==before.buffersFirstOnly+records-1&&after.buffersKept==before.buffersKept+records);
        assert(after.layoutsSent==before.layoutsSent+1&&after.layoutsKept==before.layoutsKept+2*records-1);
        // Clip-space and compact draws alternate: each change of layout is sent.
        auto clipBatch=make(3,5);assert(submitGxGeometry(clipBatch)&&vertices==clipBatch.vertices.data());
        assert(drawResidentRigid(resident,residentIndices,3,false)&&vertices==resident);
        assert(attributeChanges==changes+3);
        assert(flushGxGeometry());executeQueue();
        // Another writer of Citro3D's copies (two buffers, another layout) is
        // seen: the next draw sends complete blocks again.
        {auto* b=C3D_GetBufInfo();BufInfo_Init(b);assert(BufInfo_Add(b,resident,16,2,0x10)==0&&BufInfo_Add(b,residentIndices,8,1,0x2)==1);
         auto* a=C3D_GetAttrInfo();AttrInfo_Init(a);AttrInfo_AddLoader(a,0,GPU_FLOAT,4);AttrInfo_AddLoader(a,1,GPU_FLOAT,4);}
        const auto external=bufferBlocks;
        assert(drawResidentRigid(resident+recordBytes,residentIndices,3,false)&&vertices==resident+recordBytes);
        assert(bufferBlocks==external+1);
        // A frame begin or render-target change sends complete blocks once.
        invalidateGxGeometryBindings();const auto frameBlocks=bufferBlocks,frameChanges=attributeChanges;
        assert(drawResidentRigid(resident+recordBytes,residentIndices,3,false)&&bufferBlocks==frameBlocks+1&&attributeChanges==frameChanges+1);
        assert(drawResidentRigid(resident+recordBytes,residentIndices,3,false)&&bufferBlocks==frameBlocks+1&&attributeChanges==frameChanges+1);
        // An address outside linear memory is refused before anything is sent.
        const auto refusedWords=bufferWords;unsigned char outside[64]{};
        assert(!drawResidentRigid(outside,residentIndices,3,false)&&bufferWords==refusedWords);
        executeQueue();linearFree(resident);linearFree(residentIndices);
        std::printf("PASS attribute/buffer state against Citro3D's copy: %u resident records in %u buffer words (Alpha 6: %u), layouts sent once per change, other writers and frame begins resend\n",
            records,40+(records-1)*4,records*2*40);
    }
    // More than one arena of unique data exercises completion-before-recycle.
    const auto beforeRecycle=completions;
    for(unsigned n=0;n<250;++n){auto batch=make(128,float(n));assert(submitGxGeometry(batch));}
    assert(completions>beforeRecycle);assert(endGxGeometryFrame());executeQueue();
    assert(beginGxGeometryFrame());auto next=make(3,79);assert(submitGxGeometry(next));assert(endGxGeometryFrame());executeQueue();
    assert(beginGxGeometryFrame());casterSnapshots();
    destroyGxGeometry();assert(allocations.empty());
    std::printf("PASS actual geometry queue publication: %u draws, %u flush calls, %u arena fences; failed-flush retry, pending/cancelled exclusion, immutable queued bytes and frame reset\n",draws,flushes,completions);
}
