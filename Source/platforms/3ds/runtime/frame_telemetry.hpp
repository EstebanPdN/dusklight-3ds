#pragma once
#include "gpu_sync.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

namespace dusk::native {
// Console frame telemetry. The render owner records to RAM. During story
// tests the SD worker writes captured zone histories while the game continues.
//   sdmc:/3ds/DuskLight/perf-summary.csv  one row per stage and room, with a loop-time histogram
//   sdmc:/3ds/DuskLight/frames.csv        the most recent frames, one row each
// All inputs are native system ticks measured by the render owner.
struct FrameTelemetryInput {
    std::uint64_t frame{},submitTick{};
    // Actor-driven demos record the submitted picture number here.
    std::uint32_t demoFrame{},linearFree{},gameHeapFree{};
    // Bits 0/1/2: demo camera, running event, pending or held transition.
    std::uint8_t sceneFlags{};
    std::uint64_t loopTicks{},executionTicks{},paintTicks{},updateTicks{},drawTicks{};
    std::uint64_t paceTicks{},audioMixTicks{};
    GpuFrameTelemetry gpu{};
    float gpuTailMilliseconds{};
    std::uint32_t draws{};
    const char* stage{};
    int room{};
    // Original 30 Hz logic ticks run for this picture (real-speed scheduling).
    std::uint32_t ticks{1};
};
// Legacy diagnostics are excluded from the legacy zone statistics. Capture
// flags annotate measured intervals without excluding samples.
constexpr std::uint8_t telemetryDiagnosticFrame=1;
constexpr std::uint8_t telemetryCaptureOwner=2;
constexpr std::uint8_t telemetryCaptureWriter=4;
constexpr std::uint8_t telemetryFullDump=8;
// Main thread, before the first frame. A failed allocation disables recording.
void initializeFrameTelemetry();
// Test-mode attribution of the following samples: zone 0 means ordinary play.
void setFrameTelemetryTag(std::uint16_t zone,std::uint8_t phase);
// Marks the next recorded sample (only that one) with the given flags.
void flagNextFrameTelemetry(std::uint8_t flags);
void recordFrameTelemetry(const FrameTelemetryInput&);
// Main thread only, outside any GPU frame. Returns false if nothing was written.
bool writeFrameTelemetry(bool includeFrames);
// Statistics of the unflagged retained samples of one zone; phase -1 takes all.
struct TelemetryStats {
    std::uint32_t frames{},flagged{},saturated{};
    float loopMean{},loopP50{},loopP95{},loopP99{},loopMax{};
    float execMean{},execP95{},execMax{};
    float paint{},update{},draw{},pace{},gpuWait{},gpuTail{},audio{},draws{};
    // The GPU's own busy and idle time (frames.csv gpu_busy_ms, gpu_idle_ms).
    float gpuBusy{},gpuIdle{};
    std::uint32_t over34{},over40{},over50{};
};
// phase: a phase number; -1 every phase; -2-N every phase except N.
// excluded: with phase -1, a bit per phase number left out.
bool frameTelemetryStats(std::uint16_t zone,int phase,TelemetryStats& out,std::uint32_t excluded=0,
                         std::uint8_t excludedFlags=telemetryDiagnosticFrame);
// Appends the retained samples of one zone as CSV rows (with a header).
bool writeFrameTelemetryZone(std::FILE* file,std::uint16_t zone);
// Render owner copies the zone history. The SD writer only accesses that copy.
bool queueZoneFrameCsv(const char* path, std::uint16_t zone);
bool writePendingZoneFrameCsv();
// The most recent samples as CSV text (header included) in a caller buffer;
// returns the bytes written. No allocation, no file.
std::size_t formatRecentFrameTelemetry(char* out,std::size_t capacity,unsigned frames);
// Owned recent-history copies for the manual benchmark writer. Copy on the
// render thread, format on the writer thread.
struct FrameTelemetryRow {
    std::uint32_t frame;
    std::uint16_t loop,execution,paint,update,draw,pace,gpuBegin,gpuEnd,gpuComplete,present,gpuTail,audio;
    std::uint16_t gpuLists,gpuCopies,gpuIdle,gpuLongest;
    std::uint8_t gpuJobs;
    std::uint16_t draws,zone;
    std::uint8_t chunks,completes,reclaims;
    std::int8_t room;
    std::uint8_t phase,flags,ticks;
    char stage[8];
};
std::size_t copyRecentFrameTelemetry(std::span<FrameTelemetryRow> out);
bool writeFrameTelemetryRows(std::FILE*,std::span<const FrameTelemetryRow>);

// Samples lost because a stage/room summary table was full, and samples whose
// 16-bit time fields saturated (a stall longer than 655 ms).
std::uint32_t frameTelemetryDropped();
std::uint32_t frameTelemetrySaturated();
}
