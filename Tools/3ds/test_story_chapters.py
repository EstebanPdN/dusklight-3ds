#!/usr/bin/env python3
"""Host checks for the chapter itinerary, proof rules and result evaluator."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from analyze_story import chapter_witness, evaluate_step

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / 'Source/platforms/3ds/runtime'

CHECKS = r'''
#include "story_chapter_pad.hpp"
#include <cassert>
#include <string_view>

using namespace dusk::native;
using namespace dusk::native::story;

int main() {
    unsigned early = 0;
    for (const auto& step : steps) {
        // The camera and message tracks in demo18.stb advance for at least
        // 7,670 and 7,679 ticks respectively, plus time on dialogue.
        if (step.chapter.subject && std::string_view(step.chapter.subject) == "demo18")
            assert(step.ticks > 7679 && step.exit == 1 && (requirements(step) & arrived));
        if (std::string_view(step.id) == "palace-zant") {
            assert(step.start.room == 50 && inIntroEndRoom(step, {"D_MN08D",53}));
            assert(!inIntroEndRoom(step, {"D_MN08D",54}) && !inIntroEndRoom(step, {"D_MN08",53}));
        }
        if (step.chapter.introInput == IntroInput::kingBulblin) {
            assert(matches(step.start, {"F_SP118",1,1,0}) && step.checkpoint == Checkpoint::bulblinCampKey);
            assert(step.x > 482 && step.x < 3100 && step.z > 550 && step.z < 1500);
        }
        if (std::string_view(step.id) == "city-in-the-sky-aeralfos") {
            assert(matches(step.start, {"D_MN07B",51,0,0}) &&
                   step.checkpoint == Checkpoint::skyAeralfos);
            assert(step.chapter.introInput == IntroInput::aeralfos &&
                   (requirements(step) & chapterAction));

            DevGameStatus state{};
            std::uint32_t keys{};
            std::int16_t x{}, y{}, cstickY{};
            auto& claw = state.chapterControl.aeralfos;
            claw = {AeralfosInput::aim, 500, -1000};
            assert(chapterPad(step, state, 16, keys, x, y, cstickY) &&
                   keys == nativeX && x < 0 && y < 0);
            claw = {AeralfosInput::aim, 60, -120};
            keys = 0;
            assert(chapterPad(step, state, 17, keys, x, y, cstickY) &&
                   keys == 0 && x == 0 && y == 0);
            claw.input = AeralfosInput::boots;
            assert(chapterPad(step, state, 24, keys, x, y, cstickY) && keys == nativeY);
            keys = 0;
            claw.input = AeralfosInput::release;
            assert(chapterPad(step, state, 32, keys, x, y, cstickY) && keys == nativeA);
            keys = 0;
            state.chapterEvidence.demoTicks = 1;
            assert(chapterPad(step, state, 40, keys, x, y, cstickY) &&
                   keys == 0 && x == 0 && y == 0);
            claw.input = AeralfosInput::confirmControl;
            assert(chapterPad(step, state, 41, keys, x, y, cstickY) &&
                   keys == 0 && x > 0 && y == 0);
        }
        if (step.chapter.goal == ChapterGoal::legacy && step.script != Script::warp) {
            assert(std::string_view(step.id) == earlySteps[early++].id);
        }
        bool roomFound = false;
        for (const auto& stage : zoneStages) {
            if (std::string_view(step.start.stage) != stage.stage) continue;
            for (unsigned room = 0; room < stage.roomCount; ++room)
                roomFound |= zoneRooms[stage.firstRoom + room].room == step.start.room;
        }
        assert(roomFound);
    }
    assert(early == std::size(earlySteps));

    const auto& duel = steps.back();
    assert(std::string_view(duel.id) == "ganondorf-final-duel");
    assert((requirements(duel) & (requested | arrived | chapterAction)) ==
           (requested | arrived | chapterAction));
    assert(std::string_view(outcome(duel, origin | rendered | chapterAction, 12, 0)) == "outcome-missing");

    ChapterProof demo;
    ChapterSample animation{.timelineFrame = 180, .demoActive = true};
    for (int i = 0; i < 10; ++i) demo.record(animation);
    assert(!demo.evidence.witnessed(ChapterGoal::event));
    for (int i = 0; i < 7; ++i) {
        ++animation.timelineFrame;
        demo.record(animation);
    }
    assert(demo.evidence.witnessed(ChapterGoal::event));
    assert(demo.evidence.witnessed(ChapterGoal::bossIntro));
    assert(!demo.evidence.demoEnded);
    demo.record(ChapterSample{.eventRunning=true});
    assert(!demo.evidence.demoEnded);
    demo.record(ChapterSample{});
    assert(demo.evidence.demoEnded);

    ChapterProof phantomZant;
    ChapterSample opening{.actor = 7, .actorPresent = true, .demoActive = true,
                          .eventRunning = true, .demoCanEnd = false};
    for (int i = 0; i < 7; ++i) phantomZant.record(opening);
    assert(phantomZant.evidence.witnessed(ChapterGoal::bossIntro));
    phantomZant.record(ChapterSample{.actor = 7, .actorPresent = true, .demoCanEnd = false});
    assert(!phantomZant.evidence.demoEnded);
    phantomZant.record(opening);
    assert(!phantomZant.evidence.demoEnded);
    phantomZant.record(ChapterSample{.actor = 7, .actorPresent = true});
    assert(phantomZant.evidence.demoEnded);

    ChapterProof combat;
    ChapterProof archery;
    ChapterSample archer{.actor = 3, .health = 40, .actorPresent = true};
    archery.record(archer);
    archer.playerArrowDamage = true;
    archery.record(archer);
    assert(!archery.evidence.hits);
    archer.health = 0;
    archery.record(archer);
    archery.record(archer);
    assert(archery.evidence.hits == 1);
    ChapterSample enemy{.actor = 4, .health = 10, .actorPresent = true, .targeted = true};
    combat.record(enemy);
    enemy.health = 8;
    combat.record(enemy);
    assert(!combat.evidence.witnessed(ChapterGoal::hit));
    enemy.attack = true;
    combat.record(enemy);
    enemy.actor = 5;
    enemy.health = 5;
    enemy.attack = false;
    combat.record(enemy);
    assert(!combat.evidence.witnessed(ChapterGoal::hit));
    enemy.attack = true;
    combat.record(enemy);
    enemy.attack = false;
    enemy.health = 4;
    combat.record(enemy);
    assert(combat.evidence.witnessed(ChapterGoal::hit));
    assert(!combat.evidence.witnessed(ChapterGoal::duel));
    enemy.finishingBlow = true;
    combat.record(enemy);
    enemy.finishingBlow = false;
    enemy.actor = 6;
    enemy.duelEnding = true;
    combat.record(enemy);
    assert(!combat.evidence.witnessed(ChapterGoal::duel));
    enemy.actor = 5;
    combat.record(enemy);
    assert(combat.evidence.witnessed(ChapterGoal::duel));

    ChapterProof darknut;
    ChapterSample armored{.actor = 8, .health = 100, .actorPresent = true, .targeted = true,
        .attack = true, .acceptedSwordHits = 0};
    darknut.record(armored);
    armored.health = 90;
    darknut.record(armored); // A health change without an accepted sword hit gives no credit.
    assert(!darknut.evidence.witnessed(ChapterGoal::hit));
    ++armored.actor;
    armored.health = 100;
    armored.attack = false;
    armored.acceptedSwordHits = 5;
    darknut.record(armored); // A different Darknut can already have a nonzero count.
    assert(!darknut.evidence.witnessed(ChapterGoal::hit));
    armored.targeted = false;
    darknut.record(armored);
    armored.acceptedSwordHits = 6;
    darknut.record(armored);
    assert(!darknut.evidence.witnessed(ChapterGoal::hit));
    armored.targeted = true;
    armored.acceptedSwordHits = 7;
    darknut.record(armored); // Collision is accepted even after Link's cut animation clears.
    darknut.record(armored);
    assert(darknut.evidence.hits == 1);

    DevGameStatus status{};
    status.eventRunning = true;
    status.chapterControl.swordLock = true;
    std::uint32_t keys = 0;
    std::int16_t x = 0, y = 0, cstickY = 0;
    assert(chapterPad(duel, status, 20, keys, x, y, cstickY) && keys == nativeA);
    keys = 0;
    assert(chapterPad(duel, status, 21, keys, x, y, cstickY) && keys == 0);

    status = {};
    status.cameraForwardZ = 1.0f;
    auto& target = status.chapterControl;
    target.targetPresent = target.duelSwordDrawn = true;
    target.targetDistance = target.targetZ = 560.0f;
    assert(chapterPad(duel, status, 22, keys, x, y, cstickY) && keys == 0 && y > 0);
    x = y = 0;
    assert(chapterPad(duel, status, 24, keys, x, y, cstickY) && keys == nativeL);

    target.duelTargeted = true;
    target.duelBossWalking = true;
    keys = 0;
    x = y = 0;
    assert(chapterPad(duel, status, 24, keys, x, y, cstickY) && keys == nativeL && y > 0);
    target.duelBossWalking = false;

    target.duelClashPrompt = true;
    keys = 0;
    x = y = 0;
    assert(chapterPad(duel, status, 24, keys, x, y, cstickY));
    assert(keys == (nativeL | nativeA) && x == 0 && y == 0);
    target.duelClashPrompt = false;

    target.targetDistance = target.targetZ = 140.0f;
    keys = 0;
    assert(chapterPad(duel, status, 25, keys, x, y, cstickY) && keys == (nativeL | nativeR) && y < 0);
    target.duelClashOpening = true;
    keys = 0;
    x = y = 0;
    assert(chapterPad(duel, status, 25, keys, x, y, cstickY) && keys == (nativeL | nativeB) && y > 0);
    target.finishingPrompt = true;
    keys = 0;
    x = y = 0;
    assert(chapterPad(duel, status, 26, keys, x, y, cstickY));
    assert(keys == (nativeL | nativeA) && x == 0 && y == 0);

    const auto dangoroStep = std::find_if(steps.begin(), steps.end(), [](const Step& step) {
        return std::string_view(step.id) == "goron-mines-dangoro";
    });
    assert(dangoroStep != steps.end() && dangoroStep != steps.begin());
    const auto& dangoro = *dangoroStep;
    const auto& boots = *(dangoroStep - 1);
    assert(boots.chapter.ability == Ability::ironBoots);
    const auto toadStep = std::find_if(steps.begin(), steps.end(), [](const Step& step) {
        return std::string_view(step.id) == "lakebed-deku-toad";
    });
    assert(toadStep != steps.end());
    const auto& toad = *toadStep;
    assert(toad.checkpoint == Checkpoint::lakebed && toad.start.point == 3);
    assert(dangoro.ticks == 420 && toad.ticks == 420);

    auto input = [&](const Step& step, unsigned tick) {
        keys = 0;
        x = y = cstickY = 0;
        assert(chapterPad(step, status, tick, keys, x, y, cstickY));
    };

    status = {};
    status.position[2] = 2797.0f;
    status.cameraForwardZ = -1.0f;
    status.chapterControl.ironBootsOn = true;
    input(dangoro, 8);
    assert(keys == nativeX && x == 0 && y == 0 && cstickY == 0);
    input(dangoro, 10);
    assert(keys == 0 && x == 0 && y == 0);
    status.chapterControl.ironBootsOn = false;
    input(dangoro, 16);
    assert(keys == 0 && y > 0 && cstickY == 0);
    status.eventRunning = true;
    status.dialogueActive = true;
    input(dangoro, 18);
    assert(keys == nativeA && x == 0 && y == 0);
    status = {};

    const auto morpheel = std::find_if(steps.begin(), steps.end(), [](const Step& step) {
        return std::string_view(step.id) == "lakebed-morpheel";
    });
    assert(morpheel != steps.end() && morpheel->start.point == 0);
    input(*morpheel, 8);
    assert(keys == nativeY && x == 0 && y == 0);
    status.chapterControl.ironBootsOn = true;
    input(*morpheel, 38);
    assert(keys == 0 && x == 0 && y == 0);
    status = {};

    input(toad, 128);
    assert(keys == 0 && x == 0 && y == 0 && cstickY == 0);
    status.chapterControl.targetPresent = true;
    input(toad, 128);
    assert(keys == 0 && x == 0 && y == 0 && cstickY > 0);
    input(toad, 130);
    assert(cstickY == 0);
    status.chapterControl.firstPerson = true;
    input(toad, 136);
    assert(keys == 0 && x == 0 && y < 0 && cstickY == 0);
    status.eventRunning = true;
    input(toad, 144);
    assert(keys == 0 && x == 0 && y == 0 && cstickY == 0);
    status.eventRunning = false;
    status.chapterEvidence.demoEnded = true;
    input(toad, 152);
    assert(keys == 0 && x == 0 && y == 0 && cstickY == 0);

    for (unsigned ordinal : {71u, 72u}) {
        const auto& step = steps[ordinal - 1];
        assert(step.chapter.goal == ChapterGoal::bossIntro);
        assert(step.chapter.subject == std::string_view("E_pz"));
        assert(step.chapter.introInput == IntroInput::phantomZant && step.start.point == 0);
        status = {};
        status.cameraForwardZ = -1.0f;
        status.chapterControl.targetPresent = true;
        status.chapterControl.targetDistance = 450.0f;
        input(step, 24);
        assert(keys == 0 && x == 0 && y > 0);
        status.chapterEvidence.demoTicks = 7;
        input(step, 24);
        assert(keys == 0 && x == 0 && y == 0);
    }

    const auto& argorok = steps[67];
    assert(argorok.chapter.introInput == IntroInput::argorok && usesTickChapterPad(argorok));
    status = {};
    status.cameraForwardZ = 1.0f;
    status.position[0] = -732.755f; // Tour 003's last full-speed sample.
    status.position[2] = 3142.199f;
    input(argorok, 40);
    assert(keys == 0 && x > 0 && y < 0 && std::hypot(x, y) < 80);
    status.position[0] = argorok.x + 15.0f;
    status.position[2] = argorok.z - 15.0f;
    input(argorok, 41);
    assert(keys == 0 && x == 0 && y == 0);
    status.position[2] -= 300;
    status.chapterEvidence.demoTicks = 7;
    input(argorok, 42);
    assert(keys == 0 && x == 0 && y == 0);

    const auto& portal = steps[25];
    assert(portal.script == Script::portal && !usesTickChapterPad(portal));
    auto bind = [&](unsigned alive, unsigned tagged) {
        keys = 0;
        x = y = 0;
        status.enemyCount = alive;
        status.portalLockCount = tagged;
        castleWestPad(portal, status, true, 24, keys, x, y);
    };
    status = {};
    status.cameraForwardZ = 1.0f;
    status.enemyX = status.position[0] = -70070.0f;
    status.position[2] = 10860.0f;
    status.enemyZ = status.position[2] + 860.0f; // Untagged beast outside the full dome.
    status.enemyDistance = 860.0f;
    status.portalChargeActive = true;
    bind(3, 1);
    assert(keys == 0 && x == 0 && y == 0);
    bind(3, 2);
    assert(keys == nativeB); // Releasing would leave one survivor to revive its group.
    bind(2, 1);
    assert(keys == nativeB && x == 0 && y > 0);
    bind(2, 2);
    assert(keys == 0 && x == 0 && y == 0);
    status.portalOpen = true;
    bind(2, 1);
    assert(keys == 0 && x == 0 && y == 0);

    unsigned darknutSteps = 0;
    for (const auto& step : steps) {
        if (std::string_view(step.id) != "temple-of-time-darknut" &&
            std::string_view(step.id) != "hyrule-castle-darknut") continue;
        ++darknutSteps;
        assert(usesTickChapterPad(step));
        assert(step.chapter.goal == ChapterGoal::hit && step.chapter.subject == std::string_view("B_tn"));
        status = {};
        status.cameraForwardZ = 1.0f;
        auto& foe = status.chapterControl;
        foe.targetPresent = true;
        foe.targetZ = foe.targetDistance = 160.0f;
        foe.darknut = {DarknutStance::guarded, true, true};
        input(step, 20);
        assert(keys == nativeL);
        assert(x < 0 && y > 0);
        foe.darknut.stance = DarknutStance::swinging;
        input(step, 20);
        assert(keys == (nativeL | nativeR) && x < 0);
        assert(y > 0);
        foe.darknut.stance = DarknutStance::exposed;
        foe.targetZ = foe.targetDistance = 145.0f;
        input(step, 20);
        assert(keys == (nativeL | nativeB) && y > 0);
        input(step, 21);
        assert(keys == nativeL); // Re-arm B on the next logic tick.
        foe.targetZ = foe.targetDistance = 220.0f;
        input(step, 25);
        assert(keys == nativeL && y > 0); // Close first, then swing.
        foe.darknutJumpAttackPrompt = true;
        input(step, 25);
        assert(keys == (nativeL | nativeA)); // Use the original contextual jump attack.
        foe.targetZ = foe.targetDistance = 165.0f;
        foe.darknut.targeted = false;
        input(step, 30);
        assert((keys & nativeB) == 0);
        foe.darknut.targeted = true;
        status.chapterEvidence.hits = 1;
        input(step, 30);
        assert(keys == 0 && x == 0 && y == 0);
        status.chapterEvidence.hits = 0;
        status.eventRunning = true;
        input(step, 20);
        assert(keys == 0 && x == 0 && y == 0);
    }
    assert(darknutSteps == 2);
}
'''


class StoryChapterTest(unittest.TestCase):
    def test_zant_intro_follows_the_same_scene_into_the_first_arena(self):
        plan = dict(chapter_goal='boss-intro', subject='B_zant', stage='D_MN08D',
                    full_cutscene='1', to_stage='', intro_end_room='53')
        receipt = dict(demo_ended='1', demo_ticks='856', final_stage='D_MN08D',
                       final_room='53', origin_scene='3', final_scene='3', arrival_pictures='3')
        self.assertTrue(chapter_witness(plan, receipt))
        for field, wrong in (('demo_ended','0'), ('final_room','50'), ('final_scene','4'),
                             ('arrival_pictures','2')):
            with self.subTest(field=field):
                self.assertFalse(chapter_witness(plan, {**receipt, field: wrong}))

    def test_itinerary_proof_and_chapter_input(self):
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / 'chapters.cpp'
            executable = cpp.with_suffix('')
            cpp.write_text(CHECKS)
            subprocess.run([
                'clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pedantic',
                '-DTARGET_PC=1', '-I', str(RUNTIME), '-I', str(ROOT / 'Source/extern/aurora/include'),
                str(cpp), '-o', str(executable),
            ], check=True)
            subprocess.run([executable], check=True)

    def test_receipt_binds_reloaded_room_to_origin_and_actual_demo(self):
        plan = {
            'step': 'sacred-grove-master-sword', 'required': 4147,
            'stage': 'F_SP117', 'room': 1, 'point': 99, 'layer': 0, 'form': 'human',
            'to_stage': 'F_SP117', 'to_room': 1, 'to_point': 150, 'to_layer': 0,
            'exit': 0, 'chapter_goal': 'event', 'subject': 'demo22_02',
        }
        receipt = {
            **plan, 'result': 'pass', 'facts': 4147, 'ticks': 123, 'pictures': 30,
            'life': 12, 'rejected': 0, 'movement': 0, 'enemies': 0, 'damage_samples': 0,
            'cutscene_first': 0, 'cutscene_last': 20, 'lantern_pictures': 0,
            'attack_pictures': 0, 'midna_pictures': 0, 'portal_activated': 0,
            'origin_scene': 4, 'origin_stage': 'F_SP117', 'origin_room': 1,
            'origin_point': 99, 'origin_layer': 0, 'origin_form': 'human',
            'from_stage': 'F_SP117', 'from_room': 1, 'final_stage': 'F_SP117', 'final_room': 1,
            'final_scene': 5, 'final_point': 150, 'final_layer': 0, 'arrival_pictures': 3,
            'demo_ticks': 20, 'timeline_ticks': 19,
        }
        self.assertEqual(evaluate_step(plan, receipt), 'pass')
        for change, reason in [
            ({'origin_form': 'wolf'}, 'checkpoint'),
            ({'origin_point': 150}, 'checkpoint'),
            ({'origin_layer': 2}, 'checkpoint'),
            ({'timeline_ticks': 0}, 'chapter-action'),
            ({'final_scene': 4}, 'arrival'),
            ({'to_point': 99}, 'exit-request'),
        ]:
            with self.subTest(change=change):
                self.assertIn(reason, evaluate_step(plan, receipt | change))


if __name__ == '__main__':
    unittest.main()
