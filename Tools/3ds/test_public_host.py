#!/usr/bin/env python3
"""Run enumerated synthetic production fixtures without a disc, SDK or emulator."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--output', type=Path, default=ROOT / 'Build/public-host')
    args = parser.parse_args()
    compiler = shutil.which(args.compiler)
    if not compiler:
        parser.error('A C++20 host compiler is required')
    output = args.output.expanduser().resolve()
    output.mkdir(parents=True, exist_ok=True)
    # Each invocation owns a fresh directory; no old binary or result can pass.
    work = Path(tempfile.mkdtemp(prefix='run-', dir=output))
    source = ROOT / 'Source'
    native = source / 'platforms/3ds'
    receipt = {'status': 'running', 'scope': 'synthetic host only; no ARM, PICA execution, disc or hardware',
               'commands': [], 'cases': [], 'compiler': compiler}
    report = work / 'results.json'
    env = {**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1',
           'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}

    def run(name, command, timeout=120):
        start = time.monotonic()
        result = subprocess.run(list(map(str, command)), cwd=ROOT, env=env, capture_output=True, text=True, timeout=timeout)
        (work / (name + '.log')).write_text(result.stdout + result.stderr)
        receipt['commands'].append({'name': name, 'argv': list(map(str, command)),
                                    'returncode': result.returncode, 'seconds': round(time.monotonic() - start, 3)})
        report.write_text(json.dumps(receipt, indent=2) + '\n')
        if result.returncode:
            print(result.stdout + result.stderr, end='')
            result.check_returncode()

    flags = ['-std=c++20', '-O1', '-g', '-fsanitize=address,undefined',
             '-fno-omit-frame-pointer', '-ffunction-sections', '-fdata-sections', '-pthread']
    dead = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
    cases = [
        ('disc-bounds', [native / 'tests/disc_test.cpp', native / 'runtime/disc.cpp'], [], [work]),
        ('texture-i4', [native / 'tests/texture_i4_test.cpp', native / 'runtime/texture_i4.cpp'], [], []),
        ('texture-ia8', [native / 'tests/texture_ia8_test.cpp', native / 'runtime/texture_ia8.cpp'], [], []),
        ('pica-math', [native / 'tests/gx_pica_math_test.cpp', native / 'runtime/gx_pica_math.cpp'], [], []),
        ('boot-stage', [native / 'tests/boot_stage_test.cpp'], [], []),
    ]
    kernel = ['platforms/3ds/tests/host_primitives.cpp', 'platforms/3ds/runtime/bootstrap.cpp',
              'platforms/3ds/runtime/os_sync.cpp', 'platforms/3ds/runtime/os_arena.cpp',
              'platforms/3ds/runtime/os_time.cpp', 'src/helpers/string.cpp',
              'libs/JSystem/src/JSupport/JSUList.cpp']
    kernel += ['libs/JSystem/src/JKernel/' + x + '.cpp' for x in
               ['JKRHeap', 'JKRExpHeap', 'JKRSolidHeap', 'JKRDisposer']]
    game = ['-D' + x for x in ['TARGET_PC=1', 'DUSK_TARGET_3DS=1', 'DUSK_BUILDING_GAME=1',
                               'VERSION=0', 'PARTIAL_DEBUG=1', 'AVOID_UB=1', 'MTX_USE_PS=1', 'DUSK_TEST_KERNEL=1']]
    game += ['-I' + str(source / x) for x in ['include', 'libs/JSystem/include',
             'extern/aurora/include', 'extern/aurora/include/dolphin', 'sdk/include', 'src']]
    game += ['-include', str(source / 'extern/aurora/include/dolphin/types.h'),
             '-include', str(source / 'include/global.h'), '-Wno-multichar', '-Wno-deprecated-declarations']
    cases.append(('bootstrap', [native / 'tests/bootstrap_test.cpp', *[source / x for x in kernel]], game, []))
    pad = ['platforms/3ds/runtime/' + x + '.cpp' for x in ['pad_mapping', 'pad_clamp', 'pad_bootstrap']]
    pad += ['libs/JSystem/src/JUtility/JUTGamePad.cpp', 'src/m_Do/m_Do_controller_pad.cpp']
    cases.append(('native-pad', [native / 'tests/native_pad_test.cpp', *[source / x for x in kernel + pad]], game, []))
    try:
        for name, sources, extra, arguments in cases:
            binary = work / name
            run(name + '-build', [compiler, *flags, *extra, *sources, dead, '-o', binary], 180)
            run(name, [binary, *arguments], 120)
            receipt['cases'].append(name)
            print('PASS: ' + name, flush=True)
        run('dump-verifier', [sys.executable, ROOT / 'Tools/3ds/test_application_dump_verifier.py'], 180)
        receipt['cases'].append('dump-verifier')
        print('PASS: dump-verifier', flush=True)
        receipt['status'] = 'pass'
    except (OSError, subprocess.SubprocessError) as error:
        receipt.update(status='fail', error=str(error))
    report.write_text(json.dumps(receipt, indent=2) + '\n')
    print(receipt['status'].upper() + ': ' + str(report))
    return int(receipt['status'] != 'pass')


if __name__ == '__main__':
    raise SystemExit(main())
