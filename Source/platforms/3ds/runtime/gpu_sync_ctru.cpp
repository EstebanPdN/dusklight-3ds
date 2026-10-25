#include "gpu_sync.hpp"
#include "clock.hpp"
#include "gx_profile.hpp"
#include "../gx_texture_pica.hpp"
#include "../gx_geometry_pica.hpp"
#include <3ds.h>
#include <citro3d.h>
#include <atomic>
extern "C" OSThread* OSGetCurrentThread();
extern "C" void OSReport(const char*,...);

namespace {
std::atomic<gxCmdQueue_s*> queue{};
std::atomic<OSThread*> renderOwner{};
std::atomic<bool> queueFailure{};
dusk::native::GpuCompletion noteFailure(dusk::native::GpuCompletion result){
    if(result!=dusk::native::GpuCompletion::complete)queueFailure.store(true,std::memory_order_release);
    return result;
}
u32* frameCommands{};u32 frameCommandWords{};unsigned commandReclaims{};
// The GSP event thread calls the original queue handler and C3D callback
// inside this gate. An idle flag alone precedes callback completion in libctru.
LightLock lifecycleGate=1;
bool validQueue(const gxCmdQueue_s* input) {
    return input && input->maxEntries && input->numEntries <= input->maxEntries &&
        input->curEntry <= input->numEntries && input->lastEntry <= input->curEntry;
}
bool elapsed(std::uint64_t start,std::uint64_t ticks) {
    return std::uint64_t(svcGetSystemTick())-start>=ticks;
}
// On success the caller owns lifecycleGate; all other returns release it.
dusk::native::GpuCompletion idleAndLock(gxCmdQueue_s* input,std::uint64_t start,
        std::uint64_t ticks,bool run=true) {
    using dusk::native::GpuCompletion;
    for(;;) {
        if(!LightLock_TryLock(&lifecycleGate)) {
            if(!validQueue(input)){LightLock_Unlock(&lifecycleGate);return GpuCompletion::exhausted;}
            if(gxCmdQueueWait(input,0)&&input->lastEntry==input->numEntries)
                return GpuCompletion::complete;
            // Stopped pending entries must execute before any Stop/Clear.
            if(run)gxCmdQueueRun(input);
            LightLock_Unlock(&lifecycleGate);
        }
        if(!run||elapsed(start,ticks))return GpuCompletion::timedOut;
        svcSleepThread(1000000);
    }
}
dusk::native::GpuCompletion reserveEntries(gxCmdQueue_s* input,unsigned count,
        std::uint64_t start,std::uint64_t ticks) {
    using dusk::native::GpuCompletion;
    for(;;) {
        if(!LightLock_TryLock(&lifecycleGate)) {
            if(!validQueue(input)||!count||count>input->maxEntries){
                LightLock_Unlock(&lifecycleGate);return GpuCompletion::exhausted;
            }
            if(input->maxEntries-input->numEntries>=count){
                LightLock_Unlock(&lifecycleGate);return GpuCompletion::complete;
            }
            LightLock_Unlock(&lifecycleGate);
            const auto result=idleAndLock(input,start,ticks);
            if(result!=GpuCompletion::complete)return result;
            gxCmdQueueStop(input);gxCmdQueueClear(input);
            // Leave it stopped, matching C3D_FrameBegin. The next completion
            // or FrameEnd runs it after its complete operation is assembled.
            LightLock_Unlock(&lifecycleGate);return GpuCompletion::complete;
        }
        if(elapsed(start,ticks))return GpuCompletion::timedOut;
        svcSleepThread(1000000);
    }
}
constexpr std::uint64_t queueTimeout=500000000;
std::uint64_t queueTicks(std::uint64_t ns){
    return (ns*dusk::native::nativeTickFrequency+999999999)/1000000000;
}
}
// Capture the public queue binding made by C3D_Init/Fini. Keep Citro3D's own
// queue, IRQ handler and completion callback; do not read a private C3D context
// or replace its callbacks. Linker wrapping supplies the actual binding call.
extern "C" void __real_GX_BindQueue(gxCmdQueue_s*);
extern "C" void __wrap_GX_BindQueue(gxCmdQueue_s* input) {
    if(input)queueFailure.store(false,std::memory_order_release);
    renderOwner.store(input ? OSGetCurrentThread() : nullptr, std::memory_order_release);
    queue.store(input, std::memory_order_release);
    __real_GX_BindQueue(input);
}
// Forward the actual public IRQ handler unchanged, including the originally
// registered C3D callback. The verbatim pinned queue TU has only this entry
// renamed at compilation; the strong public handler cannot be bypassed by the
// installed event TU's co-located weak default (GNU --wrap cannot intercept it).
extern "C" void duskOriginalGxCmdQueueInterrupt(GSPGPU_Event);
extern "C" void gxCmdQueueInterrupt(GSPGPU_Event irq) {
    LightLock_Lock(&lifecycleGate);
    duskOriginalGxCmdQueueInterrupt(irq);
    LightLock_Unlock(&lifecycleGate);
}
namespace dusk::native {
std::uint64_t gxProfileTick(){return svcGetSystemTick();}
bool platformGpuFailurePending(){return queueFailure.load(std::memory_order_acquire);}
OSThread* platformGpuOwner() { return renderOwner.load(std::memory_order_acquire); }
bool platformBeginGpuCommands(){
    if(!queue.load(std::memory_order_acquire)||platformGpuOwner()!=OSGetCurrentThread())return false;
    u32 offset{};GPUCMD_GetBuffer(&frameCommands,&frameCommandWords,&offset);
    return frameCommands&&frameCommandWords>=8192&&offset==0;
}
bool platformPrepareGpuDraw(){
    if(!frameCommands||platformGpuOwner()!=OSGetCurrentThread())return false;
    u32* buffer{};u32 size{},offset{};GPUCMD_GetBuffer(&buffer,&size,&offset);
    const auto begin=reinterpret_cast<std::uintptr_t>(frameCommands),at=reinterpret_cast<std::uintptr_t>(buffer);
    if(at<begin||(at-begin)%4||(at-begin)/4>frameCommandWords||size>frameCommandWords-(at-begin)/4||offset>size)return false;
    if(platformReserveGpuEntries(1,500000000)!=GpuCompletion::complete)return false;
    // A full program/state/uniform update is smaller than this32 KiB reserve.
    // Leave room for C3D's flush/invalidate/end commands before recycling.
    if(size-offset>=8192)return true;
    if(platformCompleteGpu(500000000)!=GpuCompletion::complete)return false;
    // GPUCMD_Split normally advances through the frame's fixed command arena.
    // With every queued read complete, reusing its initial public span is safe;
    // Citro3D's render targets and hardware state remain part of the same frame.
    GPUCMD_SetBuffer(frameCommands,frameCommandWords,0);
    if(commandReclaims++<4)OSReport("Native GPU completed command block reclaimed (%lu words)\n",static_cast<unsigned long>(frameCommandWords));
    return true;
}

GpuCompletion platformQueryGpu(){
    auto* input=queue.load(std::memory_order_acquire);
    if(!input)return GpuCompletion::notReady;
    if(platformGpuOwner()!=OSGetCurrentThread())return GpuCompletion::wrongThread;
    if(LightLock_TryLock(&lifecycleGate))return GpuCompletion::notReady;
    u32 offset{};GPUCMD_GetBuffer(nullptr,nullptr,&offset);
    const bool done=!offset&&validQueue(input)&&input->lastEntry==input->numEntries&&gxCmdQueueWait(input,0);
    LightLock_Unlock(&lifecycleGate);
    return done?GpuCompletion::complete:GpuCompletion::notReady;
}
bool platformBeginGpuFrame(std::uint8_t flags){
    auto* input=queue.load(std::memory_order_acquire);
    if(!input||platformGpuOwner()!=OSGetCurrentThread()){queueFailure.store(true,std::memory_order_release);return false;}
    // VBlank delivery uses the same event thread: never hold its gate here.
    if(flags&C3D_FRAME_SYNCDRAW)C3D_FrameSync();
    const auto result=idleAndLock(input,std::uint64_t(svcGetSystemTick()),queueTicks(queueTimeout),!(flags&C3D_FRAME_NONBLOCK));
    if(result!=GpuCompletion::complete){
        if(!(flags&C3D_FRAME_NONBLOCK))noteFailure(result);
        return false;
    }
    // Nonblocking SDK admission under the IRQ/callback gate closes the race
    // between the old callback and C3D's private inFrame transition.
    const bool begun=C3D_FrameBegin(C3D_FRAME_NONBLOCK);
    LightLock_Unlock(&lifecycleGate);
    if(!begun&&!(flags&C3D_FRAME_NONBLOCK))noteFailure(GpuCompletion::exhausted);
    return begun;
}
bool platformEndGpuFrame(std::uint8_t flags){
    auto* input=queue.load(std::memory_order_acquire);
    if(!input||platformGpuOwner()!=OSGetCurrentThread()){queueFailure.store(true,std::memory_order_release);return false;}
    const auto result=idleAndLock(input,std::uint64_t(svcGetSystemTick()),queueTicks(queueTimeout));
    if(result!=GpuCompletion::complete){noteFailure(result);return false;}
    if(input->maxEntries<4){LightLock_Unlock(&lifecycleGate);noteFailure(GpuCompletion::exhausted);return false;}
    gxCmdQueueStop(input);gxCmdQueueClear(input);
    // Queue all final work while stopped. C3D sets all LCD swap flags, then
    // runs the queue. No finish callback can observe partially assembled state.
    C3D_FrameEnd(flags);
    LightLock_Unlock(&lifecycleGate);return true;
}
bool platformInvalidateGpuTextures() {
    if (!queue.load(std::memory_order_acquire) || platformGpuOwner() != OSGetCurrentThread())
        return false;
    invalidateGxTextures();
    return true;
}
GpuCompletion platformReserveGpuEntries(unsigned count,std::uint64_t nanoseconds) {
    auto* input=queue.load(std::memory_order_acquire);
    if(!input)return noteFailure(GpuCompletion::notReady);
    if(platformGpuOwner()!=OSGetCurrentThread())return noteFailure(GpuCompletion::wrongThread);
    if(!nanoseconds||nanoseconds>500000000)return noteFailure(GpuCompletion::timedOut);
    const auto ticks=(nanoseconds*nativeTickFrequency+999999999)/1000000000;
    return noteFailure(reserveEntries(input,count,std::uint64_t(svcGetSystemTick()),ticks));
}
GpuCompletion platformCompleteGpu(std::uint64_t nanoseconds) {
    GxProfileScope profile(GxProfilePart::wait);
    auto* input = queue.load(std::memory_order_acquire);
    if (!input) return noteFailure(GpuCompletion::notReady);
    if (platformGpuOwner() != OSGetCurrentThread()) return noteFailure(GpuCompletion::wrongThread);
    if (!nanoseconds || nanoseconds > 500000000) return noteFailure(GpuCompletion::timedOut);
    const auto ticks = (nanoseconds * nativeTickFrequency + 999999999) / 1000000000;
    const auto start = std::uint64_t(svcGetSystemTick());
    const auto reserved=reserveEntries(input,1,start,ticks);
    if(reserved!=GpuCompletion::complete)return noteFailure(reserved);
    if(!flushGxGeometry())return noteFailure(GpuCompletion::exhausted);
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    const auto result=idleAndLock(input,start,ticks);
    if(result==GpuCompletion::complete)LightLock_Unlock(&lifecycleGate);
    return noteFailure(result);
}
}
