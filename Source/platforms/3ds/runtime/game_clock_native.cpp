#include <aurora/time.hpp>
#include <dolphin/os.h>
#include <cmath>
#include <limits>

// Aurora's public clock ABI, backed by the native OS clock. Application suspend
// and dump pauses already stop OSGetTime; native time remains monotonic.
namespace {
struct Timing {
    OSMutex mutex;
    OSTime nativeOrigin, previousGame;
    std::int64_t scaledNs = 0;
    double fractionNs = 0;
    float scale = 1;
    Timing() {
        OSInitMutex(&mutex);
        nativeOrigin = OSGetNativeTime();
        previousGame = OSGetTime();
    }
};
Timing& timing() { static Timing state; return state; }
struct Guard {
    Timing& state;
    Guard() : state(timing()) { OSLockMutex(&state.mutex); }
    ~Guard() { OSUnlockMutex(&state.mutex); }
};
constexpr std::int64_t frequency = 40500000;
std::int64_t nanoseconds(OSTime delta) {
    if (delta < 0 || delta / frequency > INT64_MAX / 1000000000)
        OSPanic(__FILE__, __LINE__, "Native game-clock interval out of range");
    const auto whole = (delta / frequency) * 1000000000;
    const auto remainder = (delta % frequency) * 1000000000 / frequency;
    if (remainder > INT64_MAX - whole)
        OSPanic(__FILE__, __LINE__, "Native game-clock nanosecond overflow");
    return whole + remainder;
}
void sample(Timing& state) {
    const auto now = OSGetTime();
    const auto delta = now - state.previousGame;
    if (delta < 0) OSPanic(__FILE__, __LINE__, "Native game clock moved backwards");
    state.previousGame = now;
    // Carry sub-nanosecond tick fractions across samples instead of truncating
    // every OS interval. There is no new sample-buffer allocation.
    const double elapsed = double(delta / frequency) * 1000000000 +
                           double(delta % frequency) * 1000000000 / frequency;
    const double advance = elapsed * state.scale + state.fractionNs;
    if (!std::isfinite(advance) || advance >= double(INT64_MAX - state.scaledNs))
        OSPanic(__FILE__, __LINE__, "Scaled native game clock overflow");
    const auto whole = static_cast<std::int64_t>(advance);
    state.scaledNs += whole;
    state.fractionNs = advance - double(whole);
}
}
namespace aurora::time {
native_clock::time_point native_clock::now() noexcept {
    Guard guard;
    return time_point{duration{nanoseconds(OSGetNativeTime() - guard.state.nativeOrigin)}};
}
game_clock::time_point game_clock::now() noexcept {
    Guard guard;
    sample(guard.state);
    return time_point{duration{guard.state.scaledNs}};
}
void set_scale(float value) noexcept {
    if (!std::isfinite(value) || value < 0 || value > kMaximumTimeScale)
        OSPanic(__FILE__, __LINE__, "Invalid native simulation time scale");
    Guard guard;
    sample(guard.state); // Rate changes preserve elapsed time under the old rate.
    guard.state.scale = value;
}
float scale() noexcept { Guard guard; return guard.state.scale; }
}
