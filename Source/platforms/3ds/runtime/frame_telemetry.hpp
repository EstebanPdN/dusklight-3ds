#pragma once
#include "gpu_sync.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace dusk::native {
// Console frame telemetry. Samples are kept in RAM and written to the SD card
// only at exit or between test zones, never inside a measured frame: an SD
// write is a visible hitch on hardware and the loader threads use the card.
//   sdmc:/3ds/DuskLight/perf-summary.csv  one row per stage and room, with a loop-time histogram
//   sdmc:/3ds/DuskLight/frames.csv        the most recent frames, one row each
// All inputs are native system ticks measured by the render owner.
struct FrameTelemetryInput {
    std::uint64_t frame{};
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
// Sample flags. A flagged frame carries measurement cost (detailed profiling,
// a report, an SD write just before it) and is left out of every statistic.
constexpr std::uint8_t telemetryDiagnosticFrame=1;
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
    std::uint32_t over34{},over40{},over50{};
};
// phase: a phase number; -1 every phase; -2-N every phase except N.
bool frameTelemetryStats(std::uint16_t zone,int phase,TelemetryStats& out);
// Appends the retained samples of one zone as CSV rows (with a header).
bool writeFrameTelemetryZone(std::FILE* file,std::uint16_t zone);
// The most recent samples as CSV text (header included) in a caller buffer;
// returns the bytes written. No allocation, no file.
std::size_t formatRecentFrameTelemetry(char* out,std::size_t capacity,unsigned frames);
// Samples lost because a stage/room summary table was full, and samples whose
// 16-bit time fields saturated (a stall longer than 655 ms).
std::uint32_t frameTelemetryDropped();
std::uint32_t frameTelemetrySaturated();
}
