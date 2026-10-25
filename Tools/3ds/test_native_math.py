#!/usr/bin/env python3
"""Numerically test pinned Aurora C math with native compile policy on the host."""
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'Diagnostics/implementation-02'
BUILD = ROOT / 'Build/3ds-host-contracts/math'

def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    receipt = OUT / 'native-math-tests.json'
    receipt.write_text(json.dumps({'status': 'in progress; prior result superseded'}) + '\n')
    flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
             '-fno-strict-aliasing', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1', '-DMTX_USE_PS=1',
             '-I' + str(ROOT / 'Source/extern/aurora/include')]
    manifest = json.loads((ROOT / 'Source/platforms/3ds/compile/math_sources.json').read_text())
    commands, objects = [], []
    for unit in manifest['sources']:
        source = ROOT / 'Source' / unit['path']
        obj = BUILD / (source.stem + '.o')
        objects.append(str(obj))
        commands.append(['clang', '-std=c11', *flags, '-c', str(source), '-o', str(obj)])
    commands.append(['clang++', '-std=c++20', *flags,
                     str(ROOT / 'Source/platforms/3ds/tests/math_test.cpp'), *objects,
                     '-o', str(BUILD / 'math-test')])
    (OUT / 'math-test-commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    with (OUT / 'math-test-build.log').open('w') as log:
        for command in commands:
            subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
    with (OUT / 'math-test.log').open('w') as log:
        subprocess.run([str(BUILD / 'math-test')], cwd=ROOT, check=True,
                       env={**os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1',
                            'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1'},
                       stdout=log, stderr=subprocess.STDOUT)
    report = {'status': 'pass', 'origin': 'host synthetic; not hardware acceptance',
              'c_implementation_units': len(objects), 'cpp_api_link': True,
              'sanitizers': ['address', 'undefined'], 'halt_on_error': True,
              'checks': ['affine composition', 'aliasing', 'inverse and singular rejection',
                         'matrix stack', 'vector normalization', 'quaternion rotation',
                         'GX projection depth endpoints', 'PPC estimate non-finite/subnormal cases'],
              'not_covered': ['unused MTX44 extension declarations', 'PICA projection conversion',
                              'ARM numerical execution', 'performance']}
    receipt.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
