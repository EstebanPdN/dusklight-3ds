#!/usr/bin/env python3
"""Host test of the logic.txt rules: process id table and shared skin products.

Builds Source/platforms/3ds/tests/native_logic_test.cpp with the pinned Aurora
matrix code, without fused multiply-add (the 3DS VFPv2 has none), with address
and undefined-behaviour sanitizers, and runs it.
"""
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / 'Build/3ds-host-contracts/logic'


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
             '-fno-strict-aliasing', '-ffp-contract=off', '-DTARGET_PC=1', '-DMTX_USE_PS=1',
             '-I' + str(ROOT / 'Source/extern/aurora/include'), '-I' + str(ROOT / 'Source/include')]
    manifest = json.loads((ROOT / 'Source/platforms/3ds/compile/math_sources.json').read_text())
    commands, objects = [], []
    for unit in manifest['sources']:
        source = ROOT / 'Source' / unit['path']
        obj = BUILD / (source.stem + '.o')
        objects.append(str(obj))
        commands.append(['clang', '-std=c11', *flags, '-c', str(source), '-o', str(obj)])
    commands.append(['clang++', '-std=c++20', *flags, '-include',
                     str(ROOT / 'Source/extern/aurora/include/dolphin/types.h'),
                     str(ROOT / 'Source/platforms/3ds/tests/native_logic_test.cpp'), *objects,
                     '-o', str(BUILD / 'native-logic-test')])
    with (BUILD / 'build.log').open('w') as log:
        for command in commands:
            subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
    result = subprocess.run([str(BUILD / 'native-logic-test')], cwd=ROOT, check=True,
                            env={**os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1',
                                 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1'},
                            capture_output=True, text=True)
    (BUILD / 'test.log').write_text(result.stdout + result.stderr)
    # Exhaustive math proofs (billions of inputs): optimized, no sanitizers.
    exhaustive = BUILD / 'native-math-exhaustive-test'
    subprocess.run(['clang++', '-std=c++20', '-O2', '-ffp-contract=off', '-DTARGET_PC=1',
                    '-I' + str(ROOT / 'Source/extern/aurora/include'),
                    str(ROOT / 'Source/platforms/3ds/tests/native_math_exhaustive_test.cpp'),
                    '-o', str(exhaustive)], cwd=ROOT, check=True)
    math = subprocess.run([str(exhaustive)], cwd=ROOT, check=True, capture_output=True, text=True)
    (BUILD / 'math-exhaustive.log').write_text(math.stdout + math.stderr)
    report = {'status': 'pass', 'origin': 'host synthetic and exhaustive; the device verify mode '
              '(logic.txt verify) checks the game code itself',
              'output': result.stdout.strip().splitlines() + math.stdout.strip().splitlines()}
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
