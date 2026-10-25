#pragma once
#include <cstdint>
namespace dusk::native {
// Synthetic diagnostic only: exercises real GD/typed GX -> native GPU.
bool initializeCommandProbe();
bool drawCommandProbe();
void destroyCommandProbe();
struct CommandProbeStatistics { const char* failure; std::uint64_t submitted, rejected; };
CommandProbeStatistics commandProbeStatistics();
}
