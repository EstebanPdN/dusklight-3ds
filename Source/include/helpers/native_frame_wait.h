#pragma once
#include <cstdint>
#include <dolphin/os/OSMessage.h>
namespace dusk::native {
struct FrameRetraceDeadline { std::uint32_t next{}; bool initialized{}; };
// Render-owner dispatch from real platform VBlank counts. Never installs or
// replaces libctru/Citro3D interrupt callbacks. Multiple/concurrent owners reject.
void waitVideoRetrace();
std::uint32_t waitForFrameRetraces(OSMessageQueue*,unsigned interval,FrameRetraceDeadline&);
// True once if the game's display wait ran since the last call. The frame loop
// adds its own retrace wait only when an iteration never reached that wait.
bool consumeFramePaced();
// Native ticks the render owner slept in the display wait since the last call.
std::uint64_t takeFramePaceTicks();
void sleepGameTicks(std::int64_t ticks);
}
