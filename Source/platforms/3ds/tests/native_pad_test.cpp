// Synthetic HID samples only. The original JUT owner, clamp, JKR arena and
// synchronization execute on host; pad_native.cpp is separately ARM/GUI tested.
#include "../runtime/pad_mapping.hpp"
#include "../runtime/pad_bootstrap.hpp"
#include "../runtime/bootstrap.hpp"
#include "../runtime/clock.hpp"
#include "JSystem/JUtility/JUTGamePad.h"
#include "m_Do/m_Do_controller_pad.h"
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
dusk::native::NativePadSample sample{};
bool connected = true;
}
extern "C" BOOL PADInit() { return true; }
extern "C" u32 PADRead(PADStatus* status) {
    for (unsigned i = 0; i < PAD_CHANMAX; ++i) {
        status[i] = {};
        status[i].err = PAD_ERR_NO_CONTROLLER;
    }
    if (connected) status[0] = dusk::native::mapNativePad(sample);
    return 0;
}
extern "C" BOOL PADReset(u32) { return true; }
extern "C" BOOL PADRecalibrate(u32) { return true; }
extern "C" void PADSetSpec(u32 value) { assert(value == PAD_SPEC_5); }
extern "C" void PADSetAnalogMode(u32 value) { assert(value == 3); }
extern "C" void PADControlMotor(u32, u32) {}
extern "C" BOOL OSGetResetSwitchState() { return false; }
namespace dusk::native {
void logText(const char* value) { std::fputs(value, stderr); }
[[noreturn]] void fatal(const char* value) { logText(value); std::abort(); }
void waitForRetrace() {}
void invalidateCache(void*, std::uint32_t) { std::atomic_thread_fence(std::memory_order_seq_cst); }
void flushCache(void*, std::uint32_t) { std::atomic_thread_fence(std::memory_order_seq_cst); }
std::uint64_t counterTicks() { return 0; }
std::uint64_t wallMilliseconds() { return epoch1900To2000Ms; }
}
int main() {
    using namespace dusk::native;
    alignas(32) static std::array<unsigned char, 256 * 1024> arena;
    assert(initializeKernel(arena.data(), arena.size()));
    assert(initializeNativePad());
    const auto heap = kernelStats();
    updateNativePad();
    assert(nativePadSnapshot().held == 0);
    for (unsigned i = 1; i < PAD_CHANMAX; ++i)
        assert(JUTGamePad::mPadStatus[i].err == PAD_ERR_NO_CONTROLLER);
    sample = {nativeA | nativeL | nativeZR, 156, 0, 0, -156};
    updateNativePad();
    auto state = nativePadSnapshot();
    constexpr auto expected = PAD_BUTTON_A | PAD_TRIGGER_L | PAD_TRIGGER_Z;
    assert((state.held & expected) == expected && (state.pressed & expected) == expected);
    assert(state.circleX == 1.0f && state.circleY == 0.0f);
    assert(state.cstickX == 0.0f && state.cstickY == -1.0f);
    assert(state.triggerL == 1.0f && state.triggerR == 0.0f);
    assert(mDoCPd_c::getTrigA(0) && mDoCPd_c::getHoldL(0));
    assert(mDoCPd_c::getStickX3D(0) == 1.0f);
    assert(mDoCPd_c::getSubStickY(0) == -1.0f);
    assert(mDoCPd_c::m_cpadInfo[0].mGamepadErrorFlags == PAD_ERR_NONE);
    updateNativePad();
    assert(nativePadSnapshot().pressed == 0 && nativePadSnapshot().released == 0);
    assert(!mDoCPd_c::getTrigA(0) && mDoCPd_c::getHoldA(0));
    sample = {};
    updateNativePad();
    assert((nativePadSnapshot().released & expected) == expected);
    updateNativePad();
    assert(nativePadSnapshot().released == 0);
    for (int x = -32768; x <= 32767; ++x) {
        const auto positive = mapNativePad({0, s16(x), 0, s16(x), 0});
        assert(positive.stickX >= -127 && positive.stickX <= 127);
        assert(positive.substickX == positive.stickX);
        if (x >= -156 && x <= 156) {
            sample = {0, s16(x), s16(x), s16(x), s16(x)};
            updateNativePad();
            state = nativePadSnapshot();
            const auto length = std::hypot(state.circleX, state.circleY);
            assert(std::isfinite(length) && length <= 1.000001f);
            assert(std::hypot(state.cstickX, state.cstickY) <= 1.000001f);
        }
    }
    sample = {nativeB, 100, -100, 80, -80};
    updateNativePad();
    connected = false;
    updateNativePad();
    state = nativePadSnapshot();
    assert(state.held == 0 && state.circleX == 0 && state.circleY == 0);
    assert(!mDoCPd_c::getHold(0) && !mDoCPd_c::getStickValue(0));
    assert(mDoCPd_c::m_cpadInfo[0].mGamepadErrorFlags == PAD_ERR_NO_CONTROLLER);
    assert((state.released & PAD_BUTTON_B) != 0);
    connected = true;
    sample = {};
    updateNativePad();
    JUTGamePad::clearResetOccurred();
    sample.held = nativeStart | nativeX | nativeB;
    updateNativePad(); // first tick starts the chord timer
    for (unsigned i = 0; i < 14; ++i) updateNativePad();
    assert(!JUTGamePad::C3ButtonReset::sResetOccurred);
    updateNativePad();
    assert(JUTGamePad::C3ButtonReset::sResetOccurred);
    assert(kernelStats().healthy && kernelStats().free == heap.free);
    std::puts("PASS: original JUT read/edges/stick/trigger/disconnect/reset over mapped native samples; no per-tick heap allocations; physical HID unverified");
}
