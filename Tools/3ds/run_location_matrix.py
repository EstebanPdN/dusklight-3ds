#!/usr/bin/env python3
"""Run one CXI across the multi-location matrix in Azahar and summarize each location.

Emulator evidence only: frames reached, rejected draws, fatal stops, sparse
guest execution profiles and fixed-frame captures per location.
"""
import argparse
import json
from pathlib import Path
import re
import statistics
import subprocess
import sys
from azahar_run_report import inspect_run

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
FATAL = re.compile(r'(Native draw rejected: [^\n]*|OSPanic[^\n]*|PANIC[^\n]*|fatal[^\n]*|Halt[^\n]*|'
                   r'Native EFB transfer: [^\n]*|Original reset requested[^\n]*)', re.IGNORECASE)


def run(location, args):
    name = f'{args.prefix}-{location["name"]}'
    if not args.summarize_only:
        execute(location, name, args)
    return summarize(location, name, args)


def execute(location, name, args):
    command = [sys.executable, str(HERE / 'run_azahar_smoke.py'), '--name', name, '--private-disc',
               '--image', str(args.image), '--exit-frame', str(args.exit_frame), '--unthrottled',
               '--capture-frames', args.capture_frames, '--seconds', str(args.seconds),
               '--renderer', args.renderer, '--evidence-root', str(args.evidence_root)]
    if location['boot_stage']:
        command += ['--boot-stage', location['boot_stage']]
    result = subprocess.run(command, capture_output=True, text=True)
    # A failed launcher must remain visible, even if it never created a run.
    args.evidence_root.mkdir(parents=True, exist_ok=True)
    (args.evidence_root / (name + '-launcher.log')).write_text(result.stdout + result.stderr)
    (args.evidence_root / (name + '-launcher.json')).write_text(json.dumps(
        {'command': command, 'returncode': result.returncode}, indent=2) + '\n')


def summarize(location, name, args):
    out = args.evidence_root / name
    log = (out / 'guest.log').read_text(errors='replace') if (out / 'guest.log').exists() else ''
    frames = [int(f) for f in re.findall(r'Original native frame (\d+) complete', log)]
    rejected = re.findall(r'Original native frame \d+ complete; drawn=(\d+) rejected=(\d+)', log)
    executions = [float(v) for v in re.findall(r'Native frame profile: execution=([\d.]+)', log)]
    late = executions[len(executions) // 2:] or executions
    stages = re.findall(r'Original world stage=(\S*) room=(-?\d+)', log)
    degraded = sorted(set(re.findall(r'Native degraded draw: ([^\n]*)', log)))
    counts = re.findall(r'Native degraded draws simplified=(\d+) skipped=(\d+)', log)
    problems = [p for p in sorted(set(FATAL.findall(log))) if not any(d in p for d in degraded)]
    checked = inspect_run(out, args.exit_frame, [int(n) for n in args.capture_frames.split(',') if n])
    problems = list(checked['problems'])
    launcher = args.evidence_root / (name + '-launcher.json')
    launch_status = json.loads(launcher.read_text()) if launcher.exists() else None
    if launch_status and launch_status['returncode'] and checked['status'] == 'launch_or_evidence_failure':
        checked['status'] = 'launcher_failure'
        problems.append('Launcher failed; see ' + name + '-launcher.log')
    if checked['status'] != 'completed_smoke_test' and not problems:
        problems.append(checked['status'])
    return {'name': location['name'], 'boot_stage': location['boot_stage'], 'run': name,
            'status': checked['status'],
            'runner_returncode': launch_status['returncode'] if launch_status else None,
            'last_frame': max(frames, default=0), 'exit_reached': 'Native developer exit after frame' in log,
            'drawn': int(rejected[-1][0]) if rejected else 0, 'rejected': int(rejected[-1][1]) if rejected else 0,
            'stage': '/'.join(stages[-1]) if stages else None,
            'median_execution_ms': checked['profiles'].get('execution_ms', {}).get('median'),
            'profile_samples': checked['profiles'].get('execution_ms', {}).get('samples', 0),
            'degraded_reasons': degraded,
            'simplified_draws': int(counts[-1][0]) if counts else 0, 'skipped_draws': int(counts[-1][1]) if counts else 0,
            'problems': problems[:5],
            'captures': sorted(p.name for p in out.glob('frame-*.png'))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--prefix', required=True)
    parser.add_argument('--matrix', type=Path, default=HERE / 'location_matrix.json')
    parser.add_argument('--only', help='Comma-separated location names')
    parser.add_argument('--exit-frame', type=int, default=600)
    parser.add_argument('--capture-frames', default='400,600')
    parser.add_argument('--seconds', type=int, default=420)
    parser.add_argument('--renderer', default='vulkan')
    parser.add_argument('--summarize-only', action='store_true', help='Rebuild the summary from existing runs')
    parser.add_argument('--evidence-root', type=Path, default=ROOT / 'Diagnostics/implementation-43/emulator')
    args = parser.parse_args()
    locations = json.loads(args.matrix.read_text())['locations']
    if args.only:
        wanted = set(args.only.split(','))
        unknown = wanted - {l['name'] for l in locations}
        if unknown:
            parser.error('Unknown locations: ' + ', '.join(sorted(unknown)))
        locations = [l for l in locations if l['name'] in wanted]
    # One emulator at a time: several Azahar instances exhaust the owner's RAM.
    results = []
    for location in locations:
        result = run(location, args)
        results.append(result)
        print(f"{result['name']}: {result['status']}, frame={result['last_frame']}", flush=True)
    summary = {'origin': 'Azahar emulator matrix, not physical New3DS', 'image': str(args.image),
               'exit_frame': args.exit_frame, 'results': results}
    target = args.evidence_root.parent / f'matrix-{args.prefix}.json'
    target.write_text(json.dumps(summary, indent=1) + '\n')
    for r in results:
        print(f"{r['name']:18} frame={r['last_frame']:4} exit={int(r['exit_reached'])} stage={r['stage']} "
              f"rejected={r['rejected']} simplified={r['simplified_draws']} skipped={r['skipped_draws']} "
              f"exec={r['median_execution_ms']} problems={r['problems'][:2]}")
    print(target)
    return int(any(r['status'] != 'completed_smoke_test' for r in results))


if __name__ == '__main__':
    raise SystemExit(main())
