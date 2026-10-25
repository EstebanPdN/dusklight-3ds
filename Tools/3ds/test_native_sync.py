#!/usr/bin/env python3
"""Run the production native OS synchronization algorithm with host primitives."""
import json
import argparse
import hashlib
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'Diagnostics/implementation-02'
BUILD = ROOT / 'Build/3ds-host-contracts'

def main():
    global OUT,BUILD
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    if args.output:OUT=args.output;BUILD=OUT/'bin'
    OUT.mkdir(parents=True,exist_ok=True);BUILD.mkdir(parents=True,exist_ok=True)
    receipt = OUT / 'native-sync-tests.json'
    receipt.write_text(json.dumps({'status': 'in progress; prior result superseded'}) + '\n')
    command = ['clang++', '-std=c++20', '-O1', '-g', '-fsanitize=address,undefined',
               '-fno-omit-frame-pointer', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1',
               '-I' + str(ROOT / 'Source/extern/aurora/include'),
               '-I' + str(ROOT / 'Source/extern/aurora/include/dolphin'),
               '-I' + str(ROOT / 'Source/libs/JSystem/include')]
    command += [str(ROOT / 'Source/platforms/3ds' / p) for p in
                ['runtime/os_sync.cpp','runtime/jas_critical_section.cpp', 'tests/host_primitives.cpp', 'tests/os_sync_test.cpp']]
    command += ['-o', str(BUILD / 'os-sync-test')]
    (OUT / 'sync-test-command.json').write_text(json.dumps(command, indent=2) + '\n')
    with (OUT / 'sync-test-build.log').open('w') as log:
        subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
    with (OUT / 'sync-test.log').open('w') as log:
        subprocess.run([str(BUILD / 'os-sync-test')], cwd=ROOT, check=True, timeout=30,
                       env={**os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1',
                            'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1'},
                       stdout=log, stderr=subprocess.STDOUT)
    report = {'status': 'pass', 'origin': 'host synthetic; not hardware acceptance',
              'implementation': 'production os_sync.cpp; test-only host primitive adapter',
              'sanitizers': ['address', 'undefined'], 'halt_on_error': True,
              'input_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in
                              [Path(__file__),*[Path(p) for p in command if p.endswith('.cpp')]]},
              'checks': ['JAS first-use contention, nested scopes and exception release across 8 threads',
                         'recursive ownership', 'try-lock contention', 'non-owner unlock',
                         'condition releases/restores recursion atomically', 'FIFO/jam/full/empty',
                         '50000 messages including four concurrent producers',
                         'queue close wakes senders/receivers and preserves queued messages'],
              'remaining': ['libctru execution', 'thread creation/cancellation integration',
                            'thread sanitizer run']}
    receipt.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
