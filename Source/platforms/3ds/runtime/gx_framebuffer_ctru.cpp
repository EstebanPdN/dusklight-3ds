#include "gx_framebuffer.hpp"
#include "alpha_interval_target.hpp"
#include "bootstrap.hpp"
#include "../gx_pica.hpp"
#include "gpu_sync.hpp"
#include "../gx_geometry_pica.hpp"
#include "../gx_texture_pica.hpp"
#include "video_mode.hpp"
#include "developer_script.hpp"
#include "ctru_abi.hpp"
#include <3ds.h>
#include <citro3d.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
extern "C" OSThread* OSGetCurrentThread();
extern "C" void OSReport(const char*,...);
namespace dusk::native {
namespace {
bool alphaOwner(){return platformGpuOwner()&&platformGpuOwner()==OSGetCurrentThread();}
bool storageSnapshot(FramebufferHandle handle,AlphaIntervalStorage& out){
    if(!handle)return false;
    const auto* t=static_cast<const C3D_RenderTarget*>(handle);const auto& fb=*abi::frameBuf(handle);
    if(!abi::ownsColorBuffer(t)||!abi::ownsDepthBuffer(t)||!fb.colorBuf||!fb.depthBuf||abi::isBlock32(&fb)||
       abi::colorFormat(&fb)!=GPU_RB_RGBA8||abi::depthFormat(&fb)!=GPU_RB_DEPTH24_STENCIL8||abi::colorWriteMask(&fb)!=15||abi::depthWriteMask(&fb)!=3)return false;
    out={{fb.colorBuf,fb.depthBuf,fb.width,fb.height},osConvertVirtToPhys(fb.colorBuf),osConvertVirtToPhys(fb.depthBuf)};
    return true;
}
bool trackedTarget(FramebufferHandle handle){
    if(!alphaIntervalTargets.enabled)return true;
    AlphaIntervalStorage storage;
    return alphaOwner()&&storageSnapshot(handle,storage)&&alphaIntervalTargets.matches(handle,storage);
}
bool ownsAlphaIntervalTarget(const gx::AlphaIntervalTarget& target,void* user){
    // PURE: no CopySafe query here (the backend temporarily owns live masks),
    // C3D getter (marks state dirty), queue calls, state writes or allocation.
    if(user!=&alphaIntervalTargets||!alphaOwner())return false;
    const auto handle=activeFramebuffer();AlphaIntervalStorage storage;
    const auto extent=gx::currentTargetExtent;
    return extent.valid()&&target.width==extent.pixelHeight&&target.height==extent.pixelWidth&&
        storageSnapshot(handle,storage)&&storage.target==target&&
        alphaIntervalTargets.owns(handle,storage,OSGetCurrentThread());
}
}
bool configureFramebufferAlphaIntervals(bool enabled,gx::AlphaIntervalPolicy& policy){
    policy={};
    if(!alphaIntervalCopySafe()||activeFramebuffer()||offscreenFramebufferBytes()||alphaIntervalTargets.enabled)return false;
    if(!enabled)return true;
    AlphaIntervalStorage storage;
    if(!alphaOwner()||!storageSnapshot(mainFramebuffer(),storage)||!alphaIntervalTargets.add(mainFramebuffer(),storage))return false;
    alphaIntervalTargets.owner=OSGetCurrentThread();alphaIntervalTargets.enabled=true;
    policy={true,ownsAlphaIntervalTarget,&alphaIntervalTargets};return true;
}
namespace {void* readback{};std::size_t readbackSize{};}
namespace {
// The title scene leaves about 700 KiB of linear heap; its first EFB readback
// needs about 1 MiB. Cached images are rebuilt on their next use.
void* allocateLinearReleasingTextures(std::size_t bytes){
    void* memory=linearAlloc(bytes);
    for(bool fenced=false;!memory&&dusk::native::releaseLeastNeededTexture(fenced);)memory=linearAlloc(bytes);
    return memory;
}
}
namespace {void* depthReadback{};std::size_t depthReadbackSize{};}
namespace {void* retainedDepthB{};bool depthRetained{},retainedCopyComplete{},retainedCopyAttempted{};}
namespace {bool requestedBlack{},presentedBlack{};std::uint64_t retainedTargetGeneration{};}
namespace {
bool retainedGenerationMatches(FramebufferHandle handle){
    const auto* r=alphaIntervalTargets.find(handle);
    return retainedTargetGeneration?(r&&r->generation==retainedTargetGeneration):!alphaIntervalTargets.enabled;
}
}
bool platformCommitVideoMode(unsigned width,unsigned height,bool black){
    if(!configureMainFramebuffer(width,height))return false;
    requestedBlack=black;return true;
}
void updateVideoPresentation(){
    if(!alphaIntervalCopySafe())fatal("Alpha interval unsafe presentation");
    if(requestedBlack==presentedBlack)return;
    auto* target=static_cast<C3D_RenderTarget*>(mainFramebuffer());if(!target)return;
    if(requestedBlack){
        // Not C3D_RenderTargetDetachOutput: that SDK inline reads target->screen
        // and ->side, whose offsets differ with -fno-short-enums (ctru_abi.hpp).
        // The main target is the only one linked, to the top screen's left eye.
        C3D_RenderTargetSetOutput(nullptr,GFX_TOP,GFX_LEFT,0);
        u16 width,height;auto* top=gfxGetFramebuffer(GFX_TOP,GFX_LEFT,&width,&height);
        // Top LCD remains RGB8; diagnostic lower LCD is unaffected.
        std::memset(top,0,std::size_t(width)*height*3);
        GSPGPU_FlushDataCache(top,std::size_t(width)*height*3);gfxScreenSwapBuffers(GFX_TOP,false);
    }else{
        constexpr u32 flags=GX_TRANSFER_FLIP_VERT(0)|GX_TRANSFER_OUT_TILED(0)|GX_TRANSFER_RAW_COPY(0)|
            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)|GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8)|GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
        C3D_RenderTargetSetOutput(target,GFX_TOP,GFX_LEFT,flags);
    }
    presentedBlack=requestedBlack;
}
FramebufferHandle platformCreateFramebuffer(unsigned width,unsigned height){
    if(!alphaIntervalCopySafe()||!width||!height||width>1024||height>1024||(width&7)||(height&7)||
       (alphaIntervalTargets.enabled&&(!alphaOwner()||!alphaIntervalTargets.canRegister())))return nullptr;
    auto* target=C3D_RenderTargetCreate(height,width,GPU_RB_RGBA8,GPU_RB_DEPTH24_STENCIL8);
    if(target&&alphaIntervalTargets.enabled){
        AlphaIntervalStorage storage;
        if(!storageSnapshot(target,storage)||!alphaIntervalTargets.add(target,storage))return nullptr;
    }
    return target;
}
bool platformBindFramebuffer(FramebufferHandle handle){
    if(!alphaIntervalCopySafe()||!handle||!trackedTarget(handle)||!C3D_FrameDrawOn(static_cast<C3D_RenderTarget*>(handle)))return false;
    invalidatePicaCommandState();
    if(alphaIntervalTargets.enabled){
        // Outside the synchronous interval backend this is the sole canonical
        // stencil state: disabled, no writes, KEEP for every outcome.
        C3D_StencilTest(false,GPU_ALWAYS,0,0xff,0);
        C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_KEEP);
        alphaIntervalTargets.didBind(handle);
    }
    return true;
}
bool platformClearFramebuffer(FramebufferHandle handle,std::uint32_t color,std::uint32_t depth){
    if(!alphaIntervalCopySafe()||!handle||!trackedTarget(handle))return false;
    // Memory fills share the GSP queue. Submit previous PICA draws first so
    // reusing a target cannot clear it ahead of draws still in the CPU list.
    if(!flushGxGeometry()||!alphaIntervalCopySafe()||!trackedTarget(handle))return false;
    if(platformReserveGpuEntries(2,500000000)!=GpuCompletion::complete)return false;
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    C3D_RenderTargetClear(static_cast<C3D_RenderTarget*>(handle),C3D_CLEAR_ALL,color,depth);
    if(alphaIntervalTargets.enabled)alphaIntervalTargets.didClear(handle,depth);
    return true;
}
namespace {
// Read-backs: the CPU reads the finished render target straight from VRAM
// (the process maps it read-only: RSF MemoryMapping 1f000000-1f5fffff:r).
// On a New 3DS the first transfer-engine job after the minimap's 216x216
// tiled-to-linear DisplayTransfer read-back never completed, in every session
// that entered the game (Alpha 0 and Alpha 2); a read-back needs no transfer
// job at all. SD switches for A/B: readback-transfer.txt restores the transfer
// engine, readback-check.txt runs both and logs whether they agree.
int readbackMode=-1; // 0 CPU, 1 transfer engine, 2 both and compared
int readbackModeNow(){
    if(readbackMode<0){
        readbackMode=0;
        if(auto* f=std::fopen("sdmc:/3ds/DuskLight/readback-transfer.txt","rb")){std::fclose(f);readbackMode=1;}
        if(auto* f=std::fopen("sdmc:/3ds/DuskLight/readback-check.txt","rb")){std::fclose(f);readbackMode=2;}
        OSReport("Native framebuffer read-back: %s\n",readbackMode==0?"CPU reads VRAM (no transfer job)":
            readbackMode==1?"transfer engine (SD switch)":"CPU and transfer engine, compared (SD switch)");
    }
    return readbackMode;
}
bool inVram(const void* pointer,std::size_t bytes){
#if defined(__3DS__)
    const auto at=reinterpret_cast<std::uintptr_t>(pointer);
    return pointer&&bytes&&at>=OS_VRAM_VADDR&&at+bytes<=std::uintptr_t(OS_VRAM_VADDR)+OS_VRAM_SIZE;
#else
    return pointer&&bytes;
#endif
}
// The GPU finished writing: drop any CPU cache lines of the range (the VRAM
// mapping is cacheable; libctru flushes VRAM framebuffers the same way).
void invalidateVram(const void* pointer,std::size_t bytes){
    static bool reported;
    if(R_FAILED(GSPGPU_InvalidateDataCache(const_cast<void*>(pointer),bytes))&&!reported){
        reported=true;OSReport("Native framebuffer read-back: VRAM cache invalidate refused\n");
    }
}
// Tiled PICA RGBA8 to linear rows, exactly as the tiled-to-linear
// DisplayTransfer (no flip, no scaling) writes them: pixel (x,y) of an 8x8
// tile is word Morton(x,y) of the tile; tiles run along a row of the buffer.
void detileColor(const u32* tiled,u32* linear,unsigned width,unsigned height){
    static std::array<std::uint8_t,64> tileX{},tileY{};
    if(!tileX[1])for(unsigned i=0;i<64;++i){
        tileX[i]=std::uint8_t((i&1)|((i>>1)&2)|((i>>2)&4));tileY[i]=std::uint8_t(((i>>1)&1)|((i>>2)&2)|((i>>3)&4));
    }
    alignas(8) u32 tile[64];
    for(unsigned ty=0;ty<height/8;++ty)for(unsigned tx=0;tx<width/8;++tx){
        std::memcpy(tile,tiled+(std::size_t(ty)*(width/8)+tx)*64,sizeof(tile)); // sequential VRAM read
        u32* out=linear+std::size_t(ty)*8*width+std::size_t(tx)*8;
        for(unsigned i=0;i<64;++i)out[std::size_t(tileY[i])*width+tileX[i]]=tile[i];
    }
}
// The old path: tiled-to-linear DisplayTransfer into a linear buffer.
bool transferColor(const C3D_FrameBuf& fb,void* destination,std::size_t size){
    const u32 dim=GX_BUFFER_DIM(fb.width,fb.height);
    const u32 flags=GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)|GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8)|
                    GX_TRANSFER_OUT_TILED(0)|GX_TRANSFER_FLIP_VERT(0)|GX_TRANSFER_RAW_COPY(0)|GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    if(platformReserveGpuEntries(2,500000000)!=GpuCompletion::complete)return false;
    C3D_SyncDisplayTransfer(static_cast<u32*>(fb.colorBuf),dim,static_cast<u32*>(destination),dim,flags);
    // In an open Citro3D frame, SyncDisplayTransfer only queues the copy.
    if(platformCompleteGpu(500000000)!=GpuCompletion::complete)return false; // read-back transfer fence
    return R_SUCCEEDED(GSPGPU_InvalidateDataCache(destination,size));
}
}
bool platformReadFramebufferColor(FramebufferHandle handle,std::span<const std::byte>& out){
    out={};if(!alphaIntervalCopySafe()||!handle||!trackedTarget(handle)||platformCompleteGpu(500000000)!=GpuCompletion::complete||!alphaIntervalCopySafe()||!trackedTarget(handle))return false;
    const auto& fb=*abi::frameBuf(handle);
    if(abi::colorFormat(&fb)!=GPU_RB_RGBA8||abi::isBlock32(&fb)||(fb.width&7)||(fb.height&7))return false;
    const std::size_t size=std::size_t(fb.width)*fb.height*4;
    if(size>4*1024*1024)return false;
    if(size>readbackSize){
        // Grown once; a full linear heap releases cached images instead of failing.
        if(readback)linearFree(readback);readback=nullptr;readbackSize=0;
        readback=allocateLinearReleasingTextures(size);if(!readback)return false;readbackSize=size;
    }
    const int mode=readbackModeNow();
    if(mode==1){
        if(!transferColor(fb,readback,size)||!alphaIntervalCopySafe()||!trackedTarget(handle))return false;
    }else{
        if(!inVram(fb.colorBuf,size))return false;
        invalidateVram(fb.colorBuf,size);
        detileColor(static_cast<const u32*>(fb.colorBuf),static_cast<u32*>(readback),fb.width,fb.height);
        if(mode==2){
            // Up to four comparisons per target size (the minimap is 216x216).
            static std::array<std::uint32_t,8> sizes{};static std::array<unsigned,8> counts{};static unsigned checks;
            unsigned slot=0;const std::uint32_t key=(std::uint32_t(fb.width)<<16)|fb.height;
            while(slot<sizes.size()&&sizes[slot]&&sizes[slot]!=key)++slot;
            if(slot<sizes.size()&&counts[slot]<4){
                sizes[slot]=key;++counts[slot];++checks;
                if(void* reference=linearAlloc(size)){
                    const bool copied=transferColor(fb,reference,size);
                    OSReport("Native framebuffer read-back check %u: %ux%u CPU and transfer %s\n",checks,unsigned(fb.width),unsigned(fb.height),
                        !copied?"(transfer failed)":std::memcmp(reference,readback,size)?"DIFFER":"match");
                    linearFree(reference);
                }
            }
        }
    }
    out={static_cast<const std::byte*>(readback),size};return true;
}
bool platformReadFramebufferDepthImage(FramebufferHandle handle,std::span<const std::byte>& out){
    out={};
    if(depthRetained)return false; // Never overwrite retained A, even for peeks.
    if(!alphaIntervalCopySafe()||!handle||!trackedTarget(handle)||platformCompleteGpu(500000000)!=GpuCompletion::complete||!alphaIntervalCopySafe()||!trackedTarget(handle))return false;
    const auto extent=gx::currentTargetExtent;
    if(!extent.valid())return false;
    const auto& fb=*abi::frameBuf(handle);
    if(abi::isBlock32(&fb)||!fb.depthBuf||abi::depthFormat(&fb)!=GPU_RB_DEPTH24_STENCIL8||fb.width!=extent.pixelHeight||fb.height!=extent.pixelWidth)return false;
    const auto size=std::size_t(fb.width)*fb.height*4;
    if(size>4*1024*1024)return false;
    if(size>depthReadbackSize){
        if(depthReadback)linearFree(depthReadback);depthReadback=nullptr;depthReadbackSize=0;
        depthReadback=allocateLinearReleasingTextures(size);if(!depthReadback)return false;depthReadbackSize=size;
    }
    if(readbackModeNow()==1){
        if(platformReserveGpuEntries(2,500000000)!=GpuCompletion::complete)return false;
        C3D_SyncTextureCopy(static_cast<u32*>(fb.depthBuf),0,static_cast<u32*>(depthReadback),0,size,8);
        if(platformCompleteGpu(500000000)!=GpuCompletion::complete||!alphaIntervalCopySafe()||!trackedTarget(handle))return false; // readback transfer fence
        if(R_FAILED(GSPGPU_InvalidateDataCache(depthReadback,size)))return false;
    }else{
        // Raw tiled words, as the TextureCopy produced them (see the colour read-back).
        if(!inVram(fb.depthBuf,size))return false;
        invalidateVram(fb.depthBuf,size);
        std::memcpy(depthReadback,fb.depthBuf,size);
    }
    out={static_cast<const std::byte*>(depthReadback),size};return true;
}
bool platformReadFramebufferDepthView(FramebufferHandle handle,std::span<const std::byte>& out){
    out={};
    if(readbackModeNow()==1)return platformReadFramebufferDepthImage(handle,out);
    if(!alphaIntervalCopySafe()||!handle||!trackedTarget(handle)||platformCompleteGpu(500000000)!=GpuCompletion::complete||!alphaIntervalCopySafe()||!trackedTarget(handle))return false;
    const auto extent=gx::currentTargetExtent;
    if(!extent.valid())return false;
    const auto& fb=*abi::frameBuf(handle);
    if(abi::isBlock32(&fb)||!fb.depthBuf||abi::depthFormat(&fb)!=GPU_RB_DEPTH24_STENCIL8||fb.width!=extent.pixelHeight||fb.height!=extent.pixelWidth)return false;
    const auto size=std::size_t(fb.width)*fb.height*4;
    if(!inVram(fb.depthBuf,size))return false;
    invalidateVram(fb.depthBuf,size);
    out={static_cast<const std::byte*>(fb.depthBuf),size};return true;
}
DepthProbeState platformDepthProbeState(){
    DepthProbeState state;state.tick=svcGetSystemTick();
    u32* commands{};GPUCMD_GetBuffer(&commands,&state.commandWords,&state.commandOffset);
    state.commands=reinterpret_cast<std::uintptr_t>(commands);
    state.caller=reinterpret_cast<std::uintptr_t>(OSGetCurrentThread());
    state.owner=reinterpret_cast<std::uintptr_t>(platformGpuOwner());return state;
}
namespace {
DepthProbeRange probeRange(const void* pointer,std::size_t bytes,std::size_t capacity){
    return {reinterpret_cast<std::uintptr_t>(pointer),osConvertVirtToPhys(pointer),bytes,capacity};
}
bool probeDisjoint(const DepthProbeRange& a,const DepthProbeRange& b){
    const auto apart=[](std::uintptr_t x,std::size_t n,std::uintptr_t y,std::size_t m){
        return x<=y?n<=y-x:m<=x-y;
    };
    return a.address&&a.physical&&b.address&&b.physical&&
        apart(a.address,a.capacity,b.address,b.capacity)&&apart(a.physical,a.capacity,b.physical,b.capacity);
}
}
bool platformRetainDepthReadback(FramebufferHandle handle,std::span<const std::byte> image,RetainedDepthProbe& out){
    if(!alphaIntervalCopySafe())return false;
    if(depthRetained)return false; // Preserve the active caller's record on reentry.
    out={};
    if(!handle||!trackedTarget(handle)||handle!=mainFramebuffer()||!platformGpuOwner()||platformGpuOwner()!=OSGetCurrentThread()||
       image.data()!=depthReadback||image.size()!=retainedDepthProbeBytes||depthReadbackSize!=retainedDepthProbeBytes)return false;
    const auto& fb=*abi::frameBuf(handle);
    if(fb.width!=240||fb.height!=400||abi::isBlock32(&fb)||!fb.depthBuf||!fb.colorBuf||
       abi::depthFormat(&fb)!=GPU_RB_DEPTH24_STENCIL8||abi::colorFormat(&fb)!=GPU_RB_RGBA8)return false;
    if(!retainedDepthB){retainedDepthB=linearAlloc(retainedDepthProbeBytes);if(!retainedDepthB)return false;}
    out.target=handle;out.a=probeRange(depthReadback,image.size(),depthReadbackSize);
    out.b=probeRange(retainedDepthB,image.size(),retainedDepthProbeBytes);
    out.depth=probeRange(fb.depthBuf,image.size(),image.size());out.color=probeRange(fb.colorBuf,image.size(),image.size());
    out.bottom=probeRange(gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,nullptr,nullptr),320*240*2,320*240*2);
    if(!probeDisjoint(out.a,out.b)||!probeDisjoint(out.a,out.depth)||!probeDisjoint(out.b,out.depth)||
       !probeDisjoint(out.a,out.color)||!probeDisjoint(out.b,out.color)||
       !probeDisjoint(out.a,out.bottom)||!probeDisjoint(out.b,out.bottom)){out={};return false;}
    const auto* record=alphaIntervalTargets.find(handle);retainedTargetGeneration=record?record->generation:0;
    out.targetGeneration=retainedTargetGeneration;
    out.imageA=image;depthRetained=true;retainedCopyComplete=retainedCopyAttempted=false;return true;
}
bool platformCaptureRetainedDepthB(RetainedDepthProbe& probe,DepthProbeCopy& copy){
    if(!alphaIntervalCopySafe()||!depthRetained||retainedCopyAttempted||probe.imageA.data()!=depthReadback||
       probe.b.address!=reinterpret_cast<std::uintptr_t>(retainedDepthB)||
       probe.target!=mainFramebuffer()||platformGpuOwner()!=OSGetCurrentThread())return false;
    if(probe.targetGeneration!=retainedTargetGeneration||!retainedGenerationMatches(probe.target)||!trackedTarget(probe.target)||probe.depth.address!=reinterpret_cast<std::uintptr_t>(static_cast<C3D_RenderTarget*>(probe.target)->frameBuf.depthBuf))return false;
    copy={};
    retainedCopyAttempted=true;
    copy.beforePrior=platformDepthProbeState();
    const auto prior=platformCompleteGpu(500000000);copy.priorCompletion=int(prior);
    copy.beforeCopy=platformDepthProbeState();if(prior!=GpuCompletion::complete||!alphaIntervalCopySafe()||!retainedGenerationMatches(probe.target)||!trackedTarget(probe.target))return false;
    if(platformReserveGpuEntries(2,500000000)!=GpuCompletion::complete)return false;
    C3D_SyncTextureCopy(reinterpret_cast<u32*>(probe.depth.address),0,static_cast<u32*>(retainedDepthB),0,retainedDepthProbeBytes,8);
    copy.afterCopy=platformDepthProbeState();
    const auto complete=platformCompleteGpu(500000000);copy.copyCompletion=int(complete);
    copy.afterWait=platformDepthProbeState();if(complete!=GpuCompletion::complete||!alphaIntervalCopySafe()||!retainedGenerationMatches(probe.target)||!trackedTarget(probe.target))return false;
    copy.invalidateResult=GSPGPU_InvalidateDataCache(retainedDepthB,retainedDepthProbeBytes);
    if(R_FAILED(copy.invalidateResult))return false;
    retainedCopyComplete=true;probe.imageB={static_cast<const std::byte*>(retainedDepthB),retainedDepthProbeBytes};return true;
}
int platformInvalidateRetainedDepthA(){
    if(!alphaIntervalCopySafe()||!depthRetained||!retainedCopyComplete||!retainedGenerationMatches(mainFramebuffer())||platformGpuOwner()!=OSGetCurrentThread())return -1;
    return GSPGPU_InvalidateDataCache(depthReadback,retainedDepthProbeBytes);
}
bool platformReleaseRetainedDepth(){
    if(!alphaIntervalCopySafe()||!depthRetained||!retainedCopyComplete||!retainedGenerationMatches(mainFramebuffer())||platformGpuOwner()!=OSGetCurrentThread())return false;
    depthRetained=false;retainedCopyComplete=false;retainedTargetGeneration=0;return true;
}
bool platformReadFramebufferDepth(FramebufferHandle handle,unsigned x,unsigned y,std::uint32_t& out){
    const auto extent=gx::currentTargetExtent;
    if(!extent.valid()||x>=extent.logicalWidth||y>=extent.logicalHeight)return false;
    std::span<const std::byte> snapshot;
    if(!platformReadFramebufferDepthView(handle,snapshot))return false;
    return sampleFramebufferDepthImage(extent,snapshot,x,y,out);
}
bool captureMainFramebuffer(std::uint64_t frame){
    auto* target=static_cast<C3D_RenderTarget*>(mainFramebuffer());
    std::span<const std::byte> pixels;
    if(!target||!platformReadFramebufferColor(target,pixels))return false;
    mkdir("sdmc:/3ds/dusklight/captures",0777);
    char path[80];std::snprintf(path,sizeof(path),"sdmc:/3ds/dusklight/captures/frame-%06llu.rgba",static_cast<unsigned long long>(frame));
    auto* file=std::fopen(path,"wb");if(!file)return false;
    // Untiled GPU RGBA8 rows exactly as presented; the host rotates them to LCD orientation.
    const std::uint32_t header[4]={0x43534b44,0x31305041,target->frameBuf.width,target->frameBuf.height};
    bool okay=std::fwrite(header,1,sizeof(header),file)==sizeof(header)&&std::fwrite(pixels.data(),1,pixels.size(),file)==pixels.size();
    if(std::fclose(file)!=0)okay=false;
    return okay;
}
void releaseFramebufferReadback(){
    if(readback)linearFree(readback);if(depthReadback)linearFree(depthReadback);
    if(retainedDepthB)linearFree(retainedDepthB);retainedDepthB=nullptr;depthRetained=retainedCopyComplete=retainedCopyAttempted=false;
    readback=depthReadback=nullptr;readbackSize=depthReadbackSize=0;retainedTargetGeneration=0;
}
}
