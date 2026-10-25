// Host fixture for actual CTRU readback and extracted production developer code.
#include "gx_framebuffer.hpp"
#include "gpu_sync.hpp"
#include "developer_script.hpp"
#include "../../Source/platforms/3ds/gx_geometry_pica.hpp"
#include <3ds.h>
#include <citro3d.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
using namespace dusk::native;
struct OSThread {} owner,foreign;
namespace {
std::array<u32,96000> depth{},color{};
std::array<u8,320*240*2> bottom{};
C3D_RenderTarget target;
std::map<const void*,std::pair<u32,std::size_t>> physical;
std::map<void*,std::size_t> live;
u32 nextPhysical=0x10000000,commandOffset=17;
u32 commands[64]{};
unsigned allocations{},frees{},copies{},waits{},invalidations{},flushes{},ticks{},bufferReads{};
void* destinationA{};bool failWait{},failAllocation{},wrongOwner{},aliasB{};
std::string mode;
struct Pending {void* destination{};const void* source{};std::size_t bytes{};} pending;
void mapRange(const void* p,std::size_t size){physical[p]={nextPhysical,size};nextPhysical+=0x100000;}
void script(const char* name,const char* contents){
    const auto path=std::string("sdmc:/3ds/dusklight/")+name;
    auto* f=std::fopen(path.c_str(),"wb");assert(f);assert(std::fputs(contents,f)>=0);assert(!std::fclose(f));
}
void pureObservation(RetainedDepthStage stage){
    const auto w=waits,c=copies,i=invalidations,f=flushes,a=allocations;
    observeDeveloperRetainedDepth(stage);
    assert(waits==w&&copies==c&&invalidations==i&&flushes==f&&allocations==a);
}
}
extern "C" OSThread* OSGetCurrentThread(){return wrongOwner?&foreign:&owner;}
extern "C" void OSReport(const char*,...){}
extern "C" void OSPanic(const char*,int,const char* message,...){throw std::runtime_error(message);}
std::uint64_t svcGetSystemTick(){return ++ticks;}
u32 osConvertVirtToPhys(const void* p){
    if(aliasB&&live.size()==2&&p!=destinationA&&live.contains(const_cast<void*>(p)))return physical.at(destinationA).first;
    return physical.at(p).first;
}
void GPUCMD_GetBuffer(u32** p,u32* size,u32* offset){++bufferReads;if(p)*p=commands;if(size)*size=64;if(offset)*offset=commandOffset;}
int FSUSER_GetSdmcArchiveResource(FS_ArchiveResource* s){s->clusterSize=4096;s->freeClusters=100000;return 0;}
void* linearAlloc(std::size_t size){
    ++allocations;assert(size<=retainedDepthProbeBytes);
    if(failAllocation&&allocations==2)return nullptr;
    auto* p=std::malloc(size);assert(p);std::memset(p,0x5a,size);live[p]=size;mapRange(p,size);return p;
}
void linearFree(void* p){assert(!pending.bytes);assert(live.erase(p)==1);physical.erase(p);++frees;std::free(p);}
int GSPGPU_InvalidateDataCache(void* p,std::size_t size){
    ++invalidations;assert(!pending.bytes&&live.at(p)>=size);
    if(mode=="invalidate-fail"&&p==destinationA&&invalidations>3)return -1;
    if(mode=="final-invalidate"&&p==destinationA&&invalidations>3)static_cast<u8*>(p)[12]^=1;
    return 0;
}
int GSPGPU_FlushDataCache(void*,std::size_t){++flushes;return 0;}
u8* gfxGetFramebuffer(int,int,u16*,u16*){return bottom.data();}
void gfxScreenSwapBuffers(int,bool){}
C3D_RenderTarget* C3D_RenderTargetCreate(unsigned,unsigned,int,int){return &target;}
bool C3D_FrameDrawOn(C3D_RenderTarget*){return true;}
void C3D_FrameSplit(unsigned){}
void C3D_RenderTargetClear(C3D_RenderTarget*,unsigned,u32,u32){}
void C3D_RenderTargetDetachOutput(C3D_RenderTarget*){}
void C3D_RenderTargetSetOutput(C3D_RenderTarget*,int,int,u32){}
void C3D_SyncDisplayTransfer(u32* source,u32 dim,u32* dest,u32,u32){assert(!pending.bytes);pending={dest,source,std::size_t(dim>>16)*(dim&0xffff)*4};}
void C3D_SyncTextureCopy(u32* source,u32,u32* dest,u32,std::size_t size,u32 flags){
    assert(!pending.bytes&&flags==8&&size==retainedDepthProbeBytes);++copies;
    if(copies==1)destinationA=dest;
    if(mode=="mismatch"&&copies==3)source[4]^=7;
    if(copies==3)assert(dest!=destinationA);
    pending={dest,source,size};
}
FramebufferHandle dusk::native::mainFramebuffer(){return &target;}
FramebufferHandle dusk::native::activeFramebuffer(){return &target;}
bool dusk::native::configureMainFramebuffer(unsigned,unsigned){return true;}
bool dusk::native::flushGxGeometry(){return true;}
OSThread* dusk::native::platformGpuOwner(){return &owner;}
GpuCompletion dusk::native::platformCompleteGpu(std::uint64_t timeout){
    ++waits;assert(timeout==500000000);
    if(wrongOwner)return GpuCompletion::wrongThread;
    if(pending.bytes){
        if(failWait&&copies==3)return GpuCompletion::timedOut;
        std::memcpy(pending.destination,pending.source,pending.bytes);pending={};
    }
    return GpuCompletion::complete;
}
bool dusk::native::sampleFramebufferDepthImage(const gx::TargetExtent&,std::span<const std::byte>,unsigned,unsigned,std::uint32_t&){return false;}
int main(int argc,char** argv){
    assert(argc==2);mode=argv[1];
    target.frameBuf={240,400,GPU_RB_RGBA8,GPU_RB_DEPTH24_STENCIL8,false,color.data(),depth.data()};
    mapRange(depth.data(),sizeof(depth));mapRange(color.data(),sizeof(color));mapRange(bottom.data(),bottom.size());
    for(unsigned i=0;i<depth.size();++i)depth[i]=0xa5000000|i;
    if(mode!="ordinary"){
        script("depth-audit-frames.txt","450\n451\n");script("depth-retained-frames.txt","450\n");script("exit-frame.txt","452\n");script("capture-frames.txt","450\n");
    }
    if(mode=="bad-config")script("depth-retained-frames.txt","450\n450\n");
    if(mode=="not-audit")script("depth-retained-frames.txt","449\n");
    if(mode=="at-exit")script("exit-frame.txt","450\n");
    if(mode=="overflow-config")script("depth-retained-frames.txt","450\n450\n450\n450\n450\n450\n450\n450\n450\n450\n450\n450\n450\n450\n450\n450\n450\n");
    if(mode=="bad-config"||mode=="not-audit"||mode=="at-exit"||mode=="overflow-config"){
        bool threw=false;try{loadDeveloperScripts();}catch(const std::runtime_error&){threw=true;}
        assert(threw&&!allocations&&!copies&&!waits);return 0;
    }
    loadDeveloperScripts();setDeveloperFrame(450);
    if(mode=="ordinary"){
        for(unsigned i=0;i<100;++i){captureDeveloperDepthBoundary(true);pureObservation(RetainedDepthStage::afterColor);finishDeveloperRetainedDepth(true);}
        assert(!allocations&&!copies&&!waits&&!invalidations&&!bufferReads&&!ticks);return 0;
    }
    assert(developerRetainedDepthDiagnosticFrame(450)&&developerRetainedDepthDiagnosticFrame(451)&&!developerRetainedDepthDiagnosticFrame(452));
    captureDeveloperDepthBoundary(false);
    for(auto& word:depth)word+=17;
    failWait=mode=="B-wait-fail";failAllocation=mode=="alloc-fail";aliasB=mode=="physical-alias";
    try{captureDeveloperDepthBoundary(true);}
    catch(const std::runtime_error&){
        assert(mode=="B-wait-fail"||mode=="alloc-fail"||mode=="physical-alias");
        assert(!frees);
        if(mode=="B-wait-fail"){
            assert(pending.bytes&&invalidations==2);assert(!platformReleaseRetainedDepth());
            std::span<const std::byte> span;
            const auto before=waits;assert(!platformReadFramebufferDepthImage(&target,span)&&span.empty()&&waits==before);
            failWait=false;assert(platformCompleteGpu(500000000)==GpuCompletion::complete);
        }
        aliasB=false;releaseFramebufferReadback();assert(live.empty());return 0;
    }
    assert(copies==3&&allocations==2&&live.size()==2);
    std::span<const std::byte> span;const auto before=waits;
    assert(!platformReadFramebufferDepthImage(&target,span)&&span.empty()&&waits==before);
    assert(captureMainFramebuffer(450));assert(allocations==3&&live.size()==3);
    if(mode=="mutation")static_cast<u8*>(destinationA)[16]^=4;
    pureObservation(RetainedDepthStage::afterColor);
    commandOffset=0;pureObservation(RetainedDepthStage::afterFrameEnd);
    pureObservation(RetainedDepthStage::beforeConsole);
    std::memset(bottom.data(),123,bottom.size());pureObservation(RetainedDepthStage::afterConsoleWrites);
    GSPGPU_FlushDataCache(bottom.data(),bottom.size());pureObservation(RetainedDepthStage::afterConsoleFlush);
    pureObservation(RetainedDepthStage::afterReporting);
    assert(live.size()==3&&!frees);setDeveloperFrame(451);
    const auto invalidateBefore=invalidations;
    try{finishDeveloperRetainedDepth(mode!="early-exit");}
    catch(const std::runtime_error&){assert(mode=="invalidate-fail");assert(live.size()==3&&!frees);releaseFramebufferReadback();return 0;}
    assert(invalidations==invalidateBefore+1&&live.size()==3&&!frees&&copies==3);
    if(mode=="invalidate-fail")assert(false);
    mode="stable"; // Disable final-invalidation fixture injection before new copies.
    // Releasing retention permits the next pre to overwrite A with fresh data;
    // it never aliases B and requires both original completion calls.
    for(auto& word:depth)word+=9;
    assert(platformReadFramebufferDepthImage(&target,span));assert(copies==4&&allocations==3);
    assert(!std::memcmp(span.data(),depth.data(),sizeof(depth)));
    // A second selected observation reuses the bounded B allocation safely.
    RetainedDepthProbe probe;DepthProbeCopy copy;
    assert(platformRetainDepthReadback(&target,span,probe));
    const auto heldA=probe.imageA.data();const auto heldB=probe.b.address;
    assert(!platformRetainDepthReadback(&target,span,probe));
    assert(probe.imageA.data()==heldA&&probe.b.address==heldB);
    assert(platformCaptureRetainedDepthB(probe,copy));assert(allocations==3);
    const auto waited=waits;
    assert(!platformCaptureRetainedDepthB(probe,copy)&&waits==waited&&copy.copyCompletion==0);
    assert(copy.priorCompletion==0&&copy.copyCompletion==0);
    wrongOwner=true;assert(platformInvalidateRetainedDepthA()<0&&!platformReleaseRetainedDepth());wrongOwner=false;
    assert(platformInvalidateRetainedDepthA()==0&&platformReleaseRetainedDepth());
    releaseFramebufferReadback();assert(live.empty()&&allocations==frees);
    std::puts("PASS retained ownership, fenced B, pure observations, explicit invalidation and fresh next capture");
}
