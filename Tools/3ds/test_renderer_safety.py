#!/usr/bin/env python3
"""Run owned renderer-safety host regressions with ASan/UBSan, without ARM/emulator work."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    out = parser.parse_args().output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    native = ROOT / 'Source/platforms/3ds'
    aurora = ROOT / 'Source/extern/aurora'
    # Same command adapter fixture/dependencies as test_gx_command_draw.py;
    # this focused runner intentionally has no ARM compiler/build invocation.
    sources = [native / name for name in [
        'runtime/gx_texture_commands.cpp', 'gx_command_draw.cpp',
        'runtime/gx_gpu_geometry.cpp', 'runtime/gx_material.cpp',
        'runtime/gx_pica_material.cpp', 'runtime/gx_raster_plan.cpp',
        'runtime/gx_raster_color.cpp', 'runtime/gd_native.cpp',
        'tests/gx_command_draw_test.cpp', 'runtime/gx_vertex_transform.cpp',
        'runtime/gx_geometry.cpp', 'runtime/gx_pica_math.cpp',
        'runtime/gx_commands.cpp', 'runtime/gx_stream.cpp',
        'runtime/gx_vertex_decode.cpp', 'runtime/gx_rigid_decode.cpp',
        'runtime/gx_transform_commands.cpp', 'runtime/gx_vertex_commands.cpp',
        'runtime/gx_lighting_commands.cpp', 'runtime/gx_light_objects.cpp',
        'runtime/gx_tev_commands.cpp', 'gx_api.cpp', 'gx_command_api.cpp',
        'runtime/gx_state.cpp', 'runtime/resource_archive.cpp']]
    sources += [aurora / name for name in [
        'lib/dolphin/gd/GDGeometry.cpp', 'lib/dolphin/gd/GDTev.cpp',
        'lib/dolphin/gd/GDIndirect.cpp', 'lib/gx/dl.cpp', 'lib/gx/attr_fmt.cpp']]
    inputs = [*sources, Path(__file__).resolve(), ROOT / 'Tools/3ds/test_gx_lookup.py',
              native / 'tests/gx_lookup_test.cpp', native / 'gx_command_draw.hpp',
              *sorted((native / 'runtime').glob('*.hpp'))]
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    command = ['clang++', '-std=c++20', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1',
               '-I' + str(aurora / 'include'), '-Wall', '-Wextra', '-Werror',
               '-Wno-switch', '-O1', '-g', '-fsanitize=address,undefined',
               '-fno-omit-frame-pointer', *map(str, sources), '-o', str(out / 'test')]
    receipt = {'status': 'running', 'command': command, 'sha256': hashes,
               'origin': 'Actual registry and command adapter on host ASan/UBSan; GPU calls substituted',
               'limits': ['No native build, emulator, GPU execution, scene validation or physical acceptance',
                          'Allocation/completion/submit failures injected at adapter backend boundaries']}
    try:
        subprocess.run(['python3', str(ROOT / 'Tools/3ds/test_gx_lookup.py'),
                        '--output', str(out / 'lookup')], check=True, timeout=150)
        with (out / 'build.log').open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=150)
        result = subprocess.run([str(out / 'test')], capture_output=True, text=True, timeout=60,
                                env={**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1',
                                     'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})
        (out / 'test.log').write_text(result.stdout + result.stderr)
        print(result.stdout + result.stderr, end='')
        result.check_returncode()
        assert hashes == {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        receipt.update(status='pass', input_unchanged=True)
    except (OSError, subprocess.SubprocessError, AssertionError) as error:
        receipt.update(status='fail', error=str(error))
        if (out / 'build.log').exists():
            print((out / 'build.log').read_text())
    (out / 'results.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return int(receipt['status'] != 'pass')


if __name__ == '__main__':
    raise SystemExit(main())
