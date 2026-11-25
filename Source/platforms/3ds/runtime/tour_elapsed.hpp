#pragma once
#include <cstdint>

namespace dusk::native {
// Elapsed time of an automatic test after a restart (crash, stuck zone, power
// cycle). saved is the figure tour-state.txt kept at the last zone start;
// the wall clock since the test's first start also counts the zone that ended
// the previous launch and the relaunch, but after a freeze the owner may
// restart the console hours later, and those hours are not test time. The
// wall clock is believed up to resumeGapLimit seconds past the saved figure:
// one zone at its worst (150 s load limit, phases at 400 % scale, the dump)
// plus the relaunch. Beyond it the saved figure plus the limit is used.
// Pure (no SDK calls) so the host test can check it.
constexpr std::uint64_t resumeGapLimit=600;
struct ResumedElapsed {
    std::uint64_t seconds;
    // Wall clock (osGetTime, ms) taken as the test's start from now on: the
    // stopped hours must not count again at a later restart.
    std::uint64_t startedMs;
    // 0 wall clock, 1 wall clock capped (console stopped or off), 2 saved
    // figure (no start recorded, or the clock was set back).
    int source;
    std::uint64_t wallSeconds;
};
inline ResumedElapsed resumedElapsed(std::uint64_t saved,std::uint64_t startedMs,std::uint64_t nowMs){
    const auto rebased=[&](std::uint64_t seconds){return nowMs>=seconds*1000?nowMs-seconds*1000:0;};
    if(!startedMs||nowMs<=startedMs)return {saved,rebased(saved),2,0};
    const auto wall=(nowMs-startedMs)/1000;
    if(wall<saved)return {saved,rebased(saved),2,wall};
    if(wall-saved<=resumeGapLimit)return {wall,startedMs,0,wall};
    return {saved+resumeGapLimit,rebased(saved+resumeGapLimit),1,wall};
}
}
