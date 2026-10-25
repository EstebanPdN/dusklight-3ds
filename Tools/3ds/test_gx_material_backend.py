#!/usr/bin/env python3
"""Verify Citro3D material register packing and legacy backend equivalence."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'Diagnostics/implementation-16/material-backend'


def main():
    global OUT
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=OUT)
    OUT=parser.parse_args().output
    OUT.mkdir(parents=True, exist_ok=True)
    native = ROOT/'Source/platforms/3ds'
    aurora = ROOT/'Source/extern/aurora'
    sources = [native/'tests/gx_material_backend_test.cpp', native/'gx_material_pica.cpp']
    headers = [native/'gx_material_pica.hpp',native/'gx_blend_pica.hpp',native/'runtime/gx_raster_plan.hpp', native/'runtime/gx_pica_tev.hpp',native/'runtime/gx_pica_material.hpp', native/'runtime/gx_material.hpp',
               native/'tests/material_equivalence.hpp', native/'runtime/gx_commands.hpp']
    inputs = [*sources, *headers, Path(__file__).resolve()]
    installed_headers = [Path('/opt/devkitpro/libctru/include')/h for h in ['3ds/types.h','3ds/gpu/enums.h','c3d/types.h','c3d/texenv.h']]
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    common = ['-std=c++20', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1',
              '-I'+str(aurora/'include'), '-I/opt/devkitpro/libctru/include', '-Wall', '-Wextra', '-Werror']
    commands = [['clang++', *common, '-O1', '-g',
                 '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                 *map(str,sources), '-o', str(OUT/'test')],
                ['/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++', *common,
                 '-O2', '-g', '-march=armv6k', '-mtune=mpcore', '-mfloat-abi=hard',
                 '-mfpu=vfp', '-mtp=soft', '-D__3DS__', '-fstack-usage', '-c',
                 str(native/'gx_material_pica.cpp'), '-o', str(OUT/'commands-arm.o')]]
    receipt = {'status':'running', 'commands':commands, 'input_sha256':hashes, 'installed_header_sha256':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in installed_headers},
               'origin':'host ASan/UBSan execution and ARM compilation; no GPU or console execution',
               'limits':['installed CPU register packers only; GPU execution and indexed submission remain pending',
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
