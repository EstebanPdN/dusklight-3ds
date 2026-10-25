#!/usr/bin/env python3
"""Verify CPU GX command processing with original GD encoders and bounded arrays."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'Diagnostics/implementation-09/commands'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=OUT)
    args = parser.parse_args()
    out = args.output
    out.mkdir(parents=True, exist_ok=False)
    native = ROOT/'Source/platforms/3ds'
    aurora = ROOT/'Source/extern/aurora'
    sources = [native/'tests/gx_commands_test.cpp', native/'runtime/gx_commands.cpp',
               native/'runtime/gx_vertex_decode.cpp', native/'runtime/gd_native.cpp',
               aurora/'lib/dolphin/gd/GDGeometry.cpp', aurora/'lib/gx/dl.cpp',
               aurora/'lib/gx/attr_fmt.cpp']
    headers = [native/'runtime/gx_commands.hpp', native/'runtime/gx_batch_cache.hpp', native/'runtime/gx_copy_setup.hpp', native/'runtime/gx_vertex_decode.hpp',
               aurora/'include/aurora/dl.hpp', aurora/'lib/gx/native_dl_support.hpp',
               aurora/'include/dolphin/gd/GDBase.h', aurora/'include/dolphin/gx/GXAurora.h']
    inputs = [*sources, *headers, Path(__file__).resolve()]
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    common = ['-std=c++20', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1',
              '-I'+str(aurora/'include'), '-Wall', '-Wextra', '-Werror']
    commands = [['clang++', *common, '-Wno-switch', '-O1', '-g',
                 '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                 *map(str,sources), '-o', str(out/'test')],
                ['/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++', *common,
                 '-O2', '-g', '-march=armv6k', '-mtune=mpcore', '-mfloat-abi=hard',
                 '-mfpu=vfp', '-mtp=soft', '-D__3DS__', '-fstack-usage', '-c',
                 str(native/'runtime/gx_commands.cpp'), '-o', str(out/'commands-arm.o')]]
    receipt = {'status':'running', 'commands':commands, 'input_sha256':hashes,
               'origin':'host ASan/UBSan execution and ARM compilation; no GPU or console execution',
               'limits':['CPU draw callbacks only; matrices/materials/GPU submission remain pending',
                         'registry borrows memory; callers must preserve lifetime and exclusive access',
                         'upstream GDGeometry switch-enum warnings disabled for host combined build']}
    report = out/'results.json'
    report.write_text(json.dumps(receipt)+'\n')
    try:
        with (out/'build.log').open('w') as log:
            for command in commands:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=90)
        result = subprocess.run([str(out/'test')], capture_output=True, text=True, timeout=60,
                                env={**os.environ, 'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1',
                                     'UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
        (out/'test.log').write_text(result.stdout+result.stderr)
        print(result.stdout+result.stderr, end='')
        result.check_returncode()
        assert hashes == {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        receipt.update(status='pass', input_unchanged=True,
                       arm_stack_usage=(out/'commands-arm.su').read_text())
    except (OSError, subprocess.SubprocessError, AssertionError) as error:
        receipt.update(status='fail', error=str(error))
        print((out/'build.log').read_text())
    finally:
        report.write_text(json.dumps(receipt,indent=2)+'\n')
    return int(receipt['status'] != 'pass')

if __name__ == '__main__':
    raise SystemExit(main())
