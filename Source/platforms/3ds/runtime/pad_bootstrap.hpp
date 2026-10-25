#pragma once
#include <cstdint>

namespace dusk::native {
struct NativePadSnapshot {
    std::uint32_t held, pressed, released;
    float circleX, circleY, cstickX, cstickY, triggerL, triggerR;
};
bool initializeNativePad();
void updateNativePad();
NativePadSnapshot nativePadSnapshot();
}
