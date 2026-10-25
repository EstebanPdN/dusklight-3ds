#!/usr/bin/env python3
"""Classify native Azahar evidence without confusing process exit with game success."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics

FRAME = re.compile(r'Original native frame (\d+) complete; drawn=(\d+) rejected=(\d+)')
FAILURE = re.compile(r'(Native draw rejected: [^\n]*|OSPanic[^\n]*|PANIC[^\n]*|fatal:[^\n]*|'
                     r'Native shutdown failure:[^\n]*|Native EFB transfer: [^\n]*|Original reset requested[^\n]*)', re.I)
STOP_FAILURE = re.compile(rb'(?:fatal:|Native shutdown failure:|HW\.Memory[^\n]*unmapped (?:Read|Write)\d+)', re.I)
MEMORY_FAILURE = re.compile(r'HW\.Memory[^\n]*unmapped (?:Read|Write)\d+[^\n]*', re.I)


def guest_payload(line):
    return re.sub(r'^.*?OutputDebugString:\d+: ?', '', line).strip()


def failure_reasons(log):
    """Native fatal prints its marker and reason in separate OSReport calls."""
    lines = log.splitlines()
    reasons = []
    for index, line in enumerate(lines):
        for match in FAILURE.finditer(line):
            reason = match[0].strip()
            if re.fullmatch(r'fatal:\s*', reason, re.I) and index + 1 < len(lines):
                following = guest_payload(lines[index + 1])
                if following and not following.startswith('Original native frame '):
                    reason += ' ' + following
            reasons.append(reason)
    # Keep the first fault addresses/PCs, not megabytes of subsequent accesses.
    exit_at = log.find('Native developer exit after frame ')
    for index, match in enumerate(MEMORY_FAILURE.finditer(log)):
        if index == 4:
            break
        phase = ' after requested exit' if 0 <= exit_at < match.start() else ''
        reasons.append('Emulated memory fault' + phase + ': ' + match[0])
    # New lifecycle builds advertise this phase. Reaching the requested frame
    # is insufficient if worker collection or graphics teardown stops early.
    if ('Native shutdown phase=worker barrier' in log and
            'Native shutdown complete: workers=0 stackBytes=0 waitingQueues=0 graphics=stopped' not in log):
        reasons.append('Native shutdown incomplete: worker barrier entered without final graphics completion')
    return sorted(set(reasons))


def inspect_run(directory, expected_frame=None, expected_captures=(), minimum_profile_frame=300):
    directory = Path(directory)
    log_path = directory / 'guest.log'
    log = log_path.read_text(errors='replace') if log_path.exists() else ''
    receipt_path = directory / 'process.json'
    receipt = json.loads(receipt_path.read_text()) if receipt_path.exists() else None
    frames = [tuple(map(int, m.groups())) for m in FRAME.finditer(log)]
    exit_frames = [int(n) for n in re.findall(r'Native developer exit after frame (\d+)', log)]
    failures = failure_reasons(log)
    dump_evidence = [guest_payload(line) for line in log.splitlines()
                     if re.search(r'Native developer (?:dump|textures) frame=', line)]
    degraded = sorted(set(re.findall(r'Native degraded draw: ([^\n]*)', log)))
    # Earlier callbacks mislabeled successful, classified approximations as
    # rejected draws. This exact prefix is emitted only after successful submit.
    degraded = sorted(set(degraded + re.findall(
        r'Native draw rejected: (GX visual fallback: [^\n]*?) primitive=', log)))
    failures = [f for f in failures if not any(
        f.startswith('Native draw rejected: ' + d + ' primitive=') for d in degraded)]
    degraded_counts = re.findall(r'Native degraded draws simplified=(\d+) skipped=(\d+)', log)
    samples, active_frame = [], None
    for line in log.splitlines():
        match = FRAME.search(line)
        if match:
            active_frame = int(match[1])
        if 'Native frame profile:' in line and active_frame is not None:
            values = {k: float(v) for k, v in re.findall(r'(\w+)=([\d.]+)', line)}
            samples.append({'frame': active_frame, **values})
    # Frame 360 has intentional verbose vertex-layout tracing in the current runtime.
    diagnostic_frames = set(map(int, re.findall(r'Native developer (?:capture|dump|depth-audit) frame=(\d+)', log))) | {360}
    measured = [s for s in samples if s['frame'] >= minimum_profile_frame and
                s['frame'] not in diagnostic_frames and s.get('execution', 0) > 0]
    summary = {}
    for key in ('execution', 'command', 'texture', 'decode', 'vertex', 'submit', 'wait'):
        values = sorted(s[key] for s in measured if key in s)
        if values:
            summary[key + '_ms'] = {'samples': len(values), 'median': statistics.median(values),
                                   'min': min(values), 'max': max(values)}
    stages = re.findall(r'Original world stage=(\S*) room=(-?\d+)', log)
    positions = [list(map(float, xyz)) for xyz in re.findall(
        r'Original player position ([-\d.]+) ([-\d.]+) ([-\d.]+)', log)]
    captures = []
    for path in sorted(directory.glob('frame-*.png')):
        data = path.read_bytes()
        if not data.startswith(b'\x89PNG\r\n\x1a\n'):
            failures.append('Invalid PNG: ' + path.name)
        captures.append({'file': path.name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
    present = {c['file'] for c in captures}
    missing = [f'frame-{n:06d}.png' for n in expected_captures if f'frame-{n:06d}.png' not in present]
    last = frames[-1] if frames else (0, 0, 0)
    complete = bool(exit_frames and (expected_frame is None or max(exit_frames) >= expected_frame))
    if receipt is None or not frames:
        status = 'launch_or_evidence_failure'
    elif failures or last[2]:
        status = 'guest_failure'
    elif receipt.get('runner_status') == 'error' or receipt.get('runner_error') or receipt.get('cleanup_errors'):
        status = 'runner_failure'
    elif not complete or any(receipt.get(key, {}).get('requested') and
                             receipt[key].get('status') != 'complete'
                             for key in ('profile', 'debug_inventory')):
        status = 'incomplete'
    elif missing:
        status = 'missing_capture'
    elif degraded:
        status = 'completed_with_degradation'
    else:
        status = 'completed_smoke_test'
    return {'schema': 1, 'status': status, 'origin': 'Azahar emulator; not physical New3DS',
            'last_frame': last[0], 'drawn': last[1], 'rejected': last[2], 'exit_reached': complete,
            'stage': '/'.join(stages[-1]) if stages else None, 'problems': failures,
            'degraded_reasons': degraded,
            'degraded_counts': list(map(int, degraded_counts[-1])) if degraded_counts else [0, 0],
            'missing_captures': missing, 'captures': captures, 'profiles': summary,
            'profile_samples': samples, 'excluded_diagnostic_frames': sorted(diagnostic_frames),
            'player_first': positions[0] if positions else None, 'player_last': positions[-1] if positions else None,
            'receipt_present': receipt is not None, 'process': receipt,
            'dump_guest_evidence': dump_evidence,
            'hardware_validated': False, 'playability_validated': False,
            'timing_note': 'Guest tick measurements are nested and not additive. This is functional evidence only.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--expected-frame', type=int)
    parser.add_argument('--captures', default='')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    report = inspect_run(args.directory, args.expected_frame,
                         [int(n) for n in args.captures.split(',') if n])
    data = json.dumps(report, indent=2) + '\n'
    if args.output:
        args.output.write_text(data)
    print(data)
    return int(report['status'] != 'completed_smoke_test')


if __name__ == '__main__':
    raise SystemExit(main())
