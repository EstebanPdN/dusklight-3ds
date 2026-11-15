#include "gpu_sync.hpp"
#include "clock.hpp"
#include "gx_profile.hpp"
#include "gpu_canary.hpp"
#include "../gx_texture_pica.hpp"
#include "../gx_geometry_pica.hpp"
#include <3ds.h>
#include <citro3d.h>
#include <atomic>
#include <cstdio>
extern "C" OSThread* OSGetCurrentThread();
extern "C" void OSReport(const char*,...);
namespace dusk::native { void gpuCanaryListSubmitted(const u32* list,u32 bytes); }

namespace {
std::atomic<gxCmdQueue_s*> queue{};
std::atomic<OSThread*> renderOwner{};
std::atomic<bool> queueFailure{};
dusk::native::GpuCompletion noteFailure(dusk::native::GpuCompletion result){
    if(result!=dusk::native::GpuCompletion::complete){
        queueFailure.store(true,std::memory_order_release);
        // Console evidence: a PICA that never raises its IRQ leaves lastEntry
        // behind curEntry. The first few failures are enough to tell.
        static unsigned reports;
        if(reports<6){
            ++reports;const auto* q=queue.load(std::memory_order_acquire);
            OSReport("Native GPU completion failed: result=%u entries=%u submitted=%u done=%u max=%u\n",unsigned(result),
                q?unsigned(q->numEntries):0u,q?unsigned(q->curEntry):0u,q?unsigned(q->lastEntry):0u,q?unsigned(q->maxEntries):0u);
        }
    }
    return result;
}
u32* frameCommands{};u32 frameCommandWords{};unsigned commandReclaims{};
// Render-owner telemetry and incremental submission state.
dusk::native::GpuFrameTelemetry telemetry{};
unsigned chunkDraws{},drawsSinceSubmit{};
// LCD retrace count observed when the previous frame's queue (including its
// display transfer and buffer swap request) was seen complete.
u32 idleRetrace{};bool idleRetraceValid{};
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
// GPU patience. The console PICA is asynchronous: a job can be slow, or never
// signal at all. A wait that passes its ordinary timeout with a job in flight
// keeps waiting up to patienceNanoseconds: a late completion is logged with
// its time and the game goes on; a silent job is a hang and goes to the GPU
// guard (gpu_canary_ctru.cpp), which saves the evidence and restarts.
constexpr std::uint64_t patienceNanoseconds=6000000000ull;
bool jobInFlight(gxCmdQueue_s* input,gxCmdEntry_s& entry,unsigned& index,unsigned& count){
    if(LightLock_TryLock(&lifecycleGate))return false;
    const bool flying=validQueue(input)&&input->lastEntry<input->curEntry;
    if(flying){entry=input->entries[input->lastEntry];index=input->lastEntry;count=input->numEntries;}
    LightLock_Unlock(&lifecycleGate);
    return flying;
}
}
extern "C" void duskOriginalGxCmdQueueInterrupt(GSPGPU_Event);
extern "C" void duskGxJobMarkAssumed(void);
namespace {
// Lost completions. GSP keeps at most 52 pending interrupts per process and
// drops new ones while its queue is full (a long stall of the event thread),
// so a transfer that finished can go unseen while the PICA is fine. A display
// transfer or texture copy silent for a second is counted complete (logged).
// The transfer engine itself stopping keeps every later transfer silent too:
// after a few such recoveries the guard records the hang as before.
constexpr std::uint64_t transferPatienceNanoseconds=1000000000ull;
constexpr unsigned lostTransferLimit=4;
unsigned lostTransfers{};
// The last transfer counted complete without its event: a job that then stays
// silent is waiting behind a stopped transfer engine, not a draw (Alpha 2).
std::uint64_t lastAssumedTick{};unsigned lastAssumedType{};std::uint32_t lastAssumedWords[8]{};
bool assumeLostTransfer(gxCmdQueue_s* input,const gxCmdEntry_s& entry,unsigned index,unsigned waited){
    const unsigned type=entry.data[0]&0xff;
    if((type!=3&&type!=4)||lostTransfers>=lostTransferLimit)return false;
    if(LightLock_TryLock(&lifecycleGate))return false;
    const bool same=validQueue(input)&&input->lastEntry<input->curEntry&&input->lastEntry==index;
    if(same){duskOriginalGxCmdQueueInterrupt(GSPGPU_EVENT_PPF);duskGxJobMarkAssumed();}
    LightLock_Unlock(&lifecycleGate);
    if(!same)return false;
    ++lostTransfers;
    lastAssumedTick=svcGetSystemTick();lastAssumedType=type;
    for(unsigned i=0;i<8;++i)lastAssumedWords[i]=entry.data[i];
    OSReport("Native GPU transfer job %u (type %u) silent for %u ms: counted complete (lost completion %u of %u allowed)\n",
        index,type,waited,lostTransfers,lostTransferLimit);
    return true;
}
// On success the caller owns lifecycleGate; all other returns release it.
dusk::native::GpuCompletion idleAndLock(gxCmdQueue_s* input,std::uint64_t start,
        std::uint64_t ticks,bool run=true) {
    using dusk::native::GpuCompletion;
    const std::uint64_t patience=(patienceNanoseconds/1000)*dusk::native::nativeTickFrequency/1000000;
    bool late=false;gxCmdEntry_s lateEntry{};
    for(;;) {
        if(!LightLock_TryLock(&lifecycleGate)) {
            if(!validQueue(input)){LightLock_Unlock(&lifecycleGate);return GpuCompletion::exhausted;}
            if(gxCmdQueueWait(input,0)&&input->lastEntry==input->numEntries){
                if(late)dusk::native::gpuSlowJob(lateEntry.data,unsigned((std::uint64_t(svcGetSystemTick())-start)*1000/dusk::native::nativeTickFrequency));
                return GpuCompletion::complete;
            }
            // Stopped pending entries must execute before any Stop/Clear.
            if(run)gxCmdQueueRun(input);
            LightLock_Unlock(&lifecycleGate);
        }
        if(!run)return GpuCompletion::timedOut;
        if(elapsed(start,ticks)){
            gxCmdEntry_s entry{};unsigned index{},count{};
            if(!jobInFlight(input,entry,index,count)){
                // Nothing submitted is pending: an ordinary queue timeout.
                if(!late||elapsed(start,patience))return GpuCompletion::timedOut;
            }else{
                late=true;lateEntry=entry;
                const auto waited=unsigned((std::uint64_t(svcGetSystemTick())-start)*1000/dusk::native::nativeTickFrequency);
                const std::uint64_t transferPatience=(transferPatienceNanoseconds/1000)*dusk::native::nativeTickFrequency/1000000;
                if(elapsed(start,transferPatience>ticks?transferPatience:ticks)&&assumeLostTransfer(input,entry,index,waited)){
                    late=false;continue;
                }
                if(elapsed(start,patience>ticks?patience:ticks))
                    dusk::native::gpuHang(entry.data,index,count,waited);
            }
        }
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
// Every PICA command list must be in RAM before the GPU reads it. Citro3D's
// in-frame C3D_SyncTextureCopy/DisplayTransfer/MemoryFill split the frame with
// no flush flag: upstream relies on C3D_FrameEnd(0) flushing the whole linear
// heap before the queue runs. Here the queue runs mid-frame, so on a New 3DS the
// first capture of the title scene sent a stale list (no FINALIZE, no P3D IRQ):
// every later completion timed out. Azahar has no cache and never showed it.
// The draws in such a list read the geometry arena, so publish it first too.
extern "C" Result __real_GX_ProcessCommandList(u32*,u32,u8);
extern "C" Result __wrap_GX_ProcessCommandList(u32* list,u32 bytes,u8 flags) {
    if(renderOwner.load(std::memory_order_acquire)==OSGetCurrentThread())(void)dusk::native::flushGxGeometry();
    dusk::native::gpuCanaryListSubmitted(list,bytes);
    return __real_GX_ProcessCommandList(list,bytes,flags|GX_CMDLIST_FLUSH);
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
[[noreturn]] void transferTestStop(const std::uint32_t words[8]){
    lastAssumedTick=svcGetSystemTick();lastAssumedType=words[0]&0xff;
    for(unsigned i=0;i<8;++i)lastAssumedWords[i]=words[i];
    OSReport("Native GPU transfer test: job type %lu reported as a stopped transfer engine\n",static_cast<unsigned long>(lastAssumedType));
    const std::uint32_t entry[8]{0x01000101,0,0,0,0,0,0,1};
    gpuHang(entry,0,1,6000);
}
bool gpuRecentAssumedTransfer(unsigned& ageMilliseconds,unsigned& type,std::uint32_t words[8]){
    if(!lastAssumedTick)return false;
    const auto age=(std::uint64_t(svcGetSystemTick())-lastAssumedTick)*1000/dusk::native::nativeTickFrequency;
    ageMilliseconds=unsigned(age>100000?100000:age);type=lastAssumedType;
    for(unsigned i=0;i<8;++i)words[i]=lastAssumedWords[i];
    return true;
}
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
    // Incremental submission: let the PICA start on the draws assembled so far
    // while the CPU keeps translating the rest of the frame.
    if(chunkDraws&&++drawsSinceSubmit>=chunkDraws&&offset){
        if(!platformSubmitGpu())return false;
        GPUCMD_GetBuffer(&buffer,&size,&offset);
    }
    if(platformReserveGpuEntries(1,500000000)!=GpuCompletion::complete)return false;
    // A full program/state/uniform update is smaller than this32 KiB reserve.
    // Leave room for C3D's flush/invalidate/end commands before recycling.
    if(size-offset>=8192)return true;
    ++telemetry.reclaims;
    if(platformCompleteGpu(500000000)!=GpuCompletion::complete)return false;
    // GPUCMD_Split normally advances through the frame's fixed command arena.
    // With every queued read complete, reusing its initial public span is safe;
    // Citro3D's render targets and hardware state remain part of the same frame.
    GPUCMD_SetBuffer(frameCommands,frameCommandWords,0);
    if(commandReclaims++<4)OSReport("Native GPU completed command block reclaimed (%lu words)\n",static_cast<unsigned long>(frameCommandWords));
    return true;
}

void platformSetGpuChunkDraws(unsigned draws){chunkDraws=draws;drawsSinceSubmit=0;}
GpuFrameTelemetry platformTakeGpuTelemetry(){const auto value=telemetry;telemetry={};return value;}
bool platformSubmitGpu(){
    auto* input=queue.load(std::memory_order_acquire);
    if(!input||!frameCommands||platformGpuOwner()!=OSGetCurrentThread())return false;
    drawsSinceSubmit=0;
    u32 offset{};GPUCMD_GetBuffer(nullptr,nullptr,&offset);
    if(!offset)return true;
    // Keep at most two command lists ahead of the PICA: when it is the slower
    // side, let this part grow instead of filling the bounded GX queue.
    if(validQueue(input)&&input->numEntries-input->lastEntry>=2)return true;
    if(platformReserveGpuEntries(1,500000000)!=GpuCompletion::complete)return false;
    if(!flushGxGeometry()){noteFailure(GpuCompletion::exhausted);return false;}
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    // C3D_FrameBegin leaves the queue stopped; start it without waiting. The
    // IRQ gate is held only for the SDK call; a busy gate means the event
    // thread is already advancing the queue.
    if(!LightLock_TryLock(&lifecycleGate)){
        if(validQueue(input))gxCmdQueueRun(input);
        LightLock_Unlock(&lifecycleGate);
    }
    ++telemetry.chunks;return true;
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
    const auto waitStart=std::uint64_t(svcGetSystemTick());
    const auto result=idleAndLock(input,waitStart,queueTicks(queueTimeout),!(flags&C3D_FRAME_NONBLOCK));
    telemetry.beginWaitTicks+=std::uint64_t(svcGetSystemTick())-waitStart;
    if(result!=GpuCompletion::complete){
        if(!(flags&C3D_FRAME_NONBLOCK))noteFailure(result);
        return false;
    }
    // The previous frame, its LCD transfer and its swap request are complete.
    idleRetrace=C3D_FrameCounter(0);idleRetraceValid=true;drawsSinceSubmit=0;
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
    // Without a per-frame FrameSync, a very short frame after a late one could
    // queue its LCD transfer before the previous swap reached a retrace, i.e.
    // into the buffer still being scanned out. Require one retrace since the
    // previous frame was seen complete; ordinary paced frames never wait here.
    // The IRQ gate is not held: VBlank is delivered by the same event thread.
    if(idleRetraceValid&&C3D_FrameCounter(0)==idleRetrace){
        const auto presentStart=std::uint64_t(svcGetSystemTick());
        for(unsigned tries=0;tries<4&&C3D_FrameCounter(0)==idleRetrace;++tries)gspWaitForVBlank0();
        telemetry.presentWaitTicks+=std::uint64_t(svcGetSystemTick())-presentStart;
    }
    const auto waitStart=std::uint64_t(svcGetSystemTick());
    const auto result=idleAndLock(input,waitStart,queueTicks(queueTimeout));
    telemetry.endWaitTicks+=std::uint64_t(svcGetSystemTick())-waitStart;
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
    telemetry.completeWaitTicks+=std::uint64_t(svcGetSystemTick())-start;++telemetry.completes;
    drawsSinceSubmit=0;
    return noteFailure(result);
}
}
