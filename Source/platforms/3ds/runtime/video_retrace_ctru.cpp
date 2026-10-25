#include "video_retrace.hpp"
#include <3ds.h>
#include <citro3d.h>
#include "bootstrap.hpp"

namespace dusk::native {
std::uint32_t platformVideoRetraceCount(){
    // C3D's counter follows its configured IRQ rate; keep native VI at physical
    // 60 Hz. The game's30 Hz policy is two retraces, not a changed IRQ counter.
    if(C3D_FrameRate(0.f)!=60.f)fatal("Native VI requires Citro3D at the physical60 Hz IRQ rate");
    return C3D_FrameCounter(0);
}
void platformWaitVideoRetrace(){gspWaitForVBlank0();}
void platformSleepNanoseconds(std::int64_t ns){svcSleepThread(ns);}
}
