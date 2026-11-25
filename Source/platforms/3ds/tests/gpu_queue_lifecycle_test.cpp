// Host-only event-driven probe: real production completion wrapper + upstream
// libctru queue. No GPU, ARM build, emulator, real threads or elapsed sleeps.
#include <3ds.h>
#include <citro3d.h>
#include "gpu_sync.hpp"
#include "gpu_list_capture.hpp"
#include <filesystem>
#include "clock.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <string>
#include <vector>

extern "C" void __wrap_GX_BindQueue(gxCmdQueue_s*);
extern "C" void gxCmdQueueInterrupt(GSPGPU_Event);
extern "C" unsigned duskGxJobRingCopy(u32*, unsigned, u32*);
struct OSThread { unsigned id; };
namespace {
OSThread owner{1}, foreign{2};
OSThread* current = &owner;
gxCmdEntry_s entries[32]{};
gxCmdQueue_s queue{entries, 32, 0, 0, 0, nullptr, nullptr};
std::deque<unsigned> device;
u32 commands[16384]{}, offset{};
u64 tick{};
unsigned submitted{}, completed{}, slept{}, flushes{};
// Sleep lengths of the GPU waits, and a device that finishes nothing before holdUntil.
std::vector<s64> sleeps; u64 holdUntil{};
// Every report line since the last clear (the console keeps 255 characters per line).
std::string lastReport;
u32 retraces{}; unsigned retraceWaits{};
bool stalled{}, flushOkay = true, unrelatedIrq{};
std::atomic<bool> callbackActive{},releaseCallback{},callbackWaitMode{};
std::atomic<bool> captureWaiting{}, captureProgress{};
bool inFrame{},tailAssembled{};
std::mutex registry;struct LockSlot{LightLock* p{};std::mutex mutex;};LockSlot locks[16];
std::mutex& mutexFor(LightLock* p){std::lock_guard guard(registry);for(auto& s:locks)if(s.p==p)return s.mutex;for(auto& s:locks)if(!s.p){s.p=p;return s.mutex;}std::abort();}
void callback(gxCmdQueue_s*){callbackActive=true;while(!releaseCallback)std::this_thread::yield();callbackActive=false;}

void add(unsigned type) { gxCmdEntry_s e{}; e.type = type; gxCmdQueueAdd(&queue, &e); }
void event() {
    if (stalled || device.empty() || tick < holdUntil) return;
    if (unrelatedIrq) {
        // Inject a DMA IRQ while only a PPF transfer belongs to this queue.
        gxCmdQueueInterrupt(GSPGPU_EVENT_DMA);
        return;
    }
    const unsigned type = device.front(); device.pop_front(); ++completed;
    gxCmdQueueInterrupt(type == 4 ? GSPGPU_EVENT_PPF : GSPGPU_EVENT_P3D);
}
}
extern "C" OSThread* OSGetCurrentThread() { return current; }
extern "C" void OSReport(const char* format, ...) {
    char text[1024]; va_list args; va_start(args, format); std::vsnprintf(text, sizeof(text), format, args); va_end(args);
    lastReport += text;
}
extern "C" void __real_GX_BindQueue(gxCmdQueue_s*) {}
extern "C" u64 svcGetSystemTick() { tick += 268; return tick; }
extern "C" void svcSleepThread(s64 ns) {
    // GPU waits poll in 100 us steps for their first 2 ms, then in 1 ms steps.
    assert(ns == 100000 || ns == 1000000); ++slept; sleeps.push_back(ns);
    tick += u64(ns) * dusk::native::nativeTickFrequency / 1000000000;
    if(callbackWaitMode)std::this_thread::sleep_for(std::chrono::milliseconds(1));else event();
}
extern "C" void svcBreak(int) { std::abort(); }
extern "C" void LightLock_Lock(LightLock* p) {mutexFor(p).lock();}
extern "C" int LightLock_TryLock(LightLock* p) {return mutexFor(p).try_lock()?0:1;}
extern "C" void LightLock_Unlock(LightLock* p) {mutexFor(p).unlock();}
extern "C" void gspWaitForAnyEvent() { event(); }
extern "C" void gspWaitForVBlank0() { ++retraces; ++retraceWaits; }
extern "C" u32 C3D_FrameCounter(int) { return retraces; }
extern "C" Result gspSubmitGxCommand(const u32* data) {
    ++submitted; device.push_back(data[0] & 0xff); return 0;
}
extern "C" void GPUCMD_GetBuffer(u32** buffer, u32* size, u32* at) {
    if (buffer) *buffer = commands;
    if (size) *size = 16384;
    if (at) *at = offset;
}
extern "C" void GPUCMD_SetBuffer(u32*, u32, u32 at) { offset = at; }
extern "C" void C3D_FrameSplit(u8 flags) {
    assert(flags == GX_CMDLIST_FLUSH);
    if (offset) { add(1); offset = 0; }
}
extern "C" void C3D_FrameSync() {assert(!callbackActive);}
extern "C" bool C3D_FrameBegin(u8 flags) {
    assert(flags==C3D_FRAME_NONBLOCK&&!callbackActive);
    gxCmdQueueStop(&queue);gxCmdQueueClear(&queue);inFrame=true;return true;
}
extern "C" void C3D_FrameEnd(u8) {
    assert(!callbackActive);C3D_FrameSplit(GX_CMDLIST_FLUSH);
    const auto before=submitted;const auto pending=device.size();
    add(4);add(4);add(4);
    assert(submitted==before&&device.size()==pending); // stopped until all swap state is assembled
    tailAssembled=true;inFrame=false;gxCmdQueueRun(&queue);
}
// Production command-list submission wraps this SDK call (--wrap on ARM).
extern "C" Result __real_GX_ProcessCommandList(u32*, u32, u8) { return 0; }
namespace dusk::native {
bool flushGxGeometry() { ++flushes; return flushOkay; }
// GPU guard hooks (gpu_canary_ctru.cpp); no case here reaches a hang.
void gpuCanaryListSubmitted(const u32*, u32) {}
bool kickNativeSdWriterAsync(){return true;} // This fixture drives the consumer explicitly.
void waitForNativeCaptureProgress(){
    captureWaiting.store(true);captureWaiting.notify_one();
    while(!captureProgress.exchange(false))captureProgress.wait(false);
}
void signalNativeCaptureProgress(){captureProgress.store(true);captureProgress.notify_one();}
void gpuCanaryShadowState(const u32*& regs,size_t& regCount,const u32*& uniforms,size_t& uniformCount){
    static u32 registerBank[0x300]{},uniformBank[96*4]{};
    regs=registerBank;regCount=0x300;uniforms=uniformBank;uniformCount=96*4;
}
void gpuSlowJob(const std::uint32_t*, unsigned) {}
// A command list silent past the patience window is a hang: the guard saves
// evidence and never returns. Here it reports the silent job's type instead.
struct GpuHangRecorded { unsigned type; };
[[noreturn]] void gpuHang(const std::uint32_t* entry, unsigned, unsigned, unsigned) { throw GpuHangRecorded{entry[0] & 0xffu}; }
void invalidateGxTextures() {}
}
int main(int argc, char** argv) {
    using namespace dusk::native;
    assert(argc == 2); const char* name = argv[1];
    __wrap_GX_BindQueue(&queue);
    GpuCompletion result = GpuCompletion::notReady;
    if (!std::strcmp(name, "delayed")) {
        add(1); add(4); offset = 8;
        result = platformCompleteGpu(500000000);
        assert(result == GpuCompletion::complete && submitted == 3 && completed == 3 && slept >= 3 && device.empty());
    } else if (!std::strcmp(name, "postcopy")) {
        gxCmdQueueRun(&queue); add(4);
        result = platformCompleteGpu(500000000);
        assert(result == GpuCompletion::complete && submitted == 1 && completed == 1 && slept);
    } else if (!std::strcmp(name, "timeout")) {
        // A texture copy silent for one second is counted complete (GSP can
        // drop its interrupt); the device still holds the unfinished job.
        add(4); stalled = true;
        result = platformCompleteGpu(1000000);
        assert(result == GpuCompletion::complete && completed == 0 && device.size() == 1 && tick >= nativeTickFrequency);
    } else if (!std::strcmp(name, "flushfailure")) {
        flushOkay = false; offset = 8;
        result = platformCompleteGpu(500000000);
        assert(result == GpuCompletion::exhausted && submitted == 0 && offset == 8);
    } else if (!std::strcmp(name, "foreign")) {
        current = &foreign;
        result = platformCompleteGpu(500000000);
        assert(result == GpuCompletion::wrongThread && submitted == 0 && flushes == 0);
    } else if (!std::strcmp(name, "stoppedfull")) {
        // Models C3D_FrameBegin's stopped queue, already filled by queued work.
        for (unsigned i = 0; i < 32; ++i) add(1);
        assert(queue.lastEntry == 0 && queue.numEntries == 32);
        result = platformCompleteGpu(500000000);
        // Regression: every formerly stopped pending entry must execute before reclaim.
        assert(result == GpuCompletion::complete && submitted == 32 && completed == 32 && queue.numEntries == 0);
    } else if (!std::strcmp(name, "stoppedquery")) {
        add(4);
        result = platformQueryGpu();
        assert(result == GpuCompletion::notReady && queue.lastEntry == 0 && queue.numEntries == 1 && submitted == 0);
    } else if (!std::strcmp(name, "unrelatedirq")) {
        add(4); unrelatedIrq = true;
        result = platformCompleteGpu(500000000);
        assert(result == GpuCompletion::complete && submitted == 1 && completed == 0 && device.size() == 1);
    } else if (!std::strcmp(name, "headroom")) {
        for(unsigned i=0;i<31;++i)add(1);
        assert(platformReserveGpuEntries(2,500000000)==GpuCompletion::complete);
        assert(submitted==31&&completed==31&&queue.numEntries==0);
        add(1);add(4);
        assert(platformCompleteGpu(500000000)==GpuCompletion::complete);
        assert(submitted==33&&completed==33);
    } else if (!std::strcmp(name, "tail")) {
        gxCmdQueueRun(&queue);
        for(unsigned i=0;i<29;++i)add(1);
        assert(platformReserveGpuEntries(4,500000000)==GpuCompletion::complete);
        assert(submitted==29&&completed==29&&queue.numEntries==0);
        add(1);add(4);add(4);add(4);
        assert(platformCompleteGpu(500000000)==GpuCompletion::complete);
        assert(submitted==33&&completed==33);
    } else if (!std::strcmp(name, "reserve-timeout")) {
        // A command list silent past the patience window is a GPU hang: the
        // guard is called with the silent job; nothing is cleared or reused.
        for(unsigned i=0;i<32;++i)add(1);
        stalled=true;offset=8;
        bool hang=false;
        try{(void)platformReserveGpuEntries(4,1000000);}catch(const GpuHangRecorded& recorded){hang=recorded.type==1;}
        assert(hang&&tick>=6*nativeTickFrequency);
        assert(queue.numEntries==32&&queue.lastEntry==0&&offset==8&&completed==0);
        assert(submitted==1); // one job in flight (MAX_PARALLEL_CMDS)
    } else if (!std::strcmp(name, "bad-reserve")) {
        assert(platformReserveGpuEntries(0,500000000)==GpuCompletion::exhausted);
        assert(platformReserveGpuEntries(33,500000000)==GpuCompletion::exhausted);
        assert(platformReserveGpuEntries(1,0)==GpuCompletion::timedOut);
        current=&foreign;
        assert(platformReserveGpuEntries(1,500000000)==GpuCompletion::wrongThread);
        assert(submitted==0&&completed==0&&flushes==0);
    } else if (!std::strcmp(name,"callback")) {
        for(unsigned i=0;i<32;++i)add(1);
        gxCmdQueueRun(&queue);for(unsigned i=0;i<31;++i)event();
        queue.callback=callback;callbackWaitMode=true;
        std::thread irq([]{event();});while(!callbackActive)std::this_thread::yield();
        assert(queue.lastEntry==queue.numEntries&&platformQueryGpu()==GpuCompletion::notReady);
        std::thread release([]{std::this_thread::sleep_for(std::chrono::milliseconds(20));releaseCallback=true;});
        assert(platformReserveGpuEntries(4,500000000)==GpuCompletion::complete);
        assert(!callbackActive&&queue.numEntries==0&&submitted==32&&completed==32);
        irq.join();release.join();
    } else if (!std::strcmp(name,"frame-tail")) {
        gxCmdQueueRun(&queue);for(unsigned i=0;i<30;++i)add(1);offset=8;inFrame=true;
        assert(platformEndGpuFrame(0)&&tailAssembled&&!inFrame&&queue.numEntries==4);
        assert(platformCompleteGpu(500000000)==GpuCompletion::complete);
        assert(submitted==34&&completed==34&&device.empty());
    } else if (!std::strcmp(name,"begin")) {
        add(4);assert(!platformBeginGpuFrame(C3D_FRAME_NONBLOCK));
        assert(queue.numEntries==1&&submitted==0&&!platformGpuFailurePending());
        assert(platformBeginGpuFrame(0)&&inFrame&&completed==1&&queue.numEntries==0);
    } else if (!std::strcmp(name,"frame-timeout")) {
        // A texture copy silent for one second is counted complete; the frame
        // then ends normally. The stalled device still holds that copy and now
        // also the first job of the frame tail.
        add(4);stalled=true;offset=8;
        assert(platformEndGpuFrame(0)&&tailAssembled&&device.size()==2&&tick>=nativeTickFrequency);
    } else if (!std::strcmp(name,"submit")) {
        // Incremental submission: assembled commands start on the PICA without
        // any wait; the frame keeps building and a later completion drains it.
        assert(platformBeginGpuFrame(0)&&inFrame&&platformBeginGpuCommands());
        offset=8;const auto sleeps=slept;
        assert(platformSubmitGpu()&&offset==0&&flushes==1&&submitted==1&&completed==0&&slept==sleeps);
        assert(platformSubmitGpu()&&submitted==1); // nothing new: no empty command list
        offset=8;assert(platformCompleteGpu(500000000)==GpuCompletion::complete&&submitted==2&&completed==2);
        const auto telemetry=platformTakeGpuTelemetry();
        assert(telemetry.chunks==1&&telemetry.completes==1);
    } else if (!std::strcmp(name,"submit-backlog")) {
        // A PICA slower than the CPU: two lists ahead is the limit; further
        // commands accumulate instead of filling the bounded queue.
        assert(platformBeginGpuFrame(0)&&platformBeginGpuCommands());
        stalled=true;
        offset=8;assert(platformSubmitGpu()&&queue.numEntries==1);
        offset=8;assert(platformSubmitGpu()&&queue.numEntries==2);
        offset=8;assert(platformSubmitGpu()&&queue.numEntries==2&&offset==8&&slept==0);
        stalled=false;
        assert(platformCompleteGpu(500000000)==GpuCompletion::complete&&completed==3);
    } else if (!std::strcmp(name,"present-guard")) {
        // No retrace since the previous frame was seen complete: wait for one
        // before queueing another LCD transfer.
        assert(platformBeginGpuFrame(0)&&inFrame);offset=8;
        assert(platformEndGpuFrame(GX_CMDLIST_FLUSH)&&retraceWaits==1&&tailAssembled);
        assert(platformTakeGpuTelemetry().presentWaitTicks>0);
    } else if (!std::strcmp(name,"present-paced")) {
        // The display wait already crossed a retrace: the frame end adds none.
        assert(platformBeginGpuFrame(0)&&inFrame);offset=8;++retraces;
        assert(platformEndGpuFrame(GX_CMDLIST_FLUSH)&&retraceWaits==0&&tailAssembled);
        assert(platformTakeGpuTelemetry().presentWaitTicks==0);
    } else if (!std::strcmp(name,"chunk-draws")) {
        // Every second prepared draw hands the assembled commands to the PICA.
        assert(platformBeginGpuFrame(0)&&platformBeginGpuCommands());
        platformSetGpuChunkDraws(2);
        offset=8;assert(platformPrepareGpuDraw()&&submitted==0&&offset==8);
        assert(platformPrepareGpuDraw()&&submitted==1&&offset==0&&slept==0);
        platformSetGpuChunkDraws(0);
        offset=8;assert(platformPrepareGpuDraw()&&platformPrepareGpuDraw()&&submitted==1);
        assert(platformCompleteGpu(500000000)==GpuCompletion::complete);
    } else if (!std::strcmp(name,"poll-step")) {
        // A completion 0.5 ms into the wait is seen within one 100 us step; one
        // 5 ms in, within one 1 ms step. The old 1 ms steps rounded both up.
        const u64 ms = nativeTickFrequency / 1000;
        add(1); u64 start = tick; holdUntil = start + ms / 2;
        result = platformCompleteGpu(500000000);
        assert(result == GpuCompletion::complete && completed == 1 && tick - holdUntil < ms / 5);
        for (const auto step : sleeps) assert(step == 100000);
        sleeps.clear(); add(1); start = tick; holdUntil = start + 5 * ms;
        assert(platformCompleteGpu(500000000) == GpuCompletion::complete && completed == 2);
        assert(tick - holdUntil <= ms + ms / 10 && sleeps.front() == 100000 && sleeps.back() == 1000000);
    } else if (!std::strcmp(name,"complete-reasons")) {
        // A command-arena reclaim is counted apart from other full drains, and
        // the periodic line names the reasons and the calling sites.
        assert(platformBeginGpuFrame(0)&&platformBeginGpuCommands());
        offset=16384-100;assert(platformPrepareGpuDraw()&&offset==0);
        offset=8;assert(platformCompleteGpu(500000000)==GpuCompletion::complete);
        const auto telemetry=platformTakeGpuTelemetry();
        assert(telemetry.reclaims==1&&telemetry.completes==2&&telemetry.reclaimWaitTicks>0&&telemetry.reclaimWaitTicks<telemetry.completeWaitTicks);
        // The arena a frame really gets is logged once (Alpha 6 logged 65536 words).
        assert(lastReport.find("Native GPU command arena in frame: 16384 words (64 KiB)\n")!=std::string::npos);
        lastReport.clear();
        for(unsigned frame=1;frame<300;++frame)(void)platformTakeGpuTelemetry();
        assert(lastReport.find("Native GPU completes, last 300 pictures: reclaim 1")==0&&lastReport.find("other 1")!=std::string::npos);
        assert(lastReport.find("; wait polls 100us ")!=std::string::npos);
        const auto callers=lastReport.find("\nNative GPU complete callers: ");
        assert(callers!=std::string::npos&&lastReport.find(" x1 ",callers)!=std::string::npos);
        for(std::size_t at=0,end;at<lastReport.size();at=end+1){end=lastReport.find('\n',at);assert(end!=std::string::npos&&end-at<255);}
        lastReport.clear();
        for(unsigned frame=0;frame<300;++frame)(void)platformTakeGpuTelemetry();
        assert(lastReport.empty()); // nothing to report: no line
    } else if(!std::strcmp(name,"gpu-time")){
        // The GPU's own time (ctru_gxqueue.c): a command list from its
        // submission to its event is busy time, the time with nothing queued
        // idle; each picture takes the sums since the previous one.
        assert(platformBeginGpuFrame(0)&&platformBeginGpuCommands());
        (void)platformTakeGpuTelemetry();
        offset=8;assert(platformSubmitGpu());
        holdUntil=tick+nativeTickFrequency/1000; // the list takes 1 ms
        assert(platformCompleteGpu(500000000)==GpuCompletion::complete);
        auto t=platformTakeGpuTelemetry();
        assert(t.gpuLists==1&&t.gpuJobs==1&&t.gpuListTicks>=nativeTickFrequency/1000&&t.gpuLongestListTicks==t.gpuListTicks);
        assert(t.gpuCopyTicks==0&&t.gpuFillTicks==0);
        for(unsigned n=0;n<100;++n)(void)svcGetSystemTick(); // nothing queued
        t=platformTakeGpuTelemetry();
        assert(t.gpuListTicks==0&&t.gpuJobs==0&&t.gpuIdleTicks>=100*268);
        // The first part of a picture: 8 draws, then 16, 32 and the chunk size.
        platformSetGpuChunkDraws(64);platformSetGpuFirstPart(8);
        assert(platformBeginGpuFrame(0)&&platformBeginGpuCommands());
        unsigned splits[4]{};
        for(unsigned draw=1;draw<=8+16+32+64;++draw){
            offset=8;assert(platformPrepareGpuDraw());
            if(offset==0){for(auto& at:splits)if(!at){at=draw;break;}}
            while(!device.empty())event(); // the PICA keeps up (no two-ahead skip)
        }
        assert(splits[0]==8&&splits[1]==8+16&&splits[2]==8+16+32&&splits[3]==8+16+32+64);
        // The next picture starts again with 8.
        assert(platformBeginGpuFrame(0));
        for(unsigned draw=1;draw<=8;++draw){offset=8;assert(platformPrepareGpuDraw());assert((offset==0)==(draw==8));while(!device.empty())event();}
        platformSetGpuFirstPart(0);
        // A painter phase mark (gpu-phases.txt) splits the list even with two
        // lists ahead of the PICA, and a repeated mark of the same phase does not.
        platformSetGpuDiagnostics(true,false);
        assert(platformBeginGpuFrame(0)&&platformBeginGpuCommands());
        offset=8;assert(platformSubmitGpu());offset=8;assert(platformSubmitGpu());
        const auto before=queue.numEntries;
        offset=8;assert(platformSubmitGpu()&&queue.numEntries==before&&offset==8); // two ahead: no split
        markGpuPhase(gpuPhaseWorld);assert(queue.numEntries==before+1&&offset==0);
        offset=8;markGpuPhase(gpuPhaseWorld);assert(queue.numEntries==before+1&&offset==8);
        markGpuPhase(gpuPhaseNone);assert(offset==8);
        platformSetGpuDiagnostics(false,false);
        markGpuPhase(gpuPhase2D);assert(offset==8);
        assert(platformCompleteGpu(500000000)==GpuCompletion::complete);
    } else if(!std::strcmp(name,"shadow-gpu-time")){
        gpuShadowProfileMode=GpuShadowProfileMode::pass;
        assert(platformBeginGpuFrame(0)&&platformBeginGpuCommands());
        for(unsigned n=0;n<2;++n){offset=8;assert(platformSubmitGpu());}
        offset=8;
        assert(platformSubmitGpu()&&offset==8);
        assert(platformSubmitGpuShadowBoundary()&&offset==0);
        platformSetGpuShadowTag(DUSK_SHADOW_GPU_LINK_PASS);
        for(unsigned n=0;n<2;++n){offset=8;assert(platformSubmitGpuShadowBoundary());}
        platformSetGpuShadowTag(0);
        offset=8;assert(platformSubmitGpuShadowBoundary());
        assert(queue.numEntries==6&&submitted==1&&slept==0);
        for(unsigned n=0;n<3;++n)event(); // now the first tagged list is in flight
        for(unsigned n=0;n<300;++n)(void)platformTakeGpuTelemetry();
        assert(lastReport.find("Native shadow GPU jobs:")==std::string::npos);
        lastReport.clear();
        while(!device.empty()){tick+=nativeTickFrequency/1000;event();}
        u32 jobs[6][14]{};
        assert(duskGxJobRingCopy(jobs[0],6,nullptr)==6);
        const auto duration=[&](unsigned n){return ((u64(jobs[n][11])<<32)|jobs[n][10])-((u64(jobs[n][9])<<32)|jobs[n][8]);};
        for(unsigned n=0;n<300;++n)(void)platformTakeGpuTelemetry();
        const auto exact=std::to_string(duration(3)+duration(4));
        assert(lastReport.find("group=link_pass lists=2 ticks="+exact+" ")!=std::string::npos);
    } else if (!std::strcmp(name,"gpu-list-capture")) {
        const std::filesystem::path root="gpu-list-case";
        assert(std::filesystem::create_directory(root));
        const u32 initial[4]={5,0x000f0000,0,0x000f0001};
        const char* phases[]={"camera","bad-job","occupied","still"};
        for(const char* phase:phases){
            u32 payload[4];
            std::memcpy(payload,initial,sizeof(payload));
            const bool badJob=!std::strcmp(phase,"bad-job"),occupied=!std::strcmp(phase,"occupied");
            requestGpuListCapture(root.c_str(),phase,"F_SP114",1);
            assert(gpuListCaptureBoundary(60));
            gxCmdQueueStop(&queue);gxCmdQueueClear(&queue);
            add(2);
            gxCmdEntry_s list{};
            list.type=1;list.args[0]=u32(reinterpret_cast<uintptr_t>(payload));list.args[1]=sizeof(payload);
            recordSubmittedGpuList(payload,sizeof(payload));
            payload[0]=91; // command arena has already been reused when the file is written
            if(badJob)list.args[1]+=16;
            gxCmdQueueAdd(&queue,&list);add(3);
            gxCmdQueueRun(&queue);
            for(auto irq:{GSPGPU_EVENT_PSC1,GSPGPU_EVENT_P3D,GSPGPU_EVENT_PPF}){
                assert(!device.empty());
                device.pop_front();gxCmdQueueInterrupt(irq);
            }
            closeGpuListCapture(60,1234,"F_SP114",1);
            assert(gpuListCaptureBoundary(61));
            const auto path=root/(std::string("gpu-")+phase+".bin");
            assert(!std::filesystem::exists(path));
            assert(gpuListCaptureWritePending()==!badJob);
            if(occupied)assert(std::filesystem::create_directory(path));
            if(!std::strcmp(phase,"camera")){
                assert(!gpuListCaptureBoundary(62)); // An idle writer slot is not required for normal frames.
                assert(gpuListCaptureWritePending()&&!captureWaiting);
                std::thread delayed([]{captureWaiting.wait(false);writePendingGpuListCapture();});
                waitForGpuListCaptureWrite();
                delayed.join();
            }else writePendingGpuListCapture();
            assert(!gpuListCaptureWritePending());
            assert(!std::filesystem::exists(path.string()+".tmp"));
            if(badJob){assert(!std::filesystem::exists(path));continue;}
            if(occupied){assert(std::filesystem::is_directory(path));std::filesystem::remove(path);continue;}
            auto* saved=std::fopen(path.c_str(),"rb");
            assert(saved && std::fseek(saved,-long(sizeof(payload)),SEEK_END)==0);
            assert(std::fread(payload,1,sizeof(payload),saved)==sizeof(payload));
            assert(!std::memcmp(initial,payload,sizeof(initial)));
            std::fclose(saved);std::filesystem::remove(path);
        }
        assert(!gpuListCaptureBoundary(62));std::filesystem::remove(root);
    } else std::abort();
    std::printf("%s result=%d submitted=%u completed=%u remaining=%zu queue=%u/%u/%u polls=%u\n",
                name, int(result), submitted, completed, device.size(), queue.lastEntry, queue.curEntry, queue.numEntries, slept);
}
