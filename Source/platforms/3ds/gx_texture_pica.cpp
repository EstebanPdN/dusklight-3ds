#include "runtime/copy_lifecycle_probe.hpp"
#include "runtime/gx_copy_pixels.hpp"
#include "runtime/fused_color.hpp"
#include "gx_texture_pica.hpp"
#include "runtime/texture_gx.hpp"
#include "runtime/gpu_sync.hpp"
#include "runtime/gx_profile.hpp"
#include "runtime/clock.hpp"
#include <3ds/types.h>
#include <3ds/result.h>
extern "C" {
#include <3ds/services/gspgpu.h>
}
extern "C" OSThread* OSGetCurrentThread();
#include <3ds/gpu/enums.h>
extern "C" {
#include <c3d/texture.h>
}
#include <dolphin/os.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>

namespace dusk::native {
bool fusedColorCopyEnabled{};
namespace {
struct CachedTexture { gx::TextureHandle handle; C3D_Tex texture; NativeTexturePlan extent; bool dirty{},immutable{},gpuResident{};std::uint64_t used{}; };
std::array<CachedTexture,128> cache{};
unsigned cacheSlots=64;
bool rgb565Reference{};
std::array<NativeTextureFingerprint,64> halfAllowlist{};
std::size_t halfAllowlistSize{};
auto activeCache(){return std::span(cache.data(),cacheSlots);}
// Handle -> slot hint: draws look textures up by handle every frame; a verified
// hint avoids scanning every slot (a stale hint only falls back to the scan).
std::array<std::uint8_t,256> slotHint{};
CachedTexture* findCached(gx::TextureHandle handle){
    const unsigned key=unsigned(handle^(handle>>8)^(handle>>32))&255u;
    const unsigned hint=slotHint[key];
    if(hint<cacheSlots&&cache[hint].handle==handle)return &cache[hint];
    for(unsigned n=0;n<cacheSlots;++n)if(cache[n].handle==handle){slotHint[key]=std::uint8_t(n);return &cache[n];}
    return nullptr;
}
// Citro3D retains texture pointers until draw submission. Independent stable
// metadata prevents a later unit's filter from changing an earlier unit using
// the same pixel allocation. These snapshots never own/delete GPU memory.
std::array<C3D_Tex,3> samplers{};
// Last descriptor bound per unit. C3D_TexBind makes Citro3D re-send the unit and
// clear the PICA texture cache, so an identical re-bind is skipped. Any write
// into texture memory (decode, refresh) bumps contentSerial and forces a bind.
struct UnitBind {C3D_Tex desc;unsigned serial;bool valid;};
std::array<UnitBind,3> unitBinds{};
unsigned contentSerial=1;
std::uint64_t bindSkips,bindApplies;
// Diagnostic: the most recent decodes (what is re-decoded every frame).
struct DecodeEvent{std::uint64_t handle;unsigned w,h,fmt,bytes;bool dirty;std::uint64_t ticks;};
std::array<DecodeEvent,8> recentDecodes{};unsigned recentDecodeNext;
void noteDecode(const gx::Texture& input,const NativeTexturePlan& e,bool dirty,std::uint64_t ticks){
    recentDecodes[recentDecodeNext++&7]={input.handle,input.width,input.height,input.format,unsigned(e.outputBytes),dirty,ticks};
}
C3D_Tex idleTexture{};
std::array<gx::TextureHandle,3> protectedHandles{};
bool drawScope{};
std::uint64_t clock{};
std::size_t allocated;
std::size_t budget = 4 * 1024 * 1024;
const char* failure = "none";
bool reject(const char* why) { failure = why; return false; }
bool descriptorMatches(const gx::Texture& input,const NativeTexturePlan& plan){
    return plan.gxFormat==input.format&&plan.originalWidth==input.width&&
        plan.originalHeight==input.height&&plan.originalMipCount==input.mipCount;
}
// Texture category policy: images of room/stage scenery models are sampled
// from their authored mip 1 (half size per axis, a quarter of the memory).
// Characters, Link and UI keep full resolution. Keys are texel pointers.
std::array<const void*,2048> sceneryImages{};unsigned sceneryCount{};
unsigned sceneryHome(const void* p){return unsigned((reinterpret_cast<std::uintptr_t>(p)>>5)*2654435761u)&(sceneryImages.size()-1);}
int sceneryPolicy=-1; // SD opt-in until validated: sdmc:/3ds/DuskLight/texture-scenery.txt
bool sceneryImage(const void* p){
    if(sceneryPolicy<0){sceneryPolicy=0;if(auto* f=std::fopen("sdmc:/3ds/DuskLight/texture-scenery.txt","rb")){std::fclose(f);sceneryPolicy=1;}}
    if(!sceneryPolicy||!p||!sceneryCount)return false;
    for(unsigned n=0,h=sceneryHome(p);n<16;++n){const auto* v=sceneryImages[(h+n)&(sceneryImages.size()-1)];if(v==p)return true;if(!v)return false;}
    return false;
}
bool inspectForCache(const gx::Texture& input,NativeTexturePlan& plan){
    if(!inspectNativeTexture(input.bti,plan,rgb565Reference)||!descriptorMatches(input,plan))return false;
    const bool scenery=sceneryImage(input.image.data())||(sceneryCount&&sceneryImage(gx::textureAliasImage(input.handle)));
    if(scenery&&input.immutablePixels){
        NativeTexturePlan reduced;
        if(inspectNativeTexture(input.bti,reduced,rgb565Reference,true)||inspectNativeTextureBoxHalf(input.bti,reduced,rgb565Reference))plan=reduced;
        return true;
    }
    if(!input.immutablePixels||!halfAllowlistSize)return true;
    // Trust immutable registration for clean cache hits, like the existing
    // pixel cache. Revalidate content on explicit invalidation; no old borrowed
    // pointer survives in the cache. Hash only misses/refreshes, never per draw.
    if(const auto* found=findCached(input.handle);found&&!found->dirty&&found->immutable&&descriptorMatches(input,found->extent))
        {const auto& entry=*found;
            if(!entry.extent.sourceMip)return true;
            if(plan.gxFormat==4&&input.bti[1])return false;
            // Full structural inspection above validates the current borrowed
            // view. Rebase its offsets without rescanning/hash-reading pixels;
            // immutable content was proved on upload. Dirty aliases revalidate.
            const auto skipped=plan.sourceBytes-entry.extent.sourceBytes;
            plan.dataOffset+=skipped;plan.sourceBytes-=skipped;
            plan.width=entry.extent.width;plan.height=entry.extent.height;
            plan.paddedWidth=entry.extent.paddedWidth;plan.paddedHeight=entry.extent.paddedHeight;
            plan.outputBytes=entry.extent.outputBytes;plan.mipCount=entry.extent.mipCount;plan.sourceMip=1;
            return true;
        }
    NativeTexturePlan reduced;
    if(!inspectNativeTexture(input.bti,reduced,rgb565Reference,true))return true;
    NativeTextureFingerprint fingerprint;
    if(!nativeTextureFingerprint(input.bti,fingerprint))return false;
    for(std::size_t n=0;n<halfAllowlistSize;++n)if(halfAllowlist[n]==fingerprint){plan=reduced;break;}
    return true;
}
GPU_TEXCOLOR gpuFormat(NativeTextureFormat format) {
    switch(format) {
    case NativeTextureFormat::rgba8: return GPU_RGBA8;
    case NativeTextureFormat::la8: return GPU_LA8;
    case NativeTextureFormat::la4: return GPU_LA4;
    case NativeTextureFormat::rgb565: return GPU_RGB565;
    }
    return GPU_RGBA8;
}
bool protectedImage(gx::TextureHandle handle){
    for(const auto value:protectedHandles)if(value&&value==handle)return true;
    return false;
}
bool prepareIdleTexture(){
    if(idleTexture.data)return true;
    if(!C3D_TexInitWithParams(&idleTexture,nullptr,{8,8,0,GPU_RGBA8,GPU_TEX_2D,false}))return reject("GX idle texture allocation failed");
    // This fixed service image only replaces retired bindings of unused units.
    // Every consumed original image is protected and must bind its real data.
    std::memset(idleTexture.data,255,256);C3D_TexFlush(&idleTexture);
    C3D_TexSetFilter(&idleTexture,GPU_NEAREST,GPU_NEAREST);
    C3D_TexSetWrap(&idleTexture,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);return true;
}
bool retire(CachedTexture& entry,copyprobe::Reason reason=copyprobe::Reason::evict){
    ++gxProfileTotals.evictions;
    bool bound=false;for(const auto& sampler:samplers)bound|=sampler.data==entry.texture.data;
    if(bound&&!prepareIdleTexture())return false;
    for(unsigned unit=0;unit<samplers.size();++unit)if(samplers[unit].data==entry.texture.data){
        samplers[unit]=idleTexture;C3D_TexBind(unit,&samplers[unit]);unitBinds[unit].valid=false;
    }
    if(copyprobe::enabled)copyprobe::retire(entry.handle,reason);
    allocated-=entry.extent.outputBytes;C3D_TexDelete(&entry.texture);entry={};return true;
}
bool makeRoom(std::size_t bytes){
    if(bytes>budget)return reject("GX texture exceeds entire cache budget");
    unsigned free{};for(const auto& item:activeCache())free+=!item.handle;
    if(free&&bytes<=budget-allocated)return true;
    if(!drawScope)return reject(free?"GX texture cache budget exceeded":"GX texture cache slots exhausted");
    std::array<CachedTexture*,128> victims{};unsigned count{};std::size_t reclaim{};
    while((!free&&!count)||bytes>budget-allocated+reclaim){
        CachedTexture* oldest=nullptr;
        for(auto& item:activeCache())if(item.handle&&!protectedImage(item.handle)){
            bool selected=false;for(unsigned n=0;n<count;++n)selected|=victims[n]==&item;
            if(!selected&&(!oldest||item.used<oldest->used))oldest=&item;
        }
        if(!oldest)return reject("GX protected draw textures exceed cache capacity");
        victims[count++]=oldest;reclaim+=oldest->extent.outputBytes;
    }
    // Fence before deleting any allocation or changing borrowed sampler data.
    if(platformCompleteGpu(500000000)!=GpuCompletion::complete)return reject("GX texture eviction GPU completion failed");
    bool bound=false;for(unsigned n=0;n<count;++n)for(const auto& sampler:samplers)bound|=sampler.data==victims[n]->texture.data;
    if(bound&&!prepareIdleTexture())return false;
    for(unsigned n=0;n<count;++n)if(!retire(*victims[n]))return false;
    return true;
}
}
bool configureGxTextureCache(std::size_t bytes,unsigned images,bool rgb565Rgba8Reference,
                             std::span<const NativeTextureFingerprint> halfResolutionAllowlist,bool enableFusedColorCopy){
    if(!bytes||bytes>10*1024*1024||!images||images>cache.size()||allocated||drawScope||idleTexture.data||halfResolutionAllowlist.size()>halfAllowlist.size())
        return reject("GX cache configuration requires empty bounded cache");
    // Validate everything before publishing policy; caller storage may expire.
    halfAllowlist={};std::copy(halfResolutionAllowlist.begin(),halfResolutionAllowlist.end(),halfAllowlist.begin());
    halfAllowlistSize=halfResolutionAllowlist.size();
    budget=bytes;cacheSlots=images;rgb565Reference=rgb565Rgba8Reference;
    fusedColorCopyEnabled=enableFusedColorCopy;return true;
}
GxTextureDrawScope::GxTextureDrawScope(std::span<const gx::Texture> textures){
    if(drawScope||textures.size()>protectedHandles.size()){reject("GX texture draw scope invalid/reentrant");return;}
    std::array<gx::TextureHandle,3> handles{};std::size_t total{};
    for(unsigned n=0;n<textures.size();++n){
        const auto& texture=textures[n];
        if(!texture.handle){reject("GX protected texture descriptor invalid");return;}
        // A clean resident image is bound from its cached extent without
        // re-inspection (bindGxTexture's hit path), so its size is known.
        std::size_t bytes;
        if(const auto* entry=findCached(texture.handle);entry&&!entry->dirty&&entry->immutable==texture.immutablePixels&&descriptorMatches(texture,entry->extent))
            bytes=entry->extent.outputBytes;
        else{NativeTexturePlan plan;if(!inspectForCache(texture,plan)){reject("GX protected texture descriptor invalid");return;}bytes=plan.outputBytes;}
        bool duplicate=false;for(unsigned j=0;j<n;++j)duplicate|=handles[j]==texture.handle;
        if(!duplicate){if(bytes>budget-total){reject("GX draw texture working set exceeds cache budget");return;}total+=bytes;}
        handles[n]=texture.handle;
    }
    protectedHandles=handles;drawScope=true;active=true;
}
GxTextureDrawScope::~GxTextureDrawScope(){if(active){protectedHandles={};drawScope=false;}}
bool bindGxTexture(const gx::Texture& input, float& scaleU, float& scaleV,unsigned unit,bool projective) {
    GxProfileScope profile(GxProfilePart::texture);
    if(unit>=samplers.size())return reject("PICA texture unit out of range");
    if(projective&&unit!=0)return reject("PICA projected texture needs unit zero");
    if (!input.handle || input.wrapS>2 || input.wrapT>2 || input.minFilter > 5 || input.magFilter > 1)
        return reject("GX texture sampler unsupported");
    if(!input.mipCount||input.minLod>input.maxLod||input.maxLod>=input.mipCount||!std::isfinite(input.lodBias)||input.lodBias < -4||input.lodBias>=4)return reject("GX texture LOD range invalid");
    CachedTexture* entry = nullptr;
    if(drawScope&&!protectedImage(input.handle))return reject("GX binding is outside protected draw textures");
    entry=findCached(input.handle);
    auto probeReason=copyprobe::Reason::none;
    if(copyprobe::enabled){
        probeReason=!entry?copyprobe::Reason::miss:entry->dirty?copyprobe::Reason::dirty:copyprobe::Reason::hit;
        copyprobe::bindAttempt(input.handle,probeReason);
    }
    if (!entry) {
        NativeTexturePlan e;
        const bool valid = projective?(inspectNativeTexture(input.bti,e,rgb565Reference)&&descriptorMatches(input,e)):inspectForCache(input,e);
        if (!valid)
            return reject("GX texture descriptor mismatch");
        if(projective&&e.sourceMip)return reject("GX texture half policy forbids projected content");
        if((input.wrapS&&e.width!=e.paddedWidth)||(input.wrapT&&e.height!=e.paddedHeight))return reject("GX repeated padded texture unsupported");
        if(!makeRoom(e.outputBytes))return false;
        for (auto& item : activeCache()) if (!item.handle) { entry = &item; break; }
        if(!entry)return reject("GX texture eviction did not provide a slot");
        if (!C3D_TexInitWithParams(&entry->texture,nullptr,{u16(e.paddedWidth),u16(e.paddedHeight),u8(e.mipCount-1),gpuFormat(e.format),GPU_TEX_2D,false}))
            return reject("GX texture GPU allocation failed");
        auto& gpu = entry->texture;
        auto bytes = std::span(static_cast<std::uint8_t*>(gpu.data), std::size_t(C3D_TexCalcTotalSize(gpu.size,gpu.maxLevel)));
        bool decoded;
        std::uint64_t decodeTicks;
        {GxProfileScope decoding(GxProfilePart::decode);gxProfileTotals.decodedBytes+=e.outputBytes;const auto t0=gxProfileTick();
        decoded = C3D_TexCalcTotalSize(gpu.size,gpu.maxLevel) == e.outputBytes && decodeNativeTexture(input.bti, e, bytes, true);decodeTicks=gxProfileTick()-t0;}
        if (!decoded) { C3D_TexDelete(&gpu); return reject("GX texture decode failed"); }
        if(copyprobe::enabled)copyprobe::decode(input.handle,probeReason);
        noteDecode(input,e,false,decodeTicks);
        C3D_TexFlush(&gpu);++contentSerial;
        if(copyprobe::enabled)copyprobe::flushUnknown(input.handle);
        C3D_TexSetWrap(&gpu, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        entry->extent = e;
        entry->handle = input.handle;
        entry->immutable=input.immutablePixels;
        allocated += e.outputBytes;
    }
    if(!descriptorMatches(input,entry->extent)||entry->immutable!=input.immutablePixels)
        return reject("GX cached texture descriptor mismatch");
    if(projective&&entry->extent.sourceMip)return reject("GX texture half policy forbids projected content");
    if((input.wrapS&&entry->extent.width!=entry->extent.paddedWidth)||(input.wrapT&&entry->extent.height!=entry->extent.paddedHeight))return reject("GX repeated padded texture unsupported");
    if (entry->dirty) {
        // GX invalidation already completed every preceding GPU use. Borrow
        // the current binding's live resource, never retain/dereference an old
        // archive pointer while invalidating an unused cache entry.
        NativeTexturePlan current;
        if (!inspectForCache(input,current) || current.outputBytes != entry->extent.outputBytes||
            current.sourceMip!=entry->extent.sourceMip||current.format!=entry->extent.format)
            return reject("GX invalidated texture descriptor mismatch");
        auto& gpu = entry->texture;
        const auto bytes = std::size_t(C3D_TexCalcTotalSize(gpu.size, gpu.maxLevel));
        bool decoded;
        std::uint64_t decodeTicks;
        {GxProfileScope decoding(GxProfilePart::decode);gxProfileTotals.decodedBytes+=current.outputBytes;const auto t0=gxProfileTick();
        decoded=bytes==current.outputBytes&&decodeNativeTexture(input.bti,current,{static_cast<std::uint8_t*>(gpu.data),bytes},true);decodeTicks=gxProfileTick()-t0;}
        if (!decoded)
            return reject("GX invalidated texture decode failed");
        if(copyprobe::enabled)copyprobe::decode(input.handle,probeReason);
        noteDecode(input,current,true,decodeTicks);
        C3D_TexFlush(&gpu);++contentSerial;
        if(copyprobe::enabled)copyprobe::flushUnknown(input.handle);
        entry->dirty = false;
    }
    C3D_Tex sampler=entry->texture;
    sampler.param=(sampler.param&~GPU_TEXTURE_MODE(7))|GPU_TEXTURE_MODE(projective?GPU_TEX_PROJECTION:GPU_TEX_2D);
    const bool minLinear=input.minFilter==1||input.minFilter==3||input.minFilter==5;
    C3D_TexSetFilter(&sampler,input.magFilter?GPU_LINEAR:GPU_NEAREST,minLinear?GPU_LINEAR:GPU_NEAREST);
    C3D_TexSetFilterMipmap(&sampler,input.minFilter>=4?GPU_LINEAR:GPU_NEAREST);
    const auto base=entry->extent.sourceMip;
    sampler.minLevel=input.minLod>base?input.minLod-base:0;
    sampler.maxLevel=input.minFilter<2?0:input.maxLod>base?input.maxLod-base:0;
    // Halving the GPU dimensions already changes derivative-based lambda by
    // -1. Keep bias unchanged; rebase only explicit min/max LOD by sourceMip.
    C3D_TexSetLodBias(&sampler,input.lodBias);
    const GPU_TEXTURE_WRAP_PARAM wraps[]{GPU_CLAMP_TO_EDGE,GPU_REPEAT,GPU_MIRRORED_REPEAT};
    C3D_TexSetWrap(&sampler,wraps[input.wrapS],wraps[input.wrapT]);
    auto& last=unitBinds[unit];
    if(last.valid&&last.serial==contentSerial&&!std::memcmp(&last.desc,&sampler,sizeof(sampler))){
        ++bindSkips; // identical descriptor already bound to this unit
    }else{
        samplers[unit]=sampler;C3D_TexBind(unit,&samplers[unit]);
        last.desc=sampler;last.serial=contentSerial;last.valid=true;++bindApplies;
    }
    if(copyprobe::enabled)copyprobe::bindSuccess(input.handle,probeReason);
    entry->used=++clock;
    scaleU = float(entry->extent.width) / entry->extent.paddedWidth;
    scaleV = float(entry->extent.height) / entry->extent.paddedHeight;
    return true;
}
FusedColorCopyResult refreshResidentColorCopy(gx::TextureHandle handle,const ColorCopyPlan& p,
    std::span<const std::byte> pixels,std::span<std::uint8_t> bti){
    using Result=FusedColorCopyResult;
    if(!fusedColorCopyEnabled||drawScope||protectedImage(handle))return Result::skipped;
    CachedTexture* entry=nullptr;
    for(auto& item:activeCache())if(item.handle==handle){entry=&item;break;}
    if(!entry||entry->immutable||bti.size()<32)return Result::skipped;
    gx::Texture input;std::uint32_t token{};NativeTexturePlan plan;
    if(!gx::textureCommandToken(bti.data()+32,token)||!gx::textureFromCommandToken(token,input)||
       input.handle!=handle||input.immutablePixels||input.bti.data()!=bti.data()||input.bti.size()!=bti.size()||
       !inspectForCache(input,plan)||plan!=entry->extent||plan.sourceMip||plan.mipCount!=1||plan.paletteEntries||
       plan.gxFormat>6||plan.gxFormat!=p.format||plan.width!=p.width||plan.height!=p.height||plan.sourceBytes!=p.bytes)
        return Result::skipped;
    auto& gpu=entry->texture;
    const auto bytes=std::size_t(C3D_TexCalcTotalSize(gpu.size,gpu.maxLevel));
    if(!gpu.data||bytes!=plan.outputBytes||gpu.maxLevel||gpu.width!=plan.paddedWidth||
       gpu.height!=plan.paddedHeight||gpu.fmt!=gpuFormat(plan.format))return Result::skipped;
    // Successful platformReadFramebufferColor already drained geometry and the
    // transfer. Recheck the real queue (no wait/new completion ticket) before
    // overwriting any existing allocation. Failure stays fatal at GXCopyTex.
    if(!platformGpuOwner()||platformGpuOwner()!=OSGetCurrentThread()||
       platformQueryGpu()!=GpuCompletion::complete){reject("GX fused copy completion/owner failed");return Result::failed;}
    void* const data=gpu.data;
    {GxProfileScope conversion(GxProfilePart::fusedConvert);
     if(!private_fused::encode(p,pixels,bti,plan,{static_cast<std::uint8_t*>(data),bytes}))return Result::skipped;}
    // Keep exactly the old encode -> per-image invalidate order. The pure
    // kernel completes both stores without callbacks/allocations/late failure.
    if(!invalidateGxTexture(handle)){reject("GX fused copy invalidation failed");return Result::failed;}
    // All cache allocations use onVram=false, i.e. linearAlloc in Citro3D.
    // This is C3D_TexFlush's exact underlying operation/size, checking BOTH
    // libctru's IPC transport and service Result instead of discarding it.
    {GxProfileScope checkedFlush(GxProfilePart::fusedCheckedFlush);
     if(R_FAILED(GSPGPU_FlushDataCache(data,static_cast<u32>(bytes)))){
        reject("GX fused copy checked cache flush failed");return Result::failed;
     }}
    // No user callback/reentry or owner transfer is supported in this interval.
    // Reacquire current registration/metadata; never publish a saved entry under
    // a replaced ID, changed view, native layout or allocation.
    gx::Texture current;NativeTexturePlan verified;CachedTexture* found=nullptr;
    for(auto& item:activeCache())if(item.handle==handle){found=&item;break;}
    if(found!=entry||entry->immutable||!entry->dirty||entry->extent!=plan||entry->texture.data!=data||
       entry->texture.maxLevel||entry->texture.width!=plan.paddedWidth||entry->texture.height!=plan.paddedHeight||
       entry->texture.fmt!=gpuFormat(plan.format)||
       std::size_t(C3D_TexCalcTotalSize(entry->texture.size,entry->texture.maxLevel))!=bytes||
       !gx::textureFromCommandToken(token,current)||current.handle!=handle||current.immutablePixels||
       current.bti.data()!=bti.data()||current.bti.size()!=bti.size()||
       !inspectForCache(current,verified)||verified!=plan){
        reject("GX fused copy publication metadata changed");return Result::failed;
    }
    entry->dirty=false;++contentSerial;
    return Result::published;
}
void observeCopyLifecycleResidency(gx::TextureHandle handle,const ColorCopyPlan& p,bool depth,bool indexed,std::size_t snapshotBytes){
    if(!copyprobe::enabled)return;
    const CachedTexture* found=nullptr;
    for(const auto& item:activeCache())if(item.handle==handle){found=&item;break;}
    auto reason=copyprobe::Reason::eligible;
    if(depth)reason=copyprobe::Reason::depth;
    else if(indexed)reason=copyprobe::Reason::palette;
    else if(!found)reason=copyprobe::Reason::notResident;
    else if(drawScope||protectedImage(handle))reason=copyprobe::Reason::protectedDraw;
    else {const auto& e=found->extent;
        if(!found->texture.data||C3D_TexCalcTotalSize(found->texture.size,found->texture.maxLevel)!=e.outputBytes||found->immutable||e.sourceMip||e.mipCount!=1||e.gxFormat>6||e.gxFormat!=p.format||
           e.width!=p.width||e.height!=p.height||e.sourceBytes!=p.bytes)
            reason=copyprobe::Reason::layout;
    }
    copyprobe::copy(handle,reason,found!=nullptr,found&&found->dirty,
                    found?found->extent.outputBytes:0,snapshotBytes);
}
const char* gxTextureFailure() { return failure; }
}
extern "C" void duskNativeSceneryTexture(const void* image,bool add){
    using namespace dusk::native;
    if(!image)return;
    const unsigned mask=sceneryImages.size()-1;
    for(unsigned n=0,h=sceneryHome(image);n<16;++n){
        auto& slot=sceneryImages[(h+n)&mask];
        if(add&&(!slot||slot==image)){if(!slot)++sceneryCount;slot=image;return;}
        if(!add&&slot==image){
            // Backward-shift deletion keeps every probe chain intact.
            unsigned hole=(h+n)&mask;slot=nullptr;--sceneryCount;
            for(unsigned k=1;k<16;++k){
                const unsigned at=(hole+k)&mask;const void* v=sceneryImages[at];if(!v)break;
                const unsigned home=sceneryHome(v);
                if(((at-home)&mask)>=((at-hole)&mask)){sceneryImages[hole]=v;sceneryImages[at]=nullptr;hole=at;k=0;}
            }
            return;
        }
        if(!slot)return;
    }
}
namespace dusk::native {
void* gpuCaptureTexture(gx::TextureHandle handle,unsigned& width,unsigned& height){
    auto* entry=findCached(handle);
    if(!entry||entry->immutable||drawScope||protectedImage(handle)||entry->extent.format!=NativeTextureFormat::rgba8||
       entry->extent.sourceMip||entry->extent.mipCount!=1||!entry->texture.data)return nullptr;
    width=entry->extent.width;height=entry->extent.height;return &entry->texture;
}
bool gpuCaptureRead(gx::TextureHandle handle,std::span<std::uint8_t> out,unsigned paddedWidth,unsigned paddedHeight){
    auto* entry=findCached(handle);
    if(!entry||entry->texture.width!=paddedWidth||entry->texture.height!=paddedHeight||out.size()>entry->extent.outputBytes)return false;
    GSPGPU_InvalidateDataCache(entry->texture.data,out.size());
    std::memcpy(out.data(),entry->texture.data,out.size());return true;
}
void gpuCaptureCommit(gx::TextureHandle handle){
    if(auto* entry=findCached(handle)){entry->dirty=false;entry->gpuResident=true;entry->used=++clock;}
    ++contentSerial;invalidateGxTextureBindings();
}
void invalidateGxTextureBindings(){for(auto& b:unitBinds)b.valid=false;}
void gxTextureDecodeReport(){
    for(unsigned n=0;n<recentDecodes.size();++n){const auto& d=recentDecodes[n];
        if(d.handle)OSReport("Native recent decode handle=%llu %ux%u gxfmt=%u picabytes=%u %s %.3f ms\n",(unsigned long long)d.handle,d.w,d.h,d.fmt,d.bytes,d.dirty?"dirty":"new",d.ticks*1000.0/dusk::native::nativeTickFrequency);}
    recentDecodes={};
}
void gxTextureBindStatistics(std::uint64_t& skips,std::uint64_t& applies){skips=bindSkips;applies=bindApplies;}
std::size_t gxTextureBytes() { return allocated; }
std::size_t gxTextureIdleBytes(){return idleTexture.data?256:0;}
GxTextureHalfStatus gxTextureHalfStatus(){
    GxTextureHalfStatus result{halfAllowlistSize+sceneryCount};
    for(const auto& item:activeCache())if(item.handle&&item.extent.sourceMip){
        const auto& plan=item.extent;
        // Half admission requires power-of-two extents and sourceMip=1, so
        // the omitted GPU base has no padding. CMPR is decoded to PICA RGBA8:
        // use actual output BPP, never its GX compressed source block size.
        const std::size_t baseBytes=std::size_t(plan.originalWidth)*plan.originalHeight*
            nativeTextureBytesPerPixel(plan.format);
        ++result.residentImages;
        result.residentBytes+=plan.outputBytes;
        result.sameImagesFullBytes+=plan.outputBytes+baseBytes;
    }
    return result;
}
void invalidateGxTextures() {
    if(copyprobe::enabled)copyprobe::globalInvalidate();
    // GPU-resident captures own their pixels on the GPU; the CPU bytes are stale.
    for (auto& item : cache) if (item.handle&&!item.immutable&&!item.gpuResident) item.dirty = true;
}
bool invalidateGxTexture(gx::TextureHandle handle){
    if(copyprobe::enabled)copyprobe::imageInvalidate(handle);
    for(auto& item:cache)if(item.handle==handle){item.dirty=true;item.gpuResident=false;return true;}
    return true; // A registered image may not have reached a GPU binding yet.
}
bool releaseGxTexture(gx::TextureHandle handle){
    for(auto& item:cache)if(item.handle==handle){
        if(protectedImage(handle))return reject("GX texture release during protected draw");
        // The caller already drained old uses. Repair every retired sampler
        // with a real stable service allocation before deleting this image.
        return retire(item,copyprobe::Reason::release);
    }
    return true;
}
void destroyGxTextures() {
    unitBinds={};bindSkips=bindApplies=0;
    samplers={}; // Caller has finished Citro3D; no future context/draw borrows.
    for (auto& item : cache) {
        if (item.handle) {if(copyprobe::enabled)copyprobe::retire(item.handle,copyprobe::Reason::shutdown);C3D_TexDelete(&item.texture);}
        item = {};
    }
    if(idleTexture.data)C3D_TexDelete(&idleTexture);
    idleTexture={};
    protectedHandles={};drawScope=false;clock=0;
    allocated = 0;
    fusedColorCopyEnabled=false;
    budget=4*1024*1024;cacheSlots=64;rgb565Reference=false;halfAllowlist={};halfAllowlistSize=0;
}
}
