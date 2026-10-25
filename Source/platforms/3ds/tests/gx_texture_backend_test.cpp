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
int main(){
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
    assert(bindGxTexture(tb,u,v,0,true)&&C3D_TexGetType(bindings[0])==GPU_TEX_PROJECTION&&creates==2);
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
    assert(!configureGxTextureCache(10*1024*1024+1,128));
    assert(!configureGxTextureCache(8*1024*1024,129));
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
}
