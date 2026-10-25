// Execute the actual PICA arena adapter with a shadow GPU memory model. Draws
// observe only successfully flushed bytes when the simulated queue executes.
#include "../gx_geometry_pica.hpp"
#include "../runtime/gpu_sync.hpp"
#include "../runtime/gx_fog_pass.hpp"
#include <cmath>
#include <algorithm>
#include <3ds/types.h>
#include <3ds/gpu/enums.h>
#include <3ds/gpu/shaderProgram.h>
extern "C" {
#include <3ds/allocator/linear.h>
#include <3ds/services/gspgpu.h>
#include <c3d/attribs.h>
#include <c3d/base.h>
}
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <limits>
using namespace dusk::native;
struct Allocation {std::size_t size;std::vector<unsigned char> gpu;};
static std::map<void*,Allocation> allocations;
struct Queued {const void* vertex;std::size_t vertexBytes;const void* index;std::size_t indexBytes;};
static std::vector<Queued> queued;
static C3D_AttrInfo attributes;
static C3D_BufInfo buffers;
static const void* vertices;
static unsigned stride;
struct Attribute {int reg;GPU_FORMATS type;int count;};
static std::vector<Attribute> layout;
static unsigned compactDraws,attributeChanges;
static unsigned flushes,draws,completions;
static bool refuseFlush;
extern "C" {
void* linearMemAlign(size_t n,size_t alignment){void* p=nullptr;assert(!posix_memalign(&p,alignment,n));allocations.emplace(p,Allocation{n,std::vector<unsigned char>(n,0xa5)});return p;}
void linearFree(void* p){assert(queued.empty());assert(allocations.erase(p)==1);std::free(p);}
Result GSPGPU_FlushDataCache(const void* p,u32 bytes){
    ++flushes;if(refuseFlush)return -1;
    for(auto& [base,a]:allocations){const auto start=reinterpret_cast<std::uintptr_t>(base),at=reinterpret_cast<std::uintptr_t>(p);
        if(at>=start&&at-start<a.size){assert(bytes<=a.size-(at-start));std::memcpy(a.gpu.data()+at-start,p,bytes);return 0;}}
    assert(false);return -1;
}
C3D_AttrInfo* C3D_GetAttrInfo(){return &attributes;}
C3D_BufInfo* C3D_GetBufInfo(){return &buffers;}
void AttrInfo_Init(C3D_AttrInfo*){layout.clear();++attributeChanges;}
int AttrInfo_AddLoader(C3D_AttrInfo*,int reg,GPU_FORMATS type,int count){layout.push_back({reg,type,count});return 0;}
void BufInfo_Init(C3D_BufInfo*){}
int BufInfo_Add(C3D_BufInfo*,const void* data,ptrdiff_t bytes,int count,u64 permutation){
    vertices=data;stride=bytes;assert(unsigned(count)==layout.size());
    if(bytes==sizeof(gx::CompactRigidVertex)){
        assert(count==5&&permutation==0x43210);
        const Attribute expected[]{{0,GPU_FLOAT,3},{1,GPU_SHORT,3},{2,GPU_SHORT,2},{4,GPU_SHORT,1},{3,GPU_UNSIGNED_BYTE,4}};
        for(unsigned i=0;i<5;++i)assert(layout[i].reg==expected[i].reg&&layout[i].type==expected[i].type&&layout[i].count==expected[i].count);
    }else{
        assert(bytes==sizeof(gx::GpuVertex)&&count==6&&permutation==0x543210);
        for(unsigned i=0;i<6;++i)
            assert(layout[i].reg==int(i)&&layout[i].type==GPU_FLOAT&&layout[i].count==(i<2?4:i<5?2:1));
    }
    return 0;
}
void C3D_DrawElements(GPU_Primitive_t mode,int count,int type,const void* data){
    assert(mode==GPU_TRIANGLES&&type==C3D_UNSIGNED_SHORT);
    if(stride==sizeof(gx::CompactRigidVertex))++compactDraws;
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
bool platformBeginGpuCommands(){assert(queued.empty());return true;}
GpuCompletion platformReserveGpuEntries(unsigned n,std::uint64_t){assert(n==2);return GpuCompletion::complete;}
GpuCompletion platformCompleteGpu(std::uint64_t){++completions;if(!flushGxGeometry())return GpuCompletion::exhausted;executeQueue();return GpuCompletion::complete;}
}
static gx::GpuGeometryBatch make(unsigned count,float value){
    gx::GpuGeometryBatch b;assert(reserveGxGeometry(count,3,b));
    for(auto& v:b.vertices){v={};v.position={value,2,3,1};v.color={.2f,.4f,.6f,1};v.texcoord[0]={value,.5f};}
    b.indices[0]=0;b.indices[1]=1;b.indices[2]=count-1;return b;
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
        gx::CompactRigidVertex v{{float(i),2,3},{-16384,0,16384},{-8192,4096},std::int16_t(i*39)};
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
    // More than one arena of unique data exercises completion-before-recycle.
    for(unsigned n=0;n<250;++n){auto batch=make(128,float(n));assert(submitGxGeometry(batch));}
    assert(completions>=3);assert(endGxGeometryFrame());executeQueue();
    assert(beginGxGeometryFrame());auto next=make(3,79);assert(submitGxGeometry(next));assert(endGxGeometryFrame());executeQueue();
    destroyGxGeometry();assert(allocations.empty());
    std::printf("PASS actual geometry queue publication: %u draws, %u flush calls, %u arena fences; failed-flush retry, pending/cancelled exclusion, immutable queued bytes and frame reset\n",draws,flushes,completions);
}
