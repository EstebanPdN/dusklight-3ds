#!/usr/bin/env python3
"""Host checks of the automatic-test tooling: start-point choice in
generate_zone_table.py, the start-point fields of list_stage_spawns.py, the
zone statuses of analyze_tour.py, the length of the scene change, L2 and
test-resume log lines (OSReport formats into 256 bytes), the elapsed-time rule
of a restarted test (tour_elapsed.hpp, compiled with the host clang++), and
the review fixes of the CPU probe thread and the CalcDivideInfo log limiter.

With --spawns (the private start-point JSON written by list_stage_spawns.py)
the zone table is also regenerated and compared with the tree's
zone_table.inc. No disc is read and no game data is written.
"""
import argparse
import contextlib
import csv
import importlib.util
import io
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'Tools/3ds'
RUNTIME = ROOT / 'Source/platforms/3ds/runtime'


def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f'{name}.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def point(number, mode=0, event=0xff):
    return {'point': number, 'mode': mode, 'event': event, 'name': 'Link', 'position': [0, 0, 0]}


def check_start_points(g):
    sp = g.start_point
    # Event-free points first, then mode 0 among them, then the lowest number.
    assert sp('X', 0, [point(0, 0, 24), point(1, 5), point(2, 5)]) == 1
    assert sp('X', 0, [point(0, 0, 24), point(3, 5), point(4, 0)]) == 4
    # Only event points (or only the horse besides): the old rule decides.
    assert sp('X', 0, [point(3, 0, 5), point(1, 5, 7)]) == 3
    assert sp('X', 0, [point(0, 2), point(1, 0, 9)]) == 1
    # The horse (mode 2) is never chosen while anything else exists.
    assert sp('X', 0, [point(0, 2), point(1, 5)]) == 1
    # Spawn lists from before the event field count as event-free.
    assert sp('X', 0, [{'point': 2, 'mode': 0}, {'point': 0, 'mode': 5}]) == 2
    # Special starts: boar, canoe, and the fall into the Arbiter's exit.
    assert ('F_SP118', 3, 4) in g.SPECIAL_STARTS
    assert sp('F_SP118', 3, [point(0, 5, 3), point(4, 0), point(5, 0)]) == 5
    assert sp('F_SP115', 0, [point(2, 0), point(3, 0)]) == 3
    # Nothing usable at all: still a point of the room.
    assert sp('F_SP118', 3, [point(4, 0)]) == 4


def check_table(g, out):
    stages = {name: {'0': [point(0, 0, 4), point(1, 5)], '3': [point(2, 0)]} for name, _, _ in g.STAGES}
    stages['F_SP118'] = {'3': [point(0, 5, 3), point(4, 0), point(5, 0)]}
    stages['D_MN04'] = {'5': [point(0, 0, 20)]}
    stages['F_SP103'] = {'0': [point(0)], '1': [point(0)]}
    stages['R_SP01'] = {'4': [point(0)]}
    spawns = out / 'synthetic-spawns.json'
    spawns.write_text(json.dumps({'stages': stages}))
    table = out / 'synthetic-zone_table.inc'
    sys.argv = ['generate_zone_table.py', '--spawns', str(spawns), '--output', str(table)]
    printed = io.StringIO()
    with contextlib.redirect_stdout(printed):
        g.main()
    text = table.read_text()
    rooms = re.findall(r'\{(\d+),(\d+)\}', text.split('zoneStages')[0])
    names = [name for name, _, _ in g.STAGES]
    first = {}
    at = 0
    for name in names:
        first[name] = at
        at += len(stages[name])
    assert len(rooms) == at
    assert rooms[first['F_SP118']] == ('3', '5'), rooms[first['F_SP118']]
    assert rooms[first['F_SP00']] == ('0', '1'), rooms[first['F_SP00']]
    assert rooms[first['D_MN04']] == ('5', '0')
    assert 'D_MN04 r5 p0 event 20' in printed.getvalue(), printed.getvalue()
    (out / 'synthetic-generator.txt').write_text(printed.getvalue())


def check_spawn_fields(lss):
    # One PLYR chunk with two points: event 0x18 mode 0, and none (0xff) mode 5.
    def entry(params, number, y):
        return b'Link\0\0\0\0' + struct.pack('>I', params) + struct.pack('>fff', 1.0, y, 3.0) + struct.pack('>hhh', 0, 0, number) + b'\xff\xff'
    body = entry(0x1800003f, 4, 2.0) + entry(0xff00503f, 0, 7.5)
    dzr = struct.pack('>I', 1) + b'PLYR' + struct.pack('>II', 2, 16) + body
    points = lss.player_points(dzr)
    assert [(p['point'], p['mode'], p['event'], p['params']) for p in points] == [(4, 0, 0x18, '1800003f'), (0, 5, 0xff, 'ff00503f')], points
    assert points[1]['position'] == [1.0, 7.5, 3.0]


SUMMARY = ('zone,stage,room,point,form,status,load_s,frames,loop_ms,loop_p50,loop_p95,loop_p99,loop_max,fps,exec_ms,exec_p95,'
           'paint_ms,update_ms,draw_ms,gpu_wait_ms,gpu_tail_ms,audio_ms,draws,over34,over40,over50,rejected,degraded,linear_free_kb,'
           'game_free_kb,texture_kb,idle_ms,camera_ms,walk_ms,action_ms,target_ms,items_ms,still_ms,plain_ms')


def tour_row(zone, stage, status, frames, loop):
    values = [str(zone), stage, '1', '0', 'human', status, '10.0', str(frames), str(loop)] + ['1.0'] * 30
    return ','.join(values)


def check_analyzer(at, out):
    tour = out / 'tour' / '001'
    tour.mkdir(parents=True)
    rows = [SUMMARY, tour_row(1, 'F_SP103', 'ok', 500, 100.0), tour_row(2, 'F_SP116', 'moved', 500, 120.0),
            tour_row(3, 'D_MN01', 'died', 400, 900.0), tour_row(4, 'D_SB01', 'died', 0, 0.0), '5,F_SP103,0,2,human,crash-or-reset']
    (tour / 'summary.csv').write_text('\n'.join(rows) + '\n')
    (tour / 'tour.txt').write_text('mode=random\nseed=0e7bc814\n')
    reports = {2: ('F_SP116-r01-h', 'scene_change_in_zone=F_SP116 r2 -> R_SP160 r1 p2 layer -1 wipe 0, by game in event, exit 5\n'),
               3: ('D_MN01-r01-h', 'died_in_phase=walk\n'), 4: ('D_SB01-r01-h', 'died_in_phase=idle\n')}
    for zone, (name, line) in reports.items():
        folder = tour / f'{zone:03d}-{name}'
        folder.mkdir()
        (folder / 'report.txt').write_text(f'zone={zone}\n{line}')
    sys.argv = ['analyze_tour.py', str(tour)]
    printed = io.StringIO()
    with contextlib.redirect_stdout(printed):
        at.main()
    text = printed.getvalue()
    (out / 'analyzer.txt').write_text(text)
    assert 'zones measured: 2   died (left out of the totals): 2   failed: 1' in text, text
    assert 'FAILED zone 5' in text
    assert 'zone 2 F_SP116/1: F_SP116 r2 -> R_SP160 r1' in text, text
    assert '3 D_MN01/1 (walk phase)' in text and '4 D_SB01/1 (idle phase)' in text, text
    # Totals over ok and moved only: the 900 ms died zone is left out.
    assert 'within 25 FPS: 2/2; mean frame 110.0 ms' in text, text
    # A summary from Alpha 6 (statuses ok and moved only) still reads.
    old = out / 'tour' / '002'
    old.mkdir()
    (old / 'summary.csv').write_text('\n'.join([SUMMARY, tour_row(1, 'F_SP103', 'ok', 500, 100.0)]) + '\n')
    sys.argv = ['analyze_tour.py', str(old)]
    with contextlib.redirect_stdout(io.StringIO()) as second:
        at.main()
    assert 'zones measured: 1   died (left out of the totals): 0   failed: 0' in second.getvalue()
    mixed = out / 'tour' / '003'
    mixed.mkdir()
    (mixed / 'tour.txt').write_text('mode=everything\nlast_step=2\n')
    (mixed / 'summary.csv').write_text('\n'.join([
        SUMMARY, '1,F_SP104,1,24,human,pass', '2,D_MN07A,50,0,human,cutscene-incomplete',
        tour_row(3, 'R_SP107', 'render-rejected', 500, 80.0)
    ]) + '\n')
    sys.argv = ['analyze_tour.py', str(mixed)]
    with contextlib.redirect_stdout(io.StringIO()) as combined:
        at.main()
    result = combined.getvalue()
    assert 'story summary: 1/2 reported pass' in result and 'FAILED story step 2' in result
    assert 'zones measured: 1   died (left out of the totals): 0   failed: 1' in result
    assert 'FAILED zone 3' in result and 'mean frame 80.0 ms' in result


def check_scene_line():
    source = (RUNTIME / 'game_startup.cpp').read_text()
    buffer = int(re.search(r'char sceneChangeText\[(\d+)\]', source)[1])
    body = source[source.index('void observeSceneChange()'):]
    fmt = re.search(r'std::snprintf\(sceneChangeText,sizeof\(sceneChangeText\),\s*"([^"]*)"', body)[1]
    prefix = re.search(r'OSReport\("(Native scene change %lu: )%s\\n"', body)[1]
    # A long but realistic request: room 63, three listed exits, a fall.
    worst = (fmt.replace('%08lx', '%08x').replace('%p', '%s')
             % ('F_SP116', 63, 'R_SP160', 63, 123, 14, 255, 'game in cutscene', 255, 'r12,r13,s12', '0x0012abcd',
                -1234.5, 0x00400000, -202318.0, -194117.9, 35815.6))
    line = prefix.replace('%lu', '4294967295') + worst[:buffer - 1] + '\n'
    assert len(worst) <= buffer - 1, (len(worst), worst)
    assert len(line) <= 255, len(line)
    # The test loadout uses the fishing rod the game can give.
    assert 'dItemNo_FISHING_ROD_1_e,0,"fishing rod"' in source and 'dItemNo_BEE_ROD_e' not in source
    return len(worst), len(line)


def report_format(source, start):
    """The format string of the OSReport call that begins with start."""
    at = source.index('OSReport("' + start)
    parts, i = [], at + len('OSReport(')
    while True:
        m = re.compile(r'\s*"((?:[^"\\]|\\.)*)"').match(source, i)
        if not m:
            return ''.join(parts).replace('\\n', '\n')
        parts.append(m[1])
        i = m.end()


def check_log_lines():
    startup = (RUNTIME / 'game_startup.cpp').read_text()
    console = (RUNTIME / 'dev_console.cpp').read_text()
    l2 = report_format(startup, 'Native CPU mode: L2 evidence')
    resumed = report_format(console, 'Native test resumed')
    sources = re.search(r'sources\[3\]\{([^}]*)\}', console)[1]
    longest = max(re.findall(r'"([^"]*)"', sources), key=len)
    lengths = {
        'l2': len(l2.replace('%.1f', '%9.1f').replace('%.2f', '%5.2f') % (99999.9, 99999.9, 99999.9, 99.99, 99.99)),
        'resumed': len(resumed.replace('%llu', '%s') % ('9' * 10, longest, '9' * 10, '9' * 10, '600')),
    }
    for name, length in lengths.items():
        assert length <= 255, (name, length)
    return lengths


def check_review_fixes():
    startup = (RUNTIME / 'game_startup.cpp').read_text()
    probe = startup[startup.index('void reportCpuMode(bool full)'):]
    probe = probe[:probe.index('\n}\n')]
    # The probe thread is waited on through its kernel handle, result 0 only;
    # never libctru's threadJoin (returns before the thread leaves its stack)
    # or threadDetach (races the exit).
    assert 'svcWaitSynchronization(thread.kernel,100000000ll)==0' in probe
    for banned in ('threadJoin(', 'threadDetach(', 'R_SUCCEEDED('):
        assert banned not in probe, banned
    # The 4 MiB chase walks its whole cycle before it is timed.
    chase = startup[startup.index('double chaseNanoseconds('):]
    assert 'for(std::uint32_t i=0;i<count;++i)at=lines[at*8];' in chase[:chase.index('svcGetSystemTick()')]
    # One CalcDivideInfo limiter per message.
    cc = (ROOT / 'Source/src/SSystem/SComponent/c_cc_d.cpp').read_text()
    assert 'static u32 total[divideRangeMessages], skipped[divideRangeMessages];' in cc
    assert len(re.findall(r'reportDivideRange\((?:x|y|z)DivVar[13], var[13]\)', cc)) == 6


RESUME_TEST = r'''
#include "tour_elapsed.hpp"
#include <cstdio>
using namespace dusk::native;
static int failures;
static void expect(bool ok, const char* what) { if (!ok) { std::printf("FAIL %s\n", what); ++failures; } }
int main() {
    const std::uint64_t start = 3900000000000ull;  // osGetTime-like: ms since 1900
    // Auto-relaunched crash: the wall clock counts the crashed zone.
    auto r = resumedElapsed(1000, start, start + 1200 * 1000);
    expect(r.seconds == 1200 && r.source == 0 && r.startedMs == start, "wall clock within the limit");
    // Exactly at the limit: still the wall clock.
    r = resumedElapsed(1000, start, start + 1600 * 1000);
    expect(r.seconds == 1600 && r.source == 0, "wall clock at the limit");
    // Frozen for three hours: the saved figure plus the limit, start rebased.
    const std::uint64_t now = start + (1000 + 3 * 3600) * 1000ull;
    r = resumedElapsed(1000, start, now);
    expect(r.seconds == 1600 && r.source == 1 && r.wallSeconds == 1000 + 3 * 3600, "capped after a freeze");
    expect(r.startedMs == now - 1600 * 1000ull, "start rebased after a freeze");
    // A later quick restart does not count the frozen hours again.
    auto again = resumedElapsed(1700, r.startedMs, now + 150 * 1000);
    expect(again.seconds == 1750 && again.source == 0, "no second cap after the rebase");
    // Clock set back, or no start recorded: the saved figure.
    r = resumedElapsed(1000, start, start + 10 * 1000);
    expect(r.seconds == 1000 && r.source == 2 && r.startedMs == start + 10 * 1000 - 1000 * 1000, "clock set back");
    r = resumedElapsed(1000, start, start - 5000);
    expect(r.seconds == 1000 && r.source == 2, "clock before the start");
    r = resumedElapsed(1000, 0, start);
    expect(r.seconds == 1000 && r.source == 2 && r.startedMs == start - 1000 * 1000, "start not recorded");
    std::printf(failures ? "fail\n" : "pass\n");
    return failures ? 1 : 0;
}
'''


def check_resume_rule(out):
    source = out / 'resume_rule.cpp'
    source.write_text(RESUME_TEST)
    binary = out / 'resume_rule'
    flags = ['-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-I', str(RUNTIME)]
    subprocess.run(['clang++', *flags, str(source), '-o', str(binary)], check=True, capture_output=True, text=True, timeout=120)
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    (out / 'resume_rule.txt').write_text(run.stdout + run.stderr)
    assert run.returncode == 0 and run.stdout.strip().endswith('pass'), run.stdout + run.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--spawns', type=Path, help='private start-point JSON (list_stage_spawns.py output)')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    g, lss, at = load('generate_zone_table'), load('list_stage_spawns'), load('analyze_tour')
    record = {'status': 'running', 'scope': 'host Python and clang++: generator rules, spawn fields, analyzer statuses, log line lengths, resume rule, review fixes', 'cases': {}}
    try:
        check_start_points(g)
        record['cases']['start_points'] = 'PASS'
        check_table(g, out)
        record['cases']['synthetic_table'] = 'PASS'
        check_spawn_fields(lss)
        record['cases']['spawn_fields'] = 'PASS'
        check_analyzer(at, out)
        record['cases']['analyzer'] = 'PASS'
        text, line = check_scene_line()
        record['cases']['scene_line'] = f'PASS (text {text}, line {line} of 255)'
        lengths = check_log_lines()
        record['cases']['log_lines'] = 'PASS (' + ', '.join(f'{k} {v}' for k, v in lengths.items()) + ' of 255)'
        check_review_fixes()
        record['cases']['review_fixes'] = 'PASS'
        check_resume_rule(out)
        record['cases']['resume_rule'] = 'PASS'
        if args.spawns:
            table = out / 'zone_table.inc'
            sys.argv = ['generate_zone_table.py', '--spawns', str(args.spawns), '--output', str(table)]
            with contextlib.redirect_stdout(io.StringIO()) as printed:
                g.main()
            (out / 'regenerated.txt').write_text(printed.getvalue())
            assert table.read_text() == (RUNTIME / 'zone_table.inc').read_text(), 'zone_table.inc is not the generator output'
            record['cases']['tree_table_regenerated'] = 'PASS'
        record['status'] = 'pass'
    except Exception as error:
        record['status'] = 'fail'
        record['error'] = f'{type(error).__name__}: {error}'
        raise
    finally:
        (out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
        for name, result in record['cases'].items():
            print(f'{name}: {result}')
        print(record['status'])


if __name__ == '__main__':
    main()
