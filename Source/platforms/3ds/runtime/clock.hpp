#pragma once
#include <cstdint>

namespace dusk::native {
constexpr std::uint64_t nativeTickFrequency = 16756991u * 16u;
constexpr std::int64_t dolphinTickFrequency = 40500000;
constexpr std::uint64_t epoch1900To2000Ms = 3155673600000ULL;

constexpr std::int64_t counterToTicks(std::uint64_t value) {
    // Split before multiplying: a single multiply overflows after minutes.
    return static_cast<std::int64_t>(
        (value / nativeTickFrequency) * dolphinTickFrequency +
        ((value % nativeTickFrequency) * dolphinTickFrequency) / nativeTickFrequency);
}

constexpr bool wallMillisecondsToTicks(std::uint64_t milliseconds, std::int64_t& result) {
    if (milliseconds > std::uint64_t(INT64_MAX / 40500) + epoch1900To2000Ms) return false;
    result = (std::int64_t(milliseconds) - std::int64_t(epoch1900To2000Ms)) * 40500;
    return true;
}

class Clock {
    std::uint64_t start = 0, pausedAt = 0, pausedCounterTicks = 0;
    std::int64_t wallOrigin = 0;
    bool paused = false;
public:
    bool initialize(std::uint64_t counter, std::uint64_t wallMilliseconds) {
        if (!wallMillisecondsToTicks(wallMilliseconds, wallOrigin)) return false;
        if (wallOrigin > INT64_MAX - counterToTicks(UINT64_MAX)) return false;
        start = counter;
        pausedCounterTicks = 0;
        paused = false;
        return true;
    }
    void setPaused(bool value, std::uint64_t counter) {
        if (value == paused) return;
        if (value) pausedAt = counter;
        else pausedCounterTicks += counter - pausedAt;
        paused = value;
    }
    std::int64_t gameTime(std::uint64_t counter) const {
        return wallOrigin + counterToTicks((paused ? pausedAt : counter) - start - pausedCounterTicks);
    }
    std::int64_t nativeTime(std::uint64_t counter) const {
        return wallOrigin + counterToTicks(counter - start);
    }
};

// Initialize before game threads; pause on application suspend and dump pauses.
bool initializeClock();
void setClockPaused(bool paused);
}
