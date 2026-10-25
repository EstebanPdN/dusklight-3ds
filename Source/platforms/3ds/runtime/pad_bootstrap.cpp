#include "pad_bootstrap.hpp"
#include "JSystem/JUtility/JUTGamePad.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "m_Do/m_Do_controller_pad.h"

namespace dusk::native {
namespace {
// Diagnostic JUT owner retained until application exit; allocate once after JKR
// root initialization. Gameplay must replace this with mDoCPd startup ownership.
JUTGamePad* pad;
}
bool initializeNativePad() {
    if (pad || !JUTGamePad::init()) return false;
    pad = JKR_NEW JUTGamePad(JUTGamePad::EPort1);
    return pad != nullptr;
}
void updateNativePad() {
    if (pad) {
        JUTGamePad::read();
        // Exercise the real game input conversion, rather than leaving the
        // sampled HID state only in the diagnostic JUT owner. The full game's
        // startup/read path still owns reset handling and analog lock edges.
        mDoCPd_c::convert(&mDoCPd_c::m_cpadInfo[0], pad);
    }
}
NativePadSnapshot nativePadSnapshot() {
    if (!pad) return {};
    return {mDoCPd_c::getHold(0), mDoCPd_c::getTrig(0), pad->getRelease(),
            mDoCPd_c::getStickX3D(0), mDoCPd_c::getStickY(0),
            mDoCPd_c::getSubStickX3D(0), mDoCPd_c::getSubStickY(0),
            mDoCPd_c::getAnalogL(0), mDoCPd_c::getAnalogR(0)};
}
}
