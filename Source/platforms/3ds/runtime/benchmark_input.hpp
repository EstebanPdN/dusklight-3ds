#pragma once

#include <cstdint>

namespace dusk::native {
struct BenchmarkInputSite {
    std::uint64_t previousFrame{},requestTick{};
    char stage[9]{};
    int room{-1};
    unsigned zone{};
    const char* storyStep{};  // Immutable itinerary strings, safe on the HID thread.
    const char* storyPhase{};
    unsigned storyTick{};
};
struct NativeInputFrame {
    std::uint32_t held{},down{};
    std::int16_t circleX{},circleY{},cstickX{},cstickY{};
    std::uint16_t touchX{},touchY{};
};
void setBenchmarkInputSite(const BenchmarkInputSite&);
void scanNativeGameInput();
const NativeInputFrame& nativeGameInput();
bool takeBenchmarkInputEdge(BenchmarkInputSite&);
inline bool benchmarkInputReady(const BenchmarkInputSite& edge,std::uint64_t submitTick){
    return edge.requestTick<=submitTick;
}
// Zero means no sampler remains. A failed kernel wait retains its stack and TLS.
[[nodiscard]] std::int32_t stopBenchmarkInputSampler();
}
