#pragma once
#include <cstdint>
namespace dusk::native {
void waitVideoRetrace();
// The production backend uses C3D's real IRQ counter at60 Hz and GSP VBlank0.
// Host tests provide an explicit synthetic event source, never a game provider.
std::uint32_t platformVideoRetraceCount();
void platformWaitVideoRetrace();
void platformSleepNanoseconds(std::int64_t);
}
