#!/usr/bin/env python3
"""Report every submitted picture, including transitions, SD work and diagnostic frames.

Supply the tour/NNN directory. The timing gate requires a complete story or Test everything run.
A timing pass never establishes device provenance, story coverage or visual correctness.
"""
import argparse
import csv
import struct
import sys
from collections import defaultdict, namedtuple
from pathlib import Path

from analyze_story import evaluate as evaluate_story

HEADER = struct.Struct('<8s4I')
ROW_V1 = struct.Struct('<Q5I8sb3B')
ROW = struct.Struct('<Q5I8sb3BIIhHIII')
Picture = namedtuple('Picture', 'ticks frame demo cpu gpu draws stage room phase flags scene '
                     'read_us create_us process lookup_100us linear_free game_free dvd_lock_us')
CLOCK = 16756991 * 16


def read_step(path, step):
    data = path.read_bytes()
    if len(data) < HEADER.size:
        raise ValueError(f'{path.name}: incomplete header')
    magic, hz, number, size, count = HEADER.unpack_from(data)
    layout = {b'DLCAD01\0': ROW_V1, b'DLCAD02\0': ROW, b'DLCAD03\0': ROW}.get(magic)
    if not layout or (hz, number, size) != (CLOCK, step, layout.size):
        raise ValueError(f'{path.name}: invalid header')
    if not count or len(data) != HEADER.size + count * size:
        raise ValueError(f'{path.name}: incomplete trace')
    tail = (0, 0, -1, 0, 0, 0, 0) if layout is ROW_V1 else ()
    return (Picture(*fields, *tail) for fields in layout.iter_unpack(memoryview(data)[HEADER.size:]))


def metrics(rows):
    times = sorted(r[0] for r in rows)
    if not times or times[0] <= 0:
        raise ValueError('missing or nonpositive interval')
    count = len(times)
    tail = (count + 99) // 100
    return (count, count * CLOCK / sum(times), CLOCK / times[-1],
            tail * CLOCK / sum(times[-tail:]), times[-1] * 1000 / CLOCK,
            sum(t * 20 > CLOCK for t in times))


def line(label, rows):
    count, avg, minimum, low, worst, slow = metrics(rows)
    print(f'{label:34} {count:7} {avg:8.2f} {minimum:8.2f} {low:8.2f} {worst:10.2f} {slow:7}')


def resource_line(label, rows):
    scale = 1000 * len(rows)
    # These two fields have no validity bit in the original traces. A zero
    # pair before the first heap poll is indistinguishable from no measurement.
    sampled = [r for r in rows if r.linear_free or r.game_free]
    linear = str(min(r.linear_free for r in sampled) // 1024) if sampled else '-'
    game = str(min(r.game_free for r in sampled) // 1024) if sampled else '-'
    print(f'{label:20} {sum(r.cpu for r in rows) / scale:8.2f} {sum(r.gpu for r in rows) / scale:8.2f} '
          f'{sum(r.draws for r in rows) / len(rows):8.1f} {max(r.read_us for r in rows) / 1000:9.2f} '
          f'{max(r.dvd_lock_us for r in rows) / 1000:9.2f} {max(r.create_us for r in rows) / 1000:9.2f} '
          f'{linear:>10} {game:>10}')


def analyze(tour):
    info = dict(l.split('=', 1) for l in (tour / 'tour.txt').read_text().splitlines() if '=' in l)
    expected = int(info['zones'])
    if expected <= 0:
        raise ValueError('invalid planned zone count')
    with (tour / 'summary.csv').open() as source:
        summaries = list(csv.DictReader(source))
    steps, areas, cuts, events = {}, defaultdict(list), defaultdict(list), defaultdict(list)
    all_rows, issues = [], []
    mode = info.get('mode')
    story_last = int(info.get('last_step', 0)) if mode in ('story', 'everything') else 0
    transition_rows = []
    previous = None
    for step in range(1, expected + 1):
        path = tour / f'cadence-{step:03}.bin'
        if not path.exists():
            issues.append(f'step {step}: missing trace')
            continue
        try:
            rows = list(read_step(path, step))
            if any(row.ticks == 0 for row in rows):
                raise ValueError(f'{path.name}: zero picture interval')
        except (OSError, ValueError) as error:
            # A reset can leave the active file unsealed. The other steps
            # still contain useful measurements, but this run cannot pass.
            issues.append(str(error))
            continue
        steps[step] = rows
        for row in rows:
            frame, stage, room, scene = row.frame, row.stage, row.room, row.scene
            if previous is not None and frame != previous + 1:
                issues.append(f'step {step}, frame {frame}: discontinuity after {previous}')
            previous = frame
            area = f'{stage.partition(bytes(1))[0].decode("ascii") or "(loading)"}/{room}'
            areas[area].append(row)
            if scene & 1:
                cuts[(step, area)].append(row)
            if scene & 2:
                events[(step, area)].append(row)
            if scene & 4 or row.phase == 255 or (step <= story_last and row.phase in (0, 2)):
                transition_rows.append(row)
        all_rows.extend(rows)
    if not all_rows:
        raise ValueError('no complete cadence files in tour')
    if mode not in ('story', 'everything') or info.get('result') != 'complete':
        issues.append('full benchmark completion missing')
    if int(info.get('zones_finished', 0)) != expected or info.get('failures') != '0':
        issues.append('zone completion count or failure count outside gate')
    if [int(r['zone']) for r in summaries] != list(range(1, expected + 1)):
        issues.append('summary sequence incomplete')
    if mode in ('story', 'everything'):
        try:
            if (int(info['first_step']) != 1 or
                    not (story_last == expected if mode == 'story' else 0 < story_last < expected)):
                issues.append('selected route is only part of the story plan')
            for step_id, result in evaluate_story(tour):
                if result != 'pass':
                    issues.append(f'{step_id}: {result}')
        except (OSError, ValueError, KeyError) as error:
            issues.append(f'invalid story evidence: {error}')
    for row in summaries:
        if row['status'] != ('pass' if int(row['zone']) <= story_last else 'ok'):
            issues.append(f"step {row['zone']}: {row['status']} (check scenario outcome)")
    print(' ' * 34 + ' pictures  avg FPS  min FPS   1% low   worst ms    >50ms')
    line('All intact traces', all_rows)
    for group, table in [('Step', steps), ('Area', areas), ('Demo camera', cuts), ('Game event', events)]:
        for label, rows in table.items():
            line(f'{group} {label}', rows)
    for label, rows in [('Transition or pre-arrival', transition_rows),
                        ('Diagnostic flag', [r for r in all_rows if r.flags & 1]),
                        ('Capture render owner', [r for r in all_rows if r.flags & 2]),
                        ('Capture SD writer overlap', [r for r in all_rows if r.flags & 4]),
                        ('Full dump interval', [r for r in all_rows if r.flags & 8])]:
        if rows:
            line(label, rows)
    print('Resource costs by rendered area. Execution and GPU busy can overlap. Times are milliseconds.')
    print('Area                 exec avg  GPU avg    draws  read max  lock max create ms linear KiB   game KiB')
    resource_line('All areas', all_rows)
    for area, rows in areas.items():
        resource_line(area, rows)
    print('Read and DVD lock are game-thread totals per picture. Create is the longest single process creation. '
          'Memory columns are minima of the last available samples. Both-zero samples are unavailable. '
          'Older trace formats may record no load timings.')
    print('Worst intervals (frame, area, phase, demo frame, elapsed/CPU/GPU/game-thread read/longest process create ms, '
          'process ID, path lookup/DVD service acquisition ms, draws, last sampled free linear/game KiB)')
    for r in sorted(all_rows, key=lambda row: -row.ticks)[:10]:
        print(r.frame, r.stage.rstrip(bytes(1)).decode('ascii') or '(loading)', r.room, r.phase, r.demo,
              round(r.ticks * 1000 / CLOCK, 2), *(round(t / 1000, 2) for t in (r.cpu, r.gpu, r.read_us, r.create_us)),
              r.process, r.lookup_100us / 10, round(r.dvd_lock_us / 1000, 2), r.draws, r.linear_free // 1024, r.game_free // 1024)
    n, avg, minimum, low, worst, slow = metrics(all_rows)
    if slow or not (25 <= avg <= 30):
        issues.append(f'{slow} intervals below 20 FPS; average {avg:.3f} FPS')
    for issue in issues:
        print(issue, file=sys.stderr)
    return not issues


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tour', type=Path)
    try:
        passed = analyze(parser.parse_args().tour)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(2, f'{error}\n')
    sys.exit(0 if passed else 1)
