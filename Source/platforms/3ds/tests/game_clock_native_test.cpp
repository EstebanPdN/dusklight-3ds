#include <aurora/time.hpp>
#include <dolphin/os.h>
#include "dusk/game_clock.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>

namespace {
OSTime gameTicks = 0, nativeTicks = 0;
void advanceTicks(OSTime ticks, bool paused = false) {
    nativeTicks += ticks;
    if (!paused) gameTicks += ticks;
}
long long gameNs() { return aurora::time::game_clock::now().time_since_epoch().count(); }
long long nativeNs() { return aurora::time::native_clock::now().time_since_epoch().count(); }
bool close(float a, float b) { return std::abs(a-b) < 0.00001f * std::max(1.f,std::abs(b)); }
}
extern "C" OSTime OSGetTime() { return gameTicks; }
extern "C" OSTime OSGetNativeTime() { return nativeTicks; }

int main() {
    using namespace dusk::game_clock;
    initialize();
    assert(gameNs() == 0 && nativeNs() == 0);
    assert(close(get_sim_rate(),30));
    advanceTicks(1350000);
    const auto normal = advance();
    assert(normal.numSimTicks == 1 && !normal.separatePresentation && !normal.interpolating);
    assert(close(normal.dt,1.f/30.f) && close(original_frames(),1));
    assert(is_sim_frame() && is_presentation_frame()); // Original combined phase.
    begin_sim_tick();
    assert(close(original_frames(),1));
    commit_sim_tick();

    const auto start = gameNs();
    for (int i=0;i<40500;++i) { advanceTicks(1); gameNs(); }
    assert(std::llabs(gameNs()-start-1000000) <= 1); // Tick fractions survive reads.
    const auto beforeRate = gameNs();
    set_sim_rate(60);
    assert(gameNs() == beforeRate && close(get_sim_rate(),60));
    advanceTicks(1350000);
    const auto fast = advance();
    assert(fast.separatePresentation && !fast.interpolating && fast.numSimTicks >= 1);
    assert(!is_sim_frame() && is_presentation_frame());
    for (int i=0;i<fast.numSimTicks;++i) {
        begin_sim_tick();
        assert(is_sim_frame() && !is_presentation_frame());
        const double fixedSample = sample_time();
        assert(sample_time() == fixedSample && close(original_frames(),1));
        commit_sim_tick();
        assert(!is_sim_frame() && is_presentation_frame());
    }
    assert(std::llabs(gameNs()-beforeRate-66666666) <= 1);
    double interval = sample_time();
    advanceTicks(405000);
    assert(close(consume_interval(interval),0.02f));
    assert(consume_interval(interval) == 0);

    advance(); // Consume the preceding unpaused 20 ms before the pause check.
    const auto pauseGame = gameNs(), pauseNative = nativeNs();
    advanceTicks(4050000,true);
    assert(gameNs() == pauseGame && nativeNs()-pauseNative == 100000000);
    const auto epoch = g_frameTiming.presentationEpoch;
    advanceTicks(20250000,true); // Suspended OS time; no catch-up burst on resume.
    const auto gap = advance();
    assert(gap.numSimTicks == 0 && gap.presentationEpoch > epoch && gap.dt == 0);

    aurora::time::set_scale(0);
    const auto stopped = gameNs();
    advanceTicks(1350000);
    assert(gameNs() == stopped && advance().numSimTicks == 0);
    set_sim_rate(30);
    assert(gameNs() == stopped);
    advanceTicks(1350000);
    assert(advance().numSimTicks == 1 && !g_frameTiming.separatePresentation);
    assert(is_sim_frame() && is_presentation_frame());
    set_sim_rate(-1); assert(close(get_sim_rate(),1));
    set_sim_rate(10000); assert(close(get_sim_rate(),480));
    set_sim_rate(30);
    std::puts("PASS original game clock + native time: phase/ticks/scale/pause/gaps/fractions");
}
