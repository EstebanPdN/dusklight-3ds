#!/usr/bin/env python3
"""Read native log.txt, wrapped guest.log, or SD perf.log without modifying them.

Usage: python3 Tools/3ds/analyze_frame_profile.py LOG [LOG ...] [--details]
JSON goes to stdout, the human summary to stderr. Inputs are independent reports,
not concatenated or double-counted. --details includes parsed endpoint records.
No hardware FPS or per-frame tail latency can be established from these logs.
"""
import argparse
import json
import math
from pathlib import Path
import re
import statistics
import sys

from azahar_run_report import guest_payload


# These are emitted cumulative counters, not frame-local profiling costs.
CACHE_COUNTERS = {
    'topology cache': ('hits', 'misses'),
    'compact snapshot': ('hits', 'misses', 'stores', 'evictions', 'ranges',
                         'output', 'packed-changes', 'array-changes'),
    'direct shapes': ('hits', 'misses', 'keyMisses'),
    'prepared draws': ('hits', 'misses', 'stores', 'fallbacks'),
    'static geometry': ('hits', 'misses', 'stores', 'resets'),
    'shared arrays': ('hits', 'stores', 'evictions', 'copied', 'compared'),
    'texture binds': ('skipped', 'applied'),
    'uniform rows': ('skipped', 'written'),
    'PICA state filter': ('skips', 'applies'),
}
# Timers are inclusive/overlapping; grouping is attribution, never addition.
TIMERS = {
    'frame profile:': ('frame', ('execution', 'command', 'texture', 'decode',
                                'vertex', 'submit', 'wait')),
    'game phases': ('game', ('paint', 'update', 'draw')),
    'vertex paths': ('vertex_paths', ('direct', 'cpu', 'position')),
    'sparse vertex samples': ('vertex_samples', ('decode', 'transform', 'lighting', 'fog')),
    'EFB copies=': ('efb', ('copies', 'encode')),
    'GX stream total=': ('stream', ('total',)),
    'sparse array validation=': ('array_validation', ('validation',)),
    'audio execution=': ('audio_main', ('execution',)),
}
LIMITATIONS = [
    'Hardware 30 FPS is unverified. Input provenance is not proof of physical hardware.',
    'Azahar guest ticks are instruction accounting, NOT hardware cycles or FPS: '
    '4M loops measured about 8.03M/16.05M/12.05M ticks for 2/4/3 instructions.',
    'GPU execution time and individual uninstrumented frame intervals are unavailable; '
    'CPU wait is not GPU time. No per-frame p50/p95 or sustained-FPS claim.',
    'Window p50/p95 describe window means (linear interpolation, equally weighted windows), '
    'not individual frames. Windows exclude reporting, tracing, capture and dump frames.',
    'Stage/room changes are bounded by sampled endpoints, not exact transition frames; '
    'even equal endpoints cannot rule out intermediate changes.',
    'All component/painter timings are inclusive and may nest or overlap. Never sum them '
    'or subtract audio-worker elapsed from main-thread time. Missing scopes are not zero.',
    'Audio-worker elapsed/blocks cover the sampled main profiling interval, not a whole '
    'ordinary window; block cost is elapsed/blocks, not audio CPU utilization.',
    'Cache deltas require consecutive same-session/stage/room endpoints and monotonic '
    'counters; endpoint comparability is not workload replay. No counter is CPU cost.',
    'Recent decodes are a bounded eight-entry history since the preceding report, '
    'not exhaustive frame events; history costs are never added to frame decode time.',
    'Frame 360 has runtime vertex tracing; explicit capture/dump/depth diagnostics are '
    'excluded from component rankings, but their preceding ordinary windows remain usable.',
    'Cold startup means the observed empty-stage startup, not a warm-cache guarantee '
    'for later stage loading. Separate inputs are never merged.',
]


def numbers(text, required=(), capacities=()):
    """Reject malformed/negative/nonfinite numeric fields instead of partial matches."""
    result = {}
    for key, token in re.findall(r'([\w-]+)=([^\s;]+)', text):
        if key in result:
            raise ValueError('duplicate field ' + key)
        parts = token.split('/')
        if len(parts) != 1 and (len(parts) != 2 or key not in capacities):
            raise ValueError('invalid scalar/capacity field ' + key)
        if any(not re.fullmatch(r'\d+(?:\.\d+)?', part) for part in parts):
            raise ValueError('invalid numeric field ' + key)
        values = [float(part) if '.' in part else int(part) for part in parts]
        if any(not math.isfinite(value) for value in values):
            raise ValueError('nonfinite field ' + key)
        result[key] = values[0]
        if len(values) == 2:
            result[key + '_capacity'] = values[1]
        elif len(values) != 1:
            raise ValueError('invalid capacity field ' + key)
    for key in required:
        if key not in result:
            raise ValueError('missing field ' + key)
    return result


def integer_fields(values, keys):
    if any(not isinstance(values[key], int) for key in keys):
        raise ValueError('expected integer counters: ' + ', '.join(keys))


def parse_lines(lines):
    """Keep pre-marker windows/memory separate from post-marker sample scopes."""
    records, issues, diagnostics = [], [], set()
    pending, active, session = {}, None, 0
    malformed = 0

    def issue(line, reason):
        nonlocal malformed
        malformed += 1
        if len(issues) < 10:
            issues.append({'line': line, 'reason': reason})

    def start(frame, line, kind):
        nonlocal active, session, pending
        if records and (frame <= records[-1]['frame'] or kind != records[-1]['kind']):
            session += 1
        active = {'frame': frame, 'line': line, 'session': session, 'kind': kind,
                  'stage': None, 'room': None, 'timings_ms': {}, 'painters': {},
                  'cache_snapshots': {}, 'frame_counts': {}, 'recent_decodes': [],
                  **pending}
        pending = {}
        records.append(active)
        return active

    for line_number, raw in enumerate(lines, 1):
        line = guest_payload(raw)
        if not line:
            continue
        diagnostic = re.match(r'Native developer (?:capture|dump|depth-audit|retained-depth) frame=(\d+)', line)
        if diagnostic:
            diagnostics.add((session, int(diagnostic[1])))
        try:
            if line.startswith('frame='):
                match = re.fullmatch(r'frame=(\d+) stage=(\S*) (.+)', line)
                if not match:
                    raise ValueError('malformed perf.log row')
                values = numbers(match[3], ('loop_ms', 'exec_ms', 'fps', 'drawn',
                                           'rejected', 'linear_free_kib'))
                integer_fields(values, ('drawn', 'rejected', 'linear_free_kib'))
                row = start(int(match[1]), line_number, 'perf')
                row['stage'] = match[2]
                row['memory_kib'] = {'linear_free': values['linear_free_kib']}
                row['cumulative_draws'] = {key: values[key] for key in ('drawn', 'rejected')}
                # All-zero means are the emitter's no-window sentinel.
                if values['loop_ms'] or values['exec_ms']:
                    row['window'] = {'count': None, 'execution_ms': values['exec_ms'],
                                     'loop_ms': values['loop_ms'], 'detailed': 'unknown'}
                continue
            if line.startswith('Native ordinary frame window:'):
                values = numbers(line.split(' detailed=')[0], ('count', 'execution_mean', 'loop_mean'))
                integer_fields(values, ('count',))
                mode = re.search(r' detailed=(all|sparse)$', line)
                if not mode or not values['count']:
                    raise ValueError('invalid ordinary window count/mode')
                if 'window' in pending:
                    issue(line_number, 'unattached ordinary window replaced')
                pending['window'] = {'count': values['count'], 'execution_ms': values['execution_mean'],
                                     'loop_ms': values['loop_mean'], 'detailed': mode[1]}
                continue
            if line.startswith('Native renderer memory:'):
                values = numbers(line, ('game_free', 'linear_free'))
                integer_fields(values, values)
                pending['memory_bytes'] = values
                continue
            if line.startswith('Original native frame '):
                active = None
                match = re.fullmatch(r'Original native frame (\d+) complete;\s*(.*)', line)
                if not match:
                    raise ValueError('malformed frame marker')
                values = numbers(match[2], ('drawn', 'rejected'))
                integer_fields(values, values)
                start(int(match[1]), line_number, 'native')['cumulative_draws'] = values
                continue
            if line.startswith('Original world stage='):
                match = re.fullmatch(r'Original world stage=(\S*) room=(-?\d+) player=\S+', line)
                if not match or active is None:
                    raise ValueError('malformed or orphan world snapshot')
                active['stage'], active['room'] = match[1], int(match[2])
                continue
            if line.startswith('Native painter '):
                match = re.fullmatch(r'Native painter (\S+) (\d+(?:\.\d+)?) ms calls=(\d+)', line)
                if not match or active is None:
                    raise ValueError('malformed or orphan painter scope')
                elapsed = float(match[2])
                if not math.isfinite(elapsed) or match[1] in active['painters']:
                    raise ValueError('nonfinite or duplicate painter scope')
                active['painters'][match[1]] = {'ms': elapsed, 'calls': int(match[3])}
                continue
            if line.startswith('Native audio worker elapsed='):
                values = numbers(line, ('elapsed', 'blocks'))
                integer_fields(values, ('blocks',))
                if active is None:
                    raise ValueError('orphan audio worker sample')
                active['audio_worker'] = {'elapsed_ms': values['elapsed'], 'blocks': values['blocks']}
                continue
            if line.startswith('Native recent decode '):
                match = re.fullmatch(r'Native recent decode handle=(\d+) (\d+)x(\d+) gxfmt=(\d+) picabytes=(\d+) (dirty|new) (\d+(?:\.\d+)?) ms', line)
                if not match or active is None or not math.isfinite(float(match[7])):
                    raise ValueError('malformed or orphan recent decode')
                event = dict(zip(('handle', 'width', 'height', 'gxfmt', 'picabytes'),
                                 map(int, match.groups()[:5])))
                active['recent_decodes'].append({**event, 'reason': match[6], 'ms': float(match[7])})
                continue
            for label, keys in CACHE_COUNTERS.items():
                prefix = 'Native ' + label + ' '
                if line.startswith(prefix) and re.match(r'[\w-]+=', line[len(prefix):]):
                    values = numbers(line[len(prefix):], keys,
                                     capacities=('bytes',) if label == 'static geometry' else ())
                    integer_fields(values, values)
                    if active is None:
                        raise ValueError('orphan cache snapshot')
                    active['cache_snapshots'][label] = values
                    break
            else:
                for label, (group, keys) in TIMERS.items():
                    if not line.startswith('Native ' + label):
                        continue
                    if active is None:
                        raise ValueError('orphan component timing')
                    # Remove the textual prefix but retain a key attached by '='.
                    prefix = 'Native ' + (label.rsplit(' ', 1)[0] if label.endswith('=') else label)
                    body = line[len(prefix):].lstrip()
                    before, separator, after = body.partition(' ms')
                    if not separator:
                        raise ValueError('missing timer unit')
                    values = numbers(before, keys)
                    counts = numbers(after)
                    integer_fields(counts, counts)
                    active['timings_ms'][group] = {key: values[key] for key in keys}
                    if counts:
                        active['frame_counts'][group] = counts
                    break
        except (ValueError, OverflowError) as error:
            issue(line_number, str(error))
            if line.startswith(('Original native frame ', 'frame=')):
                active, pending = None, {}
    if pending:
        issue(line_number if 'line_number' in locals() else 0, 'trailing window/memory without a frame marker')
    for row in records:
        row['diagnostic'] = (row['session'], row['frame']) in diagnostics or row['frame'] == 360
    return records, {'malformed_lines': malformed, 'examples': issues}


def distribution(values):
    if not values:
        return None
    return {'samples': len(values), 'mean': round(statistics.fmean(values), 3),
            'min': round(min(values), 3), 'max': round(max(values), 3)}


def window_summary(windows):
    """Percentiles of means, not percentiles of the underlying frame intervals."""
    result = {'windows': len(windows),
              'represented_frames': sum(w['count'] for w in windows) if windows and all(w['count'] is not None for w in windows) else None}
    for key in ('execution_ms', 'loop_ms'):
        values = sorted(window[key] for window in windows)
        stats = distribution(values)
        if stats:
            for percentile in (50, 95):
                position = (len(values) - 1) * percentile / 100
                low, high = math.floor(position), math.ceil(position)
                stats[f'p{percentile}_window_mean_ms'] = round(values[low] + (values[high] - values[low]) * (position - low), 3)
            if result['represented_frames']:
                stats['frame_weighted_mean_ms'] = round(sum(w[key] * w['count'] for w in windows) / result['represented_frames'], 3)
        result[key] = stats
    return result


def scope_summary(rows, field):
    names = sorted({name for row in rows for name in row.get(field, {})})
    result = {}
    for name in names:
        if field == 'painters':
            scopes = [row[field][name] for row in rows if name in row[field]]
            result[name] = {**distribution([scope['ms'] for scope in scopes]),
                            'observed_calls': sum(scope['calls'] for scope in scopes)}
        else:
            keys = sorted({key for row in rows for key in row.get(field, {}).get(name, {})})
            result[name] = {key: distribution([row[field][name][key] for row in rows
                                              if key in row.get(field, {}).get(name, {})]) for key in keys}
    return result


def cache_summary(rows):
    result = {}
    for name, keys in CACHE_COUNTERS.items():
        observed = [row for row in rows if name in row['cache_snapshots']]
        if not observed:
            continue
        intervals, withheld, total = 0, 0, {}
        for left, right in zip(rows, rows[1:]):
            a, b = left['cache_snapshots'].get(name), right['cache_snapshots'].get(name)
            if a is None or b is None or left['stage'] is None or right['stage'] is None:
                continue
            if any(b[key] < a[key] for key in keys) or (name == 'static geometry' and b['resets'] != a['resets']):
                withheld += 1
                continue
            intervals += 1
            for key in keys:
                total[key] = total.get(key, 0) + b[key] - a[key]
        rate_keys = ('hits', 'misses') if 'misses' in keys else None
        denominator = sum(total.get(key, 0) for key in rate_keys) if rate_keys else 0
        result[name] = {'first': observed[0]['cache_snapshots'][name],
                        'last': observed[-1]['cache_snapshots'][name],
                        'comparable_intervals': intervals, 'reset_intervals_withheld': withheld,
                        'counter_delta': total or None,
                        'delta_hit_fraction': round(total['hits'] / denominator, 6) if denominator else None}
    return result


def analyze(records, diagnostics, details=False):
    segments, transitions = [], []
    previous = None
    staged_sessions = set()
    for row in records:
        identity = (row['session'], row['kind'], row['stage'], row['room'])
        phase = 'stage' if row['stage'] else 'stage_unavailable'
        if row['stage'] == '':
            phase = 'unstaged' if row['session'] in staged_sessions else 'cold_startup'
        elif row['stage'] is not None:
            staged_sessions.add(row['session'])
        if previous is None or identity != previous:
            segments.append({'session': row['session'], 'stage': row['stage'], 'room': row['room'],
                             'phase': phase,
                             'rows': [], 'windows': []})
        segment = segments[-1]
        segment['rows'].append(row)
        if 'window' in row:
            if previous == identity and row['stage'] is not None:
                segment['windows'].append(row['window'])
            else:
                transitions.append({'ending_frame': row['frame'], 'session': row['session'],
                                    'from_stage': previous[2] if previous and previous[0] == row['session'] else None,
                                    'to_stage': row['stage'], 'to_room': row['room'],
                                    'reason': 'transition_or_unknown_start', **row['window']})
        previous = identity
    for segment in segments:
        rows = segment.pop('rows')
        usable = [row for row in rows if not row['diagnostic']]
        segment['observed_frames'] = [row['frame'] for row in rows]
        segment['diagnostic_frames_excluded_from_rankings'] = [row['frame'] for row in rows if row['diagnostic']]
        segment['ordinary_window_means'] = window_summary(segment.pop('windows'))
        segment['sampled_components_ms'] = scope_summary(usable, 'timings_ms')
        painters = scope_summary(usable, 'painters')
        segment['top_observed_inclusive_painters_ms'] = dict(sorted(painters.items(), key=lambda item: (-item[1]['mean'], item[0]))[:8])
        workers = [row['audio_worker'] for row in usable if 'audio_worker' in row]
        blocks = sum(worker['blocks'] for worker in workers)
        segment['audio_worker_overlapping'] = {
            'elapsed_ms': distribution([worker['elapsed_ms'] for worker in workers]),
            'observed_blocks': blocks if workers else None,
            'elapsed_ms_per_block': round(sum(worker['elapsed_ms'] for worker in workers) / blocks, 3) if blocks else None}
        segment['memory_bytes'] = {key: distribution([row['memory_bytes'][key] for row in rows if key in row.get('memory_bytes', {})])
                                   for key in sorted({key for row in rows for key in row.get('memory_bytes', {})})}
        segment['linear_free_kib'] = distribution([row['memory_kib']['linear_free'] for row in rows if 'memory_kib' in row])
        segment['caches'] = cache_summary(rows)
        segment['sampled_frame_counts'] = scope_summary(usable, 'frame_counts')
        history = [event for row in rows for event in row['recent_decodes']]
        segment['recent_decode_history'] = {
            'reported_entries': len(history), 'elapsed_ms': distribution([event['ms'] for event in history]),
            'dirty_entries': sum(event['reason'] == 'dirty' for event in history),
            'new_entries': sum(event['reason'] == 'new' for event in history)}
        if rows[0]['frame'] == 1:
            segment['initial_frame_components_ms'] = rows[0]['timings_ms']
    result = {'status': 'ok' if records else 'no_frame_evidence',
              'hardware_30fps': 'unverified', 'gpu_time_ms': None,
              'individual_uninstrumented_frame_intervals_ms': None,
              'diagnostics': diagnostics, 'segments': segments,
              'transition_or_unassigned_window_means': transitions}
    if details:
        result['records'] = records
    return result


def human_summary(report):
    lines = [report['input'] + ':']
    for segment in report['segments']:
        windows = segment['ordinary_window_means']
        loop = windows['loop_ms']
        label = segment['stage'] or segment['phase']
        text = f"  session {segment['session']} {label} room={segment['room']}: sampled endpoints {segment['observed_frames'][0]}..{segment['observed_frames'][-1]}"
        if loop:
            text += f"; {windows['windows']} ordinary window means: loop p50={loop['p50_window_mean_ms']:.3f}, p95={loop['p95_window_mean_ms']:.3f} ms (NOT per-frame percentiles)"
        else:
            text += '; ordinary window means unavailable'
        lines.append(text)
        excluded = segment['diagnostic_frames_excluded_from_rankings']
        if excluded:
            lines.append(f'    Diagnostic endpoints excluded from CPU rankings: {excluded}')
        components = [(group + '.' + name, stats['mean']) for group, values in segment['sampled_components_ms'].items()
                      if group != 'frame' for name, stats in values.items()]
        components += [(name, stats['mean']) for name, stats in segment['sampled_components_ms'].get('frame', {}).items() if name != 'execution']
        top = sorted(components, key=lambda item: (-item[1], item[0]))[:5]
        if top:
            lines.append('    Observed inclusive CPU scopes (do not sum): ' + ', '.join(f'{name}={value:.3f} ms' for name, value in top))
        painters = segment['top_observed_inclusive_painters_ms']
        if painters:
            lines.append('    Observed inclusive painters (do not sum): ' + ', '.join(f"{name}={stats['mean']:.3f} ms" for name, stats in list(painters.items())[:3]))
    lines.append(f"  Transition/unassigned windows: {len(report['transition_or_unassigned_window_means'])}; malformed/orphan lines: {report['diagnostics']['malformed_lines']}")
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', nargs='+', type=Path)
    parser.add_argument('--details', action='store_true', help='include parsed endpoint records')
    args = parser.parse_args()
    reports = []
    for path in args.logs:
        try:
            with path.open(encoding='utf-8', errors='replace') as stream:
                records, diagnostics = parse_lines(stream)
        except OSError as error:
            parser.error(str(error))
        reports.append({'input': str(path), **analyze(records, diagnostics, args.details)})
    print(json.dumps({'limitations': LIMITATIONS, 'inputs': reports}, sort_keys=True, separators=(',', ':'), allow_nan=False))
    print('\n'.join(human_summary(report) for report in reports), file=sys.stderr)
    print('Evidence limitations:\n  ' + '\n  '.join(LIMITATIONS), file=sys.stderr)
    return 2 if any(report['status'] != 'ok' for report in reports) else 1 if any(report['diagnostics']['malformed_lines'] for report in reports) else 0


if __name__ == '__main__':
    sys.exit(main())
