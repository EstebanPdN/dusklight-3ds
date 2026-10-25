#include "os_system.hpp"
#include <dolphin/os.h>
#include <dolphin/os/OSAlarm.h>
#include <dolphin/os/OSRtc.h>
#include <atomic>
#include <array>
#include <cstring>
namespace {
struct InterruptMutex {OSMutex value;InterruptMutex(){OSInitMutex(&value);}};
OSMutex* exclusion(){static InterruptMutex mutex;return &mutex.value;}
thread_local bool interruptsEnabled=true;
std::atomic<u32> soundMode{1},progressiveMode{1};
std::array<std::byte,256> savedReset{};
std::size_t savedSize{};
// Dolphin interrupt exclusion is application data exclusion on 3DS. Kernel
// IRQs/VBlank remain enabled; every participating game worker shares this lock.
BOOL setInterrupts(bool enabled){
    const bool previous=interruptsEnabled;
    if(previous!=enabled){
        if(enabled){interruptsEnabled=true;OSUnlockMutex(exclusion());}
        else{OSLockMutex(exclusion());interruptsEnabled=false;}
    }
    return previous;
}
}
extern "C" {
void OSInit(){if(!OSGetArenaLo()||!OSGetArenaHi()||!OSGetCurrentThread())OSPanic(__FILE__,__LINE__,"Native OS requires its configured arena and thread owner");}
u32 OSGetResetCode(){return 0;} // A cold native application launch has no GCN warm-reset code.
BOOL OSDisableInterrupts(){return setInterrupts(false);}
BOOL OSEnableInterrupts(){return setInterrupts(true);}
BOOL OSRestoreInterrupts(BOOL level){return setInterrupts(level!=0);}
u32 OSGetSoundMode(){return soundMode.load(std::memory_order_acquire);}
void OSSetSoundMode(u32 mode){if(mode>1)OSPanic(__FILE__,__LINE__,"Invalid native sound mode");soundMode.store(mode,std::memory_order_release);}
u8 OSGetLanguage(){return dusk::native::platformGameLanguage();}
u32 OSGetProgressiveMode(){return progressiveMode.load(std::memory_order_acquire);}
void OSSetProgressiveMode(u32 on){if(on>1)OSPanic(__FILE__,__LINE__,"Invalid native progressive preference");progressiveMode.store(on,std::memory_order_release);}
void OSSetSaveRegion(void* start,void* end){
    const auto a=reinterpret_cast<std::uintptr_t>(start),b=reinterpret_cast<std::uintptr_t>(end);
    if(!start||b<a||b-a>savedReset.size())OSPanic(__FILE__,__LINE__,"Invalid native reset save region");
    savedSize=b-a;std::memcpy(savedReset.data(),start,savedSize);
}
void OSResetSystem(int mode,u32 code,BOOL forceMenu){
    if(mode<0||mode>2)OSPanic(__FILE__,__LINE__,"Invalid native reset mode");
    // Unwind to the native entry owner for controlled shutdown/restart. This
    // must not return into the original GCN reset's infinite retrace loop.
    throw dusk::native::ResetSignal{mode,code,forceMenu!=0};
}
void OSCancelAlarm(OSAlarm* alarm){
    if(!alarm)return;
    if(alarm->handler)OSPanic(__FILE__,__LINE__,"Native armed alarm cancellation needs its scheduler owner");
    alarm->prev=alarm->next=nullptr;alarm->fire=alarm->period=alarm->start=0;
}
void LCEnable(){__sync_synchronize();}
void LCDisable(){__sync_synchronize();}
}
