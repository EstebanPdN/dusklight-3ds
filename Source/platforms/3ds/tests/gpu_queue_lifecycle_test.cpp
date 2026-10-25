// Host-only event-driven probe: real production completion wrapper + upstream
// libctru queue. No GPU, ARM build, emulator, real threads or elapsed sleeps.
#include <3ds.h>
#include <citro3d.h>
#include "gpu_sync.hpp"
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

extern "C" void __wrap_GX_BindQueue(gxCmdQueue_s*);
extern "C" void gxCmdQueueInterrupt(GSPGPU_Event);
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
bool stalled{}, flushOkay = true, unrelatedIrq{};
std::atomic<bool> callbackActive{},releaseCallback{},callbackWaitMode{};
bool inFrame{},tailAssembled{};
std::mutex registry;struct LockSlot{LightLock* p{};std::mutex mutex;};LockSlot locks[16];
std::mutex& mutexFor(LightLock* p){std::lock_guard guard(registry);for(auto& s:locks)if(s.p==p)return s.mutex;for(auto& s:locks)if(!s.p){s.p=p;return s.mutex;}std::abort();}
void callback(gxCmdQueue_s*){callbackActive=true;while(!releaseCallback)std::this_thread::yield();callbackActive=false;}

void add(unsigned type) { gxCmdEntry_s e{}; e.type = type; gxCmdQueueAdd(&queue, &e); }
void event() {
    if (stalled || device.empty()) return;
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
extern "C" void OSReport(const char*, ...) {}
extern "C" void __real_GX_BindQueue(gxCmdQueue_s*) {}
extern "C" u64 svcGetSystemTick() { tick += 268; return tick; }
extern "C" void svcSleepThread(s64 ns) {
    assert(ns == 1000000); ++slept;
    tick += dusk::native::nativeTickFrequency / 1000;
    if(callbackWaitMode)std::this_thread::sleep_for(std::chrono::milliseconds(1));else event();
}
extern "C" void svcBreak(int) { std::abort(); }
extern "C" void LightLock_Lock(LightLock* p) {mutexFor(p).lock();}
extern "C" int LightLock_TryLock(LightLock* p) {return mutexFor(p).try_lock()?0:1;}
extern "C" void LightLock_Unlock(LightLock* p) {mutexFor(p).unlock();}
extern "C" void gspWaitForAnyEvent() { event(); }
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
    const auto before=submitted;
    add(4);add(4);add(4);
    assert(submitted==before&&device.empty()); // stopped until all swap state is assembled
    tailAssembled=true;inFrame=false;gxCmdQueueRun(&queue);
}
namespace dusk::native {
bool flushGxGeometry() { ++flushes; return flushOkay; }
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
        add(4); stalled = true;
        result = platformCompleteGpu(1000000);
        assert(result == GpuCompletion::timedOut && completed == 0 && device.size() == 1);
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
        for(unsigned i=0;i<32;++i)add(1);
        stalled=true;offset=8;
        assert(platformReserveGpuEntries(4,1000000)==GpuCompletion::timedOut);
        assert(queue.numEntries==32&&queue.lastEntry==0&&offset==8&&completed==0);
        assert(submitted==3); // retained, not cleared or reused after timeout
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
        add(4);stalled=true;offset=8;
        assert(!platformEndGpuFrame(0)&&!tailAssembled&&queue.numEntries==1&&offset==8&&platformGpuFailurePending());
    } else std::abort();
    std::printf("%s result=%d submitted=%u completed=%u remaining=%zu queue=%u/%u/%u polls=%u\n",
                name, int(result), submitted, completed, device.size(), queue.lastEntry, queue.curEntry, queue.numEntries, slept);
}
