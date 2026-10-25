// Reuse the existing host stub fixture, copied without changing its source.
// Host lifecycle/sampler test using actual installed Citro3D structs and inline
// packers. GPU allocation/bind/flush are substituted; no GPU execution claim.
#include "../gx_texture_pica.hpp"
#include "host_fused_copy_off.hpp"
#include "../runtime/texture_gx.hpp"
#include "../runtime/gpu_sync.hpp"
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
using namespace dusk::native;
static std::map<void*,std::size_t> allocations;
static std::array<C3D_Tex*,3> bindings{};
static unsigned creates,flushes,deletes,binds;
static bool refuseAllocation;
static bool inFlight{};
static unsigned completions{};
static GpuCompletion completion=GpuCompletion::complete;
namespace dusk::native {
GpuCompletion platformCompleteGpu(std::uint64_t timeout){assert(timeout==500000000);++completions;if(completion==GpuCompletion::complete)inFlight=false;return completion;}
}
bool C3D_TexInitWithParams(C3D_Tex* tex,C3D_TexCube* cube,C3D_TexInitParams p){
    if(refuseAllocation)return false;
    assert(!cube&&p.type==GPU_TEX_2D&&!p.onVram);
    unsigned bpp=0;switch(p.format){case GPU_RGBA8:bpp=4;break;case GPU_RGB565:case GPU_LA8:bpp=2;break;case GPU_LA4:bpp=1;break;default:assert(false);}
    *tex={};tex->width=p.width;tex->height=p.height;tex->fmt=p.format;tex->size=p.width*p.height*bpp;
    tex->maxLevel=p.maxLevel;const auto bytes=C3D_TexCalcTotalSize(tex->size,p.maxLevel);
    tex->data=std::calloc(bytes,1);assert(tex->data);allocations[tex->data]=bytes;++creates;return true;
}
void C3D_TexDelete(C3D_Tex* tex){
    assert(!inFlight);for(const auto* binding:bindings)if(binding)assert(binding->data!=tex->data);
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
static std::vector<u8> mipSource(bool cmpr){
    auto source=bti(4,32,32);source.resize(32+(32*32+16*16+8*8)*2);source[24]=3;
    for(unsigned n=32;n<source.size();++n)source[n]=n%251;
    if(cmpr){
        source.resize(32+(32*32+16*16+8*8)/2);source[0]=14;
        for(unsigned n=32;n<source.size();n+=8){
            source[n]=0xf8;source[n+1]=0;source[n+2]=0;source[n+3]=0x1f;
            source[n+4]=0x1b;source[n+5]=0xe4;source[n+6]=0x5a;source[n+7]=0xa5;
        }
    }
    return source;
}
static gx::Texture halfTexture(gx::TextureHandle handle,const std::vector<u8>& source){
    auto t=texture(handle,source,32,32);t.mipCount=3;t.maxLod=2;t.minFilter=5;t.immutablePixels=true;return t;
}
static void residency(std::size_t policy,std::size_t images,std::size_t actual,std::size_t full){
    const auto state=gxTextureHalfStatus();
    assert(state.policyEntries==policy&&state.residentImages==images&&
        state.residentBytes==actual&&state.sameImagesFullBytes==full);
}
static void run(bool cmpr){
    const std::size_t reduced=cmpr?1280:640,full=cmpr?5376:2688;
    const auto format=cmpr?GPU_RGBA8:GPU_RGB565;
    auto source=mipSource(cmpr);auto original=source;auto t=halfTexture(1,source);float u{},v{};
    NativeTextureFingerprint digest;assert(nativeTextureFingerprint(source,digest));
    auto policy=digest;
    assert(configureGxTextureCache(reduced,3,false,{&policy,1}));policy={}; // copied configuration
    const auto initialCreates=creates,initialBinds=binds;
    assert(!bindGxTexture(t,u,v,0,true)&&creates==initialCreates&&binds==initialBinds&&gxTextureBytes()==0);
    residency(1,0,0,0);
    {GxTextureDrawScope scope({&t,1});assert(scope&&bindGxTexture(t,u,v));}
    assert(u==1&&v==1&&bindings[0]->width==16&&bindings[0]->height==16&&bindings[0]->size==(cmpr?1024u:512u));
    assert(bindings[0]->fmt==format&&gxTextureBytes()==reduced&&allocations.at(bindings[0]->data)==reduced);
    residency(1,1,reduced,full);
    NativeTexturePlan p;assert(inspectNativeTexture(source,p,false,true));std::vector<u8> expected(p.outputBytes);
    assert(decodeNativeTexture(source,p,expected)&&std::equal(expected.begin(),expected.end(),static_cast<u8*>(bindings[0]->data)));
    const auto data=bindings[0]->data;const auto count=creates;const auto uploaded=flushes;
    if(cmpr){
        // Deliberately violate the fixture's immutable contract to prove that
        // clean preflight/bind performs no opacity scan/hash or upload. Restore
        // immediately; actual changes require explicit invalidation below.
        std::fill_n(source.begin()+32,4,0);source[36]=0xff;
        {GxTextureDrawScope scope({&t,1});assert(scope&&bindGxTexture(t,u,v));}
        assert(flushes==uploaded&&creates==count);
        source=original;
    }
    assert(!configureGxTextureCache(8192,3));
    for(unsigned min=0;min<3;++min)for(unsigned max=min;max<3;++max)
        for(unsigned filter=0;filter<6;++filter)for(float bias:{-3.5f,0.f,3.5f}){
            t.minLod=min;t.maxLod=max;t.minFilter=filter;t.lodBias=bias;t.wrapS=1;t.wrapT=2;
            assert(bindGxTexture(t,u,v,1)&&bindings[1]->data==data&&creates==count&&flushes==uploaded&&u==1&&v==1);
            assert(bindings[1]->minLevel==(min?min-1:0));
            assert(bindings[1]->maxLevel==(filter<2?0:max?max-1:0));
            assert(bindings[1]->lodBias==(static_cast<unsigned>(int(bias*256))&0x1fff));
        }
    t.minLod=0;t.maxLod=2;t.minFilter=5;
    auto bad=t;bad.width=16;const auto beforeBad=binds;assert(!bindGxTexture(bad,u,v)&&binds==beforeBad);
    bad=t;bad.immutablePixels=false;assert(!bindGxTexture(bad,u,v)&&binds==beforeBad);
    assert(!bindGxTexture(t,u,v,0,true)&&binds==beforeBad&&gxTextureBytes()==reduced);
    invalidateGxTextures();assert(bindGxTexture(t,u,v,2)&&flushes==uploaded);
    // Explicit invalidation rehashes all original pixels, including dropped L0.
    source[32]^=1;assert(invalidateGxTexture(t.handle));const auto beforeRefresh=binds;
    assert(!bindGxTexture(t,u,v)&&binds==beforeRefresh&&flushes==uploaded&&creates==count);
    residency(1,1,reduced,full);
    assert(std::equal(expected.begin(),expected.end(),static_cast<u8*>(data)));
    source=original;assert(bindGxTexture(t,u,v)&&flushes==uploaded+1&&creates==count);
    // Protection is based on original identity, not the selected mip number.
    auto next=t;next.handle=2;std::array<gx::Texture,2> excessive{t,next};const auto waits=completions;
    {GxTextureDrawScope scope(excessive);assert(!scope);}
    assert(completions==waits&&gxTextureBytes()==reduced);
    {GxTextureDrawScope scope({&t,1});assert(scope&&!releaseGxTexture(t.handle));}
    // Failed GPU fence leaves allocation and bindings live. Successful retry
    // retires the old half allocation and uploads the next at the exact budget.
    inFlight=true;completion=GpuCompletion::timedOut;const auto deleted=deletes;
    {GxTextureDrawScope scope({&next,1});assert(scope&&!bindGxTexture(next,u,v,1));}
    assert(inFlight&&deletes==deleted&&gxTextureBytes()==reduced);
    residency(1,1,reduced,full);
    completion=GpuCompletion::complete;
    {GxTextureDrawScope scope({&next,1});assert(scope&&bindGxTexture(next,u,v,1));}
    assert(!inFlight&&deletes==deleted+1&&gxTextureBytes()==reduced&&gxTextureIdleBytes()==256);
    residency(1,1,reduced,full);
    assert(releaseGxTexture(next.handle)&&gxTextureBytes()==0);
    residency(1,0,0,0);
    assert(!configureGxTextureCache(8192,3)); // idle binding still lives
    destroyGxTextures();assert(allocations.empty());
    residency(0,0,0,0);
    // Frozen reference flag remains independent of resolution (fourth arg).
    assert(configureGxTextureCache(1280,3,true,{&digest,1}));
    assert(bindGxTexture(t,u,v)&&bindings[0]->fmt==GPU_RGBA8&&gxTextureBytes()==1280&&bindings[0]->width==16);
    residency(1,1,1280,5376);
    destroyGxTextures();
    // Empty allowlist is the negative control: same immutable bytes stay full.
    assert(configureGxTextureCache(8192,3,false));
    assert(bindGxTexture(t,u,v)&&bindings[0]->width==32&&bindings[0]->maxLevel==2&&gxTextureBytes()==full);
    residency(0,0,0,0);
    destroyGxTextures();
    auto wrong=digest;wrong[0]^=1;assert(configureGxTextureCache(8192,3,false,{&wrong,1}));
    assert(bindGxTexture(t,u,v)&&bindings[0]->width==32&&gxTextureBytes()==full);destroyGxTextures();
    // RGB565 keeps its header-alpha rule. CMPR uses actual logical opacity,
    // independent of the header declaring alpha capability.
    source[1]=1;assert(configureGxTextureCache(8192,3,false,{&digest,1}));
    assert(bindGxTexture(t,u,v)&&bindings[0]->width==(cmpr?16u:32u)&&gxTextureBytes()==(cmpr?reduced:full));
    destroyGxTextures();source[1]=0;
    // An exact fingerprint cannot turn mutable captures/shadows into candidates.
    assert(configureGxTextureCache(8192,3,false,{&digest,1}));t.immutablePixels=false;
    assert(bindGxTexture(t,u,v,0,true)&&bindings[0]->width==32&&gxTextureBytes()==full);
    source[32]^=1;invalidateGxTextures();assert(bindGxTexture(t,u,v)&&gxTextureBytes()==full);
    destroyGxTextures();source=original;t.immutablePixels=true;
    // A canonical alias with relocated pixels matches the same content policy.
    auto alias=source;alias.insert(alias.begin()+32,7,0);alias[31]=39;
    auto aliasTexture=halfTexture(3,alias);assert(configureGxTextureCache(reduced,3,false,{&digest,1}));
    assert(bindGxTexture(aliasTexture,u,v)&&gxTextureBytes()==reduced&&bindings[0]->width==16);
    if(cmpr){
        const auto data=bindings[0]->data;const auto saved=std::vector<u8>(static_cast<u8*>(data),static_cast<u8*>(data)+reduced);
        const auto uploaded=flushes,bound=binds;
        for(unsigned at:{39u,39u+512u,39u+640u}){
            auto changed=alias;changed[at]=changed[at+1]=changed[at+2]=changed[at+3]=0;changed[at+4]=0xff;
            auto invalid=halfTexture(3,changed);assert(invalidateGxTexture(3));
            assert(!bindGxTexture(invalid,u,v)&&flushes==uploaded&&binds==bound);
            assert(std::equal(saved.begin(),saved.end(),static_cast<u8*>(data)));
            residency(1,1,reduced,full);
        }
        assert(bindGxTexture(aliasTexture,u,v)&&flushes==uploaded+1);
    }
    destroyGxTextures();
    // Oversize policy rejection is transactional; the earlier valid list stays.
    assert(configureGxTextureCache(reduced,3,false,{&digest,1}));std::array<NativeTextureFingerprint,65> tooMany{};
    assert(!configureGxTextureCache(8192,3,true,tooMany));
    refuseAllocation=true;assert(!bindGxTexture(t,u,v)&&gxTextureBytes()==0);refuseAllocation=false;
    assert(bindGxTexture(t,u,v)&&gxTextureBytes()==reduced&&bindings[0]->fmt==format);
    destroyGxTextures();assert(allocations.empty()&&creates==deletes&&source==original);
    // Destroy resets policy; third bool alone keeps its original reference role.
    assert(configureGxTextureCache(8192,3,true));
    assert(bindGxTexture(t,u,v)&&gxTextureBytes()==5376&&bindings[0]->width==32&&bindings[0]->fmt==GPU_RGBA8);
    destroyGxTextures();assert(allocations.empty()&&creates==deletes);
    std::puts("PASS half cache: exact effective budget/preflight, original identity/descriptors, normalized UV, LOD rebasing/bias, policy freeze/reset/copy, negative controls, refresh atomicity, projected fail-closed, protected eviction/fence/retirement");
}

int main(){run(false);run(true);}
