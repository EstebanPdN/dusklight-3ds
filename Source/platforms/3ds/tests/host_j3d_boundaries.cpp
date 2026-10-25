// Host test boundaries: real texture registrations/ownership, no GPU execution.
#include "../runtime/gpu_sync.hpp"
#include "../runtime/copy_lifecycle_probe.hpp"
#include "../runtime/alpha_interval_target.hpp"
#include "../gx_pica.hpp"
#include <cassert>
#include "../gx_texture_pica.hpp"
#include "../runtime/gx_framebuffer.hpp"
#include <dolphin/os.h>
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
namespace {std::span<const std::byte> colorPixels,depthPixels;}
void setHostFramebufferImages(std::span<const std::byte> color,std::span<const std::byte> depth){colorPixels=color;depthPixels=depth;}
OSThread* platformGpuOwner(){return OSGetCurrentThread();}
GpuCompletion platformCompleteGpu(std::uint64_t){return GpuCompletion::complete;}
bool releaseGxTexture(gx::TextureHandle){return true;}
bool invalidateGxTexture(gx::TextureHandle){return true;}
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
bool platformReadFramebufferColor(FramebufferHandle handle,std::span<const std::byte>& out){out=colorPixels;return handle&&!out.empty();}
bool platformReadFramebufferDepthImage(FramebufferHandle handle,std::span<const std::byte>& out){out=depthPixels;return handle&&!out.empty();}
}
