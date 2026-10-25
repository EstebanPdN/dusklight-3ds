#!/usr/bin/env python3
"""Verify native texture cache lifecycle and independent per-unit samplers."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'Diagnostics/implementation-21/texture-backend'
DEVKITPRO = Path(os.environ.get('DEVKITPRO', '/opt/devkitpro'))


def main():
    global OUT
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("--output",type=Path,default=OUT)
    parser.add_argument("--host-only", action="store_true", help="Run host sanitizers only; explicitly omit the separate ARM compile gate")
    args=parser.parse_args();OUT=args.output
    OUT.mkdir(parents=True, exist_ok=False)
    native = ROOT/'Source/platforms/3ds'
    aurora = ROOT/'Source/extern/aurora'
    sources = [native/'tests/gx_texture_backend_test.cpp', native/'gx_texture_pica.cpp', native/'runtime/texture_gx.cpp', native/'runtime/copy_lifecycle_probe.cpp', native/'runtime/fused_color.cpp', native/'runtime/gx_copy_pixels.cpp']
    headers = [native/'gx_texture_pica.hpp', native/'runtime/gx_state.hpp', native/'runtime/texture_gx.hpp',native/'runtime/gpu_sync.hpp',native/'runtime/copy_lifecycle_probe.hpp']
    headers += [native/p for p in ['runtime/fused_color.hpp','runtime/gx_copy_pixels.hpp','runtime/gx_profile.hpp','tests/host_fused_copy_off.hpp']]
    inputs = [*sources, *headers, Path(__file__).resolve()]
    installed_headers = [(DEVKITPRO/'libctru/include')/h for h in ['3ds/types.h','3ds/result.h','3ds/services/gspgpu.h','3ds/gpu/enums.h','c3d/types.h','c3d/texture.h']]
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    common = ['-std=c++20', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1',
              '-I'+str(aurora/'include'), '-I'+str(DEVKITPRO/'libctru/include'), '-Wall', '-Wextra', '-Werror']
    commands = [['clang++', *common, '-O1', '-g',
                 '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                 *map(str,sources), '-o', str(OUT/'test')]]
    if not args.host_only:
        commands += [[str(DEVKITPRO/'devkitARM/bin/arm-none-eabi-g++'), *common,
                 '-O2', '-g', '-march=armv6k', '-mtune=mpcore', '-mfloat-abi=hard',
                 '-mfpu=vfp', '-mtp=soft', '-D__3DS__', '-fstack-usage', '-c',
                 str(native/'gx_texture_pica.cpp'), '-o', str(OUT/'commands-arm.o')]]
    receipt = {'status':'running', 'commands':commands, 'input_sha256':hashes, 'installed_header_sha256':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in installed_headers},
               'origin':('host ASan/UBSan execution only' if args.host_only else 'host ASan/UBSan execution and ARM compilation')+'; no GPU or console execution',
               'arm_gate':{'status':'omitted' if args.host_only else 'requested',
                           'reason':'explicit --host-only; separate ARM gate is parent-owned' if args.host_only else 'default ARM compile gate'},
               'limits':['host substitutes allocation/bind/flush only; installed structs/inline samplers and real decoder are used; no GPU execution',
                         'GPU completion is also substituted; real decoder/cache/guards and stable sampler repair execute, no real GPU timing or physical console execution']}
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
                       arm_stack_usage=None if args.host_only else (OUT/'commands-arm.su').read_text())
    except (OSError, subprocess.SubprocessError, AssertionError) as error:
        receipt.update(status='fail', error=str(error))
        print((OUT/'build.log').read_text())
    finally:
        report.write_text(json.dumps(receipt,indent=2)+'\n')
    return int(receipt['status'] != 'pass')

if __name__ == '__main__':
    raise SystemExit(main())
