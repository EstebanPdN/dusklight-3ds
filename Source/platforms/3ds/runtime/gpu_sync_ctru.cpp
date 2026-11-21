#include "gpu_sync.hpp"
#include "gpu_list_capture.hpp"
#include "clock.hpp"
#include "gx_profile.hpp"
#include "gpu_canary.hpp"
#include "disc.hpp"
#include "../gx_texture_pica.hpp"
#include "../gx_geometry_pica.hpp"
#include <3ds.h>
#include <citro3d.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
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
u32* frameCommands{};u32 frameCommandWords{},loggedCommandWords{};unsigned commandReclaims{};
// Full GPU drains by reason, and the `other` ones by call site (render owner
// only). Reported every reportFrames pictures from platformTakeGpuTelemetry.
constexpr unsigned reportFrames=300,callSites=12;
struct CompleteCount {std::uint32_t count{};std::uint64_t ticks{};};
std::array<CompleteCount,unsigned(dusk::native::GpuCompleteReason::count)> completeByReason{};
struct CompleteSite {const void* caller{};CompleteCount total{};};
std::array<CompleteSite,callSites> completeSites{};CompleteCount completeSiteOverflow{};
unsigned framesSinceReport{};
// Sleeps between queue checks by step (pollPause), same window and owner.
std::uint32_t finePolls{},coarsePolls{};
// Render-owner telemetry and incremental submission state.
dusk::native::GpuFrameTelemetry telemetry{};
unsigned chunkDraws{},drawsSinceSubmit{};
// The draws of the part being assembled (platformSetGpuFirstPart): firstPart
// at the start of a picture, doubled after each automatic split up to
// chunkDraws.
unsigned firstPart{},partDraws{};
// See platformHoldGpuChunks and platformGpuSpanSerial (gpu_sync.hpp).
bool chunkHold{};std::uint32_t spanSerial{};
// GPU time diagnostics (platformSetGpuDiagnostics): one draw per list, and
// the painter phase whose draws the lists being assembled hold.
bool drawTimes{};unsigned currentPhase{};
std::uint32_t phaseSplits{},phaseSplitsHeld{};
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
// Between checks of the queue a wait sleeps without holding the IRQ gate (the
// GSP event thread takes it). 100 us steps for the first 2 ms: the usual
// frame-begin and completion tail is well under 1 ms, and the earlier 1 ms
// steps rounded every wait up (Alpha 6 console: gpu_begin 1.01 ms median
// against a 0.59 ms GPU tail). Longer waits go on in 1 ms steps. The
// deadlines below are measured in ticks and unchanged.
void pollPause(std::uint64_t start){
    constexpr std::uint64_t fineTicks=2*dusk::native::nativeTickFrequency/1000;
    const bool fine=std::uint64_t(svcGetSystemTick())-start<fineTicks;
    ++(fine?finePolls:coarsePolls);
    svcSleepThread(fine?100000:1000000);
}
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
// GPU time sums of ctru_gxqueue.c (same layout as its duskGxJobTimes).
extern "C" void duskGxJobTimesTake(void* out,unsigned bytes);
extern "C" volatile u32 duskGxQueueTag;
extern "C" volatile u32 duskGxQueueOwner;
namespace {
struct GxJobTimes {
    std::uint64_t busy[4];
    std::uint64_t idle,turnaround,longestList;
    std::uint64_t tagBusy[16];
    std::uint32_t jobs,lists;
    std::uint32_t tagJobs[16];
};
static_assert(unsigned(dusk::native::gpuPhaseCount)==DUSK_SHADOW_GPU_LINK_PASS && DUSK_SHADOW_GPU_TAG_END<=16);
// Sums over the report window (reportFrames pictures).
struct GpuTimeReport {
    std::uint64_t lists,copies,fills,idle,turnaround,longest,phase[16];
    std::uint32_t jobs,listCount,pictures,phaseLists[16];
    float longestMs;
};
GpuTimeReport gpuTimes{};
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
        pollPause(start);
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
        pollPause(start);
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
    dusk::native::recordSubmittedGpuList(list,bytes);
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
namespace {bool submitGpu(bool force);}
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
namespace {
std::atomic<OSThread*> lentGame{};
std::atomic<void(*)(void*)> lentWanted{};
std::atomic<bool> lent{};
}
void platformLendGpu(OSThread* game,OSThread* worker,bool toQueue,void (*wanted)(void* site)){
    lentGame.store(game,std::memory_order_release);lentWanted.store(wanted,std::memory_order_release);
    renderOwner.store(toQueue&&worker?worker:game,std::memory_order_release);
    lent.store(toQueue,std::memory_order_release);
}
OSThread* platformGpuOwner() {
    // Lent to the shape queue and the game thread asks: it is about to use
    // the GPU. `wanted` returns at once on the thread that is drawing the
    // queued shapes (the game thread does at a sync).
    if(lent.load(std::memory_order_relaxed))[[unlikely]]{
        if(OSGetCurrentThread()==lentGame.load(std::memory_order_relaxed))if(const auto wanted=lentWanted.load(std::memory_order_relaxed))wanted(__builtin_return_address(0));
    }
    return renderOwner.load(std::memory_order_acquire);
}
bool platformBeginGpuCommands(){
    if(!queue.load(std::memory_order_acquire)||platformGpuOwner()!=OSGetCurrentThread())return false;
    u32 offset{};GPUCMD_GetBuffer(&frameCommands,&frameCommandWords,&offset);
    // The arena a frame really gets (Alpha 6 asked for 512 KiB and logged
    // 65536 words); a larger value only, so a split before this never repeats it.
    if(frameCommandWords>loggedCommandWords){
        loggedCommandWords=frameCommandWords;
        OSReport("Native GPU command arena in frame: %lu words (%lu KiB)\n",static_cast<unsigned long>(frameCommandWords),static_cast<unsigned long>(frameCommandWords/256));
    }
    return frameCommands&&frameCommandWords>=8192&&offset==0;
}
bool platformPrepareGpuDraw(){
    if(!frameCommands||platformGpuOwner()!=OSGetCurrentThread())return false;
    u32* buffer{};u32 size{},offset{};GPUCMD_GetBuffer(&buffer,&size,&offset);
    const auto begin=reinterpret_cast<std::uintptr_t>(frameCommands),at=reinterpret_cast<std::uintptr_t>(buffer);
    if(at<begin||(at-begin)%4||(at-begin)/4>frameCommandWords||size>frameCommandWords-(at-begin)/4||offset>size)return false;
    // Incremental submission: let the PICA start on the draws assembled so far
    // while the CPU keeps translating the rest of the frame. The first part
    // of a picture may be shorter (platformSetGpuFirstPart); with drawTimes
    // every draw closes the list before it.
    const unsigned part=drawTimes?1u:partDraws?partDraws:chunkDraws;
    if(part&&!chunkHold&&++drawsSinceSubmit>=part&&offset){
        if(!platformSubmitGpu())return false;
        if(partDraws&&partDraws<chunkDraws)partDraws=std::min(chunkDraws,2*partDraws);
        GPUCMD_GetBuffer(&buffer,&size,&offset);
    }
    // The list this draw goes into is tagged with its owner (the list before
    // it, closed just above with one draw per list, kept the previous one).
    duskGxQueueOwner=dusk::native::gxDrawOwnerNow();
    if(platformReserveGpuEntries(1,500000000)!=GpuCompletion::complete)return false;
    // A full program/state/uniform update is smaller than this32 KiB reserve.
    // Leave room for C3D's flush/invalidate/end commands before recycling.
    if(size-offset>=8192)return true;
    ++telemetry.reclaims;++spanSerial;
    if(platformCompleteGpuFor(GpuCompleteReason::reclaim,500000000)!=GpuCompletion::complete)return false;
    // GPUCMD_Split normally advances through the frame's fixed command arena.
    // With every queued read complete, reusing its initial public span is safe;
    // Citro3D's render targets and hardware state remain part of the same frame.
    GPUCMD_SetBuffer(frameCommands,frameCommandWords,0);
    if(commandReclaims++<4)OSReport("Native GPU completed command block reclaimed (%lu words)\n",static_cast<unsigned long>(frameCommandWords));
    return true;
}

void platformSetGpuChunkDraws(unsigned draws){chunkDraws=draws;drawsSinceSubmit=0;partDraws=firstPart&&firstPart<chunkDraws?firstPart:0;}
void platformSetGpuFirstPart(unsigned draws){firstPart=draws;partDraws=firstPart&&firstPart<chunkDraws?firstPart:0;}
void platformSetGpuDiagnostics(bool phases,bool oneDrawPerList){gpuPhasesOn=phases||oneDrawPerList;drawTimes=oneDrawPerList;}
// Holds nest: a retained screen is recorded inside a HUD tape recording.
void platformHoldGpuChunks(bool hold){
    static unsigned holds;
    if(hold)++holds;else if(holds)--holds;
    chunkHold=holds!=0;
}
std::uint32_t platformGpuSpanSerial(){return spanSerial;}
namespace {
void reportCompletes(){
    const auto ms=[](std::uint64_t ticks){return ticks*1000.0/nativeTickFrequency;};
    const auto& reclaim=completeByReason[unsigned(GpuCompleteReason::reclaim)];
    const auto& canary=completeByReason[unsigned(GpuCompleteReason::canary)];
    const auto& other=completeByReason[unsigned(GpuCompleteReason::other)];
    OSReport("Native GPU completes, last %u pictures: reclaim %lu (%.1f ms) canary %lu (%.1f ms) other %lu (%.1f ms); wait polls 100us %lu 1ms %lu\n",
        framesSinceReport,static_cast<unsigned long>(reclaim.count),ms(reclaim.ticks),static_cast<unsigned long>(canary.count),ms(canary.ticks),
        static_cast<unsigned long>(other.count),ms(other.ticks),static_cast<unsigned long>(finePolls),static_cast<unsigned long>(coarsePolls));
#if defined(__3DS__)
    reportDiscReads(framesSinceReport);
#endif
    // The `other` ones by caller, most expensive first; resolve with addr2line
    // on the ELF. Lines stay under OSReport's 255 characters (bootstrap.cpp).
    std::array<CompleteSite,callSites> sites=completeSites;
    std::sort(sites.begin(),sites.end(),[](const CompleteSite& a,const CompleteSite& b){return a.total.ticks>b.total.ticks;});
    constexpr char prefix[]="Native GPU complete callers:";
    char text[240];std::size_t used=0;
    auto add=[&](const char* format,auto... values){
        char item[48];const int n=std::snprintf(item,sizeof(item),format,values...);
        if(n<=0)return;
        if(used&&used+std::size_t(n)>=sizeof(text)){OSReport("%s\n",text);used=0;}
        if(!used)used=std::size_t(std::snprintf(text,sizeof(text),"%s",prefix));
        used+=std::size_t(std::snprintf(text+used,sizeof(text)-used,"%s",item));
    };
    for(const auto& site:sites)if(site.caller)
        add(" %08lx x%lu %.1f ms",static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(site.caller)),static_cast<unsigned long>(site.total.count),ms(site.total.ticks));
    if(completeSiteOverflow.count)add(" more x%lu %.1f ms",static_cast<unsigned long>(completeSiteOverflow.count),ms(completeSiteOverflow.ticks));
    if(used)OSReport("%s\n",text);
    completeByReason={};completeSites={};completeSiteOverflow={};framesSinceReport=0;finePolls=coarsePolls=0;
}
}
namespace {
// GPU time of the last reportFrames pictures, per picture (ms): the PICA's
// command lists, transfers and copies, fills; idle and turnaround gaps.
// With phases on, the command-list time of each painter phase.
void reportGpuTimes(){
    auto& r=gpuTimes;
    if(!r.jobs){r={};return;} // no GPU job in the window: no line
    const double scale=1000.0/nativeTickFrequency/r.pictures;
    OSReport("Native GPU time, last %lu pictures, per picture: busy %.1f ms (lists %.1f, copies %.2f, fills %.2f), idle %.1f, turnaround %.2f; "
        "jobs %.1f, lists %.1f, longest list %.1f ms (max %.1f)\n",static_cast<unsigned long>(r.pictures),(r.lists+r.copies+r.fills)*scale,r.lists*scale,
        r.copies*scale,r.fills*scale,r.idle*scale,r.turnaround*scale,double(r.jobs)/r.pictures,double(r.listCount)/r.pictures,
        r.longest*scale,double(r.longestMs));
    if(gpuPhasesOn){
        static const char* const names[gpuPhaseCount]={"frame","sky","world","shadow","actor","xlu","particle","effect","2d"};
        char text[240];int used=std::snprintf(text,sizeof(text),"Native GPU time per phase, ms/lists per picture:");
        for(unsigned phase=0;phase<gpuPhaseCount&&used>0&&used<int(sizeof(text));++phase)
            used+=std::snprintf(text+used,sizeof(text)-used," %s %.2f/%.1f",names[phase],r.phase[phase]*scale,double(r.phaseLists[phase])/r.pictures);
        OSReport("%s\n",text);
        OSReport("Native GPU phase splits: %lu made, %lu inside a held pass (list shared by two phases)%s\n",
            static_cast<unsigned long>(phaseSplits),static_cast<unsigned long>(phaseSplitsHeld),drawTimes?"; one draw per list":"");
        phaseSplits=phaseSplitsHeld=0;
    }
    if(gpuShadowProfileMode!=GpuShadowProfileMode::off){
        constexpr const char* names[]={"link_pass","other_pass","mixed_clear","mixed_link","mixed_other","shared_caster","mixed_pass"};
        static_assert(std::size(names)==DUSK_SHADOW_GPU_TAG_END-DUSK_SHADOW_GPU_LINK_PASS);
        for(unsigned n=0;n<std::size(names);++n){
            const unsigned tag=DUSK_SHADOW_GPU_LINK_PASS+n;
            if(r.phaseLists[tag])
                OSReport("Native shadow GPU jobs: pictures=%lu group=%s lists=%lu ticks=%llu total_ms=%.3f\n",
                    static_cast<unsigned long>(r.pictures),names[n],static_cast<unsigned long>(r.phaseLists[tag]),
                    static_cast<unsigned long long>(r.phase[tag]),double(r.phase[tag])*1000.0/nativeTickFrequency);
        }
    }
    r={};
}
}
GpuFrameTelemetry platformTakeGpuTelemetry(){
    auto value=telemetry;telemetry={};
    GxJobTimes times{};duskGxJobTimesTake(&times,sizeof(times));
    value.gpuListTicks=times.busy[0];value.gpuCopyTicks=times.busy[1]+times.busy[3];value.gpuFillTicks=times.busy[2];
    value.gpuIdleTicks=times.idle;value.gpuTurnTicks=times.turnaround;value.gpuLongestListTicks=times.longestList;
    value.gpuJobs=times.jobs;value.gpuLists=times.lists;
    {auto& r=gpuTimes;
     r.lists+=value.gpuListTicks;r.copies+=value.gpuCopyTicks;r.fills+=value.gpuFillTicks;r.idle+=times.idle;r.turnaround+=times.turnaround;
     r.longest+=times.longestList;r.jobs+=times.jobs;r.listCount+=times.lists;++r.pictures;
     r.longestMs=std::max(r.longestMs,float(times.longestList*1000.0/nativeTickFrequency));
     for(unsigned phase=0;phase<16;++phase){r.phase[phase]+=times.tagBusy[phase];r.phaseLists[phase]+=times.tagJobs[phase];}}
    if(++framesSinceReport>=reportFrames){
        bool any=finePolls||coarsePolls;for(const auto& reason:completeByReason)any|=reason.count!=0;
        if(any)reportCompletes();else framesSinceReport=0;
        reportGpuTimes();
    }
    return value;
}
// The painter call names of m_Do_graphic.cpp, by the pass they draw. Read
// once per call site; the order of the tests matters (the 2D screen effects
// and the 2D particles before the generic words).
unsigned gpuPhaseForPainterCall(const char* name){
    if(!name)return gpuPhaseNone;
    const auto has=[name](const char* word){return std::strstr(name,word)!=nullptr;};
    for(const char* word:{"2DScreen","IndScreen","Zxlu","Filter","motionBlure","drawDepth2","Bloom","3Dlast","Invisible"})
        if(has(word))return gpuPhaseEffects;
    if(has("2D")||has("Item3D")||has("item3d")||has("Cursor"))return gpuPhase2D;
    if(has("particle"))return gpuPhaseParticles;
    if(has("Sky"))return gpuPhaseSky;
    if(has("Shadow"))return gpuPhaseShadows;
    if(has("Xlu"))return gpuPhaseTranslucent;
    if(has("OpaListBG")||has("DarkBG")||has("Middle"))return gpuPhaseWorld;
    if(has("OpaList"))return gpuPhaseActors;
    return gpuPhaseNone;
}
// A painter phase begins: the lists assembled so far belong to the previous
// one. Game thread only (the painter); the owner check syncs the worker, so
// everything issued before the mark is in the command buffer now.
void markGpuPhaseSlow(unsigned phase){
    if(phase>=gpuPhaseCount||!queue.load(std::memory_order_acquire)||platformGpuOwner()!=OSGetCurrentThread())return;
    if(phase==currentPhase||!frameCommands)return;
    u32 offset{};GPUCMD_GetBuffer(nullptr,nullptr,&offset);
    if(offset){
        if(chunkHold)++phaseSplitsHeld;
        else{
            if(!submitGpu(true))return;
            ++phaseSplits;
        }
    }
    currentPhase=phase;duskGxQueueTag=phase;
}
bool platformSubmitGpu(){return submitGpu(drawTimes);}
bool platformSubmitGpuShadowBoundary(){return submitGpu(true);}
void platformSetGpuShadowTag(unsigned tag){duskGxQueueTag=tag?tag:currentPhase;}
namespace {
// `force`: split here even with two lists ahead of the PICA (the GPU time
// diagnostics, whose lists must end where asked).
bool submitGpu(bool force){
    auto* input=queue.load(std::memory_order_acquire);
    if(!input||!frameCommands||platformGpuOwner()!=OSGetCurrentThread())return false;
    drawsSinceSubmit=0;
    u32 offset{};GPUCMD_GetBuffer(nullptr,nullptr,&offset);
    if(!offset)return true;
    // Keep at most two command lists ahead of the PICA: when it is the slower
    // side, let this part grow instead of filling the bounded GX queue.
    // Not when forced (a full queue then drains in platformReserveGpuEntries).
    if(!force&&validQueue(input)&&input->numEntries-input->lastEntry>=2)return true;
    if(platformReserveGpuEntries(1,500000000)!=GpuCompletion::complete)return false;
    if(!flushGxGeometry()){noteFailure(GpuCompletion::exhausted);return false;}
    C3D_FrameSplit(GX_CMDLIST_FLUSH);++spanSerial;
    // C3D_FrameBegin leaves the queue stopped; start it without waiting. The
    // IRQ gate is held only for the SDK call; a busy gate means the event
    // thread is already advancing the queue.
    if(!LightLock_TryLock(&lifecycleGate)){
        if(validQueue(input))gxCmdQueueRun(input);
        LightLock_Unlock(&lifecycleGate);
    }
    ++telemetry.chunks;return true;
}
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
    partDraws=firstPart&&firstPart<chunkDraws?firstPart:0;
    // The clear and anything before the painter's first mark.
    currentPhase=gpuPhaseFrame;duskGxQueueTag=gpuPhaseFrame;
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
namespace {
GpuCompletion completeGpu(GpuCompleteReason reason,const void* caller,std::uint64_t nanoseconds) {
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
    C3D_FrameSplit(GX_CMDLIST_FLUSH);++spanSerial;
    const auto result=idleAndLock(input,start,ticks);
    if(result==GpuCompletion::complete)LightLock_Unlock(&lifecycleGate);
    const std::uint64_t waited=std::uint64_t(svcGetSystemTick())-start;
    telemetry.completeWaitTicks+=waited;++telemetry.completes;
    if(reason==GpuCompleteReason::reclaim)telemetry.reclaimWaitTicks+=waited;
    auto& total=completeByReason[unsigned(reason)];++total.count;total.ticks+=waited;
    if(reason==GpuCompleteReason::other){
        CompleteSite* site=nullptr;
        for(auto& candidate:completeSites)if(candidate.caller==caller||!candidate.caller){site=&candidate;break;}
        auto& counted=site?(site->caller=caller,site->total):completeSiteOverflow;
        ++counted.count;counted.ticks+=waited;
    }
    drawsSinceSubmit=0;
    return noteFailure(result);
}
}
// Not inlined: the return address names the caller in the per-site counts.
__attribute__((noinline)) GpuCompletion platformCompleteGpu(std::uint64_t nanoseconds){
    return completeGpu(GpuCompleteReason::other,__builtin_return_address(0),nanoseconds);
}
GpuCompletion platformCompleteGpuFor(GpuCompleteReason reason,std::uint64_t nanoseconds){
    return completeGpu(reason,__builtin_return_address(0),nanoseconds);
}
}
