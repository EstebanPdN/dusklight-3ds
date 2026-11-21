#!/usr/bin/env python3
"""Per-thread C++ exception state (runtime/eh_globals.cpp) with host threads, and the linked-image check.

Builds Source/platforms/3ds/tests/eh_globals_test.cpp with the production
runtime/eh_globals.cpp under ASan/UBSan and TSan and runs it: one block per
thread, and the exit barrier's unwind shape for six workers (per-thread block:
no foreign exception; one shared block, as libstdc++ uses on the console: some).
With --image and --image-before, check_eh_globals.py must pass on the new image
and fail on the old one. Never builds ARM code or starts an emulator.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
src = root / 'Source'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--image', type=Path, help='linked image built with runtime/eh_globals.cpp (must pass)')
parser.add_argument('--image-before', type=Path, help='image without it, e.g. alpha15-r1.elf (must fail)')
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
files = [src / 'platforms/3ds/tests/eh_globals_test.cpp', src / 'platforms/3ds/runtime/eh_globals.cpp']
checker = root / 'Tools/3ds/check_eh_globals.py'
record = {'status': 'running', 'started_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
          'input_sha256': {str(p.relative_to(root)): sha(p) for p in [*files, checker, Path(__file__).resolve()]},
          'cases': [], 'commands': [],
          'limitations': ['The host C++ runtime is per-thread already: the chain operations are modelled, not libstdc++ itself',
                          'The image check reads ARM branches and stored addresses, not Thumb code']}
report = out / 'results.json'
report.write_text(json.dumps(record, indent=2) + '\n')
try:
    for name, sanitizer in [('asan-ubsan', 'address,undefined'), ('tsan', 'thread')]:
        binary = out / name
        command = ['clang++', '-std=c++20', '-O1', '-g', '-fno-omit-frame-pointer', '-fsanitize=' + sanitizer,
                   *map(str, files), '-o', str(binary)]
        record['commands'].append(command)
        with (out / (name + '-build.log')).open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60,
                             env={**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1',
                                  'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1', 'TSAN_OPTIONS': 'halt_on_error=1'})
        (out / (name + '-test.log')).write_text(run.stdout + run.stderr)
        print(name, run.returncode, run.stdout.strip(), flush=True)
        run.check_returncode()
        record['cases'].append({'name': name, 'exit_code': run.returncode, 'output': run.stdout.strip()})
    for name, image, expected in [('image', args.image, 0), ('image-before', args.image_before, 1)]:
        if not image:
            continue
        command = [sys.executable, '-I', str(checker), str(image.resolve())]
        record['commands'].append(command)
        run = subprocess.run(command, capture_output=True, text=True, timeout=120)
        (out / (name + '.log')).write_text(run.stdout + run.stderr)
        print(name, run.returncode, (run.stdout + run.stderr).strip().splitlines()[-1], flush=True)
        record['cases'].append({'name': name, 'image': str(image), 'exit_code': run.returncode, 'expected': expected})
        if run.returncode != expected:
            raise AssertionError(f'{name}: check_eh_globals.py returned {run.returncode}, expected {expected}')
    record['status'] = 'pass'
except (OSError, subprocess.SubprocessError, AssertionError) as error:
    record.update(status='fail', error=str(error))
    print(error, flush=True)
finally:
    record['finished_at'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    report.write_text(json.dumps(record, indent=2) + '\n')
raise SystemExit(record['status'] != 'pass')
