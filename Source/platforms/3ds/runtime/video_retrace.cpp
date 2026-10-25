#include "video_retrace.hpp"
#include "clock.hpp"
#include "helpers/native_frame_wait.h"
#include <dolphin/vi.h>
#include <dolphin/os.h>
#include <atomic>
#include <bit>

namespace {
std::atomic<VIRetraceCallback> preCallback{},postCallback{};
std::atomic_flag dispatching=ATOMIC_FLAG_INIT;
thread_local bool inCallback=false;
thread_local u32 callbackCount=0;
u32 previousCount;
bool previousReady=false;
struct DispatchGuard {
    DispatchGuard(){
        if(dispatching.test_and_set(std::memory_order_acquire))
            OSPanic(__FILE__,__LINE__,"Native VI wait requires one non-reentrant render owner");
    }
    ~DispatchGuard(){dispatching.clear(std::memory_order_release);}
};
struct CallbackGuard {
    explicit CallbackGuard(u32 count){inCallback=true;callbackCount=count;}
    ~CallbackGuard(){inCallback=false;}
};
bool dispatchObserved(){
    const u32 count=dusk::native::platformVideoRetraceCount();
    if(previousReady&&count==previousCount)return false;
    previousCount=count;previousReady=true;
    CallbackGuard guard(count);
    if(auto cb=preCallback.load(std::memory_order_acquire))cb(count);
    // A pre-callback may replace post, as on the original VI callback path.
    if(auto cb=postCallback.load(std::memory_order_acquire))cb(count);
    return true;
}
void waitNext(){
    const u32 before=dusk::native::platformVideoRetraceCount();
    do{dusk::native::platformWaitVideoRetrace();}
    while(dusk::native::platformVideoRetraceCount()==before);
    (void)dispatchObserved();
}
}
extern "C" {
u32 VIGetRetraceCount(){return inCallback?callbackCount:dusk::native::platformVideoRetraceCount();}
VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb){return preCallback.exchange(cb,std::memory_order_acq_rel);}
VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb){return postCallback.exchange(cb,std::memory_order_acq_rel);}
}
void dusk::native::waitVideoRetrace(){DispatchGuard guard;waitNext();}
std::uint32_t dusk::native::waitForFrameRetraces(OSMessageQueue* queue,unsigned interval,FrameRetraceDeadline& deadline){
    if(!queue||interval>65535)OSPanic(__FILE__,__LINE__,"Invalid native display retrace deadline");
    DispatchGuard guard;
    if(!deadline.initialized){
        deadline.next=platformVideoRetraceCount();deadline.initialized=true;
        if(!previousReady){previousCount=deadline.next;previousReady=true;}
    }
    for(;;){
        OSMessage message;
        if(!OSReceiveMessage(queue,&message,OS_MESSAGE_NOBLOCK)){
            if(!dispatchObserved())waitNext();
            if(!OSReceiveMessage(queue,&message,OS_MESSAGE_NOBLOCK))
                OSPanic(__FILE__,__LINE__,"Native display retrace callback did not enqueue its real event");
        }
        const auto count=static_cast<u32>(reinterpret_cast<std::uintptr_t>(message));
        // Modular counter comparison remains valid across the u32 IRQ wrap.
        if(std::bit_cast<s32>(count-deadline.next)>=0){
            deadline.next=count+(interval?interval:1);return count;
        }
    }
}
void dusk::native::sleepGameTicks(std::int64_t ticks){
    if(ticks<0)OSPanic(__FILE__,__LINE__,"Negative native display sleep");
    const auto seconds=ticks/dolphinTickFrequency;
    if(seconds>INT64_MAX/1000000000)OSPanic(__FILE__,__LINE__,"Native display sleep overflow");
    const auto whole=seconds*1000000000;
    // Round up partial ticks; sub-nanosecond truncation must not shorten waits.
    const auto part=((ticks%dolphinTickFrequency)*1000000000+dolphinTickFrequency-1)/dolphinTickFrequency;
    if(part>INT64_MAX-whole)OSPanic(__FILE__,__LINE__,"Native display sleep overflow");
    platformSleepNanoseconds(whole+part);
}
