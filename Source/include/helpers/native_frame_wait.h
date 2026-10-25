#pragma once
#include <cstdint>
#include <dolphin/os/OSMessage.h>
namespace dusk::native {
struct FrameRetraceDeadline { std::uint32_t next{}; bool initialized{}; };
// Render-owner dispatch from real platform VBlank counts. Never installs or
// replaces libctru/Citro3D interrupt callbacks. Multiple/concurrent owners reject.
void waitVideoRetrace();
std::uint32_t waitForFrameRetraces(OSMessageQueue*,unsigned interval,FrameRetraceDeadline&);
void sleepGameTicks(std::int64_t ticks);
}
