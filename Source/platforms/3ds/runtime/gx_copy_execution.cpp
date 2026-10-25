#include "copy_lifecycle_probe.hpp"
#include "gx_copy_pixels.hpp"
#include "gx_copy_images.hpp"
#include "texture_gx.hpp"
#include "resource_archive.hpp"
#include "gx_framebuffer.hpp"
#include "gx_stream.hpp"
#include "gpu_sync.hpp"
#include "gx_profile.hpp"
#include "gx_texture_commands.hpp"
#include "gx_state.hpp"
#include "../gx_texture_pica.hpp"
#include "../gx_pica.hpp"
#include <cstdio>
#include <vector>
#include <dolphin/gx.h>
#include <dolphin/gx/GXExtra.h>
#include <dolphin/os.h>
#include <array>
#include <cstdlib>
#include <cstring>
namespace dusk::native {
namespace {
struct CopyImage {const void* identity{};std::uint8_t* bti{};std::size_t bytes{};gx::TextureHandle handle{};bool fixed{};unsigned copies{};std::span<const std::uint8_t> palette{};std::size_t pixelBytes{};
    // Diagnostic witness from the owner-supplied admitted extent, never from
    // allocator probing or a later metadata size. Zero means unproven identity.
    std::size_t admittedPixelCapacity{};unsigned lastRawFormat{};bool lastDepth{};
};
std::array<CopyImage,16> images{};
std::size_t used{};
constexpr std::size_t budget=4*1024*1024;
[[noreturn]] void fail(const char* reason){OSPanic(__FILE__,__LINE__,"Native EFB transfer: %s",reason);}
const gx::CommandRegisters& registers(){auto* stream=gx::commandStream();if(!stream||!stream->finish())fail("command boundary");return stream->registers();}
void discard(CopyImage& image){
    if(!releaseGxTexture(image.handle)||!gx::unregisterTexture(image.handle))fail("texture release");
    if(copyprobe::enabled)copyprobe::destroy(image.handle);
    used-=image.bytes;std::free(image.bti);image={};
}
// Failure-only diagnostics. Read only our retained header and saved admission
// witness; the original identity/palette pointers below are printed, not read.
// Keep readback/completion ordering and every existing rejection unchanged.
void reportLayoutFailure(const CopyImage& image,const ColorCopyPlan& plan,
                         const gx::CopySurface& source,const void* caller){
    const auto* b=image.bti;
    OSReport("Native EFB layout old: identity=%p handle=%lu fixed=%u copies=%u fmt=%u size=%ux%u cloneBytes=%lu pixelBytes=%lu admittedPixelCapacity=%lu lastRawFmt=%u lastDepth=%u\n",
        image.identity,static_cast<unsigned long>(image.handle),unsigned(image.fixed),image.copies,
        unsigned(b[0]),(unsigned(b[2])<<8)|b[3],(unsigned(b[4])<<8)|b[5],
        static_cast<unsigned long>(image.bytes),static_cast<unsigned long>(image.bytes-32-image.palette.size()),
        static_cast<unsigned long>(image.admittedPixelCapacity),image.lastRawFormat,unsigned(image.lastDepth));
    OSReport("Native EFB layout new: rawFmt=%u fmt=%u size=%ux%u pixelBytes=%lu depth=%u red=%u mipmap=%u src=%u,%u %ux%u\n",
        unsigned(source.format),plan.format,plan.width,plan.height,static_cast<unsigned long>(plan.bytes),
        unsigned(source.depth),unsigned(plan.redChannel),unsigned(source.mipmap),source.source[0],source.source[1],source.source[2],source.source[3]);
    const auto& extent=gx::currentTargetExtent;
    OSReport("Native EFB layout context: caller=%p framebuffer=%p logical=%ux%u physical=%ux%u palette=%p paletteBytes=%lu paletteFmt=%u paletteEntries=%u used=%lu budget=%lu\n",
        caller,activeFramebuffer(),extent.logicalWidth,extent.logicalHeight,extent.pixelWidth,extent.pixelHeight,
        static_cast<const void*>(image.palette.data()),static_cast<unsigned long>(image.palette.size()),
        unsigned(b[9]),(unsigned(b[10])<<8)|b[11],static_cast<unsigned long>(used),static_cast<unsigned long>(budget));
}
void clear(const gx::CommandRegisters& r){
    for(unsigned reg:{0x4fu,0x50u,0x51u})if(!r.bpValid[reg])fail("clear state missing");
    const auto ra=r.bp[0x4f],bg=r.bp[0x50];
    const u32 color=((ra&255)<<24)|(((bg>>8)&255)<<16)|((bg&255)<<8)|((ra>>8)&255);
    // PICA reversed depth maps original GX clear far (0xffffff) to zero.
    if(!platformClearFramebuffer(activeFramebuffer(),color,0xffffff-(r.bp[0x51]&0xffffff)))fail("clear");
}
}
void releaseCopyImages(){for(auto& image:images)if(image.handle)discard(image);}
std::size_t copyImageBytes(){return used;}
bool registerIndexedCopyImage(std::span<const std::uint8_t> header,const void* identity,
                              std::size_t imageBytes,std::span<const std::uint8_t> palette){
    if(header.size()!=32||!identity||reinterpret_cast<std::uintptr_t>(identity)%32||
       !platformGpuOwner()||platformGpuOwner()!=OSGetCurrentThread()||header[0]!=9||header[8]!=1||
       header[9]>2||header[24]!=1||palette.empty()||palette.size()>512||
       palette.size()!=2*((unsigned(header[10])<<8)|header[11]))return false;
    const unsigned width=(unsigned(header[2])<<8)|header[3],height=(unsigned(header[4])<<8)|header[5];
    ColorCopyPlan plan;
    if(!planColorCopy({width,height,width,height},0,0,width,height,width,height,0x28,plan)||plan.bytes>imageBytes)return false;
    CopyImage* image=nullptr;
    for(auto& value:images){if(value.identity==identity)return false;if(!value.handle&&!image)image=&value;}
    const auto bytes=32+plan.bytes+palette.size();if(!image||bytes>budget-used)return false;
    auto* bti=static_cast<std::uint8_t*>(std::calloc(1,bytes));if(!bti)return false;
    std::memcpy(bti,header.data(),32);bti[28]=bti[29]=bti[30]=0;bti[31]=32;
    const auto offset=32+plan.bytes;for(unsigned n=0;n<4;++n)bti[12+n]=offset>>(24-8*n);
    std::memcpy(bti+offset,palette.data(),palette.size());
    gx::TextureHandle handle{};
    if(!gx::registerTexture({bti,bytes},handle)){std::free(bti);return false;}
    if(!gx::aliasTextureImage(handle,identity)||!gx::aliasTexturePalette(handle,palette.data())){
        gx::unregisterTexture(handle);std::free(bti);return false;
    }
    *image={identity,bti,bytes,handle,true,0,palette,plan.bytes,imageBytes};used+=bytes;
    if(copyprobe::enabled)copyprobe::registration(handle,identity,bti,bti+32,bytes,plan.bytes,9,width,height);
    OSReport("Native indexed capture registered: %ux%u palette=%u bytes=%lu\n",width,height,unsigned(palette.size()/2),static_cast<unsigned long>(bytes));
    return true;
}
bool refreshCopyImagePalette(const void* identity){
    if(!platformGpuOwner()||platformGpuOwner()!=OSGetCurrentThread())return false;
    for(auto& image:images)if(image.identity==identity&&!image.palette.empty()){
        auto* target=image.bti+32+image.pixelBytes;
        if(std::memcmp(target,image.palette.data(),image.palette.size())){
            if(copyprobe::enabled)copyprobe::palette(image.handle);
            if(!invalidateGxTexture(image.handle))return false;
            std::memcpy(target,image.palette.data(),image.palette.size());
        }
    }
    return true;
}
bool registerCopyImage(std::span<const std::uint8_t> bti,const void* identity){
    if(!identity||reinterpret_cast<std::uintptr_t>(identity)%32||
       !platformGpuOwner()||platformGpuOwner()!=OSGetCurrentThread())return false;
    NativeTexturePlan plan;BtiInfo info;
    if(!inspectNativeTexture(bti,plan)||inspectBti({bti.data(),bti.size()},1024,info)!=ResourceStatus::ready||
       plan.gxFormat>6||plan.mipCount!=1||info.paletteEntries||info.image.size!=plan.sourceBytes)return false;
    CopyImage* image=nullptr;
    for(auto& value:images){
        if(value.identity==identity)return false;
        if(!value.handle&&!image)image=&value;
    }
    const auto bytes=32+plan.sourceBytes;
    if(!image||bytes>budget-used)return false;
    auto* copy=static_cast<std::uint8_t*>(std::malloc(bytes));if(!copy)return false;
    std::memcpy(copy,bti.data(),32);copy[28]=copy[29]=copy[30]=0;copy[31]=32;
    std::memcpy(copy+32,info.image.data,plan.sourceBytes);
    gx::TextureHandle handle{};
    if(!gx::registerTexture({copy,bytes},handle)){std::free(copy);return false;}
    if(!gx::aliasTextureImage(handle,identity)){gx::unregisterTexture(handle);std::free(copy);return false;}
    *image={identity,copy,bytes,handle,true,0};
    // createTimg supplies the actual JKR allocation span. For generic aliases,
    // retain unknown capacity rather than infer ownership from address/format.
    if(identity==info.image.data)image->admittedPixelCapacity=bti.size()-plan.dataOffset;
    used+=bytes;
    if(copyprobe::enabled)copyprobe::registration(handle,identity,copy,copy+32,bytes,plan.sourceBytes,plan.gxFormat,plan.width,plan.height);
    OSReport("Native capture allocation registered: fmt=%u size=%ux%u bytes=%lu; no EFB copy yet\n",
        unsigned(plan.gxFormat),unsigned(plan.width),unsigned(plan.height),static_cast<unsigned long>(bytes));
    return true;
}
}
namespace {
bool optionalNextCopy{};
std::uint64_t optionalCopiesSkipped{},optionalCopiesDone{};
}
namespace dusk::native {
namespace {
// GPU-resident capture policy (SD switches for A/B and orientation calibration).
int gpuCaptureState=-1;unsigned gpuCaptureOrientation=0,gpuCaptureCheckFrames=0;
bool gpuCaptureEnabled(){
    if(gpuCaptureState<0){
        gpuCaptureState=1;
        if(auto* f=std::fopen("sdmc:/3ds/dusklight/gpu-capture.txt","rb")){const int c=std::fgetc(f);std::fclose(f);if(c=='0')gpuCaptureState=0;}
        if(auto* f=std::fopen("sdmc:/3ds/dusklight/gpu-capture-orient.txt","rb")){unsigned v=0;if(std::fscanf(f,"%u",&v)==1&&v<16)gpuCaptureOrientation=v;std::fclose(f);}
        if(auto* f=std::fopen("sdmc:/3ds/dusklight/gpu-capture-check.txt","rb")){std::fclose(f);gpuCaptureCheckFrames=3;}
        OSReport("Native GPU-resident capture: %s orientation=%u check=%u\n",gpuCaptureState?"on":"off",gpuCaptureOrientation,gpuCaptureCheckFrames);
    }
    return gpuCaptureState==1;
}
// Make the copy image's texture resident (one decode) so every later capture
// stays on the GPU even while nothing samples it.
bool gpuCaptureResident(const void* identity,gx::TextureHandle handle){
    unsigned w{},h{};if(gpuCaptureTexture(handle,w,h))return true;
    std::uint32_t token{};gx::Texture texture;
    if(!gx::textureCommandToken(identity,token)||!gx::textureFromCommandToken(token,texture)||texture.handle!=handle)return false;
    float su,sv;const bool bound=bindGxTexture(texture,su,sv,0);invalidateGxTextureBindings();
    return bound&&gpuCaptureTexture(handle,w,h);
}
// Diagnostic: compare the GPU result with the CPU reference path (readback,
// GX encode, PICA decode) under the four axis flips of the image region.
void checkGpuCapture(FramebufferHandle framebuffer,const ColorCopyPlan& plan,const CopyImage& image){
    std::span<const std::byte> pixels;
    if(!platformReadFramebufferColor(framebuffer,pixels)){OSReport("Native GPU capture check: readback failed\n");return;}
    std::vector<std::uint8_t> bti(image.bti,image.bti+image.bytes);
    NativeTexturePlan np;
    if(!encodeColorCopy(plan,pixels,{bti.data()+32,plan.bytes})||!inspectNativeTexture(bti,np,false)){OSReport("Native GPU capture check: reference failed\n");return;}
    std::vector<std::uint8_t> ref(np.outputBytes);
    if(!decodeNativeTexture(bti,np,ref)){OSReport("Native GPU capture check: decode failed\n");return;}
    unsigned w=plan.width,h=plan.height;std::vector<std::uint8_t> result(np.outputBytes);
    if(!gpuCaptureRead(image.handle,result,np.paddedWidth,np.paddedHeight)){OSReport("Native GPU capture check: texture mismatch\n");return;}
    const auto* gpu=result.data();
    auto at=[&](unsigned x,unsigned y){return (((y/8)*(np.paddedWidth/8)+x/8)*64+((x&1)|((y&1)<<1)|((x&2)<<1)|((y&2)<<2)|((x&4)<<2)|((y&4)<<3)))*4;};
    double best=1e9;unsigned bestVariant=0;
    for(unsigned v=0;v<4;++v){
        std::uint64_t sum=0;
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
            const unsigned fx=v&1?w-1-x:x,fy=v&2?h-1-y:y;
            const auto a=at(x,y),b=at(fx,fy);
            for(unsigned c=0;c<4;++c){const int d=int(gpu[a+c])-int(ref[b+c]);sum+=d<0?-d:d;}
        }
        const double mean=double(sum)/(double(w)*h*4);
        OSReport("Native GPU capture check: orientation=%u readFlip=%u mean_abs_diff=%.2f\n",gpuCaptureOrientation,v,mean);
        if(mean<best){best=mean;bestVariant=v;}
    }
    OSReport("Native GPU capture check: best readFlip=%u mean=%.2f (0 means the configured orientation is right)\n",bestVariant,best);
}
// Complete EFB texture copy from an explicit setup snapshot. Never reads the
// live register bank, so a deferred capture may run inside a draw.
void executeTextureCopy(void* identity,GXBool doClear,const gx::CopySurface& s,const gx::CommandRegisters* r,
                        FramebufferHandle framebuffer,const void* caller){
    using namespace dusk::native;
    ColorCopyPlan plan;
    const auto planner=s.depth?planDepthCopy:planColorCopy;
    if(!planner(gx::currentTargetExtent,s.source[0],s.source[1],s.source[2],s.source[3],s.width,s.height,unsigned(s.format),plan))
        fail("copy format/extent");
    if(!s.depth&&gpuCaptureEnabled()){
        CopyImage* existing=nullptr;
        for(auto& value:images)if(value.identity==identity){existing=&value;break;}
        if(existing&&existing->handle&&existing->palette.empty()&&existing->bytes==plan.bytes+32&&existing->bti[0]==plan.format&&
           (unsigned(existing->bti[2])<<8|existing->bti[3])==plan.width&&(unsigned(existing->bti[4])<<8|existing->bti[5])==plan.height&&
           gpuCaptureResident(existing->identity,existing->handle)&&
           gpuCaptureCopy(framebuffer,existing->handle,plan,gpuCaptureOrientation)){
            if(gpuCaptureCheckFrames){--gpuCaptureCheckFrames;checkGpuCapture(framebuffer,plan,*existing);}
            existing->lastRawFormat=unsigned(s.format);existing->lastDepth=false;++existing->copies;
            if(doClear&&r)clear(*r);
            return;
        }
    }
    std::span<const std::byte> pixels;
    const auto readback=s.depth?platformReadFramebufferDepthImage:platformReadFramebufferColor;
    if(!readback(framebuffer,pixels))fail("GPU readback");
    CopyImage* image=nullptr;
    for(auto& value:images)if(value.identity==identity){image=&value;break;}
    const bool indexed=image&&!image->palette.empty();
    if(indexed&&(!plan.redChannel||image->pixelBytes!=plan.bytes)){
        reportLayoutFailure(*image,plan,s,caller);
        fail("indexed copy source format/extent");
    }
    if(image&&(image->bytes!=plan.bytes+32+image->palette.size()||(!indexed&&image->bti[0]!=plan.format)||
        (unsigned(image->bti[2])<<8|image->bti[3])!=plan.width||(unsigned(image->bti[4])<<8|image->bti[5])!=plan.height)){
        if(image->fixed){
            reportLayoutFailure(*image,plan,s,caller);
            fail("original capture allocation layout changed");
        }
        discard(*image);
    }
    if(!image)for(auto& value:images)if(!value.handle){image=&value;break;}
    if(!image)fail("copy slots exhausted");
    const auto encoder=s.depth?encodeDepthCopy:encodeColorCopy;
    if(!image->handle){
        if(plan.bytes+32>budget-used)fail("copy budget exhausted");
        image->bti=static_cast<std::uint8_t*>(std::calloc(1,plan.bytes+32));if(!image->bti)fail("copy allocation");
        image->bytes=plan.bytes+32;image->identity=identity;used+=image->bytes;
        auto* b=image->bti;b[0]=plan.format;b[1]=1;b[2]=plan.width>>8;b[3]=plan.width;b[4]=plan.height>>8;b[5]=plan.height;
        b[20]=b[21]=1;b[24]=1;b[31]=32;
        if(!encoder(plan,pixels,{b+32,plan.bytes})||!gx::registerTexture({b,image->bytes},image->handle)||
           !gx::aliasTextureImage(image->handle,identity))fail("copy registration");
    }else{
        auto fused=FusedColorCopyResult::skipped;
        // Depth normalizes to IA8 too: exclude its provenance before dispatch.
        if(fusedColorCopyEnabled&&!s.depth&&!indexed)
            fused=refreshResidentColorCopy(image->handle,plan,pixels,{image->bti,image->bytes});
        if(fused==FusedColorCopyResult::failed||
           (fused==FusedColorCopyResult::skipped&&
            (!encoder(plan,pixels,{image->bti+32,plan.bytes})||!invalidateGxTexture(image->handle))))fail("copy refresh");
    }
    if(!refreshCopyImagePalette(identity))fail("copy palette refresh");
    if(copyprobe::enabled){
        copyprobe::registration(image->handle,identity,image->bti,image->bti+32,image->bytes,plan.bytes,
                                image->bti[0],plan.width,plan.height);
        observeCopyLifecycleResidency(image->handle,plan,s.depth,indexed,pixels.size());
    }
    image->lastRawFormat=unsigned(s.format);image->lastDepth=s.depth;
    if(!image->copies++)OSReport("Native actual EFB %s copy: size=%ux%u bytes=%lu\n",
        s.depth?"depth":"color",unsigned(plan.width),unsigned(plan.height),static_cast<unsigned long>(plan.bytes));
    if(doClear&&r)clear(*r);
}
struct PendingCapture {bool active{};void* identity{};gx::CopySurface setup{};FramebufferHandle framebuffer{};};
PendingCapture pendingCapture;
std::uint64_t deferredMaterialized{};
void materializePendingCapture(){
    if(!pendingCapture.active)return;
    pendingCapture.active=false;++deferredMaterialized;
    executeTextureCopy(pendingCapture.identity,GX_FALSE,pendingCapture.setup,nullptr,pendingCapture.framebuffer,nullptr);
}
}
void dropPendingCapture(){pendingCapture.active=false;}
}
// The game marks its unconditional per-frame screen capture as optional.
extern "C" void GXNativeOptionalCopy(){optionalNextCopy=true;}
namespace dusk::native {
void optionalCopyStatistics(std::uint64_t& skipped,std::uint64_t& done,std::uint64_t& materialized){skipped=optionalCopiesSkipped;done=optionalCopiesDone;materialized=deferredMaterialized;}
}
extern "C" void GXCopyTex(void* identity,GXBool doClear){
    using namespace dusk::native;
    const bool optional=optionalNextCopy;optionalNextCopy=false;
    GxProfileScope profile(GxProfilePart::copy);
    const auto& r=registers();const auto& s=r.copy.texture;
    if(gxSparseVertexProfiling){
        static unsigned reports{};
        if(reports<24){++reports;OSReport("Native EFB copy caller=%p dst=%ux%u fmt=%u depth=%u\n",__builtin_return_address(0),
            unsigned(s.width),unsigned(s.height),unsigned(s.format),unsigned(s.depth));}
    }
    if(!identity||reinterpret_cast<std::uintptr_t>(identity)%32||!s.sourceReady||!s.destinationReady)fail("texture descriptor");
    ColorCopyPlan plan;
    const auto planner=s.depth?planDepthCopy:planColorCopy;
    if(!planner(gx::currentTargetExtent,s.source[0],s.source[1],s.source[2],s.source[3],s.width,s.height,unsigned(s.format),plan)){
        OSReport("Native EFB copy rejected: fmt=%u src=%u,%u %ux%u dst=%ux%u target=%ux%u\n",unsigned(s.format),s.source[0],s.source[1],s.source[2],s.source[3],s.width,s.height,gx::currentTargetExtent.logicalWidth,gx::currentTargetExtent.logicalHeight);
        fail("copy format/extent");
    }
    if(optional&&!doClear){
        // Skip the readback/encode/re-decode while nothing sampled this capture
        // in the last 30 frames. The resident image keeps its previous pixels.
        std::uint32_t token{};
        if(gx::textureCommandToken(identity,token))gx::watchedCaptureToken=token;
        CopyImage* existing=nullptr;
        for(auto& value:images)if(value.identity==identity){existing=&value;break;}
        const bool sameLayout=existing&&existing->handle&&existing->palette.empty()&&!s.depth&&
            existing->bytes==plan.bytes+32&&existing->bti[0]==plan.format&&
            (unsigned(existing->bti[2])<<8|existing->bti[3])==plan.width&&(unsigned(existing->bti[4])<<8|existing->bti[5])==plan.height;
        {static unsigned reports;if(reports<4){++reports;OSReport("Native optional capture: identity=%p token=%lx existing=%p handle=%llu sameLayout=%d frame=%llu lastUse=%llu bytes=%lu/%lu\n",
            identity,static_cast<unsigned long>(token),static_cast<void*>(existing),existing?static_cast<unsigned long long>(existing->handle):0ull,int(sameLayout),
            static_cast<unsigned long long>(gx::textureFrameCounter),static_cast<unsigned long long>(gx::watchedCaptureLastUse),
            existing?static_cast<unsigned long>(existing->bytes):0ul,static_cast<unsigned long>(plan.bytes+32));}}
        if(sameLayout&&token&&gx::textureFrameCounter-gx::watchedCaptureLastUse>30){
            // Defer: a same-frame consumer (full-screen effect) materializes it
            // right before its draw; unused captures are never read back.
            pendingCapture={true,identity,s,activeFramebuffer()};
            gx::watchedCaptureMaterialize=materializePendingCapture;
            ++optionalCopiesSkipped;return;
        }
        pendingCapture.active=false;
        ++optionalCopiesDone;
    }
    executeTextureCopy(identity,doClear,s,&r,activeFramebuffer(),__builtin_extract_return_addr(__builtin_return_address(0)));
}
extern "C" void GXDestroyCopyTex(void* identity){
    using namespace dusk::native;
    if(!identity)return;
    (void)registers();
    if(platformCompleteGpu(500000000)!=GpuCompletion::complete)fail("copy destruction completion");
    for(auto& image:images)if(image.identity==identity){discard(image);return;}
}
extern "C" void GXCopyDisp(void* identity,GXBool doClear){
    using namespace dusk::native;
    const auto& r=registers();const auto& s=r.copy.display;
    if(!identity||!s.sourceReady||!s.destinationReady||!activeFramebuffer())fail("display descriptor");
    if(activeFramebuffer()!=mainFramebuffer())fail("display copy from offscreen target");
    ColorCopyPlan validated;
    if(!planColorCopy(gx::currentTargetExtent,s.source[0],s.source[1],s.source[2],s.source[3],s.width,s.height,6,validated))fail("display extent");
    // C3D_FrameEnd transfers this actual main surface to the top LCD. Original
    // XFB allocations are identities; C3D owns presentation/storage on 3DS.
    if(platformCompleteGpu(500000000)!=GpuCompletion::complete)fail("display completion");
    // Clearing must occur after the deferred LCD transfer, at next frame begin.
    // Main's frame loop clears then; an immediate fill would erase this frame.
    if(doClear){
        for(unsigned reg:{0x4fu,0x50u,0x51u})if(!r.bpValid[reg])fail("display clear state");
        const auto ra=r.bp[0x4f],bg=r.bp[0x50];
        const u32 color=((ra&255)<<24)|(((bg>>8)&255)<<16)|((bg&255)<<8)|((ra>>8)&255);
        if(!deferMainFramebufferClear(color,0xffffff-(r.bp[0x51]&0xffffff)))fail("deferred display clear");
    }
}
