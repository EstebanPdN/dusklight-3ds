#!/usr/bin/env python3
"""Run actual pinned J2D primitive classes against the bounded native GX capture."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'Diagnostics/implementation-05/gx-state'
BUILD = OUT / 'build'

def main():
    global OUT, BUILD
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,help='Fresh evidence directory; default preserves the legacy location')
    args=parser.parse_args()
    if args.output:
        OUT=args.output.resolve();OUT.mkdir(parents=True,exist_ok=False);BUILD=OUT/'build'
    BUILD.mkdir(parents=True, exist_ok=True)
    receipt = OUT / 'receipt.json'
    receipt.write_text(json.dumps({'status': 'in progress; prior result superseded'}) + '\n')
    native = ROOT / 'Source/platforms/3ds'
    flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
             '-fno-strict-aliasing', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1', '-DMTX_USE_PS=1',
             '-I' + str(ROOT / 'Source/extern/aurora/include')]
    includes = ['Source/include', 'Source/libs/JSystem/include', 'Source/extern/aurora/include/dolphin',
                'Source/sdk/include', 'Source/src']
    game_flags = [*flags, '-DDUSK_BUILDING_GAME=1', '-DVERSION=0', '-DPARTIAL_DEBUG=1', '-DAVOID_UB=1',
                  *['-I' + str(ROOT / p) for p in includes],
                  '-include', str(ROOT / 'Source/extern/aurora/include/dolphin/types.h'),
                  '-include', str(ROOT / 'Source/include/global.h'),
                  '-Wno-multichar', '-Wno-deprecated-declarations', '-Wno-invalid-noreturn']
    commands, objects, sources = [], [], []
    manifest = json.loads((native / 'compile/math_sources.json').read_text())
    for unit in manifest['sources']:
        source = ROOT / 'Source' / unit['path']; obj = BUILD / (source.stem + '.o')
        objects.append(str(obj)); sources.append(source)
        commands.append(['clang', '-std=c11', *flags, '-c', str(source), '-o', str(obj)])
    for name in ['J2DGrafContext', 'J2DOrthoGraph']:
        source = ROOT / 'Source/libs/JSystem/src/J2DGraph' / (name + '.cpp'); obj = BUILD / (name + '.o')
        objects.append(str(obj)); sources.append(source)
        commands.append(['clang++', '-std=c++20', *game_flags, '-c', str(source), '-o', str(obj)])
    for relative in ['runtime/gx_state.cpp', 'runtime/resource_archive.cpp', 'runtime/gx_tev_commands.cpp', 'gx_api.cpp', 'runtime/gx_transform_commands.cpp','runtime/gx_vertex_commands.cpp','runtime/gx_lighting_commands.cpp', 'runtime/gx_stream.cpp', 'runtime/gx_commands.cpp', 'runtime/gx_vertex_decode.cpp', 'gx_command_api.cpp', '../../extern/aurora/lib/gx/dl.cpp', '../../extern/aurora/lib/gx/attr_fmt.cpp']:
        source = native / relative; obj = BUILD / (source.stem + '.o')
        objects.append(str(obj)); sources.append(source)
        commands.append(['clang++', '-std=c++20', *flags, '-Wall', '-Wextra', '-Werror', '-c', str(source), '-o', str(obj)])
    test = native / 'tests/gx_state_test.cpp'; sources.extend([test, native / 'runtime/gx_state.hpp'])
    commands.append(['clang++', '-std=c++20', *game_flags, str(test), *objects, '-o', str(BUILD / 'gx-state-test')])
    arm_flags = ['-std=gnu++20', '-O2', '-g', '-march=armv6k', '-mtune=mpcore', '-mfloat-abi=hard',
                 '-mfpu=vfp', '-mtp=soft', '-D__3DS__', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1',
                 '-Wall', '-Wextra', '-Werror', '-fno-strict-aliasing', '-fstack-usage',
                 '-I' + str(ROOT / 'Source/extern/aurora/include')]
    for relative in ['runtime/gx_state.cpp', 'gx_api.cpp']:
        source = native / relative
        commands.append(['/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++', *arm_flags, '-c', str(source),
                         '-o', str(BUILD / (source.stem + '-arm.o'))])
    (OUT / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    try:
        with (OUT / 'build.log').open('w') as log:
            for command in commands:
                subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
        with (OUT / 'test.log').open('w') as log:
            subprocess.run([str(BUILD / 'gx-state-test')], cwd=ROOT, check=True,
                           env={**os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1',
                                'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1'}, stdout=log, stderr=subprocess.STDOUT)
    except subprocess.CalledProcessError as exc:
        receipt.write_text(json.dumps({'status': 'failed', 'command': exc.cmd, 'returncode': exc.returncode}, indent=2) + '\n')
        print((OUT / 'build.log').read_text()); print((OUT / 'test.log').read_text() if (OUT / 'test.log').exists() else '')
        return 1
    report = {'status': 'pass', 'origin': 'host integration; actual original J2D sources; not hardware or PICA acceptance',
              'sanitizers': ['address', 'undefined'], 'halt_on_error': True, 'native_warnings_as_errors': True,
              'armv6k_hardfloat_compile': 'pass; GX state and typed API only; not ARM execution',
              'max_vertices': 4096,
              'checks': ['original J2DGrafContext/J2DOrthoGraph fillBox/drawFrame/line', 'RGBA big-endian numeric decoding',
                         'orthographic GX projection endpoints', 'opaque/translucent blend', 'scissor rounding',
                         'six primitive captures', '4096 vertices accepted; 4097 rejected', 'incomplete/excess vertices',
                         'active unsupported state rejection; unused setup accepted', 'matrix caller lifetime',
                         'optional ordered UV capture', 'nonfinite and command order rejection',
                         'backend rejection/missing sink counters', 'reset preserves sink and texture objects',
                         'every truncated I4 BTI prefix; duplicate span rejection',
                         '64 registration and 128 texture-object capacity limits',
                         'raw unregistered pointer never dereferenced', 'loaded sampler snapshot and valid LOD recovery',
                         'destroy/reinitialize/unregister generation invalidation', 'signed S16 UV fraction eight',
                         'third TEV stage, multiple texgens and unsupported TEV rejected',
                         'indexed metadata accepted; unregistered image binding and TLUT rejected'],
              'source_sha256': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in [Path(__file__).resolve(), *sources]},
              'not_covered': ['PICA drawing', '3DS hardware execution',
                              'original J2DPicture (separate picture/results.json)', 'J2DScreen/BLO', 'gameplay']}
    receipt.write_text(json.dumps(report, indent=2) + '\n')
    print((OUT / 'test.log').read_text()); print(json.dumps(report, indent=2)); return 0

if __name__ == '__main__':
    sys.exit(main())
