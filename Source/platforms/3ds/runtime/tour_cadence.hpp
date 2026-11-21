#pragma once
#include <cstdint>
namespace dusk::native {
struct FrameTelemetryInput;
// Main render owner. Measures intervals between consecutive frame submissions,
// including work between iterations. A new step inherits the preceding boundary.
void beginTourCadence(const char* directory, unsigned step, std::uint64_t tick);
bool tourCadenceActive();
void recordTourCadence(const FrameTelemetryInput&, std::uint8_t phase, std::uint8_t flags);
bool endTourCadence();
}
