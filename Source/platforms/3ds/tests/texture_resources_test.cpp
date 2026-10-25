#include "../runtime/texture_resources.hpp"
#include "../runtime/gx_state.hpp"
#include "../runtime/gpu_sync.hpp"
#include "../runtime/texture_gx.hpp"
#include <dolphin/os.h>
#include <array>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
namespace {
std::recursive_mutex mutex;
thread_local bool interrupts=true;
thread_local OSThread thread{};
OSThread* owner{};
unsigned completions{},deletions{};
bool refuseDeletion{};
void word(unsigned char* p,unsigned v){for(unsigned i=0;i<4;++i)p[i]=v>>(24-i*8);}
void half(unsigned char* p,unsigned v){p[0]=v>>8;p[1]=v;}
auto archive(){
    auto bytes=std::make_unique<std::array<unsigned char,256>>();auto& b=*bytes;
    auto w=[&](unsigned at,unsigned v){word(b.data()+at,v);};
    std::memcpy(b.data(),"RARC",4);w(4,256);w(8,32);w(12,160);w(16,64);w(20,64);
    w(32,1);w(36,32);w(40,3);w(44,48);w(48,32);w(52,128);
    std::memcpy(b.data()+64,"ROOT",4);half(b.data()+74,3);
    w(84,0x02000005);w(104,0x02000007);w(108,UINT32_MAX);
    w(124,0x1100000a);w(132,64);
    const char names[]="root\0.\0..\0sample.bti\0";std::memcpy(b.data()+160,names,sizeof(names));
    b[192]=3;half(b.data()+194,4);half(b.data()+196,4);
    b[212]=b[213]=1;b[216]=1;w(220,32);
    for(unsigned i=224;i<256;i+=2){b[i]=0x7f;b[i+1]=0x42;}
    return bytes;
}
}
extern "C" {
OSThread* OSGetCurrentThread(){return &thread;}
BOOL OSDisableInterrupts(){const auto previous=interrupts;if(previous){mutex.lock();interrupts=false;}return previous;}
BOOL OSRestoreInterrupts(BOOL enabled){const auto previous=interrupts;if(enabled&&!interrupts){interrupts=true;mutex.unlock();}return previous;}
void OSReport(const char*,...){}
}
namespace dusk::native {
OSThread* platformGpuOwner(){return owner;}
GpuCompletion platformCompleteGpu(std::uint64_t){++completions;return owner==&thread?GpuCompletion::complete:GpuCompletion::wrongThread;}
bool releaseGxTexture(gx::TextureHandle){if(refuseDeletion)return false;++deletions;return true;}
}
int main(){
    using namespace dusk::native;owner=&thread;
    int archiveOwner{},a{},b{};auto source=archive();
    assert(registerTextureArchive(&archiveOwner,*source));
    const auto* header=source->data()+192;const auto* identity=source->data()+224;
    assert(!retainArchiveTexture(&a,reinterpret_cast<void*>(1)));
    assert(!retainArchiveTexture(&a,source->data()+255));
    std::array<unsigned char,64> object{};
    gx::initTextureObject(object.data(),identity,4,4,3,0,0,0,false);
    gx::setTextureObjectLod(object.data(),1,1,0,0,0,0,0,0);
    assert(retainArchiveTexture(&a,header)&&textureResourceBytes()==64);
    assert(retainArchiveTexture(&a,header)&&textureResourceBytes()==64);
    assert(retainArchiveTexture(&b,header)&&textureResourceBytes()==64);
    gx::loadTextureObject(object.data(),0);gx::Texture texture{};
    assert(gx::textureBinding(0,texture)&&texture.immutablePixels);const auto handle=texture.handle;
    unregisterTextureArchive(&archiveOwner);source.reset();
    assert(!retainArchiveTexture(&a,header,handle)); // No stale archive read.
    NativeTexturePlan plan;assert(inspectNativeTexture(texture.bti,plan));
    std::vector<unsigned char> pixels(plan.outputBytes);
    assert(decodeNativeTexture(texture.bti,plan,pixels)&&pixels[0]==0x7f&&pixels[1]==0x42);
    std::thread release([&]{releaseTextureResources(&a);assert(!collectTextureResources());});release.join();
    assert(textureResourceBytes()==64&&deletions==0);
    assert(collectTextureResources()&&textureResourceBytes()==64&&deletions==0);
    releaseTextureResources(&b);assert(collectTextureResources()&&textureResourceBytes()==0&&deletions==1);
    assert(!gx::textureBinding(0,texture));
    assert(gx::queryTextureObject(object.data(),texture)&&texture.width==4&&!texture.handle);
    // Reattach the existing object's sampler to a new retained allocation.
    auto next=archive();assert(registerTextureArchive(&archiveOwner,*next));
    gx::initTextureObject(object.data(),next->data()+224,4,4,3,0,0,0,false);
    gx::setTextureObjectLod(object.data(),1,1,0,0,0,0,0,0);
    assert(retainArchiveTexture(&a,next->data()+192));
    gx::loadTextureObject(object.data(),0);assert(gx::textureBinding(0,texture)&&texture.handle!=handle);
    (*next)[224]=0x2f;assert(!retainArchiveTexture(&b,next->data()+192));
    releaseTextureResources(&a);assert(collectTextureResources());unregisterTextureArchive(&archiveOwner);
    assert(!registerTextureArchive(&archiveOwner,std::span(next->data(),255)));
    // External GX-copy handles use their own provider; no arbitrary BTI read.
    assert(!retainArchiveTexture(&a,reinterpret_cast<void*>(1),UINT64_MAX));
    gx::TextureHandle copyHandle{};
    assert(gx::registerTexture(std::span(next->data()+192,64),copyHandle));
    unsigned copyToken{};assert(gx::textureCommandToken(next->data()+224,copyToken));
    assert(gx::textureFromCommandToken(copyToken,texture)&&!texture.immutablePixels);
    assert(retainArchiveTexture(&a,reinterpret_cast<void*>(1),copyHandle));
    assert(gx::unregisterTexture(copyHandle));
    assert(textureResourceBytes()==0&&deletions==2);
    // A shared original image can have independent sampler headers. It still
    // rejects a changed decode format or changed immutable pixel bytes.
    assert(registerTextureArchive(&archiveOwner,*next));
    assert(retainArchiveTexture(&a,next->data()+192));
    (*next)[198]=1;(*next)[199]=2;(*next)[213]=0;
    assert(retainArchiveTexture(&b,next->data()+192)&&textureResourceBytes()==64);
    gx::initTextureObject(object.data(),next->data()+224,4,4,3,1,2,0,false);
    gx::setTextureObjectLod(object.data(),1,0,0,0,0,0,0,0);
    gx::loadTextureObject(object.data(),0);assert(gx::textureBinding(0,texture));
    assert(texture.wrapS==1&&texture.wrapT==2&&texture.minFilter==1&&texture.magFilter==0);
    int incompatible{};(*next)[192]=4;
    assert(!retainArchiveTexture(&incompatible,next->data()+192));(*next)[192]=3;
    releaseTextureResources(&a);releaseTextureResources(&b);
    assert(collectTextureResources()&&textureResourceBytes()==0);unregisterTextureArchive(&archiveOwner);
    // Full-startup image/owner metadata limits are independent of the 4 MiB
    // texel budget. Exhaustion must preserve every old token and permit reuse.
    std::vector<std::array<unsigned char,64>> many(513);
    std::vector<unsigned> owners(2049);
    for(auto& source:many){source[0]=3;source[3]=source[5]=4;source[20]=source[21]=1;source[24]=1;source[31]=32;}
    unsigned token{};gx::resetState();
    for(unsigned n=0;n<512;++n)assert(retainTextureResource(&owners[n],many[n]));
    assert(textureResourceBytes()==512*64);
    assert(gx::textureCommandToken(many[0].data()+32,token));
    assert(!retainTextureResource(&owners[512],many[512]));
    assert(!std::strcmp(textureResourceFailure(),"texture resource image slots exhausted"));
    assert(gx::textureFromCommandToken(token,texture));
    for(unsigned n=0;n<512;++n)releaseTextureResources(&owners[n]);
    assert(collectTextureResources()&&textureResourceBytes()==0&&!gx::textureFromCommandToken(token,texture));
    for(unsigned n=0;n<2048;++n)assert(retainTextureResource(&owners[n],many[0]));
    assert(textureResourceBytes()==64&&!retainTextureResource(&owners[2048],many[0]));
    assert(!std::strcmp(textureResourceFailure(),"texture resource owners exhausted"));
    for(unsigned n=0;n<2048;++n)releaseTextureResources(&owners[n]);
    assert(collectTextureResources()&&textureResourceBytes()==0);
    assert(retainTextureResource(&owners[0],many[512]));releaseTextureResources(&owners[0]);assert(collectTextureResources()&&textureResourceBytes()==0);
    std::puts("Full startup pool: 512 image slots / 2048 owners, independent byte budget, preserved tokens on overflow, full retirement and reuse passed");
    std::array<unsigned char,2048> grass{},flower{};std::fill(grass.begin(),grass.end(),0x3b);std::fill(flower.begin(),flower.end(),0x79);
    assert(!retainRawTextureResource(&owners[0],grass,64,64,5)); // Wrong true pixel extent.
    assert(!retainRawTextureResource(&owners[0],{},32,32,5));
    assert(retainRawTextureResource(&owners[0],grass,32,32,5));
    assert(retainRawTextureResource(&owners[1],grass,32,32,5)&&textureResourceBytes()==2080);
    assert(gx::textureCommandToken(grass.data(),token)&&gx::textureFromCommandToken(token,texture));
    assert(texture.width==32&&texture.height==32&&texture.format==5&&texture.image.front()==0x3b);
    assert(retainRawTextureResource(&owners[2],flower,64,64,14));
    assert(gx::textureCommandToken(flower.data(),token)&&gx::textureFromCommandToken(token,texture));
    assert(inspectNativeTexture(texture.bti,plan));pixels.resize(plan.outputBytes);assert(decodeNativeTexture(texture.bti,plan,pixels));
    for(unsigned n=0;n<3;++n)releaseTextureResources(&owners[n]);assert(collectTextureResources()&&textureResourceBytes()==0);
    std::puts("Explicit bare GX RGB5A3/CMPR arrays: real bytes, alias sharing, bounded metadata, conversion and owner retirement passed");
    std::vector<unsigned char> ci(96);ci[0]=8;ci[3]=ci[5]=8;ci[8]=1;ci[9]=2;ci[11]=16;
    ci[15]=64;ci[20]=ci[21]=ci[24]=1;ci[31]=32;
    for(unsigned n=32;n<96;++n)ci[n]=n*73;
    std::array<unsigned char,40> tlut{};
    gx::initTextureObject(object.data(),ci.data()+32,8,8,8,0,0,0,true,0);
    gx::initTlutObject(tlut.data(),ci.data()+64,2,16);
    assert(retainTextureResource(&owners[0],ci)&&textureResourceBytes()==96);
    gx::setTextureObjectLod(object.data(),1,1,0,0,0,0,0,0);
    gx::loadTlutObject(tlut.data(),0);gx::loadTextureObject(object.data(),0);
    assert(gx::textureBinding(0,texture)&&texture.bti[8]&&texture.bti[9]==2);
    assert(gx::paletteCommandToken(ci.data()+64,token));const auto paletteToken=token;
    auto sharedPalette=ci;
    assert(retainTextureResource(&owners[1],sharedPalette,ci.data()+32)&&textureResourceBytes()==96);
    unsigned secondPaletteToken{};assert(gx::paletteCommandToken(sharedPalette.data()+64,secondPaletteToken)&&secondPaletteToken==paletteToken);
    // Fill the bounded identity table without reading any synthetic identity.
    // Overflow must preserve both real original aliases and the old token.
    for(unsigned n=0;n<2046;++n)assert(gx::aliasTexturePalette(texture.handle,reinterpret_cast<void*>(std::uintptr_t(0x10000+n*32))));
    assert(!gx::aliasTexturePalette(texture.handle,reinterpret_cast<void*>(std::uintptr_t(0x10000+2046*32))));
    assert(gx::paletteCommandToken(ci.data()+64,token)&&token==paletteToken);
    assert(gx::paletteCommandToken(sharedPalette.data()+64,token)&&token==paletteToken);
    assert(inspectNativeTexture(texture.bti,plan));pixels.resize(plan.outputBytes);assert(decodeNativeTexture(texture.bti,plan,pixels));
    const auto retainedPixels=pixels;ci.clear();ci.shrink_to_fit();
    gx::loadTlutObject(tlut.data(),0);gx::loadTextureObject(object.data(),0);
    assert(gx::textureBinding(0,texture)&&decodeNativeTexture(texture.bti,plan,pixels)&&pixels==retainedPixels);
    releaseTextureResources(&owners[0]);assert(collectTextureResources()&&textureResourceBytes()==96);
    assert(gx::textureFromCommandToken(paletteToken,texture));
    releaseTextureResources(&owners[1]);assert(collectTextureResources()&&textureResourceBytes()==0);
    assert(!gx::textureFromCommandToken(paletteToken,texture));
    assert(!gx::paletteCommandToken(sharedPalette.data()+64,token));
    gx::destroyTlutObject(tlut.data());gx::destroyTextureObject(object.data());
    std::array<unsigned char,160> overlap{};overlap[0]=8;overlap[3]=overlap[5]=8;overlap[8]=1;overlap[9]=2;overlap[11]=16;overlap[15]=96;overlap[24]=1;overlap[31]=32;
    overlap[96]=3;overlap[99]=overlap[101]=4;overlap[120]=1;overlap[127]=32;
    gx::TextureHandle first{},second{};assert(gx::registerTexture(overlap,first));
    assert(!gx::registerTexture(std::span(overlap).subspan(96),second)&&!second);
    assert(gx::paletteCommandToken(overlap.data()+96,token)&&gx::textureFromCommandToken(token,texture));
    assert(gx::unregisterTexture(first));
    assert(retainRawTextureResource(&owners[0],grass,32,32,5));
    assert(gx::textureCommandToken(grass.data(),token));const auto retryToken=token;
    releaseTextureResources(&owners[0]);refuseDeletion=true;
    assert(!collectTextureResources()&&textureResourceBytes()==2080&&gx::textureFromCommandToken(retryToken,texture));
    refuseDeletion=false;assert(collectTextureResources()&&textureResourceBytes()==0&&!gx::textureFromCommandToken(retryToken,texture));
    std::puts("Embedded CI/TLUT ownership: real retained indices/palette, typed selection, actual colors after source free, stale token retirement passed");
    // Weather GX objects share the owning archive's lifetime, including cached
    // object reuse after a different archive reoccupies exactly the same bytes.
    gx::resetState();auto weather=archive();unsigned previousToken{};
    assert(!retainArchiveTextureForLifetime(reinterpret_cast<void*>(1)));
    for(unsigned cycle=0;cycle<1200;++cycle){
        (*weather)[225]=cycle;
        assert(registerTextureArchive(&archiveOwner,*weather));
        gx::initTextureObject(object.data(),weather->data()+224,4,4,3,0,0,0,false);
        assert(retainArchiveTextureForLifetime(weather->data()+192));
        const auto beforeCompletion=completions;
        assert(retainArchiveTextureForLifetime(weather->data()+192));
        assert(completions==beforeCompletion&&textureResourceBytes()==64);
        gx::loadTextureObject(object.data(),0);assert(gx::textureBinding(0,texture));
        assert(texture.image[1]==static_cast<unsigned char>(cycle));
        assert(gx::textureCommandToken(weather->data()+224,token)&&token!=previousToken);
        previousToken=token;
        std::thread unmount([&]{unregisterTextureArchive(&archiveOwner);});unmount.join();
        assert(!retainArchiveTextureForLifetime(weather->data()+192,texture.handle));
        assert(collectTextureResources()&&textureResourceBytes()==0);
        assert(!gx::textureFromCommandToken(token,texture)&&!gx::textureBinding(0,texture));
    }
    unregisterTextureArchive(nullptr);assert(collectTextureResources());
    assert(gx::registerTexture(std::span(weather->data()+192,64),copyHandle));
    assert(retainArchiveTextureForLifetime(reinterpret_cast<void*>(1),copyHandle));
    assert(gx::unregisterTexture(copyHandle));
    std::puts("Archive-lifetime weather textures: 1200 mount/reuse/unmount cycles, exact pixels, worker retirement, no cache-hit fence and valid external captures passed");
    std::printf("Validated archive extents, owned pixel lifetime after archive free, shared owners, worker retirement, stale identity rejection and sampler reattachment (%u completions)\n",completions);
}
