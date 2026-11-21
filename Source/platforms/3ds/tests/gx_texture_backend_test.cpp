// Host lifecycle/sampler test using actual installed Citro3D structs and inline
// packers. GPU allocation/bind/flush are substituted; no GPU execution claim.
#include "../gx_texture_pica.hpp"
#include "host_fused_copy_off.hpp"
#include "../runtime/texture_gx.hpp"
#include "../runtime/gpu_sync.hpp"
#include "../runtime/gx_texture_commands.hpp"
#include "../runtime/perf_features.hpp"
#include <3ds/types.h>
#include <3ds/gpu/enums.h>
extern "C" {
#include <c3d/texture.h>
}
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <array>
#include <map>
#include <cmath>
#include <algorithm>
#include <cstdarg>
#include <string>
using namespace dusk::native;
static std::map<void*,std::size_t> allocations;
static std::array<C3D_Tex*,3> bindings{};
static unsigned creates,flushes,deletes,binds;
static bool refuseAllocation;
static unsigned refuseCount; // refuse only the next allocations (linear heap full)
static bool inFlight{};
static unsigned completions{};
static GpuCompletion completion=GpuCompletion::complete;
// Precise queue model for the ping-pong cases: each storage a queued draw
// reads, with the pixels it was queued with. Completion clears it, and so
// does a frame begin (platformBeginGpuFrame completes the queue).
static bool storageModel{};
static std::map<const void*,std::vector<u8>> queuedReaders;
namespace dusk::native {
GpuCompletion platformCompleteGpu(std::uint64_t timeout){
    assert(timeout==500000000);++completions;
    if(completion==GpuCompletion::complete){inFlight=false;queuedReaders.clear();}
    return completion;
}
}
static std::string reportText;
// The console OSReport formats into 256 bytes (runtime/bootstrap.cpp): a longer
// line loses its end and newline, and the next line is glued onto it.
// Retained 2D screens: no recording runs in this test.
extern "C" void duskScreenTapeVolatileTexture(unsigned long long){}
extern "C" void OSReport(const char* format,...){
    char line[256];va_list args;va_start(args,format);const int n=std::vsnprintf(line,sizeof line,format,args);va_end(args);
    assert(n>0&&n<int(sizeof line)&&line[n-1]=='\n');reportText+=line;
}
// Platform services the backend links but these cases never reach in a way
// that matters: decode timing, the scenery alias registry (empty policy) and
// the diagnostic capture readback's cache invalidation (host memory is coherent).
namespace dusk::native {
std::uint64_t gxProfileTick(){return 0;}
namespace gx {const void* textureAliasImage(TextureHandle){return nullptr;}}
}
extern "C" Result GSPGPU_InvalidateDataCache(const void*,u32){return 0;}
static u32 linearFree=64u<<20;
extern "C" u32 linearSpaceFree(void){return linearFree;}
bool C3D_TexInitWithParams(C3D_Tex* tex,C3D_TexCube* cube,C3D_TexInitParams p){
    if(refuseAllocation)return false;
    if(refuseCount){--refuseCount;return false;}
    assert(!cube&&p.type==GPU_TEX_2D&&!p.onVram);
    unsigned bpp=0;switch(p.format){case GPU_RGBA8:bpp=4;break;case GPU_RGB565:case GPU_LA8:bpp=2;break;case GPU_LA4:bpp=1;break;default:assert(false);}
    *tex={};tex->width=p.width;tex->height=p.height;tex->fmt=p.format;tex->size=p.width*p.height*bpp;
    tex->maxLevel=p.maxLevel;const auto bytes=C3D_TexCalcTotalSize(tex->size,p.maxLevel);
    tex->data=std::calloc(bytes,1);assert(tex->data);allocations[tex->data]=bytes;++creates;return true;
}
void C3D_TexDelete(C3D_Tex* tex){
    assert(storageModel?!queuedReaders.contains(tex->data):!inFlight);for(const auto* binding:bindings)if(binding)assert(binding->data!=tex->data);
    assert(allocations.erase(tex->data)==1);std::free(tex->data);*tex={};++deletes;
}
void C3D_TexFlush(C3D_Tex* tex){assert(allocations.contains(tex->data));++flushes;}
void C3D_TexBind(int unit,C3D_Tex* tex){assert(unit>=0&&unit<3&&allocations.contains(tex->data));bindings[unit]=tex;++binds;}
static std::vector<u8> bti(unsigned format,unsigned width=8,unsigned height=8){
    const unsigned payload=format==0?((width+7)/8)*((height+7)/8)*32:format==6?width*height*4:format==4?((width+3)/4)*((height+3)/4)*32:128;
    std::vector<u8> b(32+payload,0);b[0]=format;b[2]=width>>8;b[3]=width;b[4]=height>>8;b[5]=height;b[20]=b[21]=1;b[24]=1;b[31]=32;
    for(unsigned i=32;i<b.size();++i)b[i]=i%251;return b;
}
static gx::Texture texture(gx::TextureHandle handle,const std::vector<u8>& bytes,unsigned w=8,unsigned h=8){return {handle,bytes,std::span<const u8>(bytes).subspan(32),u16(w),u16(h),bytes[0],0,0,0,0};}
static void nativeRgb565(){
    float u{},v{};
    auto source=bti(4,9,13);auto t=texture(60000,source,9,13);
    assert(bindGxTexture(t,u,v,0)&&u==9.f/16&&v==13.f/16);
    assert(bindings[0]->fmt==GPU_RGB565&&bindings[0]->size==512&&gxTextureBytes()==512);
    assert(allocations.at(bindings[0]->data)==512);
    const auto data=bindings[0]->data;const auto count=creates;const auto flushCount=flushes;
    t.magFilter=1;assert(bindGxTexture(t,u,v,1)&&bindings[1]->data==data&&bindings[1]->fmt==GPU_RGB565);
    assert(bindings[0]->param!=bindings[1]->param&&creates==count);
    for(unsigned at=32;at<source.size();at+=2){source[at]=0xf8;source[at+1]=0;}
    invalidateGxTextures();assert(bindGxTexture(t,u,v,2)&&creates==count&&flushes==flushCount+1);
    for(unsigned at=0;at<512;at+=2){const auto* b=static_cast<u8*>(data);assert(b[at]==0&&b[at+1]==0xf8);}
    auto bad=t;bad.format=5;assert(!bindGxTexture(bad,u,v,0)&&gxTextureBytes()==512);
    destroyGxTextures();
    auto mip=bti(4,32,16);mip.resize(32+(32*16+16*8)*2);mip[24]=2;
    auto mt=texture(60001,mip,32,16);mt.mipCount=2;mt.minFilter=5;mt.maxLod=1;mt.wrapS=1;mt.wrapT=2;
    assert(bindGxTexture(mt,u,v,0)&&bindings[0]->fmt==GPU_RGB565&&bindings[0]->maxLevel==1);
    assert(bindings[0]->size==1024&&gxTextureBytes()==1280&&allocations.at(bindings[0]->data)==1280);
    NativeTexturePlan p;assert(inspectNativeTexture(mip,p));std::vector<u8> expected(p.outputBytes);
    assert(decodeNativeTexture(mip,p,expected)&&std::equal(expected.begin(),expected.end(),static_cast<u8*>(bindings[0]->data)));
    const auto mipData=bindings[0]->data;const auto mipCreates=creates;
    std::fill(mip.begin()+32,mip.end(),255);assert(invalidateGxTexture(mt.handle)&&bindGxTexture(mt,u,v,1));
    assert(creates==mipCreates&&bindings[1]->data==mipData);
    for(unsigned at=0;at<1280;++at)assert(static_cast<u8*>(mipData)[at]==255);
    destroyGxTextures();
    // Three packed 8x8 images exactly fill 384 B. Protect the oldest while a
    // fourth image evicts one other allocation after the completion boundary.
    assert(configureGxTextureCache(384,64));auto small=bti(4);
    auto a=texture(60100,small),b=texture(60101,small),c=texture(60102,small),d=texture(60103,small);
    assert(bindGxTexture(a,u,v,0)&&bindGxTexture(b,u,v,1)&&bindGxTexture(c,u,v,2)&&gxTextureBytes()==384);
    const auto protectedData=bindings[0]->data;const auto deleted=deletes;
    assert(!bindGxTexture(d,u,v,0)&&deletes==deleted);
    std::array<gx::Texture,2> consumed{a,d};inFlight=true;completion=GpuCompletion::timedOut;
    {GxTextureDrawScope scope(consumed);assert(scope&&!bindGxTexture(d,u,v,1));}
    assert(deletes==deleted&&gxTextureBytes()==384&&inFlight);
    completion=GpuCompletion::complete;
    {GxTextureDrawScope scope(consumed);assert(scope&&bindGxTexture(d,u,v,1));
     assert(!inFlight&&deletes==deleted+1&&gxTextureBytes()==384&&allocations.contains(protectedData));
     assert(bindGxTexture(a,u,v,0)&&bindings[0]->data==protectedData&&bindings[0]->fmt==GPU_RGB565);}
    assert(releaseGxTexture(d.handle)&&gxTextureBytes()==256);
    destroyGxTextures();assert(gxTextureBytes()==0&&allocations.empty());
    std::puts("PASS actual RGB565 GPU enum/allocation bytes, odd padding, independent samplers, full mip upload/refresh, byte-pressure eviction/protection/fence failure and retirement under host stubs");
}
static void rgba565Reference(){
    assert(configureGxTextureCache(4096,64,true));
    auto source=bti(4,9,13);auto t=texture(61000,source,9,13);float u{},v{};
    {GxTextureDrawScope scope({&t,1});assert(scope&&bindGxTexture(t,u,v));}
    assert(bindings[0]->fmt==GPU_RGBA8&&bindings[0]->size==1024&&gxTextureBytes()==1024);
    assert(!configureGxTextureCache(4096,64,false));
    NativeTexturePlan p;assert(inspectNativeTexture(source,p,true));
    std::vector<u8> expected(p.outputBytes);assert(decodeNativeTexture(source,p,expected));
    assert(std::equal(expected.begin(),expected.end(),static_cast<u8*>(bindings[0]->data)));
    source[32]=0xf8;source[33]=0;invalidateGxTextures();assert(bindGxTexture(t,u,v));
    assert(decodeNativeTexture(source,p,expected));
    assert(std::equal(expected.begin(),expected.end(),static_cast<u8*>(bindings[0]->data)));
    destroyGxTextures();assert(configureGxTextureCache(4096,64));
    assert(bindGxTexture(t,u,v)&&bindings[0]->fmt==GPU_RGB565&&gxTextureBytes()==512);
    destroyGxTextures();assert(configureGxTextureCache(4*1024*1024,64));
    std::puts("PASS immutable A/B cache policy, RGBA8 reference upload/refresh and packed reconfiguration");
}
// A changed dynamic image (the minimap's indexed capture) is re-decoded without
// a GPU wait while one of its two storages has no queued reader. Frame model:
// platformBeginGpuFrame completes the queue, then beginGxGeometryFrame
// advances textureFrameCounter. Every case checks that no CPU write reached
// storage a queued draw still reads.
static void pingPong(){
    storageModel=true;queuedReaders.clear();
    const auto frameBegin=[]{queuedReaders.clear();++gx::textureFrameCounter;};
    const auto draw=[](unsigned unit){const auto* p=static_cast<const u8*>(bindings[unit]->data);
        queuedReaders[p]=std::vector<u8>(p,p+allocations.at(bindings[unit]->data));};
    const auto queuedIntact=[]{
        for(const auto& [p,bytes]:queuedReaders)if(!std::equal(bytes.begin(),bytes.end(),static_cast<const u8*>(p)))return false;
        return true;
    };
    const auto decoded=[](const std::vector<u8>& source,const void* data){
        NativeTexturePlan p;assert(inspectNativeTexture(source,p));std::vector<u8> e(p.outputBytes);
        assert(decodeNativeTexture(source,p,e));return std::equal(e.begin(),e.end(),static_cast<const u8*>(data));};
    auto source=bti(4);auto t=texture(70000,source);float u{},v{};
    frameBegin();
    assert(bindGxTexture(t,u,v,0)&&gxTextureBytes()==128);draw(0);
    void* const first=bindings[0]->data;const auto waits=completions,made=creates;
    // Next frame, other draws queued: the storage was last bound before the
    // frame began, so it is rewritten in place with no wait.
    frameBegin();source[40]^=0xff;invalidateGxTextures();
    assert(bindGxTexture(t,u,v,0)&&bindings[0]->data==first&&completions==waits&&creates==made&&decoded(source,first));
    auto s=gxTextureRedecodeStatistics();assert(s.idle==1&&s.spare==0&&s.fenced==0&&s.unfenced==0&&s.alpha6Waits==1);
    draw(0);
    // Changed again in the same frame: no re-decode in an earlier frame yet,
    // so no second storage; one GPU wait as before.
    source[41]^=0xff;assert(invalidateGxTexture(t.handle));
    assert(bindGxTexture(t,u,v,0)&&bindings[0]->data==first&&completions==waits+1&&creates==made&&decoded(source,first));
    s=gxTextureRedecodeStatistics();assert(s.fenced==1&&s.spares==0&&s.alpha6Waits==2);
    draw(0);
    // Third frame: in place again; the image is now dynamic.
    frameBegin();source[42]^=0xff;invalidateGxTextures();
    assert(bindGxTexture(t,u,v,0)&&bindings[0]->data==first&&completions==waits+1);draw(0);
    // Changed while a queued draw reads it: decode into a second storage.
    source[43]^=0xff;assert(invalidateGxTexture(t.handle));
    assert(bindGxTexture(t,u,v,1)&&completions==waits+1&&creates==made+1&&queuedIntact());
    void* const second=bindings[1]->data;
    assert(second!=first&&decoded(source,second)&&gxTextureBytes()==256&&bindings[0]->data==first);
    assert(bindings[1]->fmt==bindings[0]->fmt&&bindings[1]->width==bindings[0]->width&&bindings[1]->height==bindings[0]->height);
    s=gxTextureRedecodeStatistics();assert(s.spare==1&&s.spares==1&&s.spareBytes==128&&s.spareAllocations==1);
    draw(1);
    // Both storages read by queued draws: one wait, then in place.
    source[44]^=0xff;assert(invalidateGxTexture(t.handle));
    assert(bindGxTexture(t,u,v,1)&&completions==waits+2&&bindings[1]->data==second&&creates==made+1&&decoded(source,second));
    draw(1);
    // The old storage left service before that wait: ping-pong back, no wait.
    source[45]^=0xff;assert(invalidateGxTexture(t.handle));
    assert(bindGxTexture(t,u,v,2)&&completions==waits+2&&bindings[2]->data==first&&creates==made+1&&queuedIntact()&&decoded(source,first));
    draw(2);
    s=gxTextureRedecodeStatistics();assert(s.idle==2&&s.spare==2&&s.fenced==2&&s.spares==1&&gxTextureBytes()==256&&s.alpha6Waits==6);
    reportText.clear();gxTextureDecodeReport();
    // Every changed re-decode here followed its own invalidation: the Alpha 6
    // rule waited six times, this file twice.
    assert(reportText.find("Native texture re-decode GPU waits taken=2 (0.00 ms, held=0) avoided=4 of 6 by the Alpha 6 rule, per frame taken=0.67 avoided=1.33 frames=3\n")!=std::string::npos);
    assert(reportText.find("Native texture re-decode paths idle=2 spare=2 unfenced=0 fenced=2 unchanged=0; spares=1 bytes=128, allocated=1 released=0 refused=0\n")!=std::string::npos);
    assert(reportText.find("via=spare")!=std::string::npos&&reportText.find("via=idle")!=std::string::npos&&reportText.find("via=fenced")!=std::string::npos);
    // Spare counters are per report too: nothing new since.
    reportText.clear();gxTextureDecodeReport();
    assert(reportText.find("spares=1 bytes=128, allocated=0 released=0 refused=0\n")!=std::string::npos);
    // Next frame, inside a draw scope: in place, no wait.
    frameBegin();source[46]^=0xff;invalidateGxTextures();
    {GxTextureDrawScope scope({&t,1});assert(scope&&bindGxTexture(t,u,v,0)&&bindings[0]->data==first&&completions==waits+2);}
    draw(0);
    // An unchanged source is not decoded at all.
    frameBegin();invalidateGxTextures();const auto flushed=flushes;const auto unchanged=gxTextureRedecodeStatistics().unchanged;
    assert(bindGxTexture(t,u,v,0)&&flushes==flushed&&gxTextureRedecodeStatistics().unchanged==unchanged+1);draw(0);
    // A spare unused for a while is released once idle, with no wait. Unit 1
    // still holds it and is repaired first (the stub refuses bound storage).
    assert(bindings[1]->data==second);
    const auto deleted=deletes;
    for(unsigned n=0;gxTextureRedecodeStatistics().spares;++n){
        assert(n<200);frameBegin();source[47]^=0xff;invalidateGxTextures();
        assert(bindGxTexture(t,u,v,0)&&bindings[0]->data==first);draw(0);
    }
    s=gxTextureRedecodeStatistics();
    assert(s.spareReleases==1&&s.spareBytes==0&&deletes==deleted+1&&!allocations.contains(second)&&gxTextureBytes()==128);
    assert(bindings[1]->data!=second&&gxTextureIdleBytes()==256&&completions==waits+2);
    frameBegin();assert(releaseGxTexture(t.handle)&&gxTextureBytes()==0);
    destroyGxTextures();assert(allocations.empty());
    // Build a dynamic image with a spare; returns its storages.
    const auto dynamicWithSpare=[&](gx::Texture& image,std::vector<u8>& bytes){
        frameBegin();assert(bindGxTexture(image,u,v,0));draw(0);
        frameBegin();bytes[48]^=0xff;invalidateGxTextures();assert(bindGxTexture(image,u,v,0));draw(0);
        frameBegin();bytes[48]^=0xff;invalidateGxTextures();assert(bindGxTexture(image,u,v,0));draw(0);
        bytes[49]^=0xff;assert(invalidateGxTexture(image.handle)&&bindGxTexture(image,u,v,1));draw(1);
    };
    // Memory pressure takes an idle spare before any image, with no wait and
    // outside a draw scope; the linear-heap fallback does the same.
    assert(configureGxTextureCache(384,64));
    auto other=bti(4),third=bti(4);other[50]^=1;third[51]^=1;
    auto b=texture(70001,other),c=texture(70002,third);
    const auto pressureWaits=completions;
    dynamicWithSpare(t,source);
    assert(gxTextureBytes()==256&&gxTextureRedecodeStatistics().spares==1&&completions==pressureWaits);
    assert(bindGxTexture(b,u,v,2)&&gxTextureBytes()==384);draw(2);
    // Same frame: the spare left service in this frame, so it is not idle and
    // an image outside a draw scope cannot take its room.
    assert(!bindGxTexture(c,u,v,0)&&gxTextureBytes()==384);
    frameBegin();assert(bindGxTexture(t,u,v,0));draw(0);
    assert(bindGxTexture(c,u,v,2)&&gxTextureBytes()==384&&completions==pressureWaits&&queuedIntact());
    assert(gxTextureRedecodeStatistics().spares==0&&gxTextureRedecodeStatistics().spareReleases==1);
    frameBegin();destroyGxTextures();
    assert(configureGxTextureCache(384,64));
    dynamicWithSpare(t,source);
    frameBegin();assert(bindGxTexture(t,u,v,0));draw(0);
    bool fenced=false;
    assert(releaseLeastNeededTexture(fenced)&&!fenced&&completions==pressureWaits&&gxTextureBytes()==128&&queuedIntact());
    frameBegin();destroyGxTextures();
    // No room in the budget for a second storage: the old single wait,
    // counted as refused.
    assert(configureGxTextureCache(128,64));
    frameBegin();assert(bindGxTexture(t,u,v,0));draw(0);
    frameBegin();source[52]^=0xff;invalidateGxTextures();assert(bindGxTexture(t,u,v,0));draw(0);
    frameBegin();source[52]^=0xff;invalidateGxTextures();assert(bindGxTexture(t,u,v,0));draw(0);
    const auto refusedWaits=completions;
    source[53]^=0xff;assert(invalidateGxTexture(t.handle)&&bindGxTexture(t,u,v,0));
    s=gxTextureRedecodeStatistics();
    assert(completions==refusedWaits+1&&s.spareRefusals==1&&s.spares==0&&gxTextureBytes()==128&&decoded(source,bindings[0]->data));
    // Retirement releases both storages.
    frameBegin();destroyGxTextures();
    assert(configureGxTextureCache(4096,64));
    dynamicWithSpare(t,source);
    frameBegin();assert(releaseGxTexture(t.handle)&&gxTextureBytes()==0&&gxTextureRedecodeStatistics().spares==0);
    destroyGxTextures();assert(allocations.empty()&&creates==deletes);
    assert(configureGxTextureCache(4*1024*1024,64));
    // One invalidation (the per-frame GXInvalidateTexAll) and two changed
    // images. The Alpha 6 rule waited once, at the first of them. Now b, last
    // bound in an earlier frame, is rewritten in place with no wait.
    {auto other=bti(4);other[60]^=1;auto b=texture(70003,other);
     frameBegin();assert(bindGxTexture(t,u,v,0)&&bindGxTexture(b,u,v,1));draw(0);draw(1);
     const auto before=gxTextureRedecodeStatistics();const auto quiet=completions,made2=creates;
     frameBegin();assert(bindGxTexture(t,u,v,2));draw(2); // other work of this frame
     source[54]^=0xff;other[61]^=0xff;invalidateGxTextures();
     assert(bindGxTexture(b,u,v,1)&&completions==quiet&&creates==made2&&queuedIntact()&&decoded(other,bindings[1]->data));
     s=gxTextureRedecodeStatistics();
     assert(s.idle==before.idle+1&&s.fenced==before.fenced&&s.alpha6Waits==before.alpha6Waits+1);
     // t was bound in this frame before the invalidation and has no changes
     // in earlier frames (no spare): it takes the single wait instead, which
     // the rule does not count twice. Never more waits than the rule.
     assert(bindGxTexture(t,u,v,0)&&completions==quiet+1&&queuedIntact()&&decoded(source,bindings[0]->data));
     s=gxTextureRedecodeStatistics();
     assert(s.fenced==before.fenced+1&&s.alpha6Waits==before.alpha6Waits+1&&s.spares==0);
     reportText.clear();gxTextureDecodeReport();
     assert(reportText.find("Native texture re-decode GPU waits taken=1 (0.00 ms, held=0) avoided=0 of 1 by the Alpha 6 rule")!=std::string::npos);
     frameBegin();destroyGxTextures();assert(allocations.empty());}
    // A spare in service holds the room a new image needs in the same frame.
    // Alpha 6, with no spare, had the room and waited only at the re-decode
    // of t. Now: one eviction wait, counted as held, idles the spare, which
    // goes before any image; that wait also ends the decode fence, so both
    // dirty images bound in this frame are rewritten with no further wait.
    {assert(configureGxTextureCache(384,64));
     auto other=bti(4),third=bti(4);other[62]^=1;third[63]^=1;
     auto b=texture(70004,other),c=texture(70005,third);
     dynamicWithSpare(t,source);
     assert(bindGxTexture(b,u,v,2)&&gxTextureBytes()==384);draw(2);
     source[50]^=0xff;other[64]^=0xff;invalidateGxTextures();
     reportText.clear();gxTextureDecodeReport();reportText.clear();
     const auto quiet=completions;const auto before=gxTextureRedecodeStatistics();
     {std::array<gx::Texture,3> consumed{c,t,b};GxTextureDrawScope scope(consumed);
      assert(scope&&bindGxTexture(c,u,v,0)&&completions==quiet+1&&gxTextureBytes()==384);
      auto s=gxTextureRedecodeStatistics();
      assert(s.spareHeldWaits==before.spareHeldWaits+1&&s.spares==0&&s.spareReleases==before.spareReleases+1);
      const auto made=creates;
      assert(bindGxTexture(t,u,v,1)&&bindGxTexture(b,u,v,2)&&creates==made&&completions==quiet+1&&queuedIntact());
      assert(decoded(source,bindings[1]->data)&&decoded(other,bindings[2]->data));
      s=gxTextureRedecodeStatistics();
      assert(s.unfenced==before.unfenced+2&&s.fenced==before.fenced&&s.alpha6Waits==before.alpha6Waits+1);}
     gxTextureDecodeReport();
     assert(reportText.find("Native texture re-decode GPU waits taken=1 (0.00 ms, held=1) avoided=0 of 1 by the Alpha 6 rule")!=std::string::npos);
     assert(reportText.find("Native texture re-decode paths idle=0 spare=0 unfenced=2 fenced=0 unchanged=0; spares=0 bytes=0, allocated=0 released=1 refused=0\n")!=std::string::npos);
     frameBegin();destroyGxTextures();assert(allocations.empty());}
    // An eviction Alpha 6 took too (no spare involved) also ends the decode
    // fence: Alpha 6 waited twice here, now once.
    {assert(configureGxTextureCache(256,64));
     auto other=bti(4),third=bti(4);other[66]^=1;third[67]^=1;
     auto b=texture(70006,other),c=texture(70007,third);
     frameBegin();assert(bindGxTexture(t,u,v,0)&&bindGxTexture(b,u,v,1));draw(0);draw(1);
     source[55]^=0xff;other[68]^=0xff;invalidateGxTextures();
     reportText.clear();gxTextureDecodeReport();reportText.clear();
     const auto quiet=completions;const auto before=gxTextureRedecodeStatistics();
     {std::array<gx::Texture,2> consumed{c,t};GxTextureDrawScope scope(consumed);
      assert(scope&&bindGxTexture(c,u,v,2)&&completions==quiet+1&&gxTextureBytes()==256);
      assert(bindGxTexture(t,u,v,0)&&completions==quiet+1&&queuedIntact()&&decoded(source,bindings[0]->data));}
     const auto s=gxTextureRedecodeStatistics();
     assert(s.unfenced==before.unfenced+1&&s.spareHeldWaits==before.spareHeldWaits&&s.alpha6Waits==before.alpha6Waits+1);
     gxTextureDecodeReport();
     assert(reportText.find("Native texture re-decode GPU waits taken=0 (0.00 ms, held=0) avoided=1 of 1 by the Alpha 6 rule")!=std::string::npos);
     frameBegin();destroyGxTextures();assert(allocations.empty());}
    // The linear heap is short while a spare is in service. The render
    // owner's release waits once (held), frees the spare instead of an image,
    // and only then releases images, with no second wait.
    {assert(configureGxTextureCache(4096,64));
     dynamicWithSpare(t,source);
     const auto quiet=completions;const auto before=gxTextureRedecodeStatistics();const auto linear=gxTextureLinearEvictions();
     bool fenced=false;
     assert(releaseLeastNeededTexture(fenced)&&fenced&&completions==quiet+1&&gxTextureBytes()==128&&gxTextureLinearEvictions()==linear);
     const auto s=gxTextureRedecodeStatistics();assert(s.spareHeldWaits==before.spareHeldWaits+1&&s.spares==0);
     const auto made=creates;assert(bindGxTexture(t,u,v,0)&&creates==made&&completions==quiet+1);
     assert(releaseLeastNeededTexture(fenced)&&completions==quiet+1&&gxTextureBytes()==0&&gxTextureLinearEvictions()==linear+1);
     assert(!releaseLeastNeededTexture(fenced)&&completions==quiet+1);
     frameBegin();destroyGxTextures();assert(allocations.empty());}
    // The same inside a draw: a new image's allocation is refused once. One
    // wait (held) frees the spare and the allocation succeeds; t stays.
    {assert(configureGxTextureCache(4096,64));
     auto other=bti(4);other[69]^=1;auto c=texture(70008,other);
     dynamicWithSpare(t,source);
     const auto quiet=completions;const auto before=gxTextureRedecodeStatistics();const auto linear=gxTextureLinearEvictions();
     refuseCount=1;
     {GxTextureDrawScope scope({&c,1});assert(scope&&bindGxTexture(c,u,v,0));}
     assert(!refuseCount&&completions==quiet+1&&gxTextureBytes()==256&&gxTextureLinearEvictions()==linear&&queuedIntact());
     const auto s=gxTextureRedecodeStatistics();assert(s.spareHeldWaits==before.spareHeldWaits+1&&s.spares==0);
     const auto made=creates;assert(bindGxTexture(t,u,v,1)&&creates==made);
     frameBegin();destroyGxTextures();assert(allocations.empty());}
    assert(configureGxTextureCache(4*1024*1024,64));
    storageModel=false;
    std::puts("PASS dynamic re-decode: in place after frame begin, ping-pong spare without a wait, single fence when both storages are queued, report lines within the console limit, aging/pressure/budget/retirement of the spare, held eviction waits counted and freeing spares before images, eviction waits ending the decode fence, under a per-storage queue model");
}
// worldMips: storage gets levels only when the session chose the improvement,
// for a world use of an immutable image inside the budget; the switch is asked
// at every bind, so a reference picture samples the base level of the same
// storage again with nothing converted.
static void worldMipsSwitch(){
    using namespace dusk::native::features;
    destroyGxTextures();
    float u{},v{};
    const GxWorldMipUse world{true,true,0};
    auto rgb=bti(4,64,64);auto t=texture(61000,rgb,64,64);t.immutablePixels=true;t.minFilter=t.magFilter=1;
    chosen={};
    assert(bindGxTexture(t,u,v,0,false,world)&&bindings[0]->maxLevel==0&&gxTextureBytes()==8192&&gxWorldMipStatistics().images==0);
    destroyGxTextures();
    chosen[unsigned(Id::worldMips)]=true;
    const auto made=creates;
    assert(bindGxTexture(t,u,v,0,false,world)&&creates==made+1);
    const auto stats=gxWorldMipStatistics();
    assert(stats.images==1&&stats.bytes==2048+512+128&&gxTextureBytes()==8192+2688);
    C3D_Tex expected{};C3D_TexSetLodBias(&expected,-0.5f);
    assert(bindings[0]->maxLevel==3&&bindings[0]->minLevel==0&&bindings[0]->lodBias==expected.lodBias);
    assert(bindings[0]->param&GPU_TEXTURE_MIP_FILTER(GPU_LINEAR));
    const auto data=bindings[0]->data;const auto flushCount=flushes;
    // Level 1 of the stored chain is the box average of the base level.
    {NativeTexturePlan plan;assert(inspectNativeTexture(rgb,plan,false,false,true)&&planGeneratedMips(plan,0));
     std::vector<u8> want(plan.outputBytes);assert(decodeNativeTexture(rgb,plan,want));
     assert(std::equal(want.begin(),want.end(),static_cast<u8*>(data)));}
    setReference(true);
    assert(bindGxTexture(t,u,v,0,false,world)&&bindings[0]->data==data&&bindings[0]->maxLevel==0&&creates==made+1&&flushes==flushCount);
    assert(!(bindings[0]->param&GPU_TEXTURE_MIP_FILTER(GPU_LINEAR)));
    setReference(false);
    assert(bindGxTexture(t,u,v,0,false,world)&&bindings[0]->maxLevel==3&&creates==made+1&&flushes==flushCount);
    assert(bindGxTexture(t,u,v,1,false,{})&&bindings[1]->data==data&&bindings[1]->maxLevel==0); // not a world use
    assert(releaseGxTexture(t.handle)&&gxWorldMipStatistics().images==0&&gxWorldMipStatistics().bytes==0);
    // An image the game can rewrite never gets levels.
    auto m=texture(61001,rgb,64,64);
    assert(bindGxTexture(m,u,v,0,false,world)&&bindings[0]->maxLevel==0&&gxWorldMipStatistics().images==0);
    destroyGxTextures();
    // Under the linear floor: one level, no eviction.
    linearFree=3u<<20;
    assert(bindGxTexture(t,u,v,0,false,world)&&bindings[0]->maxLevel==0&&gxWorldMipStatistics().refusedLinear==1);
    linearFree=64u<<20;destroyGxTextures();
    // Multi-bit alpha (I4 -> LA4) needs the draw's alpha test as a threshold.
    auto i4=bti(0,64,64);auto a=texture(61002,i4,64,64);a.immutablePixels=true;
    assert(bindGxTexture(a,u,v,0,false,{true,false,0})&&bindings[0]->maxLevel==0&&gxWorldMipStatistics().refusedAlpha==1);
    assert(bindGxTexture(a,u,v,0,false,{true,true,128})&&bindings[0]->maxLevel==0); // resident: no conversion again
    destroyGxTextures();
    // The 768 KiB cap: 256x256 RGB565 images add 43,648 B each; 18 fit.
    assert(configureGxTextureCache(12*1024*1024,64));
    auto big=bti(4,256,256);std::vector<gx::Texture> images;
    for(unsigned n=0;n<19;++n){images.push_back(texture(61100+n,big,256,256));images.back().immutablePixels=true;}
    for(auto& image:images)assert(bindGxTexture(image,u,v,0,false,world));
    assert(gxWorldMipStatistics().images==18&&gxWorldMipStatistics().bytes==18*43648&&gxWorldMipStatistics().refusedCap==1);
    assert(gxTextureLinearEvictions()==0);
    destroyGxTextures();chosen={};
    std::puts("PASS world mips: levels only when chosen, for immutable world uses inside the cap and the linear floor; reference pictures sample the base level of the same storage with no conversion");
}
int main(){
    worldMipsSwitch();
    rgba565Reference();
    nativeRgb565();
    creates=flushes=deletes=binds=completions=0;bindings={};

    auto a=bti(0),b=bti(3);auto ta=texture(1,a),tb=texture(2,b);float u=0,v=0;
    assert(bindGxTexture(ta,u,v,0)&&u==1&&v==1&&creates==1&&flushes==1&&gxTextureBytes()==64);
    assert(!configureGxTextureCache(8*1024*1024,128));
    const auto first=bindings[0];const auto pixels=first->data;const auto nearest=first->param;
    ta.minFilter=ta.magFilter=1;assert(bindGxTexture(ta,u,v,1));
    assert(bindings[0]==first&&first->data==pixels&&bindings[1]->data==pixels&&bindings[1]!=bindings[0]);
    assert(first->param==nearest&&bindings[1]->param!=nearest&&creates==1&&flushes==1);
    assert(bindGxTexture(tb,u,v,2)&&gxTextureBytes()==192&&creates==2);
    const auto third=bindings[2];const auto thirdPixels=third->data;
    assert(bindGxTexture(tb,u,v,0)&&bindings[0]->data==thirdPixels&&bindings[1]->data==pixels&&creates==2);
    // Bound in 2D: the CPU vertex route switches unit 0 to the per-pixel
    // projection afterwards (setGxTextureProjection).
    assert(bindGxTexture(tb,u,v,0,true)&&C3D_TexGetType(bindings[0])==GPU_TEX_2D&&creates==2);
    assert(setGxTextureProjection(true)&&C3D_TexGetType(bindings[0])==GPU_TEX_PROJECTION&&creates==2);
    assert(C3D_TexGetType(bindings[1])==GPU_TEX_2D);
    const auto projectedBinds=binds;
    assert(!bindGxTexture(tb,u,v,1,true)&&binds==projectedBinds);
    assert(bindGxTexture(tb,u,v,0)&&C3D_TexGetType(bindings[0])==GPU_TEX_2D&&creates==2);
    const auto before=binds;assert(!bindGxTexture(ta,u,v,3)&&binds==before&&creates==2);
    auto bad=ta;bad.width=16;assert(!bindGxTexture(bad,u,v,1)&&binds==before);
    bad=ta;bad.wrapS=3;assert(!bindGxTexture(bad,u,v,1));
    bad=ta;bad.handle=3;refuseAllocation=true;assert(!bindGxTexture(bad,u,v,0)&&gxTextureBytes()==192);refuseAllocation=false;
    // Invalidation is lazy: reuse the allocation only when a live resource is
    // bound again. Both units observe its changed pixels, with independent
    // samplers. Repeated invalidations without a bind do not decode twice.
    const auto sharedParam=bindings[1]->param;
    const std::vector<u8> oldPixels(static_cast<u8*>(pixels),static_cast<u8*>(pixels)+64);
    std::fill(a.begin()+32,a.end(),0xf0);
    invalidateGxTextures();invalidateGxTextures();
    assert(flushes==2&&creates==2&&std::equal(oldPixels.begin(),oldPixels.end(),static_cast<u8*>(pixels)));
    assert(bindGxTexture(ta,u,v,0)&&bindings[0]->data==pixels&&bindings[1]->data==pixels);
    assert(flushes==3&&creates==2&&bindings[1]->param==sharedParam);
    NativeTexturePlan plan;assert(inspectNativeTexture(a,plan));std::vector<u8> expected(plan.outputBytes);
    assert(decodeNativeTexture(a,plan,expected)&&expected!=oldPixels);
    assert(std::equal(expected.begin(),expected.end(),static_cast<u8*>(pixels)));
    assert(bindGxTexture(ta,u,v,2)&&flushes==3&&creates==2);
    invalidateGxTextures();
    a[3]=16;const auto beforeInvalidBind=binds;
    assert(!bindGxTexture(ta,u,v,0)&&binds==beforeInvalidBind&&flushes==3&&creates==2);
    assert(std::equal(expected.begin(),expected.end(),static_cast<u8*>(pixels)));
    a[3]=8;std::fill(a.begin()+32,a.end(),0x35);
    assert(bindGxTexture(ta,u,v,0)&&flushes==4&&creates==2);
    assert(decodeNativeTexture(a,plan,expected)&&std::equal(expected.begin(),expected.end(),static_cast<u8*>(pixels)));
    for(unsigned n=0;n<100;++n)assert(bindGxTexture(ta,u,v,n%3));
    assert(creates==2&&flushes==4);destroyGxTextures();assert(allocations.empty()&&deletes==2&&gxTextureBytes()==0);
    // Invalidating an unused image never follows its former borrowed view.
    {auto ephemeral=bti(0);auto transient=texture(400,ephemeral);assert(bindGxTexture(transient,u,v,0));}
    invalidateGxTextures();destroyGxTextures();assert(allocations.empty());
    // Padded dimensions are exposed per binding, without duplicating pixels.
    auto odd=bti(0,5,7);auto to=texture(10,odd,5,7);assert(bindGxTexture(to,u,v,0)&&u==5.0f/8&&v==7.0f/8);to.wrapS=1;assert(!bindGxTexture(to,u,v,0));destroyGxTextures();
    // Opening/menu I4: 56x32 repeats only on its unpadded T axis. Padding
    // in S must not forbid that sampler. Exercise both orientations, repeat
    // and mirror, including subsequent cached bindings and rejected changes.
    for(unsigned axis=0;axis<2;++axis)for(unsigned wrap=1;wrap<=2;++wrap){
        const unsigned w=axis?32:56,h=axis?56:32;
        auto source=bti(0,w,h);auto t=texture(40,source,w,h);
        t.wrapS=axis?wrap:0;t.wrapT=axis?0:wrap;
        const auto previousCreates=creates;
        assert(bindGxTexture(t,u,v,0)&&u==float(w)/(axis?32:64)&&v==float(h)/(axis?64:32));
        assert(creates==previousCreates+1);
        NativeTexturePlan p;assert(inspectNativeTexture(source,p));std::vector<u8> decoded(p.outputBytes);
        assert(decodeNativeTexture(source,p,decoded));
        const auto data=bindings[0]->data;const auto originalParam=bindings[0]->param;
        assert(std::equal(decoded.begin(),decoded.end(),static_cast<u8*>(data)));
        C3D_Tex sampler=*bindings[0];const GPU_TEXTURE_WRAP_PARAM mode=wrap==1?GPU_REPEAT:GPU_MIRRORED_REPEAT;
        C3D_TexSetWrap(&sampler,axis?mode:GPU_CLAMP_TO_EDGE,axis?GPU_CLAMP_TO_EDGE:mode);
        assert(bindings[0]->param==sampler.param);
        t.wrapS=t.wrapT=0;
        assert(bindGxTexture(t,u,v,1)&&bindings[1]->data==data&&creates==previousCreates+1);
        assert(bindings[0]->param==originalParam&&bindings[1]->param!=originalParam);
        t.wrapS=axis?wrap:0;t.wrapT=axis?0:wrap;
        assert(bindGxTexture(t,u,v,2)&&bindings[2]->data==data&&bindings[2]->param==originalParam);
        t.wrapS=axis?0:wrap;t.wrapT=axis?wrap:0;
        const auto beforeReject=binds;
        assert(!bindGxTexture(t,u,v,2)&&binds==beforeReject&&creates==previousCreates+1);
        t.handle=41;
        assert(!bindGxTexture(t,u,v,2)&&binds==beforeReject&&creates==previousCreates+1);
        assert(std::equal(decoded.begin(),decoded.end(),static_cast<u8*>(data)));
        destroyGxTextures();
    }
    // PICA uses the original full 32/16/8 mip chain, wrap and LOD bias.
    {std::vector<u8> mip(32+(32*32+16*16+8*8)/2,0);mip[0]=0;mip[3]=mip[5]=32;mip[24]=3;mip[31]=32;
     auto tm=texture(30,mip,32,32);tm.mipCount=3;tm.minFilter=5;tm.maxLod=2;tm.lodBias=-1.5f;tm.wrapS=1;tm.wrapT=2;
     assert(bindGxTexture(tm,u,v,0)&&gxTextureBytes()==32*32+16*16+8*8&&bindings[0]->maxLevel==2);
     assert(bindings[0]->lodBias==(static_cast<unsigned>(-384)&0x1fff));
     auto allocationsBefore=creates;assert(bindGxTexture(tm,u,v,1)&&creates==allocationsBefore);
     auto mipPixels=bindings[0]->data;std::fill(mip.begin()+32,mip.end(),0xff);auto mipFlushes=flushes;
     invalidateGxTextures();assert(bindGxTexture(tm,u,v,2)&&creates==allocationsBefore&&flushes==mipFlushes+1&&bindings[2]->data==mipPixels);
     NativeTexturePlan mipPlan;assert(inspectNativeTexture(mip,mipPlan));std::vector<u8> mipExpected(mipPlan.outputBytes);
     assert(decodeNativeTexture(mip,mipPlan,mipExpected)&&std::equal(mipExpected.begin(),mipExpected.end(),static_cast<u8*>(mipPixels)));
     destroyGxTextures();}
    // Real CI decoding feeds the same GPU allocation/sampler path. Distinct
    // embedded palettes with identical indices preserve independent colors.
    {std::vector<u8> ci(66);ci[0]=8;ci[3]=ci[5]=8;ci[8]=1;ci[9]=1;ci[11]=1;ci[15]=64;ci[24]=1;ci[31]=32;ci[64]=0xf8;
     auto green=ci;green[64]=7;green[65]=0xe0;
     auto redTex=texture(501,ci),greenTex=texture(502,green);
     assert(bindGxTexture(redTex,u,v,0)&&gxTextureBytes()==256);const auto* red=static_cast<u8*>(bindings[0]->data);
     assert(red[0]==255&&red[1]==0&&red[2]==0&&red[3]==255);
     assert(bindGxTexture(greenTex,u,v,1)&&gxTextureBytes()==512);const auto* g=static_cast<u8*>(bindings[1]->data);
     assert(g[0]==255&&g[1]==0&&g[2]==255&&g[3]==0&&red!=g);
     assert(red[0]==255&&red[3]==255);destroyGxTextures();}
    // Immutable retained pixels survive global cache invalidation without
    // reconversion. Dynamic images and explicit updates keep their old path.
    auto immutable=ta;immutable.handle=9000;immutable.immutablePixels=true;
    assert(bindGxTexture(immutable,u,v,0));const auto immutableFlushes=flushes;
    const auto immutablePixels=bindings[0]->data;
    invalidateGxTextures();assert(bindGxTexture(immutable,u,v,1));
    assert(flushes==immutableFlushes&&bindings[1]->data==immutablePixels);
    auto inconsistent=immutable;inconsistent.immutablePixels=false;
    assert(!bindGxTexture(inconsistent,u,v,2));
    assert(invalidateGxTexture(immutable.handle)&&bindGxTexture(immutable,u,v,2)&&flushes==immutableFlushes+1);
    destroyGxTextures();
    // One legal 1024x1024 RGBA8 allocation fills the exact 4 MiB cache budget.
    auto huge=bti(6,1024,1024);auto th=texture(20,huge,1024,1024);
    assert(bindGxTexture(th,u,v,0)&&gxTextureBytes()==4*1024*1024);
    assert(!bindGxTexture(ta,u,v,1)&&gxTextureBytes()==4*1024*1024);destroyGxTextures();
    for(unsigned i=0;i<64;++i){ta.handle=100+i;assert(bindGxTexture(ta,u,v,i%3));}
    ta.handle=200;assert(!bindGxTexture(ta,u,v,0));destroyGxTextures();
    // Real pressure within a draw: protect all three consumed handles before
    // binding the new one. LRU must preserve even the oldest protected images.
    std::array<void*,64> pointers{};
    for(unsigned n=0;n<64;++n){ta.handle=1000+n;assert(bindGxTexture(ta,u,v,n%3));pointers[n]=bindings[n%3]->data;}
    std::array<gx::Texture,3> consumed{texture(2000,a),texture(1000,a),texture(1001,a)};
    // The draw comes in the next frame: images the current frame has not used
    // go first, oldest first (evictionCandidate).
    ++gx::textureFrameCounter;
    inFlight=true;const auto beforeEviction=deletes;
    {GxTextureDrawScope scope(consumed);assert(scope);
     GxTextureDrawScope nested(consumed);assert(!nested);
     assert(bindGxTexture(consumed[0],u,v,0));
     assert(!inFlight&&deletes==beforeEviction+1&&!allocations.contains(pointers[2]));
     assert(allocations.contains(pointers[0])&&allocations.contains(pointers[1])&&gxTextureBytes()==64*64);
     assert(bindGxTexture(consumed[1],u,v,1)&&bindings[1]->data==pointers[0]);
     assert(bindGxTexture(consumed[2],u,v,2)&&bindings[2]->data==pointers[1]);
     assert(!releaseGxTexture(consumed[1].handle)&&allocations.contains(pointers[0]));}
    // A failed actual completion boundary changes no cache allocation/binding.
    auto incoming=texture(2001,a);const auto allocatedBefore=allocations.size();const auto deletedBefore=deletes;
    const auto boundBefore=binds;inFlight=true;completion=GpuCompletion::timedOut;
    {GxTextureDrawScope scope(std::span(&incoming,1));assert(scope);assert(!bindGxTexture(incoming,u,v,0));}
    assert(inFlight&&deletes==deletedBefore&&allocations.size()==allocatedBefore&&binds==boundBefore);
    completion=GpuCompletion::complete;
    {GxTextureDrawScope scope(std::span(&incoming,1));assert(scope);assert(bindGxTexture(incoming,u,v,0));}
    assert(!inFlight&&gxTextureBytes()==64*64);
    // Consumed working-set overflow rejects before any completion/eviction.
    std::array<gx::Texture,2> excessive{th,incoming};const auto waitsBefore=completions;
    {GxTextureDrawScope scope(excessive);assert(!scope);}
    assert(completions==waitsBefore&&deletes==deletedBefore+1);destroyGxTextures();
    // Byte pressure can retire a whole prior image and repair every unused
    // sampler with a real stable service allocation, without dereferencing old
    // borrowed CPU views. That service image is never a consumed original image.
    {auto expired=bti(6,1024,1024);auto old=texture(3000,expired,1024,1024);assert(bindGxTexture(old,u,v,0));}
    inFlight=true;incoming.handle=3001;
    {GxTextureDrawScope scope(std::span(&incoming,1));assert(scope);assert(bindGxTexture(incoming,u,v,1));}
    assert(!inFlight&&gxTextureBytes()==64&&gxTextureIdleBytes()==256);
    for(const auto* binding:bindings)if(binding&&binding->data)assert(allocations.contains(binding->data));
    assert(releaseGxTexture(incoming.handle)&&gxTextureBytes()==0&&gxTextureIdleBytes()==256);
    destroyGxTextures();assert(gxTextureIdleBytes()==0);
    assert(!configureGxTextureCache(12*1024*1024+1,128));
    assert(!configureGxTextureCache(8*1024*1024,257)); // 256 slots since Alpha 12
    assert(configureGxTextureCache(8*1024*1024,128));
    auto huge2=th;huge2.handle=8000;
    assert(bindGxTexture(th,u,v,0)&&bindGxTexture(huge2,u,v,1)&&gxTextureBytes()==8*1024*1024);
    assert(!bindGxTexture(ta,u,v,2));destroyGxTextures();
    assert(configureGxTextureCache(8*1024*1024,128));
    for(unsigned i=0;i<128;++i){ta.handle=10000+i;assert(bindGxTexture(ta,u,v,i%3));}
    ta.handle=11000;assert(!bindGxTexture(ta,u,v,0));
    inFlight=true;
    {GxTextureDrawScope scope(std::span(&ta,1));assert(scope&&bindGxTexture(ta,u,v,0));}
    assert(!inFlight&&gxTextureBytes()==128*64);destroyGxTextures();
    assert(configureGxTextureCache(10*1024*1024,128));
    auto medium=bti(6,1024,512);auto tm=texture(12000,medium,1024,512);
    assert(bindGxTexture(th,u,v,0)&&bindGxTexture(huge2,u,v,1)&&bindGxTexture(tm,u,v,2));
    assert(gxTextureBytes()==10*1024*1024&&!bindGxTexture(ta,u,v,0));
    destroyGxTextures();
    assert(allocations.empty()&&creates==deletes);
    std::printf("PASS three-unit sampler independence, shared pixels, lazy invalidation/reconversion without reallocation, mip refresh, expired unused views, descriptor rejection, exact 4 MiB budget, 64 slots, failure and teardown (%u allocations)\n",creates);
    pingPong();
}
