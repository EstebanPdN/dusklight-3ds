#!/usr/bin/env python3
"""Compare sparse native guest profiles; these are not hardware FPS results."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics


def read_profiles(path):
    text = path.read_text()
    diagnostic_frames = {int(n) for n in re.findall(r'Native developer (?:capture|dump|depth-audit) frame=(\d+)', text)}
    rows = []
    chunks = re.split(r'Original native frame (\d+) complete;', text)
    for index in range(1, len(chunks), 2):
        frame, chunk = int(chunks[index]), chunks[index + 1]
        row = {'frame': frame, 'traced': frame == 360, 'diagnostic': frame in diagnostic_frames}
        for label, prefix in [('profile', 'Native frame profile:'),
                              ('paths', 'Native vertex paths'),
                              ('geometry', 'Native EFB copies='),
                              ('rigid', 'Native GPU rigid vertices=')]:
            line = re.search(re.escape(prefix) + r'[^\n]*', chunk)
            if line:
                row[label] = {key: float(value) for key, value in
                              re.findall(r'([\w-]+)=([\d.]+)', line[0])}
        row['counters'] = {key: int(value) for key, value in
                           re.findall(r'([\w-]+)=(\d+)', chunk.splitlines()[0])}
        for label, prefix in [('world', 'Original world stage='),
                              ('player', 'Original player position '),
                              ('control', 'Original Link control ')]:
            match = re.search(re.escape(prefix) + r'[^\n]*', chunk)
            if match:
                # Allocation addresses may differ across binaries.
                row[label] = re.sub(r' player=0x[0-9a-f]+', '', match[0])
        if 'profile' in row:
            rows.append(row)
    return rows


def read_windows(path):
    """A window precedes its terminating frame marker and excludes that frame."""
    result, pending = {}, None
    for line in path.read_text().splitlines():
        window = re.search(r'Native ordinary frame window: count=(\d+) execution_mean=([\d.]+) loop_mean=([\d.]+) ms detailed=(all|sparse)', line)
        if window:
            pending = {'count': int(window[1]), 'execution_ms': float(window[2]),
                       'loop_ms': float(window[3]), 'detailed': window[4]}
        frame = re.search(r'Original native frame (\d+) complete;', line)
        if frame and pending is not None:
            result[int(frame[1])] = pending
            pending = None
    return result


def matched_windows(before, after, before_profiles, after_profiles, minimum_frame=420):
    """Endpoint workload checks help comparability; they are not replay proof."""
    profiles = {r['frame']: r for r in before_profiles}
    candidates = {r['frame']: r for r in after_profiles}
    rows = []
    for frame, right in sorted(after.items()):
        left = before.get(frame)
        a, b = profiles.get(frame), candidates.get(frame)
        if (frame < minimum_frame or left is None or a is None or b is None or
                left['count'] != right['count'] or not left['count'] or
                not a.get('world') or not a.get('geometry') or signature(a) != signature(b)):
            continue
        rows.append({'ending_frame': frame, 'ordinary_frames': left['count'],
                     'baseline': left, 'candidate': right,
                     'saved_ms': {k: left[k]-right[k] for k in ('execution_ms', 'loop_ms')}})
    return rows


def signature(row):
    """Conservative workload filter, not a deterministic replay guarantee."""
    return (row.get('world'), row.get('player'), row.get('control'),
            tuple(row.get('geometry', {}).get(k) for k in ['vertices', 'lit', 'fogged']),
            tuple(row.get('rigid', {}).get(k) for k in ['vertices', 'direct-decode', 'compact']),
            row.get('paths', {}).get('position-only'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline', type=Path)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    before, after = read_profiles(args.baseline), read_profiles(args.candidate)
    base = {row['frame']: row for row in before}
    matches = []
    for row in after:
        previous = base.get(row['frame'])
        if (row['frame'] < 300 or row['traced'] or row['diagnostic'] or not previous or previous['diagnostic'] or
                'world' not in row or 'geometry' not in row or
                signature(previous) != signature(row)):
            continue
        values = {}
        for group, key in [('profile', 'execution'), ('profile', 'command'),
                           ('profile', 'vertex'), ('paths', 'direct'),
                           ('paths', 'cpu'), ('paths', 'position')]:
            left, right = previous[group][key], row[group][key]
            values[key] = {'before_ms': left, 'after_ms': right,
                           'saved_ms': left - right,
                           'reduction_percent': 100 * (left - right) / left if left else None}
        matches.append({'frame': row['frame'], 'metrics': values})
    windows = matched_windows(read_windows(args.baseline), read_windows(args.candidate), before, after)
    report = {
        'origin': 'Azahar native guest tick profiles, not physical hardware',
        'limits': ['Sparse samples only; nested timings must not be summed.',
                   'Same frame, player/control and vertex counts filter comparisons; input polling and audio remain asynchronous.',
                   'Frame 360 is excluded because it traces individual vertex layouts.',
                   'Capture/dump frames are excluded from sparse profile comparisons.',
                   'Ordinary windows exclude diagnostic frames in the guest; endpoint checks cannot prove every intermediate frame matches.',
                   'No sustained FPS, full-game stability, or hardware acceptance.'],
        'inputs': {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                   for path in [args.baseline, args.candidate]},
        'movie_playback_mismatch_errors': {
            str(path): len(re.findall(r'Movie <Error>[^\n]*Expected to read type', path.read_text()))
            for path in [args.baseline, args.candidate]},
        'baseline': before, 'candidate': after, 'matched_samples': matches,
        'matched_ordinary_windows': windows,
        'ordinary_median_saved_ms': {key: statistics.median(w['saved_ms'][key] for w in windows)
                                     for key in ('execution_ms', 'loop_ms')} if windows else {},
        'median_saved_ms': {key: statistics.median(m['metrics'][key]['saved_ms'] for m in matches)
                            for key in matches[0]['metrics']} if matches else {},
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'matched_frames': [m['frame'] for m in matches],
                      'median_saved_ms': report['median_saved_ms'],
                      'ordinary_windows': [w['ending_frame'] for w in windows],
                      'ordinary_median_saved_ms': report['ordinary_median_saved_ms']}, indent=2))


if __name__ == '__main__':
    main()
