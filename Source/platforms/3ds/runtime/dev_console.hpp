#pragma once
#include "story_benchmark.hpp"
#include "story_chapter_proof.hpp"
#include "story_portal.hpp"
#include "boot_stage.hpp"
#include <cstdint>
#include <cstdio>

namespace dusk::native {
enum class TourVisualReason : unsigned;
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
//          shot1.bmp: the view at the end of the camera phase (phase 1).
//          shot2.bmp and bottom.bmp: the end of the zone; the items phase
//          comes before still/plain/profile, so an item (bow, clawshot,
//          Dominion Rod aim) may still be out in shot2.
//          The last phase, menu (Alpha 8), opens the collection screen with
//          START, holds it and closes it again: its pictures measure the
//          game's own 2D menus on the console. shot3.bmp is taken with the
//          menu open; report.txt menu_pictures_open= counts the pictures the
//          game was paused in. The plain, profile and menu phases stay out
//          of the zone's overall figures, which so remain comparable with
//          earlier builds. tour-options.txt menu=0 leaves the phase out.
//          Link's life is restored to the zone's starting value at the start
//          of every phase; a zone where he dies anyway ends "died" and its
//          frames after the death are left out of the statistics. A zone
//          the game itself leaves ends "moved"; report.txt then names the
//          request (scene_change_in_zone: exit, caller, speed). A restarted
//          test keeps its elapsed time (tour-state.txt elapsed=, started_ms=;
//          the wall clock counts at most 10 minutes past the last zone
//          start, so a frozen or switched-off console adds no hours).
//          When the last zone is done the bottom screen shows TESTS
//          FINISHED and the game stands still. A button or a touch keeps the
//          console on; after 15 minutes without one the log is written and
//          the console powers off (it is left running overnight).
//   EXIT   leaves the game (writes perf-summary.csv and frames.csv)
// L+R+A takes the complete RAM + VRAM + LCD dump at once (one packed file).
struct ZoneRoom {std::uint8_t room;std::int16_t point;};
// primary: index (within the stage) of the room taken when one room stands for the stage.
struct ZoneStage {const char* stage;const char* label;std::uint16_t firstRoom;std::uint8_t roomCount,primary,mainTour;};
#include "zone_table.inc"

// Sampled by the game loop once per completed frame.
struct DevGameStatus {
    std::uint64_t submitTick{};
    std::uint64_t frame{};
    const char* stage{""};
    int room{-1},point{},layer{};
    bool playScene{},playerReady{},stageChanging{};
    // A zone change is held behind the curtain (runtime/scene_transition.hpp).
    bool transitionHeld{};
    bool submittedTransition{}; // state at submission, before the curtain can release
    float loopMs{},execMs{},paintMs{},updateMs{},gpuWaitMs{};
    // Original logic ticks this picture carried (real-speed scheduling).
    unsigned ticks{1};
    std::uint32_t draws{};
    std::uint64_t rejected{},degraded{};
    std::uint32_t textureBytes{},geometryBytes{},gameHeapFree{},linearFree{};
    float position[3]{};
    // Link's life in quarter hearts (-1 without a player); 0 means he died.
    int life{-1};
    // The game is paused by one of its menus (collection, map, item wheel).
    bool menuOpen{};
    // Scene change requests the game made itself (rising edges of the next
    // stage; test, console and boot-stage travels not counted) and a
    // description of the latest request: stages, who, exit, caller, speed.
    std::uint32_t sceneChanges{};
    const char* sceneChange{""};
    // Populated only during the story benchmark.
    story::Change storyChange{};
    story::PortalWarpSample portalWarp{};
    story::ChapterEvidence chapterEvidence{};
    story::ChapterControl chapterControl{};
    std::uint32_t chapterScene{};
    bool wolf{}, cutsceneCamera{}, midnaVisible{}, lanternLit{}, portalOpen{}, eventRunning{}, dialogueActive{};
    unsigned cutsceneFrame{}, enemyCount{}, damagedEnemies{};
    unsigned portalLockCount{};
    bool portalChargeActive{};
    std::uint32_t playSceneId{};
    float cameraForwardX{}, cameraForwardZ{}, enemyX{}, enemyZ{}, enemyDistance{};
    bool attacked{};
};
// What the automatic test asks of the second-core worker: off (the onecore
// phase: every draw on the game thread), as in play (it may rest where it
// draws too little, gx_worker_ctru.cpp), or on for a measurement (the still
// phase: a rest in progress ends and none starts, so that onecore minus
// still is the worker's effect at that spot; a rest during still made the
// two phases measure the same thing in the first Alpha 10 gate).
enum class SecondCoreTest:std::uint8_t {off,play,measured};
struct DevGameHooks {
    // Requests the original stage change (form 0 human, 1 wolf). False when
    // the current scene cannot accept it yet; the caller retries. The test
    // always plays on an empty save with its own equipment; a manual trip
    // keeps the save the player loaded.
    bool (*warp)(const char* stage,int room,int point,int layer,int form,bool test){};
    // Appends renderer and memory counters as key=value lines.
    void (*report)(std::FILE*){};
    // Requests the complete application dump (same as L+R+A).
    void (*fullDump)(bool waitForInput){};
    bool (*fullDumpBusy)(){};
    // Records the completed picture before the harness advances. RAM only.
    void (*benchmarkCapture)(){};
    void (*tourVisualCapture)(TourVisualReason){};
    // Logs why a stage is not starting: disc state, scene overlap, and the
    // processes still being created.
    void (*stuck)(){};
    // One line describing the equipment and state the last test warp applied
    // (clothes, sword, shield, X/Y items, hearts, time of day).
    const char* (*loadout)(){};
    // Restores Link's life to the value the zone started with when it is
    // lower but above zero (between frames, main thread). Alpha 6: 12 of 93
    // random zones ended on the Game Over screen and measured it.
    void (*refillLife)(){};
    // The second-core worker during the automatic test, between two
    // pictures (SecondCoreTest). Does nothing when the worker is off for
    // the session.
    void (*secondCore)(SecondCoreTest state){};
};
// The zone the automatic test is entering: its form (0 human, 1 wolf) and,
// in the random test, the seed of its random state (0: the fixed test
// equipment). The warp and the direct boot into a resumed zone both use it.
// zone is the 1-based zone number (0 outside a test) and tour its folder:
// a full dump records both, because dump folders are numbered in order of
// capture, not by zone (a crashed zone leaves no dump and shifts the rest).
struct DevZoneLoadout {
    int form{};
    std::uint32_t seed{};
    unsigned zone{};
    const char* tour{""};
    story::Checkpoint checkpoint{};
};
struct DevStoryContext {
    const char* step{};
    const char* phase{};
    unsigned tick{};
    const story::Step* definition{};
};
DevStoryContext devConsoleStoryContext();
DevZoneLoadout devConsoleZoneLoadout();
struct DevZoneCaptureContext {
    DevZoneLoadout loadout;
    const char* phase{};
    const char* stage{};
    int room{}, point{}, layer{-1};
    DevStoryContext story;
    unsigned phasePicture{}; // zero-based, before the harness advances
};
DevZoneCaptureContext devConsoleZoneCaptureContext();
bool devConsoleTourVisualActive();
// Before the boot stage is read and before the log file is first opened.
void initializeDevConsole(const DevGameHooks&);
// A resumed or automatic test starts directly in its next zone.
bool devConsoleBootStage(BootStage& stage);
// Once per frame after hidScanInput.
void updateDevConsoleInput(std::uint32_t held,std::uint32_t down);
// True while the console or the test owns the pad: the game then receives
// only the virtual input (all zero outside a test).
bool devConsoleOwnsPad();
// The game's normal catch-up loop may read the pad more than once per picture.
void devConsolePadTick(unsigned offset, const DevGameStatus* chapterStatus = nullptr);
void devConsoleVirtualPad(std::uint32_t& keys,std::int16_t& circleX,std::int16_t& circleY,std::int16_t& cstickX,std::int16_t& cstickY);
bool devConsoleExitRequested();
// The automatic test ran its last zone: the finished screen is up. The game
// loop then only calls serviceDevConsoleHold once per display refresh (no
// logic, no drawing: the top screen keeps its last picture) until a button or
// a touch ends the hold, or its time (15 minutes; tour-options.txt hold=
// seconds, 0 none) runs out and the console is asked to power off. After a
// cancelled hold the caller resets its frame timing.
bool devConsoleHolding();
void serviceDevConsoleHold();
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
