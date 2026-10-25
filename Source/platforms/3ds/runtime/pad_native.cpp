#include "pad_mapping.hpp"
#ifdef DUSK_GAME_STARTUP
#include "application_dump_ctru.hpp"
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

// libctru application startup owns HID/IRRST initialization and shutdown.
// The application's once-per-tick hidScanInput also scans IRRST. PADRead uses
// that snapshot; it must not scan twice and consume application key edges.
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
    hidCircleRead(&circle);
    irrstCstickRead(&cstick);
    u32 keys = hidKeysHeld();
    dusk::native::applyScriptedInput(keys, circle.dx, circle.dy);
#ifdef DUSK_GAME_STARTUP
    if(dusk::native::applicationDumpInputSuppressed()){
        keys=0;circle={};cstick={};
    }
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
