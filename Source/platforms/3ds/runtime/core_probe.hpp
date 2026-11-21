#pragma once
#include <cstdio>
// End-of-test probe. dev_console.cpp runs it once, after the last zone of an
// automatic test that ran every zone, when its results are already on the
// card. It measures on the console what no emulator gives and what the next
// performance step (GX translation off core 0) needs to know:
//  - how fast one busy thread runs on core 2 beside the audio mixer and on
//    core 1 under APT_SetAppCpuTimeLimit at 30, 50 and 80 percent, against
//    the same loop on core 0 (core 1 also runs the system modules, and an
//    application gets a share of it only after that call);
//  - what a small GSP request costs on core 0 while that thread is busy;
//  - what a cache flush costs through GSP (one IPC each, what the renderer
//    uses) and through the kernel call (svc-flush.txt).
// It takes about a second, draws nothing and leaves the time limit as it
// found it. Its "core_probe_*=" lines go to `out` (tour.txt) and to the log.
// tour-options.txt probe=0 leaves it out.
namespace dusk::native {
void runCoreProbe(std::FILE* out);
}
