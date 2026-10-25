#!/usr/bin/env python3
"""Host integration checks. Synthetic input only; no hardware claims."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'Source'
OUT = ROOT / 'Diagnostics/implementation-03'
BUILD = ROOT / 'Build/3ds-host-contracts'

def main():
    global OUT, BUILD
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--case', action='append', choices=['bootstrap-asan','bootstrap-tsan','diagnostics-arm','disc'])
    args = parser.parse_args()
    if args.output: OUT=args.output; BUILD=OUT/'bin'
    OUT.mkdir(parents=True,exist_ok=True); BUILD.mkdir(parents=True,exist_ok=True)
    commands = []
    flags = ['-std=c++20', '-O1', '-g', '-fno-omit-frame-pointer']
    includes = ['include', 'libs/JSystem/include', 'extern/aurora/include',
                'extern/aurora/include/dolphin', 'sdk/include', 'src']
    game = ['-D' + x for x in ['TARGET_PC=1', 'DUSK_TARGET_3DS=1', 'DUSK_BUILDING_GAME=1',
            'VERSION=0', 'PARTIAL_DEBUG=1', 'AVOID_UB=1', 'MTX_USE_PS=1', 'DUSK_TEST_KERNEL=1']]
    game += ['-I' + str(SOURCE / p) for p in includes]
    game += ['-Wno-multichar', '-Wno-deprecated-declarations', '-Wno-invalid-noreturn']
    for p in ['extern/aurora/include/dolphin/types.h', 'include/global.h']:
        game += ['-include', str(SOURCE / p)]
    kernel = ['platforms/3ds/tests/bootstrap_test.cpp', 'platforms/3ds/tests/host_primitives.cpp',
              'platforms/3ds/runtime/bootstrap.cpp', 'platforms/3ds/runtime/os_sync.cpp',
              'platforms/3ds/runtime/os_arena.cpp', 'platforms/3ds/runtime/os_time.cpp',
              'src/helpers/string.cpp', 'libs/JSystem/src/JSupport/JSUList.cpp']
    kernel += ['libs/JSystem/src/JKernel/' + p + '.cpp' for p in
               ['JKRHeap', 'JKRExpHeap', 'JKRSolidHeap', 'JKRDisposer']]
    cases = [('bootstrap-asan', 'address,undefined', game, kernel, []),
             ('bootstrap-tsan', 'thread', game, kernel, []),
             ('diagnostics-arm', 'address,undefined', [], ['platforms/3ds/tests/diagnostics_test.cpp'], []),
             ('disc', 'address,undefined', [], ['platforms/3ds/runtime/disc.cpp',
                                              'platforms/3ds/tests/disc_test.cpp'], [str(BUILD)])]
    if args.case: cases=[c for c in cases if c[0] in args.case]
    inputs=[Path(__file__).resolve(),*[SOURCE/p for p in set(s for c in cases for s in c[3])],
            *[SOURCE/'platforms/3ds/runtime'/p for p in ['bootstrap.hpp','clock.hpp','os_sync.hpp','primitives.hpp']]]
    hashes={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in inputs}
    (OUT/'bootstrap-tests.json').write_text(json.dumps({'status':'running','input_sha256':hashes})+'\n')
    for name, sanitizer, extra, sources, run_args in cases:
        binary = BUILD / (name + '-session03-test')
        command = ['clang++', *flags, '-fsanitize=' + sanitizer, *extra,
                   *[str(SOURCE / p) for p in sources], '-o', str(binary)]
        commands.append(command)
        (OUT / 'bootstrap-test-commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        with (OUT / (name + '-build.log')).open('w') as log:
            subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
        env = {**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1',
               'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1', 'TSAN_OPTIONS': 'halt_on_error=1'}
        result = subprocess.run([str(binary), *run_args], env=env, text=True, capture_output=True, timeout=60)
        (OUT / (name + '-test.log')).write_text(result.stdout + result.stderr)
        print(name, 'exit', result.returncode, result.stdout)
        result.check_returncode()
    (OUT / 'bootstrap-tests.json').write_text(json.dumps({
        'status': 'pass', 'origin': 'synthetic host, not ARM execution',
        'cases': [x[0] for x in cases],
        'input_sha256':hashes,
        'input_unchanged':hashes=={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in inputs}, 'native_primitives': 'host test backend only',
        'hardware_dump_io_and_memory_map': 'UNVERIFIED'}, indent=2) + '\n')

if __name__ == '__main__':
    main()
