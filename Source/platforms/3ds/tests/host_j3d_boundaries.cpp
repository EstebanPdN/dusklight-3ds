// Host test boundaries: real texture registrations/ownership, no GPU execution.
#include "../runtime/gpu_sync.hpp"
#include "../runtime/copy_lifecycle_probe.hpp"
#include "../runtime/alpha_interval_target.hpp"
#include "../gx_pica.hpp"
#include <cassert>
#include "../gx_texture_pica.hpp"
#include "../runtime/gx_framebuffer.hpp"
#include <dolphin/os.h>
#include <array>
#include <vector>
#include <mutex>
namespace {
std::recursive_mutex exclusion;
thread_local bool enabled=true;
}
extern "C" BOOL OSDisableInterrupts() {
    const bool before=enabled;exclusion.lock();enabled=false;return before;
}
extern "C" BOOL OSRestoreInterrupts(BOOL before) {
    const bool old=enabled;enabled=before;exclusion.unlock();return old;
}
namespace dusk::native {
namespace {
std::span<const std::byte> colorPixels,depthPixels;
struct HostTexture {
    gx::TextureHandle handle{};
    NativeTexturePlan plan{};
    bool immutable{},dirty{};
    std::vector<std::uint8_t> pixels;
};
std::array<HostTexture,16> textures{};
bool textureScope{};
HostTexture* findTexture(gx::TextureHandle handle){
    for(auto& texture:textures)if(texture.handle==handle)return &texture;
    return nullptr;
}
}
unsigned hostCaptureReads{},hostCaptureBinds{},hostCaptureDecodes{},hostCaptureAllocations{},hostCaptureInvalidations{};
unsigned hostBindingInvalidations{},hostCaptureEvent{},hostReadEvent{},hostInvalidateEvent{},hostBindEvent{};
void resetHostCaptureObservations(){
    hostCaptureReads=hostCaptureBinds=hostCaptureDecodes=hostCaptureAllocations=hostCaptureInvalidations=0;
    hostBindingInvalidations=hostCaptureEvent=hostReadEvent=hostInvalidateEvent=hostBindEvent=0;
}
std::span<const std::uint8_t> hostCapturePixels(gx::TextureHandle handle){
    if(auto* texture=findTexture(handle))return texture->pixels;
    return {};
}
bool hostCaptureDirty(gx::TextureHandle handle){
    if(auto* texture=findTexture(handle))return texture->dirty;
    return false;
}
void setHostFramebufferImages(std::span<const std::byte> color,std::span<const std::byte> depth){colorPixels=color;depthPixels=depth;}
OSThread* platformGpuOwner(){return OSGetCurrentThread();}
GpuCompletion platformCompleteGpu(std::uint64_t){return GpuCompletion::complete;}
bool releaseGxTexture(gx::TextureHandle handle){
    if(auto* texture=findTexture(handle))*texture={};
    return true;
}
bool invalidateGxTexture(gx::TextureHandle handle){
    ++hostCaptureInvalidations;hostInvalidateEvent=++hostCaptureEvent;
    if(auto* texture=findTexture(handle))texture->dirty=true;
    return true;
}
// This fixture substitutes the cache and never executes Citro3D/GPU work. It
// retains stale converted bytes until bind, like the real dirty-cache contract.
GxTextureDrawScope::GxTextureDrawScope(std::span<const gx::Texture> inputs){
    if(!textureScope&&inputs.size()<=3){textureScope=true;active=true;}
}
GxTextureDrawScope::~GxTextureDrawScope(){if(active)textureScope=false;}
bool bindGxTexture(const gx::Texture& input,float& scaleU,float& scaleV,unsigned unit,bool projective){
    ++hostCaptureBinds;hostBindEvent=++hostCaptureEvent;
    if(!input.handle||unit!=0||projective)return false;
    auto* texture=findTexture(input.handle);
    if(!texture){
        for(auto& entry:textures)if(!entry.handle){texture=&entry;break;}
        if(!texture)return false;
        texture->handle=input.handle;texture->dirty=true;++hostCaptureAllocations;
    }
    if(texture->dirty){
        NativeTexturePlan plan;
        if(!inspectNativeTexture(input.bti,plan)||plan.originalWidth!=input.width||
           plan.originalHeight!=input.height||plan.gxFormat!=input.format||plan.originalMipCount!=input.mipCount)return false;
        texture->pixels.resize(plan.outputBytes);
        if(!decodeNativeTexture(input.bti,plan,texture->pixels,true))return false;
        texture->plan=plan;texture->immutable=input.immutablePixels;texture->dirty=false;++hostCaptureDecodes;
    }
    scaleU=float(texture->plan.width)/texture->plan.paddedWidth;
    scaleV=float(texture->plan.height)/texture->plan.paddedHeight;
    return true;
}
void* gpuCaptureTexture(gx::TextureHandle handle,unsigned& width,unsigned& height){
    auto* texture=findTexture(handle);
    if(!texture||textureScope||texture->immutable||texture->plan.format!=NativeTextureFormat::rgba8||
       texture->plan.sourceMip||texture->plan.mipCount!=1||texture->pixels.empty())return nullptr;
    width=texture->plan.width;height=texture->plan.height;return texture;
}
bool gpuCaptureRead(gx::TextureHandle,std::span<std::uint8_t>,unsigned,unsigned){return false;}
bool gpuCaptureCopy(void*,std::uint64_t,const ColorCopyPlan&,unsigned){return false;}
void invalidateGxTextureBindings(){++hostBindingInvalidations;}
// This fixture substitutes the entire native cache. It cannot exercise fusion;
// the real copy caller must take its original encoder branch while OFF.
bool fusedColorCopyEnabled{};
FusedColorCopyResult refreshResidentColorCopy(gx::TextureHandle,const ColorCopyPlan&,
    std::span<const std::byte>,std::span<std::uint8_t>){
    assert(!fusedColorCopyEnabled && "host J3D cache substitute requires fused copy OFF");
    assert(false && "OFF copy caller must never dispatch fused refresh");
    return FusedColorCopyResult::failed;
}
// This fixture never executes the PICA alpha backend. As in the copy-pixel
// fixture, accept its idle boundary only while stencil ownership is disabled.
bool alphaIntervalCopySafe(){
    assert(!alphaIntervalTargets.enabled && !alphaIntervalTargets.owner);
    return true;
}
// This fixture substitutes the native cache; residency observations require
// the real cache and must never be silently accepted with the probe enabled.
void observeCopyLifecycleResidency(gx::TextureHandle,const ColorCopyPlan&,bool,bool,std::size_t){
    assert(!copyprobe::enabled && "host J3D cache substitute requires the copy probe OFF");
}
FramebufferHandle platformCreateFramebuffer(unsigned,unsigned){return nullptr;}
bool platformBindFramebuffer(FramebufferHandle handle){return handle!=nullptr;}
bool platformClearFramebuffer(FramebufferHandle handle,std::uint32_t,std::uint32_t){return handle!=nullptr;}
bool platformReadFramebufferColor(FramebufferHandle handle,std::span<const std::byte>& out){
    ++hostCaptureReads;hostReadEvent=++hostCaptureEvent;out=colorPixels;return handle&&!out.empty();
}
bool platformReadFramebufferDepthImage(FramebufferHandle handle,std::span<const std::byte>& out){out=depthPixels;return handle&&!out.empty();}
}
