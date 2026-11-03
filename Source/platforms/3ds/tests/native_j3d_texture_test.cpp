#include "../runtime/bootstrap.hpp"
#include "../runtime/clock.hpp"
#include "../runtime/gx_state.hpp"
#include "../runtime/gx_texture_commands.hpp"
#include "../runtime/gx_stream.hpp"
#include <JSystem/J3DGraphBase/J3DGD.h>
#include <dolphin/gd.h>
#include "../runtime/texture_gx.hpp"
#include "../runtime/texture_resources.hpp"
#include "../native/game_source.hpp"
#include "../runtime/gx_copy_images.hpp"
#include "../runtime/gx_framebuffer.hpp"
#include <dolphin/gx/GXExtra.h>
#include <JSystem/JKernel/JKRSolidHeap.h>
#include "../runtime/archive_bootstrap.hpp"
#include "../runtime/disc.hpp"
#include <JSystem/J3DGraphBase/J3DTexture.h>
#include <JSystem/J3DGraphBase/J3DShapeDraw.h>
#include <JSystem/J3DGraphBase/J3DSys.h>
#include <JSystem/J3DGraphBase/J3DTevs.h>
#include <m_Do/m_Do_graphic.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>
extern "C" void GXNativeOptionalCopy();
// These two game globals are the only m_Do_graphic.cpp boundary substituted
// by this host harness; registration and J3D assignment are production code.
ResTIMG* mDoGph_gInf_c::mFrameBufferTimg{};
void* mDoGph_gInf_c::mFrameBufferTex{};
namespace dusk::native {
bool captureReport{};
char lastReport[256]{};
void setHostFramebufferImages(std::span<const std::byte>,std::span<const std::byte>);
void optionalCopyStatistics(std::uint64_t&,std::uint64_t&,std::uint64_t&);
void resetHostCaptureObservations();
std::span<const std::uint8_t> hostCapturePixels(gx::TextureHandle);
bool hostCaptureDirty(gx::TextureHandle);
extern unsigned hostCaptureReads,hostCaptureBinds,hostCaptureDecodes,hostCaptureAllocations,hostCaptureInvalidations;
extern unsigned hostBindingInvalidations,hostReadEvent,hostInvalidateEvent,hostBindEvent;
void logText(const char* text) {
    if(captureReport)std::snprintf(lastReport,sizeof(lastReport),"%s",text);
    std::fputs(text, stderr);
}
[[noreturn]] void fatal(const char* text) { logText(text); std::abort(); }
void waitForRetrace() { std::atomic_thread_fence(std::memory_order_seq_cst); }
void invalidateCache(void*, std::uint32_t) { std::atomic_thread_fence(std::memory_order_seq_cst); }
void flushCache(void*, std::uint32_t) { std::atomic_thread_fence(std::memory_order_seq_cst); }
std::uint64_t counterTicks() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() * (nativeTickFrequency / 1000);
}
std::uint64_t wallMilliseconds() { return epoch1900To2000Ms; }
}
namespace gx=dusk::native::gx;
using namespace dusk::native;
using Bytes=std::vector<std::uint8_t>;
using Span=std::span<const std::uint8_t>;
static unsigned word(Span b,unsigned at){assert(at+4<=b.size());return (unsigned(b[at])<<24)|(unsigned(b[at+1])<<16)|(unsigned(b[at+2])<<8)|b[at+3];}
static void put(Bytes& b,unsigned at,unsigned n){b[at]=n>>24;b[at+1]=n>>16;b[at+2]=n>>8;b[at+3]=n;}
static Bytes synthetic(unsigned count){
    Bytes b(32+64*count);std::memcpy(b.data(),"TEX1",4);put(b,4,b.size());b[9]=count;put(b,12,32);
    for(unsigned i=0;i<count;++i){const unsigned h=32+32*i;b[h]=0;b[h+3]=8;b[h+5]=8;b[h+20]=b[h+21]=1;b[h+24]=1;put(b,h+28,32*count);
        for(unsigned n=0;n<32;++n)b[32+32*count+32*i+n]=(17*i+n)&255;
    }
    return b;
}
static Bytes retainedArchive(unsigned value){
    Bytes b(256);
    std::memcpy(b.data(),"RARC",4);put(b,4,256);put(b,8,32);put(b,12,160);put(b,16,64);put(b,20,64);
    put(b,32,1);put(b,36,32);put(b,40,3);put(b,44,48);put(b,48,32);put(b,52,128);
    std::memcpy(b.data()+64,"ROOT",4);b[75]=3;
    put(b,84,0x02000005);put(b,104,0x02000007);put(b,108,UINT32_MAX);
    put(b,124,0x1100000a);put(b,132,64);
    const char names[]="root\0.\0..\0sample.bti\0";std::memcpy(b.data()+160,names,sizeof(names));
    b[192]=3;b[195]=4;b[197]=4;b[212]=b[213]=1;b[216]=1;put(b,220,32);
    std::fill(b.begin()+224,b.end(),value);
    return b;
}
static void ownedLifetimeTests(){
    gx::resetState();int firstOwner{},secondOwner{};auto first=retainedArchive(0x31),second=retainedArchive(0x73);
    assert(registerTextureArchive(&firstOwner,first)&&registerTextureArchive(&secondOwner,second));
    auto* firstHeader=reinterpret_cast<ResTIMG*>(first.data()+192);
    auto* secondHeader=reinterpret_cast<ResTIMG*>(second.data()+192);
    gx::Texture retained;unsigned oldToken{};
    {
        J3DTexture a(1,firstHeader),b(1,firstHeader);
        assert(textureResourceBytes()==64);
        std::array<std::uint8_t,32> firstContext{},copyContext{},secondContext{};
        assert(a.getNativeImageContext(firstContext)&&b.getNativeImageContext(copyContext)&&copyContext==firstContext);
        std::array<std::uint8_t,32> imageDigest{},expectedContext{};
        assert(dusk::pica::identifyImage({first.data()+192,32},{first.data()+224,32},{},imageDigest));
        assert(dusk::pica::identifyImageContext({&imageDigest,1},expectedContext)&&expectedContext==firstContext);
        a.loadGX(0,GX_TEXMAP0);assert(gx::textureBinding(0,retained));
        assert(retained.image.data()!=first.data()+224&&retained.image.front()==0x31);
        assert(gx::textureCommandToken(first.data()+224,oldToken));
        b.addResTIMG(1,reinterpret_cast<ResTIMG*>(second.data()+160));
        b.addResTIMG(1,reinterpret_cast<ResTIMG*>(second.data()+128));
        assert(b.getNum()==3 && b.getImgDataPtr(0)==first.data()+224 &&
               b.getImgDataPtr(1)==second.data()+224 && b.getImgDataPtr(2)==second.data()+224);
        assert(b.getNativeImageContext(copyContext)&&copyContext!=firstContext);
        // Original named/external assignment uses the source header, even
        // though the destination's header is copied into another allocation.
        a.entryNum(1);a.setResTIMG(0,*secondHeader);a.loadGX(0,GX_TEXMAP0);
        assert(gx::textureBinding(0,retained)&&retained.image.front()==0x73);
        assert(a.getNativeImageContext(secondContext)&&secondContext!=firstContext);
        unregisterTextureArchive(&secondOwner);second.clear();second.shrink_to_fit();
        // The source archive may disappear; identity uses the retained original
        // provenance, not the copied header's now-unrelated relative offset.
        assert(a.getNativeImageContext(copyContext)&&copyContext==secondContext);
        a.entryNum(1);captureReport=true;assert(!a.getNativeImageContext(copyContext));
        assert(!std::strcmp(lastReport,"Native image context rejected slot=0 count=1 reason=unassigned-slot\n"));
        captureReport=false;
        a.setResTIMG(0,*firstHeader);assert(a.getNativeImageContext(copyContext)&&copyContext==firstContext);
        // Sampler-only variants of a shared original image remain distinct.
        firstHeader->wrapS=1;
        a.setResTIMG(0,*firstHeader);assert(a.getNativeImageContext(copyContext)&&copyContext!=firstContext);
        firstHeader->wrapS=0;
        assert(textureResourceBytes()==128);
        assert(gx::textureFromCommandToken(oldToken,retained));
    }
    assert(collectTextureResources()&&textureResourceBytes()==0);
    assert(!gx::textureFromCommandToken(oldToken,retained));
    // The real original solid-heap disposer invokes the native texture
    // destructor even when the original resource owner frees a whole heap.
    auto* solid=JKRSolidHeap::create(8192,JKRGetRootHeap(),false);assert(solid);
    assert(solid->getRemainingUsedSize(solid->getStartAddr())==-1);
    auto* head=static_cast<u8*>(solid->alloc(64,32));
    auto* tail=static_cast<u8*>(solid->alloc(96,-32));
    assert(head&&tail&&solid->getRemainingUsedSize(head)>=64);
    assert(solid->getRemainingUsedSize(head+64)==-1);
    assert(solid->getRemainingUsedSize(tail)>=96);
    solid->freeTail();assert(solid->getRemainingUsedSize(tail)==-1);
    assert(solid->getRemainingUsedSize(head)>=64);
    solid->freeAll();assert(solid->getRemainingUsedSize(head)==-1);
    auto* previous=solid->becomeCurrentHeap();
    auto* texture=JKR_NEW J3DTexture(1,firstHeader);assert(texture);
    JKRHeap::setCurrentHeap(previous);
    texture->loadGX(0,GX_TEXMAP0);assert(textureResourceBytes()==64);
    assert(gx::textureCommandToken(first.data()+224,oldToken));
    solid->destroy();assert(collectTextureResources()&&textureResourceBytes()==0);
    assert(!gx::textureFromCommandToken(oldToken,retained));
    unregisterTextureArchive(&firstOwner);unregisterTextureArchive(&secondOwner);
    // Independent decompressed BMD/TEX1 allocation, outside any RARC.
    auto* allocation=static_cast<u8*>(JKRAllocFromHeap(JKRGetRootHeap(),128,32));assert(allocation);
    std::memcpy(allocation+32,first.data()+192,64);
    {
        J3DTexture standalone(1,reinterpret_cast<ResTIMG*>(allocation+32));
        standalone.loadGX(0,GX_TEXMAP0);assert(gx::textureBinding(0,retained)&&retained.image.front()==0x31);
    }
    {
        JUTTexture standalone(reinterpret_cast<ResTIMG*>(allocation+32),0);
        standalone.load(GX_TEXMAP0);assert(gx::textureBinding(0,retained)&&retained.image.front()==0x31);
        standalone.load(GX_TEXMAP1);assert(gx::textureBinding(1,retained)&&textureResourceBytes()==64);
    }
    JKRFreeToHeap(JKRGetRootHeap(),allocation);assert(collectTextureResources()&&textureResourceBytes()==0);
    gx::resetState();
    std::puts("PASS native J3D ownership: shared images, source-header assignment/repeated growth, retained tokens, actual solid-heap disposal and bounded interior EXP allocation");
}
static void imageIdentityDiagnosticTests(){
    gx::resetState();
    alignas(32) std::array<u8,64> retained{},altered{};
    retained[0]=3;retained[3]=retained[5]=4;retained[20]=retained[21]=retained[24]=1;retained[31]=32;
    altered=retained;altered[24]=2;
    gx::TextureHandle handle{};assert(gx::registerTexture(retained,handle,true));
    assert(gx::aliasTextureImage(handle,altered.data()+32));
    {
        J3DTexture texture(1,reinterpret_cast<ResTIMG*>(altered.data()));
        std::array<std::uint8_t,32> context{};captureReport=true;
        assert(!texture.getNativeImageContext(context));
        assert(!std::strcmp(lastReport,"Native image context rejected slot=0 count=1 reason=invalid-image-identity\n"));
        texture.setResTIMG(0,*reinterpret_cast<ResTIMG*>(retained.data()));
        lastReport[0]=0;assert(texture.getNativeImageContext(context)&&!lastReport[0]);
        captureReport=false;
    }
    assert(gx::unregisterTexture(handle));gx::resetState();
}
static void originalShapeStorageTests(){
    // Model archives outlive their shape objects. A fixed resource heap must
    // not duplicate unchanged geometry larger than its own remaining space.
    alignas(32) std::array<u8,8192> archiveList{};
    const u8 command[]={GX_TRIANGLES,0,3,0,11,12,3,21,22,6,31,32};
    std::memcpy(archiveList.data(),command,sizeof(command));
    const auto original=archiveList;
    auto* heap=JKRSolidHeap::create(1024,JKRGetRootHeap(),false);assert(heap);
    auto* previous=heap->becomeCurrentHeap();
    const auto free=heap->getTotalFreeSize();
    const GXVtxDescList descriptor[]={{GX_VA_PNMTXIDX,GX_DIRECT},{GX_VA_POS,GX_INDEX16},{GX_VA_NULL,GX_NONE}};
    {
        J3DShapeDraw first(archiveList.data(),archiveList.size());
        J3DShapeDraw second(archiveList.data(),32,descriptor);
        assert(first.countVertex(3)==3&&second.countVertex(3)==3);
        assert(heap->getTotalFreeSize()==free);
        second.addTexMtxIndexInDL(3,1,30);
        assert(archiveList==original); // mutation must not overwrite the archive
        assert(second.getDisplayList()!=archiveList.data());
        assert(second.countVertex(4)==3&&first.countVertex(3)==3);
        for(unsigned i=0;i<3;++i){
            const auto* vertex=second.getDisplayList()+3+4*i;
            assert(vertex[0]==3*i&&vertex[1]==30+3*i);
            assert(vertex[2]==11+10*i&&vertex[3]==12+10*i);
        }
    }
    JKRHeap::setCurrentHeap(previous);heap->destroy();
    std::puts("PASS original native shapes: fixed-heap borrowed primitives, vertex counts, texture-matrix copy-on-write, archive preserved");
}
static void good(){const auto stats=gx::statistics();if(stats.errors)std::fprintf(stderr,"GX: %s at %s\n",gx::errorText(stats.firstError),stats.firstErrorCall);assert(!stats.errors);}
static void deferredTextureListTests(){
    gx::resetState();
    auto* allocation=static_cast<u8*>(JKRAllocFromHeap(JKRGetRootHeap(),192,32));assert(allocation);
    std::memset(allocation,0,192);
    auto* ci=allocation;ci[0]=8;ci[3]=ci[5]=8;ci[8]=1;ci[9]=2;ci[11]=16;
    ci[15]=64;ci[20]=ci[21]=ci[24]=1;ci[31]=32;
    for(unsigned n=32;n<96;++n)ci[n]=n*59;
    auto* direct=allocation+96;direct[0]=3;direct[3]=direct[5]=4;
    direct[20]=direct[21]=direct[24]=1;direct[31]=32;
    std::memset(direct+32,0x73,32);
    alignas(32) std::array<ResTIMG,2> named{};
    std::memcpy(&named[0],ci,32);std::memcpy(&named[1],direct,32);
    named[0].imageOffset=0;named[1].imageOffset=0;
    const ResTIMG unresolved=named[0];
    {
        J3DTexture texture(2,named.data());j3dSys.setTexture(&texture);
        std::array<std::uint8_t,32> context{};
        captureReport=true;context.fill(0xff);assert(!texture.getNativeImageContext(context));
        assert((context==std::array<std::uint8_t,32>{}));
        assert(!std::strcmp(lastReport,"Native image context rejected slot=0 count=2 reason=unresolved-image\n"));
        gx::CommandMemory memory;gx::CommandProcessor processor(memory);
        alignas(32) std::array<u8,96> bytes;bytes.fill(0xa5);GDLObj gd;
        GDInitGDLObj(&gd,bytes.data(),bytes.size());GDSetCurrent(&gd);
        const u16 first=0,second=1;loadTexNo(0,first);assert(GDGetCurrOffset()==55);
        loadTexNo(1,second);assert(GDGetCurrOffset()==75);GDSetCurrent(nullptr);
        assert(processor.execute({bytes.data(),75}));gx::Texture bound;const char* failure{};
        assert(!gx::resolveCommandTexture(processor.registers(),0,bound,failure));
        assert(!gx::resolveCommandTexture(processor.registers(),1,bound,failure));
        assert(textureResourceBytes()==0);
        texture.setResTIMG(0,*reinterpret_cast<ResTIMG*>(ci));
        assert(!texture.getNativeImageContext(context)); // One unresolved original slot still refuses.
        assert(!std::strcmp(lastReport,"Native image context rejected slot=1 count=2 reason=unresolved-image\n"));
        texture.setResTIMG(1,*reinterpret_cast<ResTIMG*>(direct));
        lastReport[0]=0;assert(texture.getNativeImageContext(context)&&!lastReport[0]);
        captureReport=false;
        std::array<std::array<std::uint8_t,32>,2> images{};
        std::array<std::uint8_t,32> expected{};
        assert(dusk::pica::identifyImage({ci,32},{ci+32,32},{ci+64,32},images[0]));
        assert(dusk::pica::identifyImage({direct,32},{direct+32,32},{},images[1]));
        assert(dusk::pica::identifyImageContext(images,expected)&&context==expected);
        texture.setResTIMG(0,unresolved);captureReport=true;
        assert(!texture.getNativeImageContext(context));
        assert(!std::strcmp(lastReport,"Native image context rejected slot=0 count=2 reason=unresolved-image\n"));
        texture.setResTIMG(0,*reinterpret_cast<ResTIMG*>(ci));
        lastReport[0]=0;assert(texture.getNativeImageContext(context)&&context==expected&&!lastReport[0]);
        captureReport=false;
        GDSetCurrent(&gd);GDSetCurrOffset(0);loadTexNo(0,first);assert(GDGetCurrOffset()==55);
        loadTexNo(1,second);assert(GDGetCurrOffset()==75);GDSetCurrent(nullptr);
        for(unsigned n=75;n<bytes.size();++n)assert(bytes[n]==0xa5);
        assert(processor.execute({bytes.data(),75}));
        assert(gx::resolveCommandTexture(processor.registers(),0,bound,failure)&&bound.format==8);
        assert(gx::resolveCommandTexture(processor.registers(),1,bound,failure)&&bound.format==3&&bound.image.front()==0x73);
        j3dSys.setTexture(nullptr);good();
    }
    assert(collectTextureResources()&&textureResourceBytes()==0);
    JKRFreeToHeap(JKRGetRootHeap(),allocation);gx::resetState();
    std::puts("PASS actual J3D deferred external CI/direct lists: unresolved draws reject, original assignment patches exact 55/20-byte spans, real tokens/palettes resolve and retire");
}
static void paletteLifetimeTests(){
    gx::resetState();
    gx::CommandMemory memory;gx::CommandProcessor processor(memory);std::array<u8,128> scratch{};
    gx::CommandStream stream(processor,scratch);assert(gx::setCommandStream(&stream));
    auto* allocation=static_cast<u8*>(JKRAllocFromHeap(JKRGetRootHeap(),160,32));assert(allocation);
    std::memset(allocation,0,160);auto* b=allocation+32;
    b[0]=8;b[3]=b[5]=8;b[8]=1;b[9]=2;b[11]=16;b[15]=64;b[20]=b[21]=b[24]=1;b[31]=32;
    for(unsigned n=32;n<96;++n)b[n]=n*59;
    gx::Texture bound;unsigned oldPaletteToken{};
    // Repeated actual original construction/destruction must retire its plain
    // GXTlutObj metadata as well as retained image/palette bytes.
    for(unsigned n=0;n<600;++n){
        {
            J3DTexture original(1,reinterpret_cast<ResTIMG*>(b));
            assert(textureResourceBytes()==96);original.loadGX(0,GX_TEXMAP0);
            assert(gx::textureBinding(0,bound));
            const char* failure{};gx::Texture commandBound;
            assert(gx::resolveCommandTexture(processor.registers(),0,commandBound,failure)&&commandBound.handle==bound.handle);
            assert(gx::paletteCommandToken(original.getTlutDataPtr(0),oldPaletteToken));
            NativeTexturePlan plan;assert(inspectNativeTexture(bound.bti,plan));Bytes pixels(plan.outputBytes);assert(decodeNativeTexture(bound.bti,plan,pixels));
        }
        assert(collectTextureResources()&&textureResourceBytes()==0&&!gx::textureFromCommandToken(oldPaletteToken,bound));
        good();
    }
    int archiveOwner{};auto archive=retainedArchive(0x31);archive.resize(288);put(archive,4,288);put(archive,16,96);put(archive,20,96);put(archive,132,96);
    std::memcpy(archive.data()+192,b,96);assert(registerTextureArchive(&archiveOwner,archive));
    for(unsigned n=0;n<600;++n){
        {JUTTexture original(reinterpret_cast<ResTIMG*>(archive.data()+192),0);original.load(GX_TEXMAP0);assert(gx::textureBinding(0,bound)&&textureResourceBytes()==96);}
        assert(collectTextureResources()&&textureResourceBytes()==0);good();
    }
    assert(gx::setCommandStream(nullptr));
    unregisterTextureArchive(&archiveOwner);JKRFreeToHeap(JKRGetRootHeap(),allocation);
    std::puts("PASS original J3D/JUT CI palettes: 1200 actual lifecycles, bounded EXP/archive source, original slot loads, exact retention/metadata retirement");
}
static void captureLifetimeTests(){
    gx::resetState();
    assert(initializeFramebuffers(reinterpret_cast<void*>(1),8,8)&&beginFramebufferFrame());
    alignas(32) std::array<u8,96> color{};
    alignas(32) std::array<u8,64> depth{};
    for(auto* header:{color.data(),depth.data()}){header[3]=header[5]=4;header[20]=header[21]=1;header[24]=1;header[31]=32;}
    color[0]=6;depth[0]=3;
    // Actual initial allocation bytes survive registration; no capture is
    // inferred from this nonzero sentinel content.
    std::fill(color.begin()+32,color.end(),0x4b);std::fill(depth.begin()+32,depth.end(),0x6c);
    assert(registerCopyImage(color,color.data()+32)&&registerCopyImage(depth,depth.data()+32));
    assert(!registerCopyImage(color,color.data()+32)&&copyImageBytes()==160);
    unsigned colorToken{},depthToken{};
    assert(gx::textureCommandToken(color.data()+32,colorToken)&&gx::textureCommandToken(depth.data()+32,depthToken));
    gx::CommandMemory memory;gx::CommandProcessor commands(memory);
    std::array<u8,128> scratch;gx::CommandStream stream(commands,scratch);
    {
        J3DTexture a(1,reinterpret_cast<ResTIMG*>(color.data())),b(1,reinterpret_cast<ResTIMG*>(depth.data()));
        assert(textureResourceBytes()==0); // Already owned by the live copy provider.
        std::array<std::uint8_t,32> context{};context.fill(0xff);captureReport=true;
        assert(!a.getNativeImageContext(context)&&(context==std::array<std::uint8_t,32>{}));
        assert(!std::strcmp(lastReport,"Native image context rejected slot=0 count=1 reason=mutable-pixels\n"));
        assert(!b.getNativeImageContext(context));
        assert(!std::strcmp(lastReport,"Native image context rejected slot=0 count=1 reason=mutable-pixels\n"));
        captureReport=false;
        gx::Texture image;assert(gx::textureFromCommandToken(colorToken,image)&&image.image.front()==0x4b);
        a.loadGX(0,GX_TEXMAP0);assert(gx::textureBinding(0,image)&&image.image.front()==0x4b);
        assert(gx::setCommandStream(&stream));
        std::array<std::byte,256> rgba{},z{};
        for(unsigned pass=0;pass<2;++pass){
            for(unsigned i=0;i<64;++i){
                rgba[i*4]=std::byte(16+pass);rgba[i*4+1]=std::byte(32+pass);
                rgba[i*4+2]=std::byte(64+pass);rgba[i*4+3]=std::byte(128+pass);
                const unsigned pica=0xabcde000|pass*0x100;
                std::memcpy(z.data()+i*4,&pica,4);
            }
            setHostFramebufferImages(rgba,z);
            GXSetTexCopySrc(0,0,8,8);GXSetTexCopyDst(4,4,GX_TF_RGBA8,GX_TRUE);GXCopyTex(color.data()+32,GX_FALSE);
            GXSetTexCopyDst(4,4,GX_TF_Z16,GX_TRUE);GXCopyTex(depth.data()+32,GX_FALSE);
            captureReport=true;assert(!a.getNativeImageContext(context));
            assert(!std::strcmp(lastReport,"Native image context rejected slot=0 count=1 reason=mutable-pixels\n"));
            captureReport=false;
            unsigned token{};assert(gx::textureCommandToken(color.data()+32,token)&&token==colorToken);
            assert(gx::textureCommandToken(depth.data()+32,token)&&token==depthToken);
            assert(gx::textureFromCommandToken(colorToken,image));
            assert(image.image[0]==16+pass&&image.image[1]==128+pass&&image.image[32]==64+pass&&image.image[33]==32+pass);
            assert(gx::textureFromCommandToken(depthToken,image));
            const unsigned expected=0xffffff-((0xabcde000|pass*0x100)&0xffffff);
            assert(image.image[0]==expected>>16&&image.image[1]==((expected>>8)&255));
            // Real original J3D commands resolve the refreshed resource using
            // the same token created before the first framebuffer copy.
            a.loadGX(0,GX_TEXMAP0);b.loadGX(0,GX_TEXMAP1);assert(stream.finish());
            const char* reason;assert(gx::resolveCommandTexture(commands.registers(),1,image,reason));
            assert(image.handle==(depthToken&0x7fffff)&&image.image[0]==expected>>16);
        }
        GXDestroyCopyTex(color.data()+32);GXDestroyCopyTex(depth.data()+32);
        assert(!gx::textureFromCommandToken(colorToken,image)&&!gx::textureFromCommandToken(depthToken,image));
        assert(gx::setCommandStream(nullptr));
    }
    assert(collectTextureResources()&&textureResourceBytes()==0&&copyImageBytes()==0);
    good();assert(endFramebufferFrame());releaseFramebufferRecords();gx::resetState();
    std::puts("PASS native capture lifecycle: original allocation bytes, J3D external assignment, stable color/Z16 command tokens, refresh and exact retirement; synthetic GPU snapshots only");
}
static void framebufferImageIdentityTests(){
    gx::resetState();
    assert(initializeFramebuffers(reinterpret_cast<void*>(1),8,8)&&beginFramebufferFrame());
    gx::CommandMemory memory;gx::CommandProcessor commands(memory);
    std::array<u8,128> scratch{};gx::CommandStream stream(commands,scratch);
    assert(gx::setCommandStream(&stream));

    // Distinct headers address the same image: pointer equality of the image
    // alone is not enough to identify the original game framebuffer TIMG.
    alignas(32) std::array<u8,128> allocation{};
    auto* impostor=allocation.data();auto* original=impostor+32;auto* pixels=original+32;
    for(auto* header:{impostor,original}){
        header[0]=6;header[3]=header[5]=4;header[20]=header[21]=header[24]=1;
    }
    impostor[31]=64;original[31]=32;
    assert(registerCopyImage({original,96},pixels));
    auto* timg=reinterpret_cast<ResTIMG*>(original);
    mDoGph_gInf_c::mFrameBufferTimg=timg;mDoGph_gInf_c::mFrameBufferTex=pixels;
    constexpr std::array<std::uint8_t,32> role{
        0xf0,0x50,0x3c,0x86,0xef,0x37,0xca,0xb3,0x2a,0xe1,0x4f,0x76,0xea,0x47,0x8b,0xa7,
        0x6c,0xcd,0x3e,0x8d,0x1f,0x03,0x7d,0xe0,0xaa,0x9c,0xc8,0x52,0xa7,0x71,0x7c,0x7c};
    constexpr std::array<std::uint8_t,32> expectedContext{
        0xeb,0x9d,0x3e,0x62,0x27,0xc5,0x09,0xd4,0x21,0x31,0x20,0xf4,0x65,0xde,0x60,0xa5,
        0xe1,0x6b,0x31,0xe6,0xb7,0x79,0xf5,0x52,0xa0,0xd6,0x4d,0x17,0xc4,0xd3,0x46,0x49};
    assert(dusk::pica::identifyFramebufferColor()==role);
    std::array<std::uint8_t,32> composed{};
    assert(dusk::pica::identifyImageContext({&role,1},composed)&&composed==expectedContext);
    {
        ResTIMG slot{};J3DTexture texture(1,&slot);
        std::array<std::uint8_t,32> context{};
        auto rejected=[&]{
            context.fill(0xff);captureReport=true;assert(!texture.getNativeImageContext(context));captureReport=false;
            assert((context==std::array<std::uint8_t,32>{}));
            assert(!std::strcmp(lastReport,"Native image context rejected slot=0 count=1 reason=mutable-pixels\n"));
        };
        texture.setResTIMG(0,*reinterpret_cast<ResTIMG*>(impostor));rejected();
        texture.setResTIMG(0,*timg);
        assert(texture.getNativeImageContext(context)&&context==expectedContext);
        mDoGph_gInf_c::mFrameBufferTex=impostor;
        texture.setResTIMG(0,*timg);rejected();
        mDoGph_gInf_c::mFrameBufferTex=pixels;

        unsigned token{};assert(gx::textureCommandToken(pixels,token));
        assert(registeredFramebufferCapture(pixels,token&0x7fffff));
        std::array<std::byte,256> rgba{};
        for(unsigned pass=0;pass<2;++pass){
            for(unsigned i=0;i<64;++i){
                rgba[4*i]=std::byte(16+pass);rgba[4*i+1]=std::byte(32+pass);
                rgba[4*i+2]=std::byte(64+pass);rgba[4*i+3]=std::byte(128+pass);
            }
            setHostFramebufferImages(rgba,{});
            GXSetTexCopySrc(0,0,8,8);GXSetTexCopyDst(4,4,GX_TF_RGBA8,GX_TRUE);GXCopyTex(pixels,GX_FALSE);
            gx::Texture image;unsigned refreshed{};
            assert(gx::textureCommandToken(pixels,refreshed)&&refreshed==token);
            assert(gx::textureFromCommandToken(token,image)&&image.image[0]==16+pass);
            texture.setResTIMG(0,*timg);
            assert(texture.getNativeImageContext(context)&&context==expectedContext);
        }
        GXDestroyCopyTex(pixels);
        assert(!registeredFramebufferCapture(pixels,token&0x7fffff));

        // Neither a direct registration nor an explicitly matching alias
        // proves that the live framebuffer copy provider owns the image.
        gx::TextureHandle direct{};assert(gx::registerTexture({original,96},direct));
        texture.setResTIMG(0,*timg);rejected();
        assert(gx::aliasTextureImage(direct,impostor));
        texture.setResTIMG(0,*timg);rejected();
        assert(gx::aliasTextureImage(direct,pixels));
        assert(!registeredFramebufferCapture(pixels,direct));
        texture.setResTIMG(0,*timg);rejected();
        assert(gx::unregisterTexture(direct));

        // A token can resolve an alias before a different matching GX object
        // registration. It must refer to the object's registration, not merely
        // some RGBA8 alias with the right original pointer.
        alignas(32) std::array<u8,288> otherExtent{};
        otherExtent[0]=6;otherExtent[3]=otherExtent[5]=8;
        otherExtent[20]=otherExtent[21]=otherExtent[24]=1;otherExtent[31]=32;
        assert(registerCopyImage(otherExtent,pixels));
        gx::Texture image;unsigned aliased{};
        assert(gx::textureCommandToken(pixels,aliased)&&gx::textureFromCommandToken(aliased,image));
        const auto other=image.handle;
        assert(registeredFramebufferCapture(pixels,other));
        assert(gx::registerTexture({original,96},direct));
        texture.setResTIMG(0,*timg);rejected();
        assert(gx::textureCommandToken(pixels,aliased)&&gx::textureFromCommandToken(aliased,image)&&image.handle==other);
        texture.loadGX(0,GX_TEXMAP0);assert(gx::textureBinding(0,image)&&image.handle==direct);
        GXDestroyCopyTex(pixels);assert(gx::unregisterTexture(direct));

        alignas(32) std::array<u8,64> nonRgba{};
        nonRgba[0]=3;nonRgba[3]=nonRgba[5]=4;
        nonRgba[20]=nonRgba[21]=nonRgba[24]=1;nonRgba[31]=32;
        assert(registerCopyImage(nonRgba,nonRgba.data()+32));
        mDoGph_gInf_c::mFrameBufferTimg=reinterpret_cast<ResTIMG*>(nonRgba.data());
        mDoGph_gInf_c::mFrameBufferTex=nonRgba.data()+32;
        texture.setResTIMG(0,*mDoGph_gInf_c::mFrameBufferTimg);rejected();
        GXDestroyCopyTex(nonRgba.data()+32);

        alignas(32) std::array<u8,1312> mipmapped{};
        mipmapped[0]=6;mipmapped[3]=mipmapped[5]=16;mipmapped[16]=1;
        mipmapped[20]=mipmapped[21]=1;mipmapped[24]=2;mipmapped[31]=32;
        assert(gx::registerTexture(mipmapped,direct)&&gx::aliasTextureImage(direct,mipmapped.data()+32));
        mDoGph_gInf_c::mFrameBufferTimg=reinterpret_cast<ResTIMG*>(mipmapped.data());
        mDoGph_gInf_c::mFrameBufferTex=mipmapped.data()+32;
        texture.setResTIMG(0,*mDoGph_gInf_c::mFrameBufferTimg);rejected();
        assert(gx::unregisterTexture(direct));
    }
    mDoGph_gInf_c::mFrameBufferTimg=nullptr;mDoGph_gInf_c::mFrameBufferTex=nullptr;
    assert(gx::setCommandStream(nullptr)&&collectTextureResources()&&textureResourceBytes()==0&&copyImageBytes()==0);
    good();assert(endFramebufferFrame());releaseFramebufferRecords();gx::resetState();
    std::puts("PASS framebuffer role SHA256 and original J3D/GX identity: mutable capture accepted, refreshed and guarded by source, alias, registration, format and mip count");
}
static void framebufferCaptureAcquisitionTests(){
    // Real copy/registration/encode/decoder code with a substituted cache and
    // framebuffer readback; this does not claim Citro3D or GPU execution.
    gx::resetState();gx::watchedCaptureToken=0;gx::watchedCaptureLastUse=0;
    gx::textureFrameCounter=100;gx::watchedCaptureMaterialize=nullptr;
    assert(initializeFramebuffers(reinterpret_cast<void*>(1),8,8)&&beginFramebufferFrame());
    alignas(32) std::array<u8,96> color{},other{};
    for(auto* image:{color.data(),other.data()}){
        image[0]=6;image[3]=image[5]=4;image[20]=image[21]=image[24]=1;image[31]=32;
        std::fill(image+32,image+96,0x4b);
        assert(registerCopyImage({image,96},image+32));
    }
    gx::CommandMemory memory;gx::CommandProcessor commands(memory);
    std::array<u8,128> scratch{};gx::CommandStream stream(commands,scratch);
    assert(gx::setCommandStream(&stream));
    unsigned token{};assert(gx::textureCommandToken(color.data()+32,token));
    const gx::TextureHandle handle=token&0x7fffff;
    assert(registeredFramebufferCapture(color.data()+32,handle));
    auto checkPixel=[&](unsigned a,unsigned b,unsigned g,unsigned r){
        const auto pixels=hostCapturePixels(handle);assert(pixels.size()>=4);
        assert(pixels[0]==a&&pixels[1]==b&&pixels[2]==g&&pixels[3]==r);
    };
    void* texture{};unsigned width{},height{};
    resetHostCaptureObservations();
    assert(acquireFramebufferCapture(color.data()+32,texture,width,height)&&texture&&width==4&&height==4);
    assert(hostCaptureAllocations==1&&hostCaptureDecodes==1);checkPixel(0x4b,0x4b,0x4b,0x4b);
    void* const resident=texture;

    std::array<std::byte,256> framebuffer{};
    auto fill=[&](unsigned a,unsigned b,unsigned g,unsigned r){
        for(unsigned i=0;i<64;++i){framebuffer[4*i]=std::byte(a);framebuffer[4*i+1]=std::byte(b);
            framebuffer[4*i+2]=std::byte(g);framebuffer[4*i+3]=std::byte(r);}
        setHostFramebufferImages(framebuffer,{});
    };
    fill(11,22,33,44);
    GXSetTexCopySrc(0,0,8,8);GXSetTexCopyDst(4,4,GX_TF_RGBA8,GX_FALSE);
    GXCopyTex(color.data()+32,GX_FALSE);
    assert(hostCaptureDirty(handle));checkPixel(0x4b,0x4b,0x4b,0x4b);
    resetHostCaptureObservations();
    assert(acquireFramebufferCapture(color.data()+32,texture,width,height)&&texture==resident&&width==4&&height==4);
    assert(!hostCaptureDirty(handle)&&hostCaptureReads==0&&hostCaptureBinds==1&&hostCaptureDecodes==1&&
           hostCaptureAllocations==0&&hostBindingInvalidations==1);
    checkPixel(11,22,33,44);

    std::uint64_t skipped{},done{},materialized{};
    optionalCopyStatistics(skipped,done,materialized);
    const auto priorSkipped=skipped,priorDone=done,priorMaterialized=materialized;
    fill(55,66,77,88);resetHostCaptureObservations();
    GXNativeOptionalCopy();GXCopyTex(color.data()+32,GX_FALSE);
    optionalCopyStatistics(skipped,done,materialized);
    assert(skipped==priorSkipped+1&&done==priorDone&&materialized==priorMaterialized);
    assert(gx::watchedCaptureToken==token&&gx::watchedCaptureLastUse==0&&hostCaptureReads==0);
    checkPixel(11,22,33,44);
    void* otherTexture{};unsigned otherWidth{},otherHeight{};
    assert(acquireFramebufferCapture(other.data()+32,otherTexture,otherWidth,otherHeight));
    assert(otherTexture!=resident&&otherWidth==4&&otherHeight==4&&hostCaptureReads==0&&gx::watchedCaptureLastUse==0);
    optionalCopyStatistics(skipped,done,materialized);assert(materialized==priorMaterialized);

    resetHostCaptureObservations();
    assert(acquireFramebufferCapture(color.data()+32,texture,width,height)&&texture==resident&&width==4&&height==4);
    optionalCopyStatistics(skipped,done,materialized);
    assert(materialized==priorMaterialized+1&&gx::watchedCaptureLastUse==gx::textureFrameCounter);
    assert(hostCaptureReads==1&&hostCaptureInvalidations==1&&hostCaptureBinds==1&&hostCaptureDecodes==1&&
           hostCaptureAllocations==0&&hostBindingInvalidations==1&&!hostCaptureDirty(handle));
    assert(hostReadEvent<hostInvalidateEvent&&hostInvalidateEvent<hostBindEvent);
    checkPixel(55,66,77,88);
    resetHostCaptureObservations();
    assert(acquireFramebufferCapture(color.data()+32,texture,width,height)&&texture==resident);
    assert(hostCaptureReads==0&&hostCaptureBinds==1&&hostCaptureDecodes==0&&hostCaptureAllocations==0);

    // Destroying the copy cancels its outstanding deferred capture; neither
    // a later callback nor an acquisition may revive the retired identity.
    gx::textureFrameCounter=101;gx::watchedCaptureLastUse=0;
    resetHostCaptureObservations();GXNativeOptionalCopy();GXCopyTex(color.data()+32,GX_FALSE);
    optionalCopyStatistics(skipped,done,materialized);assert(skipped==priorSkipped+2&&materialized==priorMaterialized+1);
    GXDestroyCopyTex(color.data()+32);assert(gx::watchedCaptureMaterialize);
    gx::watchedCaptureMaterialize();optionalCopyStatistics(skipped,done,materialized);
    assert(materialized==priorMaterialized+1&&hostCaptureReads==0);
    texture=reinterpret_cast<void*>(1);width=height=1;
    assert(!acquireFramebufferCapture(color.data()+32,texture,width,height)&&!texture&&!width&&!height);
    GXDestroyCopyTex(other.data()+32);
    gx::watchedCaptureToken=gx::watchedCaptureLastUse=gx::textureFrameCounter=0;gx::watchedCaptureMaterialize=nullptr;
    setHostFramebufferImages({},{});assert(gx::setCommandStream(nullptr)&&copyImageBytes()==0);
    good();assert(endFramebufferFrame());releaseFramebufferRecords();gx::resetState();
    std::puts("PASS capture acquisition ordering, dirty refresh, identity and destruction with mocked cache/readback; no GPU execution");
}
static void indexedCaptureTests(){
    gx::resetState();assert(initializeFramebuffers(reinterpret_cast<void*>(1),8,8)&&beginFramebufferFrame());
    alignas(32) std::array<u8,128> original{};
    auto* b=original.data();b[0]=9;b[1]=2;b[3]=b[5]=8;b[8]=1;b[9]=2;b[11]=2;b[15]=96;
    b[20]=b[21]=1;b[24]=1;b[31]=32;b[96]=0x80;b[98]=0xfc;
    const std::span<const u8> palette{b+96,4};
    assert(!registerIndexedCopyImage({b,32},b+32,63,palette));
    assert(!registerIndexedCopyImage({b,32},b+32,64,{b+96,3}));
    assert(registerIndexedCopyImage({b,32},b+32,64,palette));
    assert(!registerIndexedCopyImage({b,32},b+32,64,palette));
    gx::CommandMemory memory;gx::CommandProcessor commands(memory);std::array<u8,128> scratch{};gx::CommandStream stream(commands,scratch);
    assert(gx::setCommandStream(&stream));
    unsigned token;assert(gx::textureCommandToken(b+32,token));
    {
        JUTTexture originalTexture(reinterpret_cast<ResTIMG*>(b),0);
        std::array<std::byte,256> rgba{};
        for(unsigned i=0;i<64;++i){rgba[4*i]=std::byte(255);rgba[4*i+1]=std::byte(80);rgba[4*i+2]=std::byte(250);rgba[4*i+3]=std::byte(1);}
        setHostFramebufferImages(rgba,{});
        GXSetTexCopySrc(0,0,8,8);GXSetTexCopyDst(8,8,GX_CTF_R8,GX_FALSE);GXCopyTex(b+32,GX_FALSE);
        originalTexture.load(GX_TEXMAP0);assert(stream.finish());
        gx::Texture texture;const char* why;assert(gx::resolveCommandTexture(commands.registers(),0,texture,why));
        assert(texture.format==9&&texture.handle==(token&0x7fffff));
        for(auto index:texture.image)assert(index==1); // Red channel, never RGB luminance.
        NativeTexturePlan plan;assert(inspectNativeTexture(texture.bti,plan));std::vector<u8> decoded(plan.outputBytes);
        assert(decodeNativeTexture(texture.bti,plan,decoded));assert(decoded[0]==255&&decoded[3]==255&&decoded[1]==0);
        b[98]=0x80;b[99]=0x1f; // Live palette becomes blue after capture, before load.
        originalTexture.load(GX_TEXMAP0);assert(stream.finish());
        assert(gx::resolveCommandTexture(commands.registers(),0,texture,why)&&texture.handle==(token&0x7fffff));
        assert(decodeNativeTexture(texture.bti,plan,decoded));assert(decoded[0]==255&&decoded[1]==255&&decoded[3]==0);
        GXDestroyCopyTex(b+32);assert(!gx::textureFromCommandToken(token,texture));
    }
    assert(gx::setCommandStream(nullptr)&&collectTextureResources()&&copyImageBytes()==0&&textureResourceBytes()==0);
    good();assert(endFramebufferFrame());releaseFramebufferRecords();gx::resetState();
    std::puts("PASS actual R8 -> CI8 capture, original JUT load, stable token, live palette refresh, exact red indices, RGB5A3 output and bounded retirement");
}
static unsigned runBlock(Span block){
    gx::resetState();gx::TextureBlockResources resources;assert(gx::registerTextureBlock(block,resources));
    const unsigned count=resources.count,offset=word(block,12);assert(count>0&&count<=8);
    gx::CommandMemory commandMemory;gx::CommandProcessor commands(commandMemory);
    std::array<u8,128> scratch;gx::CommandStream stream(commands,scratch);
    {
        // Actual original constructor initializes TGXTexObj objects from ResTIMG;
        // actual loadGX binds them; actual RAII destructors release them.
        J3DTexture texture(count,reinterpret_cast<ResTIMG*>(const_cast<std::uint8_t*>(block.data()+offset)));
        for(unsigned i=0;i<count;++i){
            texture.loadGX(i,static_cast<GXTexMapID>(i));gx::Texture bound{};assert(gx::textureBinding(i,bound));
            assert(bound.handle==resources.handles[i]);const auto* header=texture.getResTIMG(i);
            assert(bound.image.data()==texture.getImgDataPtr(i));assert(bound.bti.data()==reinterpret_cast<const std::uint8_t*>(header));
            assert(bound.width==header->width&&bound.height==header->height&&bound.format==header->format);
            assert(bound.wrapS==header->wrapS&&bound.wrapT==header->wrapT&&bound.minFilter==header->minFilter&&bound.magFilter==header->magFilter);
            // Original J3D material list commands resolve the same borrowed image.
            alignas(32) u8 bytes[128];GDLObj gd;GDInitGDLObj(&gd,bytes,sizeof(bytes));GDSetCurrent(&gd);
            const auto id=static_cast<GXTexMapID>(i);
            J3DGDSetTexImgPtr(id,texture.getImgDataPtr(i));
            J3DGDSetTexImgAttr(id,header->width,header->height,static_cast<GXTexFmt>(header->format));
            J3DGDSetTexLookupMode(id,static_cast<GXTexWrapMode>(header->wrapS),static_cast<GXTexWrapMode>(header->wrapT),
                static_cast<GXTexFilter>(header->minFilter),static_cast<GXTexFilter>(header->magFilter),
                header->minLOD*.125f,header->maxLOD*.125f,header->LODBias*.01f,header->biasClamp,header->doEdgeLOD,
                static_cast<GXAnisotropy>(header->maxAnisotropy));
            const auto size=GDGetCurrOffset();GDSetCurrent(nullptr);assert(size==20&&commands.execute({bytes,size}));
            gx::Texture fromCommands;const char* reason;
            assert(gx::resolveCommandTexture(commands.registers(),i,fromCommands,reason));
            assert(fromCommands.handle==bound.handle&&fromCommands.image.data()==bound.image.data());
            assert(fromCommands.minFilter==bound.minFilter&&fromCommands.magFilter==bound.magFilter);
            // Then exercise the original object's typed load in command mode.
            assert(gx::setCommandStream(&stream));texture.loadGX(i,id);assert(stream.finish());
            assert(gx::setCommandStream(nullptr));
            assert(gx::resolveCommandTexture(commands.registers(),i,fromCommands,reason)&&fromCommands.handle==bound.handle);
            NativeTexturePlan plan;assert(inspectNativeTexture(bound.bti,plan));
            Bytes output(plan.outputBytes);assert(decodeNativeTexture(bound.bti,plan,output));
            // Result is transient and never written to disk.
        }
        good();
        // State reset retains objects/registrations, but must clear bindings.
        gx::resetState();for(unsigned i=0;i<count;++i){gx::Texture bound;assert(!gx::textureBinding(i,bound));texture.loadGX(i,static_cast<GXTexMapID>(i));assert(gx::textureBinding(i,bound));}
        good();
    }
    for(unsigned i=0;i<count;++i){gx::Texture bound{};assert(!gx::textureBinding(i,bound)&&bound.handle==0&&bound.bti.empty());}
    assert(gx::releaseTextureBlock(resources)&&resources.count==0);
    for(unsigned i=0;i<count;++i){gx::Texture expired;const char* reason;
        assert(!gx::resolveCommandTexture(commands.registers(),i,expired,reason));}
    good();return count;
}
static void syntheticTests(){
    auto original=synthetic(8);for(unsigned cycle=0;cycle<40;++cycle){auto bytes=original;runBlock(bytes);assert(bytes==original);}
    auto bytes=synthetic(3);gx::TextureBlockResources resources;
    for(unsigned size=0;size<bytes.size();++size){gx::resetState();assert(!gx::registerTextureBlock(Span(bytes.data(),size),resources)&&resources.count==0);}
    for(unsigned mutation=0;mutation<7;++mutation){auto b=bytes;
        switch(mutation){case 0:b[0]='Z';break;case 1:put(b,4,b.size()-1);break;case 2:b[9]=17;break;case 3:put(b,12,33);break;case 4:put(b,32+28,32);break;case 5:put(b,64+28,65);break;case 6:b[64]=8;break;}
        gx::resetState();assert(!gx::registerTextureBlock(b,resources));assert(resources.count==0);for(auto h:resources.handles)assert(h==0);
        // Rollback must leave enough room and no aliases for the valid block.
        gx::resetState();assert(gx::registerTextureBlock(bytes,resources));assert(gx::releaseTextureBlock(resources));good();
    }
    // Exact aliases share one owned image; external placeholders are explicit.
    {auto alias=bytes;put(alias,64+28,64);gx::resetState();assert(gx::registerTextureBlock(alias,resources)&&resources.count==2);assert(gx::releaseTextureBlock(resources));}
    {auto external=bytes;put(external,64+28,0);gx::resetState();assert(!gx::registerTextureBlock(external,resources));gx::resetState();assert(gx::registerTextureBlock(external,resources,true)&&resources.count==2);assert(gx::releaseTextureBlock(resources));}
    // Exhaustion after a partial batch must preserve existing registrations.
    auto big=synthetic(63);auto small=synthetic(2);gx::TextureBlockResources held;
    gx::resetState();assert(gx::registerTextureBlock(big,held));assert(!gx::registerTextureBlock(small,resources)&&resources.count==0);
    gx::resetState();assert(gx::releaseTextureBlock(held));assert(gx::registerTextureBlock(big,held)&&gx::releaseTextureBlock(held));good();
    // Failed reuse must leave an existing resource set intact.
    gx::resetState();assert(gx::registerTextureBlock(bytes,resources));const auto handles=resources.handles;
    assert(!gx::registerTextureBlock(bytes,resources)&&resources.handles==handles);assert(gx::releaseTextureBlock(resources));
    // Explicit resource invalidation makes already-loaded original objects stale.
    gx::resetState();assert(gx::registerTextureBlock(bytes,resources));
    {J3DTexture texture(3,reinterpret_cast<ResTIMG*>(bytes.data()+32));texture.loadGX(0,GX_TEXMAP0);gx::Texture bound;assert(gx::textureBinding(0,bound));
     assert(gx::releaseTextureBlock(resources));assert(!gx::textureBinding(0,bound));texture.loadGX(0,GX_TEXMAP0);assert(!gx::textureBinding(0,bound));}
    good();gx::Texture absent{};assert(!gx::textureBinding(8,absent));
    // Empty TEX1 is valid. A malformed release must not partially unregister.
    auto empty=synthetic(0);put(empty,12,0);assert(gx::registerTextureBlock(empty,resources)&&gx::releaseTextureBlock(resources));
    assert(gx::registerTextureBlock(bytes,resources));auto corrupt=resources;corrupt.handles[1]=corrupt.handles[0];
    assert(!gx::releaseTextureBlock(corrupt));
    GXTexObj object{};GXInitTexObj(&object,bytes.data()+128,8,8,GX_TF_I4,GX_CLAMP,GX_CLAMP,GX_FALSE);GXLoadTexObj(&object,GX_TEXMAP0);
    gx::Texture bound{};assert(gx::textureBinding(0,bound));GXInitTexObjTlut(&object,0);
    assert(gx::statistics().firstError==gx::Error::invalidParameter&&gx::textureBinding(0,bound)); // Invalid CI-only setter preserves the existing direct image.
    GXDestroyTexObj(&object);assert(gx::releaseTextureBlock(resources));
    // Limit applies to validated pixels, not header plus the enclosing view.
    Bytes largest(32+4*1024*1024);largest[0]=6;largest[2]=largest[4]=4;largest[24]=1;largest[31]=32;
    gx::TextureHandle largeHandle=0;gx::resetState();assert(gx::registerTexture(largest,largeHandle)&&gx::unregisterTexture(largeHandle));good();
    std::puts("PASS original J3DTexture: 40 construction/load/destruction cycles, 8 maps, zero-copy resources, rollback and stale-binding invalidation");
}
static void privateTitle(const char* path){
    const auto before=kernelStats();unsigned count=0;std::size_t archiveSize=0;
    {Disc disc;assert(disc.open(path)==DiscStatus::ready);ArchiveBootstrap archive;
     constexpr ArchiveLimits limits{{8*1024*1024,4096,65536,255},8*1024*1024};
     assert(archive.load(disc,"/res/Object/Title.arc",limits)==ArchiveStatus::ready);archiveSize=archive.archiveBytes();
     ResourceSpan model;assert(archive.findBasename("titlelogo_r.bmd",model)==ArchiveStatus::ready);Span bmd(model.data,model.size);
     assert(bmd.size()>=32&&!std::memcmp(bmd.data(),"J3D2bmd3",8)&&word(bmd,8)==bmd.size());std::size_t offset=32;Span block;
     for(unsigned n=0;n<word(bmd,12);++n){assert(offset+8<=bmd.size());const auto size=word(bmd,offset+4);assert(size>=8&&size<=bmd.size()-offset);
        if(!std::memcmp(bmd.data()+offset,"TEX1",4)){assert(block.empty());block=bmd.subspan(offset,size);}offset+=size;}
     assert(!block.empty());count=runBlock(block);
     // Exercise the production ownership path as well: no test-side TEX1
     // registration before the original J3DTexture constructor.
     const auto headerOffset=word(block,12);
     {
        J3DTexture texture(count,reinterpret_cast<ResTIMG*>(const_cast<u8*>(block.data()+headerOffset)));
        assert(textureResourceBytes()>0);
        for(unsigned i=0;i<count;++i){
            texture.loadGX(i,static_cast<GXTexMapID>(i));gx::Texture bound;
            assert(gx::textureBinding(i,bound)&&bound.image.data()!=texture.getImgDataPtr(i));
            unsigned token{};assert(gx::textureCommandToken(texture.getImgDataPtr(i),token));
            NativeTexturePlan plan;assert(inspectNativeTexture(bound.bti,plan));
            std::vector<u8> decoded(plan.outputBytes);assert(decodeNativeTexture(bound.bti,plan,decoded));
        }
     }
     assert(collectTextureResources()&&textureResourceBytes()==0);
     assert(archive.close()==ArchiveStatus::ready);
    }
    assert(kernelStats().free==before.free&&kernelStats().healthy);
    std::printf("PASS private original JKR -> J3DTexture -> GX bindings/J3D command tokens -> native conversion: %u textures, direct TEX1 plus automatic retained ownership, archive_bytes=%zu, payload_saved=false, JKR free restored\n",count,archiveSize);
}
int main(int argc,char** argv){
    static_assert(sizeof(ResTIMG)==32);alignas(32) static std::array<unsigned char,8*1024*1024> arena;
    assert(initializeKernel(arena.data(),arena.size()));const auto before=kernelStats();syntheticTests();originalShapeStorageTests();ownedLifetimeTests();imageIdentityDiagnosticTests();deferredTextureListTests();paletteLifetimeTests();captureLifetimeTests();framebufferImageIdentityTests();framebufferCaptureAcquisitionTests();indexedCaptureTests();
    if(argc==3&&!std::strcmp(argv[1],"--private-disc"))privateTitle(argv[2]);else assert(argc==1);
    assert(kernelStats().free==before.free&&kernelStats().healthy);
}
