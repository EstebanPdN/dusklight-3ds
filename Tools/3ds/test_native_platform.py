#!/usr/bin/env python3
"""Test production arena and clock adapters with synthetic host inputs."""
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'Diagnostics/implementation-02'
BUILD = ROOT / 'Build/3ds-host-contracts'

def main():
    receipt = OUT / 'native-platform-tests.json'
    receipt.write_text(json.dumps({'status': 'in progress; prior result superseded'}) + '\n')
    commands = []
    for name in ['arena', 'time']:
        command = ['clang++', '-std=c++20', '-O1', '-g', '-fsanitize=address,undefined',
                   '-fno-omit-frame-pointer', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1',
                   '-I' + str(ROOT / 'Source/extern/aurora/include')]
        command += [str(ROOT / 'Source/platforms/3ds' / p) for p in
                    [f'runtime/os_{name}.cpp', 'tests/host_primitives.cpp', f'tests/os_{name}_test.cpp']]
        command += ['-o', str(BUILD / f'os-{name}-test')]
        commands.append(command)
        with (OUT / f'{name}-test-build.log').open('w') as log:
            subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
        with (OUT / f'{name}-test.log').open('w') as log:
            subprocess.run([command[-1]], cwd=ROOT, check=True, timeout=30,
                           env={**os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1',
                                'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1'},
                           stdout=log, stderr=subprocess.STDOUT)
    (OUT / 'platform-test-commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    report = {'status': 'pass', 'origin': 'host synthetic; not hardware acceptance',
              'sanitizers': ['address', 'undefined'], 'halt_on_error': True,
              'arena': ['exclusive ownership', 'head/tail alignment', 'overflow rejection', 'exhaustion'],
              'time': ['counter scaling against 128-bit reference', 'pause/resume', 'RTC independence',
                       '32-bit tick wrap', 'Gregorian calendar and leap day'],
              'remaining': ['physical counter/RTC reads', 'APT suspend integration', '124 MiB budget integration']}
    receipt.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
