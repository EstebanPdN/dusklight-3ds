#include "../runtime/gx_framebuffer.hpp"
#include "../runtime/gx_stream.hpp"
#include "../runtime/gpu_sync.hpp"
#include "../runtime/gx_copy_pixels.hpp"
#include "../gx_geometry_pica.hpp"
#include "gx_copy_reference.hpp"
#include "framebuffer_depth_order_hooks.hpp"
#include <citro3d.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>
using namespace dusk::native;
using namespace dusk::native::gx;
namespace {
struct Surface {
    std::vector<u32> color,depth;
    C3D_RenderTarget target;
    Surface(unsigned width,unsigned height):color(width*height),depth(width*height){
        target.frameBuf={height,width,GPU_RB_RGBA8,GPU_RB_DEPTH24_STENCIL8,false,color.data(),depth.data()};
    }
};
std::vector<std::unique_ptr<Surface>> surfaces;
unsigned clears{},copies{},completions{},flushes{},splits{},presents{};
bool failFlush{},failBind{},failAlloc{},failInvalidate{};
bool failTransferWait{};
struct PendingCopy {void* destination{};const void* source{};std::size_t bytes{};} pendingCopy;
GpuCompletion completion=GpuCompletion::complete;
C3D_RenderTarget* bound{};
std::vector<const char*> events;
void present(){assert(bound);++presents;events.push_back("present");}
unsigned interleave(unsigned x,unsigned y){
    // Independent multiplication masks, rather than production's bit loop.
    x=(x|(x<<2))&0x13;x=(x|(x<<1))&0x15;
    y=(y|(y<<2))&0x13;y=(y|(y<<1))&0x15;
    return x|(y<<1);
}
u32 sceneDepth(unsigned generation,unsigned x,unsigned y){return (generation*0x12345+x*193+y*1031)&0xffffff;}
void drawScene(unsigned generation){
    assert(bound);events.push_back("draw");
    auto& fb=bound->frameBuf;auto* data=static_cast<u32*>(fb.depthBuf);
    for(unsigned y=0;y<fb.width;++y)for(unsigned x=0;x<fb.height;++x){
        const unsigned px=fb.width-1-y,py=x;
        const auto at=(std::size_t(py/8)*(fb.width/8)+px/8)*64+interleave(px%8,py%8);
        data[at]=0xa5000000|(0xffffff-sceneDepth(generation,x,y));
    }
}
u32 expected(unsigned generation,unsigned x,unsigned y){
    const auto e=currentTargetExtent;
    return sceneDepth(generation,std::uint64_t(x)*e.pixelWidth/e.logicalWidth,
                      std::uint64_t(y)*e.pixelHeight/e.logicalHeight);
}
template<class F>void rejected(F action){bool failed=false;try{action();}catch(const std::runtime_error&){failed=true;}assert(failed);}
void drain(unsigned generation,unsigned count){
    dDlst_peekZ_c queue;std::array<u32,64> values{};
    for(unsigned i=0;i<count;++i){
        queue.mEntries[i]={s16(i*37%608),s16(i*61%448),&values[i]};
    }
    queue.mCount=count;const auto before=copies;
    queue.peekData();assert(queue.mCount==0&&copies==before+(count?1:0));
    for(unsigned i=0;i<count;++i)assert(values[i]==expected(generation,queue.mEntries[i].x,queue.mEntries[i].y));
}
void currentDepthCopy(unsigned generation){
    std::span<const std::byte> snapshot;const auto before=copies;
    assert(platformReadFramebufferDepthImage(activeFramebuffer(),snapshot)&&copies==before+1);
    ColorCopyPlan plan;const auto e=currentTargetExtent;
    assert(planDepthCopy(e,0,0,e.logicalWidth,e.logicalHeight,7,5,0x13,plan));
    std::vector<std::uint8_t> actual(plan.bytes),reference(plan.bytes);
    assert(encodeDepthCopy(plan,snapshot,actual));assert(referenceEncodeDepthCopy(plan,snapshot,reference));assert(actual==reference);
    u32 sample;assert(sampleFramebufferDepthImage(e,snapshot,300,200,sample)&&sample==expected(generation,300,200));
}
}
extern "C" void OSPanic(const char*,int,const char* message,...){throw std::runtime_error(message);}
bool dusk::native::flushGxGeometry(){++flushes;return !failFlush;}
GpuCompletion dusk::native::platformReserveGpuEntries(unsigned n,std::uint64_t){assert(n==2);return completion;}
GpuCompletion dusk::native::platformCompleteGpu(std::uint64_t){
    ++completions;
    if(completion!=GpuCompletion::complete)return completion;
    if(pendingCopy.bytes){
        if(failTransferWait)return GpuCompletion::timedOut;
        std::memcpy(pendingCopy.destination,pendingCopy.source,pendingCopy.bytes);pendingCopy={};
    }
    return completion;
}
void* linearAlloc(std::size_t size){return failAlloc?nullptr:std::malloc(size);}
void linearFree(void* pointer){std::free(pointer);}
int GSPGPU_InvalidateDataCache(void*,std::size_t){assert(!pendingCopy.bytes);return failInvalidate?-1:0;}
int GSPGPU_FlushDataCache(void*,std::size_t){return 0;}
u8* gfxGetFramebuffer(int,int,u16*,u16*){std::abort();}
void gfxScreenSwapBuffers(int,bool){std::abort();}
C3D_RenderTarget* C3D_RenderTargetCreate(unsigned height,unsigned width,int,int){
    surfaces.push_back(std::make_unique<Surface>(width,height));return &surfaces.back()->target;
}
bool C3D_FrameDrawOn(C3D_RenderTarget* target){if(failBind)return false;bound=target;return true;}
void C3D_FrameSplit(unsigned){++splits;events.push_back("split");}
void C3D_RenderTargetClear(C3D_RenderTarget* target,unsigned,u32 color,u32 depth){
    ++clears;events.push_back("clear");auto& fb=target->frameBuf;
    std::fill_n(static_cast<u32*>(fb.colorBuf),fb.width*fb.height,color);
    std::fill_n(static_cast<u32*>(fb.depthBuf),fb.width*fb.height,depth);
}
void C3D_RenderTargetDetachOutput(C3D_RenderTarget*){}
void C3D_RenderTargetSetOutput(C3D_RenderTarget*,int,int,u32){}
void C3D_SyncDisplayTransfer(u32* source,u32 dim,u32* dest,u32,u32){assert(!pendingCopy.bytes);pendingCopy={dest,source,std::size_t(dim>>16)*(dim&0xffff)*4};}
void C3D_SyncTextureCopy(u32* source,u32,u32* dest,u32,std::size_t bytes,u32){++copies;events.push_back("snapshot");assert(!pendingCopy.bytes);pendingCopy={dest,source,bytes};}
int main(){
    CommandMemory memory;CommandProcessor processor(memory);std::array<u8,256> scratch{};
    CommandStream stream(processor,scratch);assert(setCommandStream(&stream));
    JFWDisplay display;Surface main(16,8);
    assert(!beginFramebufferRender()&&!clearFramebufferForRender(1,2));
    assert(initializeFramebuffers(&main.target,16,8)&&configureMainFramebuffer(608,448));
    assert(beginFramebufferFrame());drawScene(1);
    std::span<const std::byte> colorSnapshot;
    main.color[0]=0x12345678;assert(platformReadFramebufferColor(activeFramebuffer(),colorSnapshot));
    u32 firstColor{};std::memcpy(&firstColor,colorSnapshot.data(),4);assert(firstColor==main.color[0]);
    drain(1,0);drain(1,1);drain(1,64);
    // Queue the display-copy clear, present the old scene, then start frame 2.
    assert(deferMainFramebufferClear(0xdeadbeef,0));assert(endFramebufferFrame());present();
    const auto firstClears=clears;events.clear();
    assert(beginFramebufferFrame());assert(clears==firstClears&&presents==1);
    drain(1,64); // Actual production queue body must see frame 1 depth.
    display.clearEfb(0,0,608,448,{1,2,3,4});
    assert(clears==firstClears+1&&main.color[0]==0x01020304&&main.depth[0]==0);
    assert(events.size()==3&&std::strcmp(events[0],"snapshot")==0&&std::strcmp(events[1],"split")==0&&std::strcmp(events[2],"clear")==0);
    assert(beginFramebufferRender()&&beginFramebufferRender()&&clears==firstClears+1);
    u32 value=0;auto before=copies;
    GXPeekZ(17,23,&value);assert(value==0xffffff&&copies==before+1);
    drawScene(2);drain(2,64);currentDepthCopy(2);
    // Generic GXPeekZ always reads fresh, including multiple new draws.
    before=copies;GXPeekZ(500,300,&value);assert(value==expected(2,500,300));
    drawScene(3);GXPeekZ(500,300,&value);assert(value==expected(3,500,300)&&copies==before+2);
    currentDepthCopy(3);
    // Every batch acquires a fresh snapshot after an intervening explicit clear.
    assert(platformClearFramebuffer(activeFramebuffer(),0,0x654321));
    FramebufferDepthQuery query{1,2,&value};peekFramebufferDepthBatch({&query,1});assert(value==0x9abcde);
    drawScene(4);drain(4,64);
    // Offscreen clear/bind/restore never retains a batch's borrowed image.
    assert(beginOffscreenFramebuffer(16,8));drawScene(5);
    GXPeekZ(3,4,&value);assert(value==expected(5,3,4));
    assert(!beginFramebufferRender()&&!clearFramebufferForRender(7,8));
    assert(restoreMainFramebuffer());drain(4,64);currentDepthCopy(4);
    // A failed clear retains the request and retries once at the render boundary.
    assert(deferMainFramebufferClear(0x11223344,0x123456));
    failFlush=true;before=clears;assert(!beginFramebufferRender()&&clears==before);failFlush=false;
    assert(beginFramebufferRender()&&clears==before+1&&main.depth[0]==0x123456);
    assert(beginFramebufferRender()&&clears==before+1);
    assert(deferMainFramebufferClear(0,0));failFlush=true;
    rejected([&]{display.clearEfb(0,0,608,448,{8,7,6,5});});assert(clears==before+1);failFlush=false;
    assert(beginFramebufferRender()&&clears==before+2&&main.color[0]==0x08070605);
    // Failed frame bind must not consume a clear; peeks remain old until render.
    drawScene(6);assert(deferMainFramebufferClear(0,0));assert(endFramebufferFrame());present();
    failBind=true;assert(!beginFramebufferFrame());failBind=false;
    assert(beginFramebufferFrame());drain(6,64);assert(beginFramebufferRender());
    drawScene(7);
    // No outputs are published on count, coordinate, command or GPU failures.
    value=0xbadbad;before=copies;std::array<FramebufferDepthQuery,65> tooMany{};
    rejected([&]{peekFramebufferDepthBatch(tooMany);});
    FramebufferDepthQuery invalid{608,1,&value};rejected([&]{peekFramebufferDepthBatch({&invalid,1});});
    assert(copies==before&&value==0xbadbad);
    FramebufferDepthQuery null{65535,65535,nullptr};peekFramebufferDepthBatch({&null,1});GXPeekZ(65535,65535,nullptr);assert(copies==before);
    const u8 partial=0x61;assert(stream.write({&partial,1}));
    rejected([&]{peekFramebufferDepthBatch({&query,1});});
    rejected([&]{display.clearEfb(0,0,608,448,{0,0,0,0});});assert(copies==before&&value==0xbadbad);assert(stream.reset());
    for(auto error:{GpuCompletion::notReady,GpuCompletion::wrongThread,GpuCompletion::timedOut,GpuCompletion::exhausted}){
        completion=error;rejected([&]{peekFramebufferDepthBatch({&query,1});});
        rejected([&]{GXPeekZ(1,2,&value);});assert(copies==before&&value==0xbadbad);
    }
    completion=GpuCompletion::complete;
    releaseFramebufferReadback();failAlloc=true;rejected([&]{peekFramebufferDepthBatch({&query,1});});failAlloc=false;assert(copies==before&&value==0xbadbad);
    failInvalidate=true;rejected([&]{peekFramebufferDepthBatch({&query,1});});failInvalidate=false;assert(copies==before+1&&value==0xbadbad);
    peekFramebufferDepthBatch({&query,1});assert(value==expected(7,1,2));
    // A copy can fail AFTER successful draw completion. Neither a borrowed
    // span nor a game depth output may be exposed until that transfer finishes.
    value=0xbadbad;failTransferWait=true;
    rejected([&]{peekFramebufferDepthBatch({&query,1});});assert(value==0xbadbad&&pendingCopy.bytes);
    failTransferWait=false;assert(platformCompleteGpu(1)==GpuCompletion::complete);
    colorSnapshot={reinterpret_cast<const std::byte*>(main.color.data()),4};failTransferWait=true;
    assert(!platformReadFramebufferColor(activeFramebuffer(),colorSnapshot)&&colorSnapshot.empty()&&pendingCopy.bytes);
    failTransferWait=false;assert(platformCompleteGpu(1)==GpuCompletion::complete);
    const auto good=currentTargetExtent;currentTargetExtent.pixelWidth=15;
    value=0xbadbad;rejected([&]{peekFramebufferDepthBatch({&query,1});});assert(value==0xbadbad);currentTargetExtent=good;
    std::span<const std::byte> snapshot;assert(platformReadFramebufferDepthImage(activeFramebuffer(),snapshot));
    assert(!sampleFramebufferDepthImage(good,snapshot.first(snapshot.size()-1),1,2,value));
    rejected([&]{display.clearEfb(1,0,607,448,{0,0,0,0});});
    assert(endFramebufferFrame());before=copies;rejected([&]{peekFramebufferDepthBatch({&query,1});});assert(copies==before);
    releaseFramebufferRecords();releaseFramebufferReadback();assert(setCommandStream(nullptr));
    std::printf("PASS: real framebuffer lifecycle/backend + production peek/JFW clear bodies; previous depth before clear, one/64 queries=one snapshot, fresh generic peeks/depth copies, target/clear transitions and failures (%u snapshots)\n",copies);
}
