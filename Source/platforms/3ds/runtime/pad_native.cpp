#include "gpu_canary.hpp"
#include "pad_mapping.hpp"
#ifdef DUSK_GAME_STARTUP
#include "application_dump_ctru.hpp"
#include "benchmark_capture.hpp"
#include "benchmark_input.hpp"
#include <3ds.h>
#include "dev_console.hpp"
#endif
#include "developer_script.hpp"
#include <dolphin/os.h>
#include <3ds/types.h>
extern "C" {
#include <3ds/services/hid.h>
#include <3ds/services/irrst.h>
}

namespace {
using namespace dusk::native;
static_assert(nativeA == KEY_A && nativeB == KEY_B && nativeStart == KEY_START);
static_assert(nativeX == KEY_X && nativeY == KEY_Y);
static_assert(nativeRight == KEY_DRIGHT && nativeLeft == KEY_DLEFT);
static_assert(nativeUp == KEY_DUP && nativeDown == KEY_DDOWN);
static_assert(nativeL == KEY_L && nativeR == KEY_R);
static_assert(nativeZL == KEY_ZL && nativeZR == KEY_ZR);
bool initialized;
}

#ifdef DUSK_GAME_STARTUP
extern "C" void OSReport(const char*,...);
namespace dusk::native {
namespace {
LightLock inputGate;
LightEvent samplerWake;
bool inputInitialized{},samplerTried{},samplerStopping{};
Thread sampler{};
BenchmarkInputSite inputSite;
BenchmarkCaptureInput aLatch;
BenchmarkTriggerQueue<BenchmarkInputSite,32> inputEdges;
NativeInputFrame latest,frameInput;
unsigned lostEdges{};
BenchmarkInputSite lastLostEdge;

void reportInputOverflow(unsigned lost,const BenchmarkInputSite& last){
    if(lost)OSReport("Native benchmark input: %u edge(s) omitted (edge queue full); last request tick=%llu after frame=%llu zone=%u stage=%.8s room=%d\n",
        lost,static_cast<unsigned long long>(last.requestTick),static_cast<unsigned long long>(last.previousFrame),
        last.zone,last.stage,last.room);
}

void sampleHid(){
    hidScanInput();
    const u32 raw=hidKeysHeld();
    if(aLatch.sample(inputSite.zone!=0,raw,KEY_A)){
        auto edge=inputSite;
        edge.requestTick=svcGetSystemTick();
        if(!inputEdges.push(edge)){++lostEdges;lastLostEdge=edge;}
    }
    latest.held=inputSite.zone?raw&~KEY_A:aLatch.physical(raw,KEY_A);
    circlePosition circle{},cstick{};
    touchPosition touch{};
    hidCircleRead(&circle);irrstCstickRead(&cstick);hidTouchRead(&touch);
    latest.circleX=circle.dx;latest.circleY=circle.dy;
    latest.cstickX=cstick.dx;latest.cstickY=cstick.dy;
    latest.touchX=touch.px;latest.touchY=touch.py;
}

void inputSampler(void*){
    for(;;){
        LightLock_Lock(&inputGate);
        const bool stopping=samplerStopping, active=inputSite.zone!=0||aLatch.ownsHold();
        if(active&&!stopping)sampleHid();
        LightLock_Unlock(&inputGate);
        if(stopping)return;
        if(active)svcSleepThread(4000000);
        else LightEvent_Wait(&samplerWake);
    }
}
}

void setBenchmarkInputSite(const BenchmarkInputSite& site){
    if(!inputInitialized){
        LightLock_Init(&inputGate);
        LightEvent_Init(&samplerWake,RESET_ONESHOT);
        // Baseline a button already held before activating capture.
        const auto previous=hidKeysHeld();
        if(site.zone)sampleHid();
        else aLatch.sample(false,previous,KEY_A);
        frameInput=latest;
        frameInput.held=previous;
        inputInitialized=true;
    }
    LightLock_Lock(&inputGate);
    const bool wake=!inputSite.zone&&site.zone;
    inputSite=site;
    LightLock_Unlock(&inputGate);
    if(site.zone&&!samplerTried){
        samplerTried=true;
        s32 priority=0x30;
        svcGetThreadPriority(&priority,CUR_THREAD_HANDLE);
        // Wake even if the render core is busy. Only the scanner touches
        // libctru's input globals while an automated benchmark is active.
        sampler=threadCreate(inputSampler,nullptr,8*1024,priority>0x18?priority-1:priority,-2,false);
        OSReport(sampler?"Native benchmark input: sole HID sampler started (4 ms polling interval)\n":
            "Native benchmark input: sampler unavailable; capture uses render-tick HID sampling\n");
    }
    if(wake&&sampler)LightEvent_Signal(&samplerWake);
}

void scanNativeGameInput(){
    if(!inputInitialized)setBenchmarkInputSite({});
    LightLock_Lock(&inputGate);
    if(!sampler||(!inputSite.zone&&!aLatch.ownsHold()))sampleHid();
    const auto previous=frameInput.held;
    frameInput=latest;
    if(inputSite.zone)frameInput.held&=~KEY_A;
    frameInput.down=frameInput.held&~previous;
    const unsigned lost=lostEdges;
    const auto last=lastLostEdge;
    lostEdges=0;
    LightLock_Unlock(&inputGate);
    reportInputOverflow(lost,last);
}
const NativeInputFrame& nativeGameInput(){return frameInput;}
bool takeBenchmarkInputEdge(BenchmarkInputSite& edge){return inputEdges.pop(edge);}

std::int32_t stopBenchmarkInputSampler(){
    if(!sampler)return 0;
    // libctru hides the handle as soon as the worker starts threadExit.
    // The sampler cannot return until this owner publishes samplerStopping.
    const Handle kernel=threadGetHandle(sampler);
    LightLock_Lock(&inputGate);
    samplerStopping=true;
    LightLock_Unlock(&inputGate);
    LightEvent_Signal(&samplerWake);
    const Result joined=svcWaitSynchronization(kernel,1000000000LL);
    if(joined!=0)return joined;
    threadFree(sampler);
    sampler=nullptr;
    LightLock_Lock(&inputGate);
    const auto lost=lostEdges;
    const auto last=lastLostEdge;
    lostEdges=0;
    LightLock_Unlock(&inputGate);
    reportInputOverflow(lost,last);
    OSReport("Native shutdown phase=benchmark input sampler: joined\n");
    return 0;
}
}
#endif

// libctru application startup owns HID/IRRST initialization and shutdown.
// The native game loop publishes one stable input snapshot per picture.
// PADRead never scans HID and applies game scripts after physical filtering.
extern "C" BOOL PADInit() {
    initialized = hidSharedMem != nullptr && irrstSharedMem != nullptr;
    return initialized;
}
extern "C" u32 PADRead(PADStatus* status) {
    if (!status) return 0;
    for (unsigned port = 0; port < PAD_CHANMAX; ++port) {
        status[port] = {};
        status[port].err = PAD_ERR_NO_CONTROLLER;
    }
    if (!initialized || !hidSharedMem || !irrstSharedMem) return 0;
    circlePosition circle{}, cstick{};
#ifdef DUSK_GAME_STARTUP
    const auto& input=dusk::native::nativeGameInput();
    circle={input.circleX,input.circleY};
    cstick={input.cstickX,input.cstickY};
    u32 keys=input.held;
#else
    hidCircleRead(&circle);
    irrstCstickRead(&cstick);
    u32 keys=hidKeysHeld();
#endif
    dusk::native::applyScriptedInput(keys, circle.dx, circle.dy);
#ifdef DUSK_GAME_STARTUP
    // After a restart of the console GPU guard: pass the warnings and logos.
    if(dusk::native::gpuCanaryPressA())keys|=KEY_A;
#endif
#ifdef DUSK_GAME_STARTUP
    if(dusk::native::applicationDumpInputSuppressed()){
        keys=0;circle={};cstick={};
    }
    // The developer console or its automatic test owns the pad: the game
    // receives only the test's scripted input.
    if(dusk::native::devConsoleOwnsPad())
        dusk::native::devConsoleVirtualPad(keys,circle.dx,circle.dy,cstick.dx,cstick.dy);
#endif
    status[0] = dusk::native::mapNativePad(
        {keys, circle.dx, circle.dy, cstick.dx, cstick.dy});
    return 0; // No hardware rumble on New3DS.
}
extern "C" BOOL PADReset(u32) { return initialized; }
extern "C" BOOL PADRecalibrate(u32) { return initialized; }
extern "C" void PADSetSpec(u32) {}
extern "C" void PADSetAnalogMode(u32) {}
extern "C" void PADControlMotor(u32, u32) {} // Device has no rumble motor.
extern "C" void PADControlAllMotors(const u32*) {}
extern "C" BOOL OSGetResetSwitchState() { return false; } // No console reset switch.
