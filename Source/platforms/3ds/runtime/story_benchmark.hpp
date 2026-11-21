#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace dusk::native::story {

enum class Checkpoint : std::uint8_t {
    none, opening, ordon, faron, faronCleared, eldinTwilight, eldinCleared, lanayruTwilight, lanayruCleared,
    ranch, prison, sewers, forestTemple, forestBoomerang, kakariko, mines, minesBow,
    zoraIce, lanayruSpirit, escort, lakebed, lakebedClawshot, desperateHour, zeldaReunion, grove, desert,
    arbiter, arbiterSpinner, mirrorShard, snowpeak, snowpeakBall, timeTemple,
    timeRod, hiddenVillage, sky, skyClawshots, mirrorComplete, palace,
    palaceLight, castle, ganonHorse, ganonDuel, lanayruWestOpened, bulblinCampKey, skyAeralfos
};
enum class Script : std::uint8_t { walk, cutscene, equipment, route, fight, portal, warp };
enum class ChapterGoal : std::uint8_t { legacy, movement, exit, ability, event, bossIntro, hit, duel };
enum class IntroInput : std::uint8_t { standard, dangoro, dekuToad, walkToTrigger, kingBulblin, argorok, phantomZant, aeralfos };
enum class Ability : std::uint8_t {
    none, horse, wolfSense, boomerang, ironBoots, bow, zoraSwim, clawshot, spinner,
    ironBall, dominionRod, doubleClawshots, lightSword
};
struct Chapter {
    ChapterGoal goal{};
    const char* subject{}; // Disc actor name for combat or REVT name for a cinematic.
    Ability ability{};
    IntroInput introInput{};
    int introEndRoom{-1}; // An arena loaded by the same play scene during the intro.
};
enum Phase : std::uint8_t { loading, play, arrival, evidence };
inline constexpr const char* phaseNames[]{"loading", "play", "arrival", "evidence"};

struct Place {
    const char* stage{};
    int room{-1}, point{-1}, layer{-1};
};
struct RoutePoint { float x, z; };
// Field0.arc/dat/portal.dat entry 7, and the original player warp scene request.
inline constexpr int castleWestPortal = 7, castleWestSwitch = 3, castleWestWarpSpawn = 254;
inline constexpr int playerWarpPoint = -4;
struct Step {
    const char* id;
    Checkpoint checkpoint;
    Place start;
    bool wolf;
    Script script;
    unsigned ticks;
    float x{}, z{};
    Place destination{};
    int exit{-1};
    Chapter chapter{};
    std::optional<RoutePoint> via{};
    int warpPortal{-1}, warpSpawn{-1};
};

inline bool isDarknutFight(const Step& step) {
    return step.chapter.goal == ChapterGoal::hit && step.chapter.subject &&
           std::string_view(step.chapter.subject) == "B_tn";
}

inline bool usesTickChapterPad(const Step& step) {
    return step.chapter.introInput == IntroInput::argorok || isDarknutFight(step);
}

// Checkpoints follow the quest order. Target positions for the two short
// gate reversals and the spring cinematic come from the recorded playthrough.
// Spawn points and exit indices are the original room records.
inline constexpr Step earlySteps[]{
    {"ordon-spring-opening", Checkpoint::opening, {"F_SP104",1,24,13}, false, Script::cutscene, 1500,
        0,0,{"F_SP103",1,30,9},15},
    {"ordon-village", Checkpoint::ordon, {"F_SP103",0,22,-1}, false, Script::walk, 85},
    {"ordon-woods-gate", Checkpoint::ordon, {"F_SP103",1,2,-1}, false, Script::route, 75,
        -1660,-4240,{"F_SP104",1,0,-1},1},
    {"faron-lantern", Checkpoint::faron, {"F_SP108",4,9,-1}, false, Script::equipment, 100},
    {"faron-field", Checkpoint::faronCleared, {"F_SP121",6,11,0}, false, Script::walk, 86},
    {"faron-field-return", Checkpoint::faronCleared, {"F_SP121",6,0,-1}, false, Script::route, 100,
        -42900,107650,{"F_SP108",4,8,-1},0},
    // The Twilight entrance event requests the destination directly on layer 14.
    {"eldin-twilight-entry", Checkpoint::eldinTwilight, {"F_SP121",1,22,0}, false, Script::route, 125,
        -23450,73050,{"F_SP121",2,10,14},-1},
    {"kakariko-gorge-midna", Checkpoint::eldinTwilight, {"F_SP121",3,2,-1}, true, Script::fight, 340,
        -9700,58550},
    {"eldin-field", Checkpoint::eldinCleared, {"F_SP121",0,15,0}, false, Script::walk, 82},
    {"lanayru-field", Checkpoint::lanayruTwilight, {"F_SP121",10,3,-1}, true, Script::walk, 80},
    {"castle-west-portal", Checkpoint::lanayruTwilight, {"F_SP122",8,6,-1}, true, Script::portal, 900,
        -70000,10500},
    {"castle-town-west-gate", Checkpoint::lanayruCleared, {"F_SP116",2,0,0}, false, Script::route, 90,
        -6575,-264,{"F_SP122",8,1,-1},1},
    // These spawns face into town beside their return triggers. Run up each
    // street before turning back through the authored exit volume.
    {"castle-town-square", Checkpoint::lanayruCleared, {"F_SP116",0,4,-1}, false, Script::route, 105,
        -3250,-8,{"F_SP116",2,2,-1},1,{},RoutePoint{-2260,-8}},
    {"castle-town-south-road", Checkpoint::lanayruCleared, {"F_SP116",3,2,-1}, false, Script::route, 105,
        0,3270,{"F_SP116",0,5,-1},1,{},RoutePoint{-5,4160}},
};
#include "story_chapter_steps.inc"
inline constexpr unsigned stepCount = steps.size();
inline constexpr unsigned planVersion = 6;
inline constexpr std::uint32_t seed = 0x53544f52; // STOR

inline bool sameArea(const Place& expected, const Place& actual) {
    return expected.stage && actual.stage && expected.stage[0] && actual.stage[0] && expected.room == actual.room &&
        std::string_view(expected.stage) == actual.stage;
}
inline bool inIntroEndRoom(const Step& step, const Place& actual) {
    return step.chapter.introEndRoom >= 0 && sameArea({step.start.stage,step.chapter.introEndRoom},actual);
}
inline bool matches(const Place& expected, const Place& actual) {
    return sameArea(expected, actual) && (expected.point == -1 || expected.point == actual.point)
        && (expected.layer < 0 || expected.layer == actual.layer);
}

struct Change {
    char fromStage[9]{}, toStage[9]{};
    int fromRoom{-1}, toRoom{-1}, point{-1}, layer{-1}, exit{-1};
    int warpPortal{-1}, warpSpawn{-1};
};
inline bool matchesExit(const Step& step, const Change& change) {
    const Place from{change.fromStage, change.fromRoom};
    const Place to{change.toStage, change.toRoom, change.point, change.layer};
    if (step.script == Script::warp)
        return step.warpPortal >= 0 && step.warpSpawn >= 0 &&
            change.point == playerWarpPoint && change.warpPortal == step.warpPortal
            && change.warpSpawn == step.warpSpawn && change.exit == -1
            && !sameArea(from, to) && sameArea(step.start, from) && matches(step.destination, to);
    const bool scriptedReload = step.chapter.goal == ChapterGoal::event && step.start.point >= 0
        && step.destination.point >= 0 && step.destination.point != step.start.point;
    return change.point >= 0 && (!sameArea(from, to) || scriptedReload) && sameArea(step.start, from)
        && matches(step.destination, to) && change.exit == step.exit;
}

inline bool matchesArrival(const Step& step, const Place& place, std::uint32_t originScene, std::uint32_t scene) {
    return originScene && scene && originScene != scene && matches(step.destination, place);
}

enum Fact : unsigned {
    origin = 1u << 0, rendered = 1u << 1, moved = 1u << 2, cutscene = 1u << 3,
    requested = 1u << 4, arrived = 1u << 5, equipment = 1u << 6, attacked = 1u << 7,
    enemies = 1u << 8, damage = 1u << 9, midna = 1u << 10, portal = 1u << 11,
    chapterAction = 1u << 12, playerWarp = 1u << 13
};
inline constexpr unsigned requirements(const Step& step) {
    constexpr unsigned base = origin | rendered;
    switch (step.chapter.goal) {
    case ChapterGoal::legacy: break;
    case ChapterGoal::movement: return base | moved;
    case ChapterGoal::exit: return base | moved | requested | arrived;
    case ChapterGoal::duel: return base | chapterAction | requested | arrived;
    default: return base | chapterAction | (step.destination.stage ? requested | arrived : 0u);
    }
    switch (step.script) {
    case Script::walk: return base | moved;
    case Script::route: return base | moved | requested | arrived;
    case Script::cutscene: return base | cutscene | requested | arrived;
    case Script::equipment: return base | equipment;
    case Script::fight: return base | midna | enemies | attacked | damage;
    case Script::portal: return base | midna | enemies | attacked | damage | cutscene | portal;
    case Script::warp: return base | midna | playerWarp | requested | arrived;
    }
    return base;
}
inline const char* outcome(const Step& step, unsigned facts, int life, std::uint64_t rejected) {
    if (life == 0) return "died";
    if (life < 0) return "player-unavailable";
    if (rejected) return "render-rejected";
    return (facts & requirements(step)) == requirements(step) ? "pass" : "outcome-missing";
}

// Standard Circle Pad range. Convert a world-space target into the normal
// camera-relative pad, leaving Link's collision and movement to the game.
inline void steer(float dx, float dz, float forwardX, float forwardZ, std::int16_t& x, std::int16_t& y) {
    const float distance = std::hypot(dx, dz);
    const float cameraLength = std::hypot(forwardX, forwardZ);
    if (!std::isfinite(distance) || distance < 1 || !std::isfinite(cameraLength) || cameraLength < 0.5f) return;
    const float scale = 150 / (distance * cameraLength);
    // Link adds half a turn to JUT's atan2(padX, -padY).
    x = std::int16_t(std::clamp((dz * forwardX - dx * forwardZ) * scale, -150.0f, 150.0f));
    y = std::int16_t(std::clamp((dx * forwardX + dz * forwardZ) * scale, -150.0f, 150.0f));
}
}
