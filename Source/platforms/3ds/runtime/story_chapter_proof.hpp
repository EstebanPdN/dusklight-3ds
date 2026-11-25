#pragma once

#include "story_benchmark.hpp"

namespace dusk::native::story {

inline const char* chapterGoalName(ChapterGoal goal) {
    switch (goal) {
    case ChapterGoal::movement: return "movement";
    case ChapterGoal::exit: return "exit";
    case ChapterGoal::ability: return "ability";
    case ChapterGoal::event: return "event";
    case ChapterGoal::bossIntro: return "boss-intro";
    case ChapterGoal::hit: return "hit";
    case ChapterGoal::duel: return "duel";
    default: return "legacy";
    }
}

enum class DarknutStance : std::uint8_t { none, entering, guarded, swinging, exposed };
enum class AeralfosInput : std::uint8_t { none, approach, draw, aim, boots, release, confirmControl };

struct AeralfosControl {
    AeralfosInput input{};
    std::int16_t yawError{}, pitchError{};
};

struct ChapterControl {
    bool bowTargeted{}, darknutJumpAttackPrompt{};
    float targetX{}, targetZ{}, targetDistance{};
    bool targetPresent{}, finishingPrompt{}, swordLock{};
    bool duelTargeted{}, duelSwordDrawn{}, duelBossWalking{}, duelClashPrompt{}, duelClashOpening{}, duelDown{};
    bool ironBootsOn{}, firstPerson{};
    struct {
        DarknutStance stance{};
        bool targeted{}, swordDrawn{};
    } darknut;
    AeralfosControl aeralfos;
};

struct ChapterSample {
    bool playerArrowDamage{};
    unsigned actor{}, timelineFrame{};
    int health{};
    bool actorPresent{}, targeted{}, attack{}, abilityActive{}, demoActive{}, eventRunning{},
        finishingBlow{}, duelEnding{};
    std::optional<std::uint8_t> acceptedSwordHits{};
    bool demoCanEnd{true};
};

struct ChapterEvidence {
    unsigned abilityTicks{}, demoTicks{}, timelineTicks{}, hits{};
    bool finishingBlow{}, duelEnding{}, demoEnded{};

    bool witnessed(ChapterGoal goal) const {
        switch (goal) {
        case ChapterGoal::ability: return abilityTicks >= 3;
        case ChapterGoal::event: return demoTicks >= 7 && timelineTicks >= 7;
        case ChapterGoal::bossIntro: return demoTicks >= 7;
        case ChapterGoal::hit: return hits != 0;
        case ChapterGoal::duel: return hits != 0 && finishingBlow && duelEnding;
        default: return false;
        }
    }
};

// Feed this only from the verified origin scene, once per original logic
// tick. A different actor, a disappearing actor, or an unarmed health delta
// cannot count as a hit. Finishing the duel also needs the game's own exit.
struct ChapterProof {
    ChapterEvidence evidence{};

    void record(const ChapterSample& sample) {
        if (sample.abilityActive) ++evidence.abilityTicks;
        // Actor-controlled cameras can briefly drop their command while the
        // event still owns the player. Wait for actual control to return.
        if (sample.demoCanEnd && !sample.demoActive && !sample.eventRunning && evidence.demoTicks >= 7)
            evidence.demoEnded = true;
        if (sample.demoActive && (!previous.actorPresent || sample.actor == previous.actor)) {
            evidence.demoTicks = std::max(evidence.demoTicks, ++demoRun);
            if (!previous.demoActive || sample.timelineFrame < previous.timelineFrame)
                timelineStart = sample.timelineFrame;
            evidence.timelineTicks = std::max(evidence.timelineTicks, sample.timelineFrame - timelineStart);
        } else {
            demoRun = sample.demoActive ? 1 : 0;
            timelineStart = sample.timelineFrame;
        }

        if (sample.actorPresent && previous.actorPresent && sample.actor == previous.actor) {
            if (sample.playerArrowDamage && sample.health < previous.health) {
                ++evidence.hits;
            } else if (sample.targeted || previous.targeted) {
                if (sample.acceptedSwordHits) {
                    // Darknut resets health during damage and can shed armor independently.
                    if (previous.acceptedSwordHits && *sample.acceptedSwordHits > *previous.acceptedSwordHits)
                        evidence.hits += *sample.acceptedSwordHits - *previous.acceptedSwordHits;
                } else if (!previous.acceptedSwordHits && attackTicks && sample.health < previous.health) {
                    ++evidence.hits;
                }
            }
        } else {
            attackTicks = 0;
        }
        if (sample.actorPresent && sample.targeted && sample.attack) attackTicks = 5;
        else if (attackTicks) --attackTicks;

        if (sample.actorPresent && sample.finishingBlow &&
            (sample.targeted || (previous.actorPresent && previous.actor == sample.actor && previous.targeted))) {
            finishingActor = sample.actor;
            evidence.finishingBlow = true;
        }
        if (evidence.finishingBlow && sample.actorPresent &&
            sample.actor == finishingActor && sample.duelEnding)
            evidence.duelEnding = true;
        previous = sample;
    }

private:
    ChapterSample previous{};
    unsigned attackTicks{}, demoRun{}, timelineStart{}, finishingActor{};
};

}
