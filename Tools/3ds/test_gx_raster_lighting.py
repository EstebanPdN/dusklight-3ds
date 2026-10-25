#!/usr/bin/env python3
"""Verify GX raster lighting and selected normal matrices."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'Diagnostics/implementation-22/lighting'


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    native = ROOT/'Source/platforms/3ds'
    aurora = ROOT/'Source/extern/aurora'
    sources = [native/'tests/gx_raster_lighting_test.cpp', native/'runtime/gx_raster_color.cpp',native/'runtime/gx_vertex_transform.cpp',native/'runtime/gx_pica_math.cpp']
    headers = [native/'runtime/gx_pica_material.hpp', native/'runtime/gx_material.hpp',
               native/'runtime/gx_raster_color.hpp',native/'runtime/gx_vertex_transform.hpp',native/'runtime/gx_pica_math.hpp',native/'runtime/gx_commands.hpp']
    inputs = [*sources, *headers, Path(__file__).resolve()]
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    common = ['-std=c++20', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1',
              '-I'+str(aurora/'include'), '-Wall', '-Wextra', '-Werror']
    commands = [['clang++', *common, '-O1', '-g',
                 '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                 *map(str,sources), '-o', str(OUT/'test')],
                ['/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++', *common,
                 '-O2', '-g', '-march=armv6k', '-mtune=mpcore', '-mfloat-abi=hard',
                 '-mfpu=vfp', '-mtp=soft', '-D__3DS__', '-fstack-usage', '-c',
                 str(native/'runtime/gx_raster_color.cpp'), '-o', str(OUT/'commands-arm.o')]]
    receipt = {'status':'running', 'commands':commands, 'input_sha256':hashes,
               'origin':'host ASan/UBSan execution and ARM compilation; no GPU or console execution',
               'limits':['Analytic CPU fixtures and pinned shader semantics; no original environment initialization or hardware fixed-point proof',
                         'no GPU timing or physical console execution']}
    report = OUT/'results.json'
    report.write_text(json.dumps(receipt)+'\n')
    try:
        with (OUT/'build.log').open('w') as log:
            for command in commands:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=90)
        result = subprocess.run([str(OUT/'test')], capture_output=True, text=True, timeout=60,
                                env={**os.environ, 'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1',
                                     'UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
        (OUT/'test.log').write_text(result.stdout+result.stderr)
        print(result.stdout+result.stderr, end='')
        result.check_returncode()
        assert hashes == {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        receipt.update(status='pass', input_unchanged=True,
                       arm_stack_usage=(OUT/'commands-arm.su').read_text())
    except (OSError, subprocess.SubprocessError, AssertionError) as error:
        receipt.update(status='fail', error=str(error))
        print((OUT/'build.log').read_text())
    finally:
        report.write_text(json.dumps(receipt,indent=2)+'\n')
    return int(receipt['status'] != 'pass')

if __name__ == '__main__':
    raise SystemExit(main())
