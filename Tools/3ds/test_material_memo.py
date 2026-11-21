#!/usr/bin/env python3
"""Verify the J2D material memo against the real native typed GX setters."""
import argparse, hashlib, json, os, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    out = parser.parse_args().output
    out.mkdir(parents=True, exist_ok=False)
    n = ROOT/'Source/platforms/3ds'; aurora = ROOT/'Source/extern/aurora'
    sources = [n/x for x in ['runtime/gx_pixel_commands.cpp', 'runtime/gx_stream.cpp', 'runtime/gx_tev_commands.cpp',
                             'gx_api.cpp', 'gx_command_api.cpp', 'runtime/gx_raster_plan.cpp', 'runtime/gx_pica_math.cpp',
                             'runtime/gx_transform_commands.cpp', 'runtime/gx_vertex_commands.cpp',
                             'runtime/gx_lighting_commands.cpp', 'runtime/gx_state.cpp', 'runtime/gx_commands.cpp',
                             'runtime/gx_vertex_decode.cpp', 'runtime/resource_archive.cpp', 'runtime/gd_native.cpp',
                             'tests/material_memo_test.cpp']]
    sources += [aurora/x for x in ['lib/gx/dl.cpp', 'lib/gx/attr_fmt.cpp', 'lib/dolphin/gd/GDGeometry.cpp',
                                   'lib/dolphin/gd/GDPixel.cpp', 'lib/dolphin/gd/GDTev.cpp', 'lib/dolphin/gd/GDIndirect.cpp']]
    headers = [n/x for x in ['runtime/gx_commands.hpp', 'runtime/gx_stream.hpp', 'runtime/gx_tev_commands.hpp',
                             'runtime/gx_lighting_commands.hpp', 'runtime/gx_transform_commands.hpp', 'runtime/gx_state.hpp']]
    inputs = [Path(__file__).resolve(), *sources, *headers]
    hashes = {str(x.relative_to(ROOT)): hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs}
    common = ['-std=c++20', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1', '-I'+str(aurora/'include'),
              '-Wall', '-Wextra', '-Werror', '-Wno-switch']
    # devkitARM: $DEVKITARM, /opt/devkitpro or ~/devkitpro.
    candidates = [Path(os.environ['DEVKITARM'])/'bin/arm-none-eabi-g++'] if os.environ.get('DEVKITARM') else []
    candidates += [Path('/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++'),
                   Path.home()/'devkitpro/devkitARM/bin/arm-none-eabi-g++']
    arm = str(next((c for c in candidates if c.exists()), candidates[0]))
    commands = [['clang++', *common, '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                 *map(str, sources), '-o', str(out/'test')],
                [arm, *common, '-O2', '-g', '-march=armv6k', '-mtune=mpcore', '-mfloat-abi=hard', '-mfpu=vfp',
                 '-mtp=soft', '-D__3DS__', '-fstack-usage', '-c', str(n/'runtime/gx_commands.cpp'),
                 '-o', str(out/'commands-arm.o')]]
    receipt = {'status': 'running', 'commands': commands, 'input_sha256': hashes,
               'origin': 'host ASan/UBSan execution of the native typed setters and ARM compilation; no GPU or console execution',
               'limits': ['texture loads are modelled at the register level of commandTextureBinding',
                          'the J2D call order is copied from J2DMatBlock.cpp, not compiled from it',
                          'speed is not measured here: see the j2d.material profile scopes on the console']}
    report = out/'results.json'
    report.write_text(json.dumps(receipt)+'\n')
    try:
        with (out/'build.log').open('w') as log:
            for command in commands:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)
        result = subprocess.run([str(out/'test')], capture_output=True, text=True, timeout=120,
                                env={**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1',
                                     'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})
        (out/'test.log').write_text(result.stdout+result.stderr)
        print(result.stdout+result.stderr, end='')
        result.check_returncode()
        assert hashes == {str(x.relative_to(ROOT)): hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs}
        receipt.update(status='pass', input_unchanged=True, arm_stack_usage=(out/'commands-arm.su').read_text())
    except (OSError, subprocess.SubprocessError, AssertionError) as error:
        receipt.update(status='fail', error=str(error))
        print((out/'build.log').read_text())
    finally:
        report.write_text(json.dumps(receipt, indent=2)+'\n')
    return int(receipt['status'] != 'pass')


if __name__ == '__main__':
    raise SystemExit(main())
