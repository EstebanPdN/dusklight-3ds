#!/usr/bin/env python3
"""Host regression tests for the production story route rules and receipt evaluator."""
import csv
from pathlib import Path
import subprocess
import tempfile
import unittest

from analyze_story import evaluate, evaluate_step

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / 'Source/platforms/3ds/runtime'

ROUTES = r'''
#include "story_benchmark.hpp"
#include "story_portal.hpp"
#include <cassert>
#include <cstring>
#include <set>
using namespace dusk::native::story;

int main() {
    std::set<std::string_view> ids;
    auto previous = Checkpoint::none;
    unsigned transitions = 0;
    for (const auto& step : earlySteps) {
        assert(step.checkpoint >= previous && step.checkpoint != Checkpoint::none);
        previous = step.checkpoint;
    }
    for (const auto& step : steps) {
        assert(step.checkpoint != Checkpoint::none);
        assert(ids.insert(step.id).second && step.start.point >= 0 && step.ticks > 0);
        if (!step.destination.stage) continue;
        ++transitions;
        Change change{};
        std::strcpy(change.fromStage, step.start.stage);
        std::strcpy(change.toStage, step.destination.stage);
        change.fromRoom = step.start.room;
        change.toRoom = step.destination.room;
        change.point = step.destination.point;
        change.layer = step.destination.layer;
        change.exit = step.exit;
        change.warpPortal = step.warpPortal;
        change.warpSpawn = step.warpSpawn;
        assert(matchesExit(step, change));
        auto fall = change;
        std::strcpy(fall.toStage, fall.fromStage);
        fall.toRoom = fall.fromRoom;
        fall.point = -1;
        assert(!matchesExit(step, fall));
        auto wrongExit = change;
        ++wrongExit.exit;
        assert(!matchesExit(step, wrongExit));
        auto wrongPoint = change;
        ++wrongPoint.point;
        assert(!matchesExit(step, wrongPoint));
        if (step.script == Script::warp) {
            auto checkpointLoad = change;
            checkpointLoad.point = step.warpSpawn;
            assert(!matchesExit(step, checkpointLoad));
            auto wrongPortal = change;
            ++wrongPortal.warpPortal;
            assert(!matchesExit(step, wrongPortal));
            auto wrongSpawn = change;
            ++wrongSpawn.warpSpawn;
            assert(!matchesExit(step, wrongSpawn));
            assert(!matchesArrival(step, {step.destination.stage, step.destination.room, step.warpSpawn}, 5, 6));
        }
        auto unrelated = change;
        ++unrelated.fromRoom;
        assert(!matchesExit(step, unrelated));
        assert(!matchesArrival(step, step.destination, 5, 5));
        assert(!matchesArrival(step, step.destination, 0, 5));
        assert(matchesArrival(step, step.destination, 5, 6));
        assert(!matchesArrival(step, step.start, 5, 6));
        assert(std::string_view(outcome(step, requirements(step) & ~arrived, 12, 0)) == "outcome-missing");
    }
    assert(transitions >= 6);

    const auto& eldin = steps[15];
    assert(std::string_view(eldin.id) == "eldin-twilight-entry");
    assert(requirements(eldin) == (origin | rendered | moved | requested | arrived));
    assert(matchesExit(eldin, Change{"F_SP121", "F_SP121", 1, 2, 10, 14, -1}));
    assert(!matchesExit(eldin, Change{"F_SP121", "F_SP121", 1, 2, 10, 0, -1}));
    assert(!matchesExit(eldin, Change{"F_SP121", "F_SP121", 1, 2, 10, 14, 0}));

    for (const auto& step : steps) {
        const auto required = requirements(step);
        assert(std::string_view(outcome(step, required, 12, 0)) == "pass");
        assert(std::string_view(outcome(step, required, 0, 0)) == "died");
        assert(std::string_view(outcome(step, required, -1, 0)) == "player-unavailable");
        assert(std::string_view(outcome(step, required, 12, 1)) == "render-rejected");
        for (unsigned bit = 1; bit <= playerWarp; bit <<= 1)
            if (required & bit)
                assert(std::string_view(outcome(step, required & ~bit, 12, 0)) == "outcome-missing");
    }
    std::int16_t x = 0, y = 0;
    steer(0, 100, 0, 200, x, y);
    assert(x == 0 && y == 150);
    steer(0, 100, 100, 0, x, y);
    assert(x == 150 && y == 0);
    steer(100, 0, 0, -200, x, y);
    assert(x == 150 && y == 0);
    PortalWarpProof warp;
    assert(!warp.witnessed());
    for (int i = 0; i < 4; ++i) warp.record({.departing=true});
    warp.record({.confirmed=true});
    for (int i = 0; i < 2; ++i) warp.record({.departing=true});
    assert(!warp.witnessed());
    warp.record({.departing=true});
    assert(warp.witnessed());
}
'''

# A story route from the random-tour evidence. Legacy "moved" and the
# request alone are insufficient. Actual matched arrival has its own witness.
PLAN = {
    'zone': 11, 'step': 'castle-town-west-gate', 'required': 55,
    'stage': 'F_SP116', 'room': 2, 'to_stage': 'F_SP122', 'to_room': 8,
    'to_point': 1, 'to_layer': -1, 'exit': 1,
}
RECEIPT = {
    **PLAN, 'result': 'pass', 'facts': 55, 'ticks': 40, 'pictures': 25,
    'movement': 268, 'cutscene_first': 0, 'cutscene_last': 0, 'enemies': 0,
    'damage_samples': 0, 'midna_pictures': 0, 'lantern_pictures': 0, 'attack_pictures': 0,
    'portal_activated': 0, 'life': 12, 'rejected': 0, 'from_stage': 'F_SP116', 'from_room': 2,
    'final_stage': 'F_SP122', 'final_room': 8, 'final_point': 1, 'final_layer': -1,
    'origin_scene': 21, 'final_scene': 24, 'arrival_pictures': 3,
}


def write_csv(path, sample, entries):
    with path.open('w', newline='') as file:
        writer = csv.DictWriter(file, sample)
        writer.writeheader()
        writer.writerows(entries)


class StoryBenchmarkTest(unittest.TestCase):
    def test_player_warp_requires_original_selection_departure_and_arrival(self):
        required = 3 | 16 | 32 | 1024 | 8192
        plan = {**PLAN, 'step': 'midna-warp-to-castle-west', 'stage': 'F_SP121', 'room': 10,
                'to_point': -4, 'exit': -1, 'required': required, 'warp_portal': 7, 'warp_spawn': 254}
        receipt = {**RECEIPT, 'step': plan['step'], 'required': required, 'facts': required,
                   'from_stage': 'F_SP121', 'from_room': 10, 'exit': -1, 'midna_pictures': 7,
                   'to_point': -4, 'final_point': -4, 'warp_portal': 7, 'warp_spawn': 254,
                   'warp_confirmed': 1, 'warp_departure_pictures': 12}
        self.assertEqual(evaluate_step(plan, receipt), 'pass')
        for mutation in ({'to_point': 254, 'final_point': 254}, {'warp_portal': 8}, {'warp_spawn': 7},
                         {'warp_confirmed': 0}, {'warp_departure_pictures': 2}, {'arrival_pictures': 2},
                         {'final_point': 254}, {'final_scene': receipt['origin_scene']}):
            with self.subTest(mutation=mutation):
                self.assertNotEqual(evaluate_step(plan, {**receipt, **mutation}), 'pass')

    def test_cpp_outcome_and_camera_relative_controls(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source, executable = work / 'story.cpp', work / 'story'
            source.write_text(ROUTES)
            subprocess.run([
                'clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pedantic',
                '-I', str(RUNTIME), str(source), '-o', str(executable),
            ], check=True)
            subprocess.run([executable], check=True)

    def test_wrong_exit_reset_control_and_arrival_are_rejected(self):
        self.assertEqual(evaluate_step(PLAN, RECEIPT), 'pass')
        for change, reason in [
            ({'to_stage': 'F_SP116', 'to_room': 2, 'to_point': -1, 'exit': -1}, 'exit-request'),
            ({'final_scene': 21}, 'arrival'),
            ({'final_point': 2}, 'arrival'),
            ({'arrival_pictures': 0}, 'arrival'),
            ({'pictures': 0}, 'rendered'),
            ({'movement': 'nan'}, 'movement'),
            ({'life': 0}, 'health'),
            ({'rejected': 1}, 'renderer'),
        ]:
            with self.subTest(change=change):
                self.assertIn(reason, evaluate_step(PLAN, RECEIPT | change))

    def test_actions_need_live_witnesses(self):
        for field, bit, name in [
            ('cutscene_last', 3, 'cutscene'), ('lantern_pictures', 6, 'lantern'),
            ('attack_pictures', 7, 'attack'), ('enemies', 8, 'multiple-enemies'),
            ('damage_samples', 9, 'enemy-damage'), ('midna_pictures', 10, 'midna'),
            ('portal_activated', 11, 'portal'),
        ]:
            with self.subTest(field=field):
                required = 3 | 1 << bit
                plan = PLAN | {'required': required}
                receipt = RECEIPT | {'required': required, 'facts': required, field: 0}
                self.assertEqual(evaluate_step(plan, receipt), f'missing {name}')
                self.assertEqual(evaluate_step(plan, receipt | {field: 10 if field == 'cutscene_last' else
                                                               2 if field == 'enemies' else 1}), 'pass')

    def test_missing_crashed_and_duplicate_outcomes(self):
        with tempfile.TemporaryDirectory() as directory:
            tour = Path(directory)
            (tour / 'tour.txt').write_text('mode=story\nstory_version=1\nfirst_step=11\nlast_step=11\n')
            write_csv(tour / 'story-plan.csv', PLAN, [PLAN])
            result = tour / 'story.csv'
            write_csv(result, RECEIPT, [])
            self.assertEqual(evaluate(tour), [(PLAN['step'], 'missing outcome')])
            write_csv(result, RECEIPT, [RECEIPT | {'result': 'crash-or-reset', 'facts': 0}])
            self.assertIn('crash-or-reset', evaluate(tour)[0][1])
            write_csv(result, RECEIPT, [RECEIPT, RECEIPT])
            with self.assertRaisesRegex(ValueError, 'duplicate'):
                evaluate(tour)


if __name__ == '__main__':
    unittest.main()
