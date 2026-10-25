#pragma once
#include <cstddef>
#include <cstdint>
#include "runtime/bootstrap.hpp"
#include "preview.hpp"
#include "runtime/resource_archive.hpp"

namespace dusk::native {
// Zero means released; 1/2 are quick/full. Initially held chords are ignored.
class DumpArm {
    bool released = false;
    int mode = 0;
    std::uint64_t start = 0;
public:
    int update(int heldMode, std::uint64_t milliseconds, bool busy) {
        if (!heldMode) { released = true; mode = 0; return 0; }
        if (busy) { released = false; mode = 0; return 0; }
        if (released) { released = false; mode = heldMode; start = milliseconds; }
        if (mode != heldMode) { mode = 0; return 0; }
        if (mode && milliseconds - start >= 1000) { int result = mode; mode = 0; return result; }
        return 0;
    }
    bool arming() const { return mode != 0; }
};
struct DiagnosticSnapshot {
    std::uint64_t frame;
    double frameMs, p50, p95, p99;
    HeapStats gameHeap;
    std::uint32_t appRegionBytes, linearFree;
    std::uint32_t buttons;
    bool visible, new3ds, kernelOkay, workerOkay, previewReady;
    int discStatus;
};
bool startDiagnosticWorker();
void stopDiagnosticWorker();
bool startupFinished();
bool startupWorkerOkay();
int startupDiscStatus();
const char* startupResourceStatus();
bool startupPreviewImage(PreviewImage& output);
struct StartupJ2dResources { ResourceSpan nintendo, warning, prompt; };
// Immutable borrowed data, valid until stopDiagnosticWorker(). All J2D/GPU
// consumers must be destroyed/drained before the worker is stopped.
bool startupJ2dResources(StartupJ2dResources& output);
bool dumpBusy();
bool requestDump(int mode, const DiagnosticSnapshot& snapshot);
const char* dumpStateText();
std::uint32_t dumpProgressKiB();
std::uint32_t dumpTotalKiB();
const char* completedDumpPath();
}
