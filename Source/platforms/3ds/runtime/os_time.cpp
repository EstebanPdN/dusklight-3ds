#include "clock.hpp"
#include "primitives.hpp"
#include <dolphin/os.h>
#include <chrono>

namespace {
dusk::native::Clock gameClock;
s32 clockLock;
bool clockReady;
struct Guard {
    Guard() {
        if (!clockReady) OSPanic(__FILE__, __LINE__, "Native clock is not initialized");
        dusk::native::lock(&clockLock);
    }
    ~Guard() { dusk::native::unlock(&clockLock); }
};
}

bool dusk::native::initializeClock() {
    if (clockReady) return true;
    if (!gameClock.initialize(counterTicks(), wallMilliseconds())) return false;
    lockInit(&clockLock);
    clockReady = true;
    return true;
}
void dusk::native::setClockPaused(bool paused) {
    Guard guard;
    gameClock.setPaused(paused, counterTicks());
}

extern "C" {
OSTime OSGetTime() {
    Guard guard;
    return gameClock.gameTime(dusk::native::counterTicks());
}
OSTick OSGetTick() { return static_cast<OSTick>(OSGetTime()); }
OSTime OSGetNativeTime() {
    Guard guard;
    return gameClock.nativeTime(dusk::native::counterTicks());
}
OSTime OSGetSystemTime() {
    std::int64_t value;
    if (!dusk::native::wallMillisecondsToTicks(dusk::native::wallMilliseconds(), value))
        OSPanic(__FILE__, __LINE__, "Native wall clock out of range");
    return value;
}
void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* output) {
    if (!output) OSPanic(__FILE__, __LINE__, "Null calendar output");
    namespace c = std::chrono;
    auto seconds = ticks / dusk::native::dolphinTickFrequency;
    auto subsecond = ticks % dusk::native::dolphinTickFrequency;
    if (subsecond < 0) { --seconds; subsecond += dusk::native::dolphinTickFrequency; }
    const auto dateTime = c::sys_days{c::year{2000} / 1 / 1} + c::seconds{seconds};
    const auto day = c::floor<c::days>(dateTime);
    const c::year_month_day date{day};
    const c::hh_mm_ss time{dateTime - day};
    const auto microseconds = subsecond * 1000000 / dusk::native::dolphinTickFrequency;
    *output = {int(time.seconds().count()), int(time.minutes().count()), int(time.hours().count()),
               int(unsigned(date.day())), int(unsigned(date.month())) - 1, int(date.year()),
               int(c::weekday{day}.c_encoding()),
               int((day - c::sys_days{date.year() / 1 / 1}).count()),
               int(microseconds / 1000), int(microseconds % 1000)};
}
OSTime OSCalendarTimeToTicks(OSCalendarTime* input) {
    if (!input) OSPanic(__FILE__, __LINE__, "Null calendar input");
    namespace c = std::chrono;
    if (input->year < 1 || input->year > 9000 || input->mon < 0 || input->mon > 11 ||
        input->mday < 1 || input->mday > 31 ||
        input->hour < 0 || input->hour > 23 || input->min < 0 || input->min > 59 ||
        input->sec < 0 || input->sec > 59 || input->msec < 0 || input->msec > 999 ||
        input->usec < 0 || input->usec > 999)
        OSPanic(__FILE__, __LINE__, "Invalid calendar date");
    const c::year_month_day date{c::year{input->year}, c::month{unsigned(input->mon + 1)},
                                c::day{unsigned(input->mday)}};
    if (!date.ok()) OSPanic(__FILE__, __LINE__, "Invalid calendar day");
    const auto delta = c::sys_days{date} - c::sys_days{c::year{2000} / 1 / 1};
    const std::int64_t seconds = c::duration_cast<c::seconds>(delta).count() +
                                input->hour * 3600 + input->min * 60 + input->sec;
    return seconds * dusk::native::dolphinTickFrequency +
           std::int64_t(input->msec * 1000 + input->usec) * dusk::native::dolphinTickFrequency / 1000000;
}
}
