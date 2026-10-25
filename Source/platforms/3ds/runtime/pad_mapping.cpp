#include "pad_mapping.hpp"
#include <algorithm>

namespace dusk::native {
namespace {
s8 axis(s16 value) {
    // Initial calibration maps +/-156 to the PAD range. Physical calibration
    // is pending. Saturate before narrowing; original PAD supplies the deadzone.
    return static_cast<s8>(std::clamp(int(value), -156, 156) * 127 / 156);
}
}
PADStatus mapNativePad(const NativePadSample& input) {
    PADStatus result{};
    constexpr struct { u32 native; u16 game; } buttons[] = {
        {nativeA, PAD_BUTTON_A}, {nativeB, PAD_BUTTON_B},
        {nativeX, PAD_BUTTON_X}, {nativeY, PAD_BUTTON_Y},
        {nativeStart, PAD_BUTTON_START},
        {nativeRight, PAD_BUTTON_RIGHT}, {nativeLeft, PAD_BUTTON_LEFT},
        {nativeUp, PAD_BUTTON_UP}, {nativeDown, PAD_BUTTON_DOWN},
        {nativeL, PAD_TRIGGER_L}, {nativeR, PAD_TRIGGER_R},
        {nativeZL | nativeZR, PAD_TRIGGER_Z},
    };
    for (const auto& mapping : buttons)
        if (input.held & mapping.native) result.button |= mapping.game;
    result.stickX = axis(input.circleX);
    result.stickY = axis(input.circleY);
    result.substickX = axis(input.cstickX);
    result.substickY = axis(input.cstickY);
    // New3DS shoulders are digital: original clamp maps 180 to 150, then JUT
    // divides by 150 to provide the game's lock-on trigger value of 1.0.
    result.triggerLeft = input.held & nativeL ? 180 : 0;
    result.triggerRight = input.held & nativeR ? 180 : 0;
    result.analogA = input.held & nativeA ? 255 : 0;
    result.analogB = input.held & nativeB ? 255 : 0;
    result.err = PAD_ERR_NONE;
    return result;
}
}
