#pragma once

#include "dev_console.hpp"
#include "pad_mapping.hpp"

namespace dusk::native::story {

inline void castleWestPad(const Step& step, const DevGameStatus& state, bool afterIntro,
    unsigned tick, std::uint32_t& keys, std::int16_t& x, std::int16_t& y) {
    if (state.portalOpen) return;
    const unsigned alive = state.enemyCount, tagged = state.portalLockCount;
    // One survivor can call its fallen group back. Take the first beast
    // alone, then keep B held until the remaining pair is bound.
    if (state.portalChargeActive && tagged &&
        (tagged >= alive || (alive == 3 && tagged == 1))) return;

    float gx = step.x, gz = step.z;
    if (afterIntro) {
        const bool followBeast = alive == 3 || (alive == 2 && tagged == 1);
        if (followBeast) {
            gx = state.enemyX;
            gz = state.enemyZ;
        } else {
            // Intersection of the original beasts' 1,200-unit chase radii.
            gx = -70070.0f;
            gz = 10860.0f;
        }
        if (alive && (state.portalChargeActive || (state.enemyDistance < 1100.0f && tick % 16 < 8)))
            keys |= nativeB;
        if (followBeast && state.enemyDistance < 350.0f) return;
    }
    const float dx = gx - state.position[0], dz = gz - state.position[2];
    const float distance = std::hypot(dx, dz);
    if (distance > 18.0f) {
        steer(dx, dz, state.cameraForwardX, state.cameraForwardZ, x, y);
        if (afterIntro && distance < 120.0f) { x /= 3; y /= 3; }
    }
}

inline void duelPad(const DevGameStatus& last, unsigned tick, std::uint32_t& keys,
    std::int16_t& x, std::int16_t& y) {
    const auto& target = last.chapterControl;
    if (target.swordLock) {
        if (tick % 2 == 0) keys |= nativeA;
        return;
    }
    if (last.eventRunning || !target.targetPresent) return;

    const float dx = target.targetX - last.position[0];
    const float dz = target.targetZ - last.position[2];
    const float distance = target.targetDistance;
    // His airborne attacks clear the lock. Release L briefly until it catches again.
    if (distance < 800.0f && (target.duelTargeted || tick % 8 < 6)) keys |= nativeL;
    if (target.finishingPrompt || target.duelClashPrompt) {
        if (tick % 2 == 0) keys |= nativeA;
        return;
    }
    if (!target.duelSwordDrawn && tick % 12 == 0) keys |= nativeB;
    if (target.duelClashOpening || target.duelDown) {
        if (distance > 115.0f) steer(dx, dz, last.cameraForwardX, last.cameraForwardZ, x, y);
        if (target.duelClashOpening && distance < 180.0f && tick % 5 == 0) keys |= nativeB;
        return;
    }

    // Close enough to make him stop walking, then back off into his charge range.
    const float near = target.duelBossWalking ? 445.0f : 555.0f;
    const float far = target.duelBossWalking ? 485.0f : 600.0f;
    if (!target.duelTargeted || distance > far)
        steer(dx, dz, last.cameraForwardX, last.cameraForwardZ, x, y);
    else if (distance < near)
        steer(-dx, -dz, last.cameraForwardX, last.cameraForwardZ, x, y);
    if (distance < 475.0f) keys |= nativeR;
}

inline void darknutPad(const Step& step, const DevGameStatus& state, unsigned tick,
    std::uint32_t& keys, std::int16_t& x, std::int16_t& y) {
    if (state.chapterEvidence.hits) return;
    const auto& target = state.chapterControl;
    const auto& darknut = target.darknut;
    const float distance = target.targetDistance;
    const auto approach = [&] {
        steer((target.targetPresent ? target.targetX : step.x) - state.position[0],
            (target.targetPresent ? target.targetZ : step.z) - state.position[2],
            state.cameraForwardX, state.cameraForwardZ, x, y);
    };
    if (!target.targetPresent || darknut.stance == DarknutStance::entering) {
        approach();
        return;
    }
    if (distance < 750.0f && (darknut.targeted || tick % 8 < 6)) keys |= nativeL;
    if (!darknut.swordDrawn) {
        if (distance < 410.0f && tick % 5 == 0) keys |= nativeB;
    }
    const float dx = target.targetX - state.position[0];
    const float dz = target.targetZ - state.position[2];
    if (darknut.stance == DarknutStance::swinging && distance < 350.0f) {
        keys |= nativeR;
    }
    if (darknut.stance == DarknutStance::exposed && darknut.targeted) {
        if (distance > 120.0f) approach();
        if (tick % 5 == 0) {
            if (distance < 155.0f) keys |= nativeB;
            else if (distance > 200.0f && distance < 285.0f && darknut.swordDrawn &&
                     target.darknutJumpAttackPrompt) keys |= nativeA;
        }
    } else if (distance > 280.0f || !darknut.targeted) {
        approach();
    } else {
        // Keep advancing around the shield while it is up. A frontal stop
        // at about 220 units leaves Link outside the normal sword swing.
        const float inward = distance > 135.0f ? 0.85f : 0.0f;
        steer(dz + inward * dx, -dx + inward * dz,
            state.cameraForwardX, state.cameraForwardZ, x, y);
    }
}

inline bool chapterPad(const Step& step, const DevGameStatus& last, unsigned tick,
    std::uint32_t& keys, std::int16_t& x, std::int16_t& y, std::int16_t& cstickY) {
    const auto goal = step.chapter.goal;
    if (goal <= ChapterGoal::exit) return false;

    if (goal == ChapterGoal::duel) {
        duelPad(last, tick, keys, x, y);
        return true;
    }
    if (last.eventRunning || goal == ChapterGoal::event) {
        if (last.dialogueActive && tick % 18 == 0) keys |= nativeA;
        return true;
    }
    if (isDarknutFight(step)) {
        darknutPad(step, last, tick, keys, x, y);
        return true;
    }
    if (goal == ChapterGoal::ability) {
        const unsigned cycle = tick % 65;
        switch (step.chapter.ability) {
        case Ability::horse:
            y = 125;
            if (tick % 50 == 15) keys |= nativeA;
            break;
        case Ability::zoraSwim:
            y = 110;
            if (tick % 20 == 9) keys |= nativeA;
            break;
        case Ability::lightSword:
            if (tick % 12 == 8) keys |= nativeB;
            break;
        case Ability::wolfSense:
            if (cycle == 8) keys |= nativeX;
            break;
        case Ability::ironBoots:
        case Ability::spinner:
            if (cycle == 8) keys |= nativeX;
            if (tick > 24) y = 85;
            break;
        default:
            // Pull out, aim and release the equipped item using its button.
            if ((cycle >= 8 && cycle < 26) || (cycle >= 36 && cycle < 52)) keys |= nativeX;
            break;
        }
        return true;
    }

    const auto& target = last.chapterControl;
    if (step.chapter.introInput == IntroInput::aeralfos) {
        const auto& claw = target.aeralfos;
        if (last.chapterEvidence.demoEnded) return true;
        if (last.chapterEvidence.demoTicks && claw.input != AeralfosInput::confirmControl) return true;
        switch (claw.input) {
        case AeralfosInput::approach:
            steer(step.x - last.position[0], step.z - last.position[2],
                last.cameraForwardX, last.cameraForwardZ, x, y);
            break;
        case AeralfosInput::draw:
            keys |= nativeX;
            break;
        case AeralfosInput::aim: {
            // Releasing X shoots. The first-person pad uses the opposite
            // sign for yaw and the same sign for pitch.
            const auto aim = [](int error) -> std::int16_t {
                return std::abs(error) > 320 ? (error > 0 ? 100 : -100) : 0;
            };
            x = aim(-claw.yawError);
            y = aim(claw.pitchError);
            if (x || y) keys |= nativeX;
            break;
        }
        case AeralfosInput::boots:
            if (tick % 8 == 0) keys |= nativeY;
            break;
        case AeralfosInput::release:
            if (tick % 8 == 0) keys |= nativeA;
            break;
        case AeralfosInput::confirmControl:
            x = 90;
            break;
        case AeralfosInput::none:
            break;
        }
        return true;
    }
    if (step.chapter.introInput == IntroInput::dangoro) {
        if (last.chapterEvidence.demoEnded) return true;
        if (target.ironBootsOn) {
            // X toggles the Iron Boots equipped by the mines checkpoint.
            if (tick % 8 < 2) keys |= nativeX;
        } else {
            steer(step.x - last.position[0], step.z - last.position[2],
                last.cameraForwardX, last.cameraForwardZ, x, y);
        }
        return true;
    }
    if (step.chapter.introInput == IntroInput::phantomZant) {
        if (!last.chapterEvidence.demoTicks)
            steer(step.x - last.position[0], step.z - last.position[2],
                last.cameraForwardX, last.cameraForwardZ, x, y);
        return true;
    }
    if (step.chapter.introInput == IntroInput::kingBulblin) {
        // Approach from the key guard's street. The boar fences across
        // the central lane block Link on foot.
        const float px = last.position[0], pz = last.position[2];
        float wx = step.x, wz = step.z;
        if (pz < -1100.0f && px > 3550.0f) {
            wx = 4470.0f; wz = -1000.0f;
        } else if (px > 1950.0f && pz < -450.0f) {
            wx = 1800.0f; wz = -850.0f;
        }
        if (std::abs(px - 1800.0f) < 180.0f && pz > -650.0f && pz < -450.0f && tick % 12 == 0)
            keys |= nativeA;
        steer(wx - px, wz - pz, last.cameraForwardX, last.cameraForwardZ, x, y);
        return true;
    }
    if (step.chapter.introInput == IntroInput::argorok) {
        const float dx = step.x - last.position[0];
        const float dz = step.z - last.position[2];
        const float distance = std::hypot(dx, dz);
        // Stop inside L7ODR's trigger while its event request is pending.
        if (!last.chapterEvidence.demoTicks && distance > 35.0f) {
            steer(dx, dz, last.cameraForwardX, last.cameraForwardZ, x, y);
            const float pace = std::clamp(distance / 220.0f, 0.45f, 1.0f);
            x = std::int16_t(x * pace);
            y = std::int16_t(y * pace);
        }
        return true;
    }
    if (step.chapter.introInput == IntroInput::walkToTrigger) {
        steer(step.x - last.position[0], step.z - last.position[2],
            last.cameraForwardX, last.cameraForwardZ, x, y);
        return true;
    }
    if (step.chapter.introInput == IntroInput::dekuToad) {
        if (!target.targetPresent || last.chapterEvidence.demoEnded) return true;
        if (target.firstPerson) {
            y = -150; // The original subject camera pitches up on Circle Pad down.
        } else if (tick % 8 < 2) {
            cstickY = 150; // Enter the original subject camera with C-stick up.
        }
        return true;
    }
    if (goal == ChapterGoal::bossIntro && step.chapter.ability == Ability::zoraSwim) {
        // The authored entrance is directly above Morpheel. Swim-forward
        // takes Link out of the 1,600-unit sphere while he descends.
        if (!target.ironBootsOn && tick % 30 == 8) keys |= nativeY;
        if (target.targetPresent && target.targetDistance > 200.0f)
            steer(target.targetX - last.position[0], target.targetZ - last.position[2],
                last.cameraForwardX, last.cameraForwardZ, x, y);
        return true;
    }

    const bool aimAtActor = goal != ChapterGoal::bossIntro && target.targetPresent;
    const float dx = (aimAtActor ? target.targetX : step.x) - last.position[0];
    const float dz = (aimAtActor ? target.targetZ : step.z) - last.position[2];
    if (goal == ChapterGoal::hit && step.chapter.ability == Ability::bow) {
        // Follow the approach to the main street before drawing the bow.
        if (last.position[2] < step.z) {
            steer(step.x - last.position[0], step.z - last.position[2],
                last.cameraForwardX, last.cameraForwardZ, x, y);
        } else if (target.targetPresent) {
            if (!target.bowTargeted || target.targetDistance > 450.0f)
                steer(dx, dz, last.cameraForwardX, last.cameraForwardZ, x, y);
            if (target.targetDistance < 800.0f) {
                if (target.bowTargeted || tick % 8 < 6) keys |= nativeL;
                if (target.bowTargeted && tick % 32 < 18) keys |= nativeX;
            }
        }
        return true;
    }
    if (goal == ChapterGoal::hit && std::string_view(step.chapter.subject) == "E_rd") {
        // Close to sword range on the same street. Circling at 235 units
        // leaves Link swinging beside the archers at the village windows.
        if (!target.targetPresent || target.targetDistance > 85.0f)
            steer(dx, dz, last.cameraForwardX, last.cameraForwardZ, x, y);
        if (target.targetPresent && target.targetDistance < 650.0f) keys |= nativeL;
        if (target.targetPresent && target.targetDistance < 160.0f && tick % 7 == 0) keys |= nativeB;
        return true;
    }
    if (goal == ChapterGoal::bossIntro || !target.targetPresent || target.targetDistance > 235.0f)
        steer(dx, dz, last.cameraForwardX, last.cameraForwardZ, x, y);
    if (target.targetPresent && target.targetDistance < 800.0f) keys |= nativeL;
    if (goal == ChapterGoal::bossIntro && target.targetPresent && target.targetDistance < 750.0f &&
        tick % 16 == 8)
        keys |= nativeB;
    if (goal != ChapterGoal::bossIntro && target.targetPresent && target.targetDistance < 375.0f) {
        // Keep the target under lock and circle when it defends.
        if (!x && !y) x = (tick % 160 < 80) ? 80 : -80;
        if (tick % 11 == 0) keys |= nativeB;
    }
    return true;
}

}
