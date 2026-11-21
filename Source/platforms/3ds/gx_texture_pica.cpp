#include "runtime/gx_state_serial.hpp"
#include "runtime/screen_tape.hpp"
#include "runtime/copy_lifecycle_probe.hpp"
#include "runtime/gx_copy_pixels.hpp"
#include "runtime/fused_color.hpp"
#include "gx_texture_pica.hpp"
#include "runtime/texture_gx.hpp"
#include "runtime/gpu_sync.hpp"
#include "runtime/gx_profile.hpp"
#include "runtime/gx_texture_commands.hpp"
#include "runtime/clock.hpp"
#include "runtime/perf_features.hpp"
#include <3ds/types.h>
#include <3ds/result.h>
extern "C" {
#include <3ds/services/gspgpu.h>
#include <3ds/allocator/linear.h>
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
#include <utility>

namespace dusk::native {
bool fusedColorCopyEnabled{};
namespace {
struct CachedTexture { gx::TextureHandle handle; C3D_Tex texture; NativeTexturePlan extent; bool dirty{},immutable{},gpuResident{};
    // In VRAM and rendered by the PICA itself (see gpuCaptureVramTexture):
    // never decoded from CPU bytes and never evicted for linear memory.
    bool vram{};
    std::uint64_t used{};std::uint32_t frame{}; // frame of the last bind
    bool worldUse{}; // bound by a world-model use (worldMips) at least once
    // Checksum of the source bytes of the last decode (mutable images only).
    // The per-frame global invalidation marks every game-written image dirty;
    // an unchanged source keeps its decoded pixels instead of a new decode
    // and GPU cache flush. Cleared whenever the GPU copy stops mirroring the
    // CPU bytes (explicit image invalidation, GPU capture, fused copy).
    bool summed{};std::uint64_t sourceSum{};
    // Ping-pong storage of a dynamic image (see redecodeTarget): the same
    // descriptor at another address. Frame of the last bind of that storage,
    // fence epoch when it left service, and the last frame it was needed.
    C3D_Tex spare{};std::uint32_t spareFrame{},spareEpoch{},spareNeeded{};
    // Frames of the latest changed re-decode and of one in an earlier frame.
    std::uint32_t redecodeFrame{},earlierRedecodeFrame{};bool redecoded{},redecodedEarlier{}; };
// 256 slots since Alpha 12 (128 before). With the byte budget far from full,
// the slot count was what forced evictions, each a whole-GPU wait (console
// Alpha 11: 3,153 waits, 12.2 s; D_MN09A r50 168 per 300 pictures).
std::array<CachedTexture,256> cache{};
unsigned cacheSlots=64;
bool rgb565Reference{};
// 16-bit storage for immutable images that hold the same picture in it
// (texture_gx.hpp compact); SD switch texture-compact-off.txt keeps RGBA8.
bool compactTextures{true};
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
// How a changed re-decode kept its CPU write away from pixels queued draws
// may still read (redecodeTarget).
enum class RedecodePath:std::uint8_t{none,unfenced,idle,spare,fenced};
const char* redecodePathName(RedecodePath path){
    switch(path){
    case RedecodePath::unfenced:return "unfenced";
    case RedecodePath::idle:return "idle";
    case RedecodePath::spare:return "spare";
    case RedecodePath::fenced:return "fenced";
    default:return "none";
    }
}
// Diagnostic: the most recent decodes (what is re-decoded every frame).
struct DecodeEvent{std::uint64_t handle;unsigned w,h,fmt,bytes;bool dirty;std::uint64_t ticks;RedecodePath path;};
std::array<DecodeEvent,8> recentDecodes{};unsigned recentDecodeNext;
void noteDecode(const gx::Texture& input,const NativeTexturePlan& e,bool dirty,std::uint64_t ticks,RedecodePath path=RedecodePath::none){
    recentDecodes[recentDecodeNext++&7]={input.handle,input.width,input.height,input.format,unsigned(e.outputBytes),dirty,ticks,path};
}
std::uint64_t dirtyUnchanged;
// Set by every image invalidation. Draws queued before it may still read an
// image's pixels (GX syncs no longer wait: gx_gpu_sync.cpp). Cleared by every
// queue completion of this file (completeGpu): a later draw reads an image
// only after binding it, and that bind re-decodes a dirty image first.
bool decodeFence;
// The Alpha 6 rule waited at the first changed re-decode after every
// invalidation, eviction waits or not. Mirrored only to report the waits
// avoided against it.
bool alpha6Fence;
// Successful queue completions requested by this file. Spare storage that left
// service at an older epoch has no queued reader any more.
std::uint32_t fenceEpoch;
GxTextureRedecodeStatistics redecodes{};
GxTextureRedecodeStatistics reportedRedecodes{};std::uint32_t reportedFrame{};
// Ping-pong policy: only an image with a changed re-decode in an earlier frame
// of the last spareHistoryFrames gets a second storage (the minimap's indexed
// capture is re-decoded every frame). At most spareLimit of them and
// spareByteLimit in all, inside the cache budget; a spare not needed for
// spareKeepFrames goes back once idle.
constexpr std::uint32_t spareHistoryFrames=60,spareKeepFrames=120;
constexpr unsigned spareLimit=4;
constexpr std::size_t spareByteLimit=1024*1024;
std::uint64_t linearEvictions; // images released because the linear heap refused an allocation
// worldMips memory. Generated levels add a third to an image, in the linear
// heap like every cached image (VRAM would need a transfer-engine copy per
// image, and the console's transfer engine is where its GPU stops were). The
// console's Alpha 19 automatic test had 4.5-9.3 MB of linear heap free in its
// 172 zones with no texture eviction; the owner's Alpha 20 village play 2.2
// MB, with 6.1 MB of cached images. Levels are made only while the heap keeps
// mipLinearFloor free after the allocation and the levels of all resident
// images stay under mipByteCap, so that they never make an image or a frame
// copy evict another; an image refused keeps its one level.
constexpr std::size_t mipByteCap=768*1024,mipLinearFloor=3*1024*1024;
// Sampled as the game samples its own chains (1,082 of the disc's 1,112
// authored chains use GX_LIN_MIP_LIN): trilinear, which has no visible line
// where one level gives way to the next. Half a level sharper than the
// game's own chains: the base level alone up to 1.4x minification and in the
// mix up to 2.8x, so near surfaces stay as they were; past that (far terrain
// and foliage, where neighbouring pixels fetch texels far apart) only reduced
// levels are read.
constexpr float worldMipLodBias=-0.5f;
GxWorldMipStatistics mipStats{};
// Header, palette and texels: exactly the bytes the decoder reads. Two sums,
// each over every other word: any one changed word changes its sum for
// certain (xor, then a multiplication by an odd number), and a word costs
// half of what it did when both sums took every word (the images an
// invalidation marks are summed again in every picture: 1.1 ms of the
// Alpha 9 profile of Ordon Village). The value never leaves the session.
std::uint64_t sourceChecksum(const gx::Texture& input,const NativeTexturePlan& plan){
    std::uint32_t a=2166136261u,b=0x9e3779b9u;
    auto run=[&](std::span<const std::uint8_t> bytes){
        std::size_t i=0;const auto* at=bytes.data();
        auto word=[&](std::size_t offset){std::uint32_t w;std::memcpy(&w,at+offset,4);return w;};
        for(;i+16<=bytes.size();i+=16){
            a=(a^word(i))*16777619u;b=(b^word(i+4))*0x9e3779b1u;
            a=(a^word(i+8))*16777619u;b=(b^word(i+12))*0x9e3779b1u;
        }
        for(;i+4<=bytes.size();i+=4){a=(a^word(i))*16777619u;std::swap(a,b);}
        for(;i<bytes.size();++i){a=(a^at[i])*16777619u;std::swap(a,b);}
        // The tail above swaps the sums: mix its length so that an odd and
        // an even number of swaps cannot stand for each other.
        a=(a^std::uint32_t(bytes.size()))*16777619u;
    };
    run(input.bti.first(std::min<std::size_t>(32,input.bti.size())));
    if(plan.paletteEntries&&plan.paletteOffset<=input.bti.size()&&std::size_t(plan.paletteEntries)*2<=input.bti.size()-plan.paletteOffset)
        run(input.bti.subspan(plan.paletteOffset,std::size_t(plan.paletteEntries)*2));
    run(input.image);
    return (std::uint64_t(a)<<32)|b;
}
C3D_Tex idleTexture{};
std::array<gx::TextureHandle,4> protectedHandles{};
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
// Read when the game names its first scenery image (duskNativeSceneryTexture,
// the background actor on the game thread): a draw never opens a file, and
// draws may run on the second-core worker.
void readSceneryPolicy(){
    if(sceneryPolicy<0){sceneryPolicy=0;if(auto* f=std::fopen("sdmc:/3ds/DuskLight/texture-scenery.txt","rb")){std::fclose(f);sceneryPolicy=1;}}
}
bool sceneryImage(const void* p){
    if(sceneryPolicy<=0||!p||!sceneryCount)return false;
    for(unsigned n=0,h=sceneryHome(p);n<16;++n){const auto* v=sceneryImages[(h+n)&(sceneryImages.size()-1)];if(v==p)return true;if(!v)return false;}
    return false;
}
bool inspectForCache(const gx::Texture& input,NativeTexturePlan& plan){
    // Compact only images the game cannot rewrite: the format follows the
    // content, and a rewritten image would change format under its storage.
    const bool compact=compactTextures&&input.immutablePixels;
    if(!inspectNativeTexture(input.bti,plan,rgb565Reference,false,compact)||!descriptorMatches(input,plan))return false;
    const bool scenery=sceneryImage(input.image.data())||(sceneryCount&&sceneryImage(gx::textureAliasImage(input.handle)));
    if(scenery&&input.immutablePixels){
        NativeTexturePlan reduced;
        if(inspectNativeTexture(input.bti,reduced,rgb565Reference,true,compact)||inspectNativeTextureBoxHalf(input.bti,reduced,rgb565Reference))plan=reduced;
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
    if(!inspectNativeTexture(input.bti,reduced,rgb565Reference,true,compact))return true;
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
    case NativeTextureFormat::rgba5551: return GPU_RGBA5551;
    }
    return GPU_RGBA8;
}
bool protectedImage(gx::TextureHandle handle){
    for(const auto value:protectedHandles)if(value&&value==handle)return true;
    return false;
}
// Eviction victim. Images the current frame has not used go first, oldest
// first. When every image was used this frame (its set does not fit), evict the
// most recently used one: plain LRU evicts exactly the image the next frame
// needs first and re-decodes the whole set every frame (title shots decoded
// ~9 MB a frame); this way only the part that does not fit is decoded again.
CachedTexture* evictionCandidate(std::span<CachedTexture* const> taken={}){
    CachedTexture* stale=nullptr;CachedTexture* recent=nullptr;
    for(auto& item:activeCache()){
        // A VRAM image frees no linear memory and is redrawn every frame.
        if(!item.handle||item.vram||protectedImage(item.handle))continue;
        bool skip=false;for(auto* value:taken)skip|=value==&item;
        if(skip)continue;
        if(item.frame!=gx::textureFrameCounter){if(!stale||item.used<stale->used)stale=&item;}
        else if(!recent||item.used>recent->used)recent=&item;
    }
    return stale?stale:recent;
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
bool completeGpu(){
    if(platformCompleteGpu(500000000)!=GpuCompletion::complete)return false;
    ++fenceEpoch;decodeFence=false;return true;
}
// Point every unit still holding this storage at the service image before the
// storage is freed, so no later draw of an unused unit can reach it.
bool unbindStorage(const void* data){
    if(!data)return true;
    bool bound=false;for(const auto& sampler:samplers)bound|=sampler.data==data;
    if(!bound)return true;
    if(!prepareIdleTexture())return false;
    for(unsigned unit=0;unit<samplers.size();++unit)if(samplers[unit].data==data){
        samplers[unit]=idleTexture;C3D_TexBind(unit,&samplers[unit]);unitBinds[unit].valid=false;
    }
    ++gxPicaStateSerial;return true;
}
// Every frame begins with the whole GPU queue complete (platformBeginGpuFrame,
// then beginGxGeometryFrame advances textureFrameCounter), so storage last
// bound in an earlier frame has no queued reader. Neither has storage that
// left service before one of this file's completions.
bool spareIdle(const CachedTexture& e){
    return e.spare.data&&(e.spareFrame!=gx::textureFrameCounter||e.spareEpoch!=fenceEpoch);
}
bool releaseSpare(CachedTexture& e){
    if(!e.spare.data)return true;
    if(!unbindStorage(e.spare.data))return false;
    const auto bytes=e.extent.outputBytes;
    allocated-=bytes;redecodes.spareBytes-=bytes;--redecodes.spares;++redecodes.spareReleases;
    C3D_TexDelete(&e.spare);e.spare={};++gxPicaStateSerial;++gxTextureStorageSerial;return true;
}
// Memory pressure: the idle spare needed longest ago goes first, with no wait.
bool releaseIdleSpare(){
    CachedTexture* oldest=nullptr;
    for(auto& item:activeCache())
        if(spareIdle(item)&&(!oldest||gx::textureFrameCounter-item.spareNeeded>gx::textureFrameCounter-oldest->spareNeeded))oldest=&item;
    return oldest&&releaseSpare(*oldest);
}
std::size_t storageBytes(const CachedTexture& e){return e.extent.outputBytes*(e.spare.data?2:1);}
// A spare in service left it in this frame and after this file's last
// completion: queued draws may still read it, so only a wait frees it.
std::size_t busySpareBytes(const CachedTexture& e){return e.spare.data&&!spareIdle(e)?e.extent.outputBytes:0;}
std::size_t busySpareBytes(){std::size_t total{};for(const auto& item:activeCache())total+=busySpareBytes(item);return total;}
// An eviction wait. spareHeld: spares in service held the memory that was
// missing, and Alpha 6, which had no spares, might not have waited. Such waits
// count with the re-decode waits taken, so the report never overstates the
// waits avoided.
bool evictionFence(bool spareHeld){
    const auto t0=gxProfileTick();
    if(!completeGpu())return false;
    if(spareHeld){++redecodes.spareHeldWaits;redecodes.fenceTicks+=gxProfileTick()-t0;}
    return true;
}
void noteRedecode(CachedTexture& e){
    const auto now=gx::textureFrameCounter;
    if(e.redecoded&&e.redecodeFrame!=now){e.earlierRedecodeFrame=e.redecodeFrame;e.redecodedEarlier=true;}
    e.redecodeFrame=now;e.redecoded=true;
}
// Re-decoded with changed pixels in an earlier frame, recently.
bool dynamicImage(const CachedTexture& e){
    const auto now=gx::textureFrameCounter;
    if(!e.redecoded)return false;
    std::uint32_t earlier;
    if(e.redecodeFrame!=now)earlier=e.redecodeFrame;
    else if(e.redecodedEarlier)earlier=e.earlierRedecodeFrame;
    else return false;
    return now-earlier<=spareHistoryFrames;
}
bool allocateSpare(CachedTexture& e,std::size_t bytes){
    if(e.spare.data||e.immutable||e.vram||!dynamicImage(e))return false;
    if(redecodes.spares>=spareLimit||bytes>spareByteLimit-redecodes.spareBytes||bytes>budget-allocated){++redecodes.spareRefusals;return false;}
    C3D_Tex fresh{};
    if(!C3D_TexInitWithParams(&fresh,nullptr,{e.texture.width,e.texture.height,u8(e.texture.maxLevel),e.texture.fmt,GPU_TEX_2D,false})){
        ++redecodes.spareRefusals;return false;
    }
    if(std::size_t(C3D_TexCalcTotalSize(fresh.size,fresh.maxLevel))!=bytes){C3D_TexDelete(&fresh);++redecodes.spareRefusals;return false;}
    // The current descriptor at the new address: samplers built from either
    // storage are identical except for the pixel address.
    e.spare=e.texture;e.spare.data=fresh.data;
    e.spareFrame=gx::textureFrameCounter-1;e.spareEpoch=fenceEpoch;e.spareNeeded=gx::textureFrameCounter; // never read
    allocated+=bytes;redecodes.spareBytes+=bytes;++redecodes.spares;++redecodes.spareAllocations;return true;
}
// Storage for a changed re-decode. JFWDisplay::preGX invalidates every image
// each frame, and a full GPU wait before each rewrite cost 10-25 ms per frame
// with the dungeon minimap (Alpha 6 console run). In order:
// - unfenced: a completion since the last invalidation (decodeFence clear);
// - idle: the current storage was last bound in an earlier frame;
// - spare: write the image's second storage, idle the same way, or allocate
//   one for a dynamic image (ping-pong); the old storage becomes the spare;
// - fenced: otherwise wait for the GPU once, as before.
// Every path decodes the same pixels; only the address of the storage differs.
bool redecodeTarget(CachedTexture& e,std::size_t bytes,C3D_Tex*& target,RedecodePath& path){
    target=&e.texture;
    if(alpha6Fence){alpha6Fence=false;++redecodes.alpha6Waits;}
    if(!decodeFence){path=RedecodePath::unfenced;return true;}
    if(e.frame!=gx::textureFrameCounter){path=RedecodePath::idle;return true;}
    if(spareIdle(e)||allocateSpare(e,bytes)){target=&e.spare;path=RedecodePath::spare;return true;}
    const auto t0=gxProfileTick();
    if(!completeGpu())return reject("GX texture re-decode GPU completion failed");
    redecodes.fenceTicks+=gxProfileTick()-t0;
    path=RedecodePath::fenced;return true;
}
// After a decode into the spare: it serves, and the old storage waits until
// its queued readers are complete.
void swapToSpare(CachedTexture& e){
    const auto lastBind=e.frame;
    std::swap(e.texture,e.spare);
    e.spareFrame=lastBind;e.spareEpoch=fenceEpoch;e.spareNeeded=gx::textureFrameCounter;
}
void countRedecode(RedecodePath path){
    switch(path){
    case RedecodePath::unfenced:++redecodes.unfenced;break;
    case RedecodePath::idle:++redecodes.idle;break;
    case RedecodePath::spare:++redecodes.spare;break;
    case RedecodePath::fenced:++redecodes.fenced;break;
    default:break;
    }
}
bool retire(CachedTexture& entry,copyprobe::Reason reason=copyprobe::Reason::evict){
    ++gxProfileTotals.evictions;++gxPicaStateSerial;++gxTextureStorageSerial;
    if(!unbindStorage(entry.texture.data)||!unbindStorage(entry.spare.data))return false;
    if(copyprobe::enabled)copyprobe::retire(entry.handle,reason);
    if(entry.spare.data){
        allocated-=entry.extent.outputBytes;redecodes.spareBytes-=entry.extent.outputBytes;--redecodes.spares;++redecodes.spareReleases;
        C3D_TexDelete(&entry.spare);
    }
    if(nativeTextureGeneratedMips(entry.extent)){--mipStats.images;mipStats.bytes-=nativeTextureGeneratedBytes(entry.extent);}
    allocated-=entry.extent.outputBytes;C3D_TexDelete(&entry.texture);entry={};return true;
}
// Generated levels for an image entering the cache (worldMips): only for a
// plain world-model use, when the session chose the improvement, inside the
// budget above. Multi-bit alpha needs the draw's alpha test as one threshold
// to keep its coverage (texture_gx.hpp).
bool planWorldMips(const gx::Texture& input,const NativeTexturePlan& plain,GxWorldMipUse use,bool projective,NativeTexturePlan& levels){
    if(!use.world||projective||!input.immutablePixels||!features::chosen[unsigned(features::Id::worldMips)])return false;
    const bool multiBitAlpha=plain.format!=NativeTextureFormat::rgb565&&plain.format!=NativeTextureFormat::rgba5551;
    if(multiBitAlpha&&!use.thresholdKnown){++mipStats.refusedAlpha;return false;}
    levels=plain;
    if(!planGeneratedMips(levels,use.thresholdKnown?use.reference:0)){++mipStats.refusedShape;return false;}
    if(mipStats.bytes+nativeTextureGeneratedBytes(levels)>mipByteCap||levels.outputBytes>budget-allocated){++mipStats.refusedCap;return false;}
    if(linearSpaceFree()<levels.outputBytes+mipLinearFloor){++mipStats.refusedLinear;return false;}
    return true;
}
bool makeRoom(std::size_t bytes){
    if(bytes>budget)return reject("GX texture exceeds entire cache budget");
    unsigned free{};for(const auto& item:activeCache())free+=!item.handle;
    // Idle spare storage holds no image of its own: it goes first, unfenced.
    while(bytes>budget-allocated&&releaseIdleSpare()){}
    if(free&&bytes<=budget-allocated)return true;
    if(!drawScope)return reject(free?"GX texture cache budget exceeded":"GX texture cache slots exhausted");
    // Spares in service are freed by the completion below, before any image:
    // images go only for the room they cannot give.
    const auto busy=busySpareBytes();
    std::array<CachedTexture*,std::tuple_size_v<decltype(cache)>> victims{};unsigned count{};std::size_t reclaim=busy;
    while((!free&&!count)||bytes>budget-allocated+reclaim){
        CachedTexture* oldest=evictionCandidate({victims.data(),count});
        if(!oldest)return reject("GX protected draw textures exceed cache capacity");
        victims[count++]=oldest;reclaim+=storageBytes(*oldest)-busySpareBytes(*oldest);
    }
    // Fence before deleting any allocation or changing borrowed sampler data.
    // With no image victim, only spares in service held the room: no wait in
    // Alpha 6.
    if(!evictionFence(free&&!count))return reject("GX texture eviction GPU completion failed");
    bool bound=false;
    for(const auto& sampler:samplers){
        for(unsigned n=0;n<count;++n)bound|=sampler.data==victims[n]->texture.data;
        for(const auto& item:activeCache())bound|=item.spare.data&&sampler.data==item.spare.data;
    }
    if(bound&&!prepareIdleTexture())return false;
    for(unsigned n=0;n<count;++n)if(!retire(*victims[n]))return false;
    // The completion left every spare idle.
    while(bytes>budget-allocated&&releaseIdleSpare()){}
    return bytes<=budget-allocated||reject("GX texture spare release failed");
}
}
void setGxTextureCompact(bool on){compactTextures=on;}
bool configureGxTextureCache(std::size_t bytes,unsigned images,bool rgb565Rgba8Reference,
                             std::span<const NativeTextureFingerprint> halfResolutionAllowlist,bool enableFusedColorCopy){
    if(!bytes||bytes>12*1024*1024||!images||images>cache.size()||allocated||drawScope||idleTexture.data||halfResolutionAllowlist.size()>halfAllowlist.size())
        return reject("GX cache configuration requires empty bounded cache");
    // Validate everything before publishing policy; caller storage may expire.
    halfAllowlist={};std::copy(halfResolutionAllowlist.begin(),halfResolutionAllowlist.end(),halfAllowlist.begin());
    halfAllowlistSize=halfResolutionAllowlist.size();
    budget=bytes;cacheSlots=images;rgb565Reference=rgb565Rgba8Reference;
    fusedColorCopyEnabled=enableFusedColorCopy;return true;
}
// The checksum bindGxTexture keeps of an image the game can rewrite (header,
// palette and texels), taken now.
bool gxTextureSourceSum(const gx::Texture& input,std::uint64_t& sum){
    NativeTexturePlan plan;
    if(!input.handle||!inspectForCache(input,plan))return false;
    sum=sourceChecksum(input,plan);return true;
}
GxTextureDrawScope::GxTextureDrawScope(std::span<const gx::Texture> textures){
    if(drawScope||textures.size()>protectedHandles.size()){reject("GX texture draw scope invalid/reentrant");return;}
    decltype(protectedHandles) handles{};std::size_t total{};
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
bool bindGxTexture(const gx::Texture& input, float& scaleU, float& scaleV,unsigned unit,bool projective,GxWorldMipUse mipUse) {
    GxProfileScope profile(GxProfilePart::texture);
    ++gxPicaStateSerial;
    // A retained screen being recorded must not keep a picture of pixels the
    // game can rewrite, unless its fingerprint holds them (runtime/screen_tape.hpp).
    if(!input.immutablePixels)duskScreenTapeVolatileTexture(input.handle);
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
        // Levels only where they fit as they are: a refused allocation keeps
        // the image's one level, never an eviction for its levels.
        const NativeTexturePlan plain=e;NativeTexturePlan levels;
        const bool tryLevels=planWorldMips(input,e,mipUse,projective,levels);
        bool created=tryLevels&&C3D_TexInitWithParams(&entry->texture,nullptr,
            {u16(levels.paddedWidth),u16(levels.paddedHeight),u8(levels.mipCount-1),gpuFormat(levels.format),GPU_TEX_2D,false});
        if(tryLevels&&!created)++mipStats.refusedAllocation;
        if(created)e=levels;
        const C3D_TexInitParams params{u16(e.paddedWidth),u16(e.paddedHeight),u8(e.mipCount-1),gpuFormat(e.format),GPU_TEX_2D,false};
        if(!created)created=C3D_TexInitWithParams(&entry->texture,nullptr,params);
        // The linear heap can be full or fragmented before the cache budget
        // is: release idle spare storage (no wait), then after one wait the
        // spares it idled and the images used longest ago, and try again.
        while(!created&&releaseIdleSpare())created=C3D_TexInitWithParams(&entry->texture,nullptr,params);
        for(bool fenced=false;!created&&drawScope;){
            CachedTexture* oldest=evictionCandidate();
            const auto busy=busySpareBytes();
            if(!oldest&&(fenced||!busy))break;
            if(!fenced){
                // Spares in service may hold the missing memory: counted.
                if(!evictionFence(busy!=0))break;
                fenced=true;
                while(!created&&releaseIdleSpare())created=C3D_TexInitWithParams(&entry->texture,nullptr,params);
                continue;
            }
            if(!retire(*oldest,copyprobe::Reason::evict))return false;
            ++linearEvictions;
            created=C3D_TexInitWithParams(&entry->texture,nullptr,params);
        }
        if (!created)
            return reject("GX texture GPU allocation failed");
        auto& gpu = entry->texture;
        bool decoded;
        std::uint64_t decodeTicks;
        auto decode=[&]{
        auto bytes = std::span(static_cast<std::uint8_t*>(gpu.data), std::size_t(C3D_TexCalcTotalSize(gpu.size,gpu.maxLevel)));
        {GxProfileScope decoding(GxProfilePart::decode);gxProfileTotals.decodedBytes+=e.outputBytes;const auto t0=gxProfileTick();
        decoded = C3D_TexCalcTotalSize(gpu.size,gpu.maxLevel) == e.outputBytes && decodeNativeTexture(input.bti, e, bytes, true);decodeTicks=gxProfileTick()-t0;}
        };
        decode();
        if(!decoded&&nativeTextureGeneratedMips(e)){
            // Making the levels needs a scratch allocation: refused, the
            // image keeps its one level in storage of its own size.
            ++mipStats.refusedAllocation;C3D_TexDelete(&gpu);e=plain;
            if(!C3D_TexInitWithParams(&gpu,nullptr,{u16(e.paddedWidth),u16(e.paddedHeight),u8(e.mipCount-1),gpuFormat(e.format),GPU_TEX_2D,false}))
                return reject("GX texture GPU allocation failed");
            decode();
        }
        if (!decoded) { C3D_TexDelete(&gpu); return reject("GX texture decode failed"); }
        if(copyprobe::enabled)copyprobe::decode(input.handle,probeReason);
        noteDecode(input,e,false,decodeTicks);
        C3D_TexFlush(&gpu);++contentSerial;
        if(copyprobe::enabled)copyprobe::flushUnknown(input.handle);
        C3D_TexSetWrap(&gpu, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        entry->extent = e;
        entry->handle = input.handle;
        entry->immutable=input.immutablePixels;
        entry->summed=!input.immutablePixels;
        if(entry->summed)entry->sourceSum=sourceChecksum(input,e);
        allocated += e.outputBytes;
        if(nativeTextureGeneratedMips(e)){++mipStats.images;++mipStats.made;mipStats.bytes+=nativeTextureGeneratedBytes(e);}
    }
    if(!descriptorMatches(input,entry->extent)||entry->immutable!=input.immutablePixels)
        return reject("GX cached texture descriptor mismatch");
    if(projective&&entry->extent.sourceMip)return reject("GX texture half policy forbids projected content");
    if((input.wrapS&&entry->extent.width!=entry->extent.paddedWidth)||(input.wrapT&&entry->extent.height!=entry->extent.paddedHeight))return reject("GX repeated padded texture unsupported");
    if (entry->vram) entry->dirty = false; // the PICA owns these pixels
    if (entry->dirty) {
        // Borrow the current binding's live resource, never retain/dereference
        // an old archive pointer while invalidating an unused cache entry.
        NativeTexturePlan current;
        // An explicitly invalidated image keeps the levels it was stored with.
        if (!inspectForCache(input,current) ||
            (nativeTextureGeneratedMips(entry->extent)&&!planGeneratedMips(current,entry->extent.mipAlphaReference)) ||
            current.outputBytes != entry->extent.outputBytes||
            current.sourceMip!=entry->extent.sourceMip||current.format!=entry->extent.format)
            return reject("GX invalidated texture descriptor mismatch");
        const auto bytes = std::size_t(C3D_TexCalcTotalSize(entry->texture.size, entry->texture.maxLevel));
        const std::uint64_t sum=sourceChecksum(input,current);
        if(entry->summed&&entry->sourceSum==sum&&!copyprobe::enabled)++dirtyUnchanged;
        else{
        if(bytes!=current.outputBytes)return reject("GX invalidated texture decode failed");
        C3D_Tex* target;RedecodePath path;
        if(!redecodeTarget(*entry,bytes,target,path))return false;
        bool decoded;
        std::uint64_t decodeTicks;
        {GxProfileScope decoding(GxProfilePart::decode);gxProfileTotals.decodedBytes+=current.outputBytes;const auto t0=gxProfileTick();
        decoded=decodeNativeTexture(input.bti,current,{static_cast<std::uint8_t*>(target->data),bytes},true);decodeTicks=gxProfileTick()-t0;}
        // A failed decode into the spare leaves the serving storage untouched.
        if (!decoded)
            return reject("GX invalidated texture decode failed");
        if(copyprobe::enabled)copyprobe::decode(input.handle,probeReason);
        noteDecode(input,current,true,decodeTicks,path);
        C3D_TexFlush(target);++contentSerial;
        if(target!=&entry->texture)swapToSpare(*entry);
        countRedecode(path);noteRedecode(*entry);
        if(copyprobe::enabled)copyprobe::flushUnknown(input.handle);
        entry->sourceSum=sum;entry->summed=true;
        }
        entry->dirty = false;
    }
    C3D_Tex sampler=entry->texture;
    // Bound in plain 2D mode: the rigid shaders divide a projected coordinate
    // per vertex. The CPU vertex route asks for the PICA projection mode
    // afterwards (setGxTextureProjection) when a draw needs the per-pixel
    // divide.
    (void)projective;
    sampler.param=(sampler.param&~GPU_TEXTURE_MODE(7))|GPU_TEXTURE_MODE(GPU_TEX_2D);
    const bool minLinear=input.minFilter==1||input.minFilter==3||input.minFilter==5;
    C3D_TexSetFilter(&sampler,input.magFilter?GPU_LINEAR:GPU_NEAREST,minLinear?GPU_LINEAR:GPU_NEAREST);
    // worldMips is asked here, at every bind: off (or a reference picture),
    // the same storage is sampled at its base level exactly as before.
    const bool generated=nativeTextureGeneratedMips(entry->extent);
    if(mipUse.world){++mipStats.worldBinds;entry->worldUse=true;if(!generated)++mipStats.residentWithout;}
    if(generated&&!mipUse.world&&features::on(features::Id::worldMips))++mipStats.levelBaseBinds;
    if(generated&&mipUse.world&&features::on(features::Id::worldMips)){
        ++mipStats.sampledBinds;
        C3D_TexSetFilterMipmap(&sampler,GPU_LINEAR);
        sampler.minLevel=0;sampler.maxLevel=entry->extent.mipCount-1;
        C3D_TexSetLodBias(&sampler,input.lodBias+worldMipLodBias);
    }else{
    C3D_TexSetFilterMipmap(&sampler,input.minFilter>=4?GPU_LINEAR:GPU_NEAREST);
    const auto base=entry->extent.sourceMip;
    sampler.minLevel=input.minLod>base?input.minLod-base:0;
    sampler.maxLevel=input.minFilter<2?0:input.maxLod>base?input.maxLod-base:0;
    // Halving the GPU dimensions already changes derivative-based lambda by
    // -1. Keep bias unchanged; rebase only explicit min/max LOD by sourceMip.
    C3D_TexSetLodBias(&sampler,input.lodBias);
    }
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
    entry->used=++clock;entry->frame=gx::textureFrameCounter;
    scaleU = float(entry->extent.width) / entry->extent.paddedWidth;
    scaleV = float(entry->extent.height) / entry->extent.paddedHeight;
    return true;
}
// Sampling mode of the texture bound to unit 0. In projection mode the PICA
// divides the coordinate by its third component at every pixel: exact for a
// texture mapped in screen space, where a per-vertex divide is wrong across
// any triangle with depth (and leaves the ground near the camera black: every
// build from 2026-11-13 h to 2026-11-14 i did, after the mode was blamed for a
// GPU stop it did not cause). The next bind of unit 0 returns to 2D.
bool setGxTextureProjection(bool projective){
    auto& last=unitBinds[0];
    if(!last.valid)return !projective;
    const u32 mode=GPU_TEXTURE_MODE(projective?GPU_TEX_PROJECTION:GPU_TEX_2D);
    if((last.desc.param&GPU_TEXTURE_MODE(7))==mode)return true;
    last.desc.param=(last.desc.param&~GPU_TEXTURE_MODE(7))|mode;
    samplers[0]=last.desc;C3D_TexBind(0,&samplers[0]);++bindApplies;
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
    entry->dirty=false;entry->summed=false;++contentSerial;++gxPicaStateSerial;
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
    readSceneryPolicy();
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
// Moves a GPU-resident capture into VRAM once, so the PICA renders straight
// into the texture its receivers sample. The console's transfer engine never
// finished the per-frame VRAM->linear copy of the shadow image (Alpha 0, run
// 10: TextureCopy job silent for 6 s on the first gameplay frame); this path
// has no copy at all.
void* gpuCaptureVramTexture(gx::TextureHandle handle,unsigned& width,unsigned& height){
    auto* entry=findCached(handle);
    if(!entry||entry->immutable||drawScope||protectedImage(handle)||entry->extent.format!=NativeTextureFormat::rgba8||
       entry->extent.sourceMip||entry->extent.mipCount!=1||!entry->texture.data||entry->texture.maxLevel)return nullptr;
    if(!entry->vram){
        // The linear copy may still be read by queued draws. A VRAM image is
        // never decoded again, so its spare storage goes too.
        if(!completeGpu()||!releaseSpare(*entry))return nullptr;
        C3D_Tex moved{};
        if(!C3D_TexInitWithParams(&moved,nullptr,{entry->texture.width,entry->texture.height,0,entry->texture.fmt,GPU_TEX_2D,true}))return nullptr;
        bool bound=false;for(const auto& sampler:samplers)bound|=sampler.data==entry->texture.data;
        if(bound&&!prepareIdleTexture()){C3D_TexDelete(&moved);return nullptr;}
        for(unsigned unit=0;unit<samplers.size();++unit)if(samplers[unit].data==entry->texture.data){
            samplers[unit]=idleTexture;C3D_TexBind(unit,&samplers[unit]);unitBinds[unit].valid=false;
        }
        C3D_TexSetWrap(&moved,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
        C3D_TexDelete(&entry->texture);entry->texture=moved;
        entry->vram=true;entry->dirty=false;entry->summed=false;++gxPicaStateSerial;++gxTextureStorageSerial;
    }
    width=entry->extent.width;height=entry->extent.height;return &entry->texture;
}
bool gpuCaptureRead(gx::TextureHandle handle,std::span<std::uint8_t> out,unsigned paddedWidth,unsigned paddedHeight){
    auto* entry=findCached(handle);
    if(!entry||entry->texture.width!=paddedWidth||entry->texture.height!=paddedHeight||out.size()>entry->extent.outputBytes)return false;
    GSPGPU_InvalidateDataCache(entry->texture.data,out.size());
    std::memcpy(out.data(),entry->texture.data,out.size());return true;
}
void gpuCaptureCommit(gx::TextureHandle handle){
    if(auto* entry=findCached(handle)){entry->dirty=false;entry->gpuResident=true;entry->summed=false;entry->used=++clock;entry->frame=gx::textureFrameCounter;}
    ++contentSerial;invalidateGxTextureBindings();
}
void invalidateGxTextureBindings(){for(auto& b:unitBinds)b.valid=false;++gxPicaStateSerial;}
void gxTextureDecodeReport(){
    for(unsigned n=0;n<recentDecodes.size();++n){const auto& d=recentDecodes[n];
        if(d.handle)OSReport("Native recent decode handle=%llu %ux%u gxfmt=%u picabytes=%u %s %.3f ms%s%s\n",(unsigned long long)d.handle,d.w,d.h,d.fmt,d.bytes,d.dirty?"dirty":"new",
            d.ticks*1000.0/dusk::native::nativeTickFrequency,d.dirty?" via=":"",d.dirty?redecodePathName(d.path):"");}
    recentDecodes={};
    // Since the previous report. Waits taken: changed re-decodes that waited,
    // plus eviction waits while spares in service held the memory (held).
    // Avoided: against the Alpha 6 rule, which waited at the first changed
    // re-decode after each invalidation. Every wait counted here follows its
    // own invalidation and changed re-decode since the previous wait, so
    // taken <= rule; a report boundary can split the pair, hence the clamp.
    // Two lines: the console OSReport keeps 255 characters (bootstrap.cpp).
    const auto now=gxTextureRedecodeStatistics();const auto& was=reportedRedecodes;
    const unsigned frames=gx::textureFrameCounter-reportedFrame;
    const auto fenced=now.fenced-was.fenced,held=now.spareHeldWaits-was.spareHeldWaits,taken=fenced+held;
    const auto rule=now.alpha6Waits-was.alpha6Waits,avoided=rule>taken?rule-taken:0;
    const double perFrame=frames?1.0/frames:0.0;
    using ull=unsigned long long;
    OSReport("Native texture re-decode GPU waits taken=%llu (%.2f ms, held=%llu) avoided=%llu of %llu by the Alpha 6 rule, per frame taken=%.2f avoided=%.2f frames=%u\n",
        ull(taken),(now.fenceTicks-was.fenceTicks)*1000.0/dusk::native::nativeTickFrequency,ull(held),
        ull(avoided),ull(rule),double(taken)*perFrame,double(avoided)*perFrame,frames);
    OSReport("Native texture re-decode paths idle=%llu spare=%llu unfenced=%llu fenced=%llu unchanged=%llu; spares=%u bytes=%lu, allocated=%llu released=%llu refused=%llu\n",
        ull(now.idle-was.idle),ull(now.spare-was.spare),ull(now.unfenced-was.unfenced),ull(fenced),ull(now.unchanged-was.unchanged),
        now.spares,static_cast<unsigned long>(now.spareBytes),ull(now.spareAllocations-was.spareAllocations),
        ull(now.spareReleases-was.spareReleases),ull(now.spareRefusals-was.spareRefusals));
    reportedRedecodes=now;reportedFrame=gx::textureFrameCounter;
    const auto& m=mipStats;
    OSReport("Native world mips images=%u bytes=%lu made=%llu binds=%llu sampled=%llu base=%llu without=%llu refused shape=%llu alpha=%llu cap=%llu linear=%llu alloc=%llu; linear free=%lu evictions=%llu\n",
        m.images,static_cast<unsigned long>(m.bytes),ull(m.made),ull(m.worldBinds),ull(m.sampledBinds),ull(m.levelBaseBinds),ull(m.residentWithout),
        ull(m.refusedShape),ull(m.refusedAlpha),ull(m.refusedCap),ull(m.refusedLinear),ull(m.refusedAllocation),
        static_cast<unsigned long>(linearSpaceFree()),ull(linearEvictions));
    // The world images of the last two pictures: with levels, and without
    // them (what a chain would add, or why none can be made).
    unsigned with{},without{},impossible{};std::size_t withBase{},withLevels{},withoutBase{},wouldAdd{};
    for(const auto& item:activeCache()){
        if(!item.handle||!item.worldUse||gx::textureFrameCounter-item.frame>1)continue;
        const auto& e=item.extent;const std::size_t base=std::size_t(e.paddedWidth)*e.paddedHeight*nativeTextureBytesPerPixel(e.format);
        if(nativeTextureGeneratedMips(e)){++with;withBase+=base;withLevels+=nativeTextureGeneratedBytes(e);continue;}
        if(e.originalMipCount>1||e.sourceMip)continue; // authored levels
        auto chain=e;
        if(!planGeneratedMips(chain,0)){++impossible;continue;}
        ++without;withoutBase+=base;wouldAdd+=nativeTextureGeneratedBytes(chain);
    }
    OSReport("Native world mips last pictures: with levels %u (base %lu B, levels %lu B); without %u (base %lu B, a chain adds %lu B); no chain possible %u\n",
        with,static_cast<unsigned long>(withBase),static_cast<unsigned long>(withLevels),without,static_cast<unsigned long>(withoutBase),
        static_cast<unsigned long>(wouldAdd),impossible);
}
GxTextureRedecodeStatistics gxTextureRedecodeStatistics(){auto value=redecodes;value.unchanged=dirtyUnchanged;return value;}
void gxTextureBindStatistics(std::uint64_t& skips,std::uint64_t& applies){skips=bindSkips;applies=bindApplies;}
std::size_t gxTextureBytes() { return allocated; }
std::uint64_t gxTextureLinearEvictions() { return linearEvictions; }
GxWorldMipStatistics gxWorldMipStatistics(){return mipStats;}
bool releaseLeastNeededTexture(bool& fenced){
    if(releaseIdleSpare())return true; // no image lost, no wait
    CachedTexture* victim=evictionCandidate();
    const auto busy=busySpareBytes();
    if(!victim&&(fenced||!busy))return false;
    if(!fenced){
        // Spares in service may hold the missing memory: counted. The wait
        // idles them, and one goes before any image.
        if(!evictionFence(busy!=0))return false;
        fenced=true;
        if(releaseIdleSpare())return true;
        if(!victim)return false;
    }
    if(!retire(*victim,copyprobe::Reason::evict))return false;
    ++linearEvictions;return true;
}
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
    ++gxPicaStateSerial;
    if(copyprobe::enabled)copyprobe::globalInvalidate();
    // GPU-resident captures own their pixels on the GPU; the CPU bytes are stale.
    for (auto& item : cache) {
        if (item.handle&&!item.immutable&&!item.gpuResident) item.dirty = true;
        // Each GXInvalidateTexAll (every frame, JFWDisplay::preGX): a spare no
        // longer needed goes back to the budget when no queued draw reads it.
        if(item.spare.data&&gx::textureFrameCounter-item.spareNeeded>spareKeepFrames&&spareIdle(item))releaseSpare(item);
    }
    decodeFence = true;alpha6Fence=true;
}
bool invalidateGxTexture(gx::TextureHandle handle){
    ++gxPicaStateSerial;
    if(copyprobe::enabled)copyprobe::imageInvalidate(handle);
    for(auto& item:cache)if(item.handle==handle){
        if(item.vram)return true; // rendered by the PICA, never from CPU bytes
        item.dirty=true;item.gpuResident=false;item.summed=false;decodeFence=alpha6Fence=true;return true;
    }
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
        if (item.spare.data) C3D_TexDelete(&item.spare);
        item = {};
    }
    if(idleTexture.data)C3D_TexDelete(&idleTexture);
    idleTexture={};
    protectedHandles={};drawScope=false;clock=0;
    allocated = 0;mipStats.images=0;mipStats.bytes=0;redecodes={};alpha6Fence=false;reportedRedecodes={};reportedFrame=gx::textureFrameCounter;dirtyUnchanged=0;
    fusedColorCopyEnabled=false;
    budget=4*1024*1024;cacheSlots=64;rgb565Reference=false;halfAllowlist={};halfAllowlistSize=0;
}
}
