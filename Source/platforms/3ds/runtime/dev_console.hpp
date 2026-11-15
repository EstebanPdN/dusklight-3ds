#pragma once
#include "boot_stage.hpp"
#include <cstdint>
#include <cstdio>

namespace dusk::native {
// Developer console on the bottom screen (touch tabs) and the automatic test
// mode. Ordinary play is unchanged until the player touches a tab or holds
// START+SELECT:
//   STATS  frame time, scene and memory figures; SHADER A/B asks the next
//          start for the single-texture shader route (the GPU guard turns it
//          off again if it stops this console)
//   ZONES  every stage of the disc; D-pad moves, LEFT/RIGHT picks the room,
//          X the form, A travels there through the original PLAY_SCENE change
//   TEST   automatic test: travels through a list of zones and in each one
//          plays a fixed input script (camera, walking, rolling, sword,
//          targeting, items) while recording telemetry. START+SELECT starts
//          and stops it. Every zone leaves a report, the frame samples, two
//          screenshots and the log under sdmc:/3ds/DuskLight/tour/NNN/.
//   EXIT   leaves the game (writes perf-summary.csv and frames.csv)
// L+R+A takes the complete RAM + VRAM + LCD dump at once (one packed file).
struct ZoneRoom {std::uint8_t room;std::int16_t point;};
// primary: index (within the stage) of the room taken when one room stands for the stage.
struct ZoneStage {const char* stage;const char* label;std::uint16_t firstRoom;std::uint8_t roomCount,primary,mainTour;};
#include "zone_table.inc"

// Sampled by the game loop once per completed frame.
struct DevGameStatus {
    std::uint64_t frame{};
    const char* stage{""};
    int room{-1},point{},layer{};
    bool playScene{},playerReady{},stageChanging{};
    float loopMs{},execMs{},paintMs{},updateMs{},gpuWaitMs{};
    // Original logic ticks this picture carried (real-speed scheduling).
    unsigned ticks{1};
    std::uint32_t draws{};
    std::uint64_t rejected{},degraded{};
    std::uint32_t textureBytes{},geometryBytes{},gameHeapFree{},linearFree{};
    float position[3]{};
};
struct DevGameHooks {
    // Requests the original stage change (form 0 human, 1 wolf). False when
    // the current scene cannot accept it yet; the caller retries. The test
    // always plays on an empty save with its own equipment; a manual trip
    // keeps the save the player loaded.
    bool (*warp)(const char* stage,int room,int point,int layer,int form,bool test){};
    // Appends renderer and memory counters as key=value lines.
    void (*report)(std::FILE*){};
    // Requests the complete application dump (same as L+R+A).
    void (*fullDump)(){};
    bool (*fullDumpBusy)(){};
    // Logs why a stage is not starting: disc state, scene overlap, and the
    // processes still being created.
    void (*stuck)(){};
};
// Before the boot stage is read and before the log file is first opened.
void initializeDevConsole(const DevGameHooks&);
// A resumed or automatic test starts directly in its next zone.
bool devConsoleBootStage(BootStage& stage);
// Once per frame after hidScanInput.
void updateDevConsoleInput(std::uint32_t held,std::uint32_t down);
// True while the console or the test owns the pad: the game then receives
// only the virtual input (all zero outside a test).
bool devConsoleOwnsPad();
void devConsoleVirtualPad(std::uint32_t& keys,std::int16_t& circleX,std::int16_t& circleY,std::int16_t& cstickX,std::int16_t& cstickY);
bool devConsoleExitRequested();
// Smoothed picture rate and game speed (percent of the original 30 Hz logic).
void devConsoleRates(float& fps,int& speedPercent);
// The test's profile phase wants the sparse detailed profile and its report.
bool devConsoleProfiling();
// Main owner, after the GPU frame is closed. Returns true when it spent time
// outside the game (SD writes): the caller resets its frame timing.
bool serviceDevConsole(const DevGameStatus&);
// Something else printed on the bottom screen (dump progress): repaint it.
void devConsoleRedraw();
// After the game loop: a clean exit ends a running test instead of resuming it.
void shutdownDevConsole();
// Native log ring access (main.cpp), used to keep each zone's log lines.
std::size_t nativeLogPosition();
void writeNativeLogSince(std::size_t mark,std::FILE* file);
}
