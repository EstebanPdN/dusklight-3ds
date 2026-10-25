#include "../runtime/clock.hpp"
#include <dolphin/os.h>
#include <cassert>
#include <cstdio>
#include <initializer_list>

namespace {
std::uint64_t counter = 100, wall = dusk::native::epoch1900To2000Ms;
}
namespace dusk::native {
std::uint64_t counterTicks() { return counter; }
std::uint64_t wallMilliseconds() { return wall; }
}

int main() {
    using namespace dusk::native;
    static_assert(counterToTicks(nativeTickFrequency) == dolphinTickFrequency);
    static_assert(counterToTicks(nativeTickFrequency * 86400ULL) == dolphinTickFrequency * 86400ULL);
    static_assert(counterToTicks(UINT64_MAX) > 0);
    // Check split arithmetic against a wider reference at extreme and ordinary intervals.
    for (std::uint64_t ticks : {0ULL, 1ULL, 268111855ULL, 268111856ULL, 268111857ULL,
                               1000000000000000000ULL, 18446744073709551615ULL}) {
        const auto reference = static_cast<std::int64_t>(
            static_cast<unsigned __int128>(ticks) * dolphinTickFrequency / nativeTickFrequency);
        assert(counterToTicks(ticks) == reference);
    }
    std::int64_t converted;
    assert(wallMillisecondsToTicks(epoch1900To2000Ms, converted) && converted == 0);
    assert(wallMillisecondsToTicks(epoch1900To2000Ms - 1, converted) && converted == -40500);
    assert(!wallMillisecondsToTicks(UINT64_MAX, converted));
    assert(initializeClock() && initializeClock());
    assert(OSGetTime() == 0 && OSGetSystemTime() == 0);
    counter += nativeTickFrequency;
    assert(OSGetTime() == dolphinTickFrequency);
    setClockPaused(true);
    counter += 10 * nativeTickFrequency;
    assert(OSGetTime() == dolphinTickFrequency);
    assert(OSGetNativeTime() == 11 * dolphinTickFrequency);
    setClockPaused(true); // Repeated pause must preserve the original pause point.
    setClockPaused(false);
    counter += nativeTickFrequency;
    assert(OSGetTime() == 2 * dolphinTickFrequency);
    wall += 60000; // RTC adjustment must not alter elapsed game time.
    assert(OSGetSystemTime() == 60 * dolphinTickFrequency);
    assert(OSGetTime() == 2 * dolphinTickFrequency);
    counter += 1000 * nativeTickFrequency;
    assert(OSGetTick() == static_cast<OSTick>(1002 * dolphinTickFrequency));

    OSCalendarTime date{};
    OSTicksToCalendarTime(0, &date);
    assert(date.year == 2000 && date.mon == 0 && date.mday == 1 && date.wday == 6 && date.yday == 0);
    OSTicksToCalendarTime(-40500, &date);
    assert(date.year == 1999 && date.mon == 11 && date.mday == 31);
    assert(date.hour == 23 && date.min == 59 && date.sec == 59 && date.msec == 999 && date.usec == 0);
    for (int year : {1900, 2000, 2024, 2026, 2100}) {
        for (int month = 0; month < 12; ++month) {
            OSCalendarTime original{17, 23, 12, 1, month, year, 0, 0, 456, 789};
            const auto time = OSCalendarTimeToTicks(&original);
            OSTicksToCalendarTime(time, &date);
            assert(date.year == year && date.mon == month && date.mday == 1);
            assert(date.hour == 12 && date.min == 23 && date.sec == 17 && date.msec == 456);
            assert(date.usec >= 788 && date.usec <= 789); // Integer 40.5-tick microseconds.
        }
    }
    OSCalendarTime leap{0, 0, 0, 29, 1, 2024, 0, 0, 0, 0};
    OSTicksToCalendarTime(OSCalendarTimeToTicks(&leap), &date);
    assert(date.mday == 29 && date.yday == 59);
    Clock invalid;
    assert(!invalid.initialize(0, UINT64_MAX));
    std::puts("PASS: exact counter scaling, pause/resume, RTC independence, tick wrap and Gregorian dates");
}
