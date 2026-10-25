#pragma once
#include <dolphin/pad.h>

namespace dusk::native {
// HID key bits are checked against installed libctru in pad_native.cpp.
enum NativePadKey : u32 {
    nativeA = 1u << 0, nativeB = 1u << 1, nativeStart = 1u << 3,
    nativeRight = 1u << 4, nativeLeft = 1u << 5,
    nativeUp = 1u << 6, nativeDown = 1u << 7,
    nativeR = 1u << 8, nativeL = 1u << 9,
    nativeX = 1u << 10, nativeY = 1u << 11,
    nativeZL = 1u << 14, nativeZR = 1u << 15,
};
struct NativePadSample {
    u32 held;
    s16 circleX, circleY, cstickX, cstickY;
};
// Produces pre-clamp PADStatus. Original JUTGamePad owns clamp, button edges,
// repeats, stick normalization and reset chords at the simulation tick rate.
PADStatus mapNativePad(const NativePadSample& input);
}
