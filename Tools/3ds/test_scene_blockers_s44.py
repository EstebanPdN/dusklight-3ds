#!/usr/bin/env python3
"""Host-only session44 clip/alpha/safety regressions; no native build or emulator."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
EVIDENCE = ROOT / 'Diagnostics/implementation-44/scene-blockers'
OUT = EVIDENCE / 'final-host'
NATIVE = ROOT / 'Source/platforms/3ds'
AURORA = ROOT / 'Source/extern/aurora'


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    sources = [NATIVE / name for name in '''
        runtime/gx_texture_commands.cpp gx_command_draw.cpp
        runtime/gx_gpu_geometry.cpp runtime/gx_material.cpp
        runtime/gx_pica_material.cpp runtime/gx_raster_plan.cpp
        runtime/gx_raster_color.cpp runtime/gd_native.cpp
        tests/gx_command_draw_test.cpp runtime/gx_vertex_transform.cpp
        runtime/gx_geometry.cpp runtime/gx_pica_math.cpp runtime/gx_commands.cpp
        runtime/gx_stream.cpp runtime/gx_vertex_decode.cpp runtime/gx_rigid_decode.cpp
        runtime/gx_transform_commands.cpp runtime/gx_vertex_commands.cpp
        runtime/gx_lighting_commands.cpp runtime/gx_light_objects.cpp
        runtime/gx_tev_commands.cpp gx_api.cpp gx_command_api.cpp
        runtime/gx_state.cpp runtime/resource_archive.cpp
        '''.split()]
    sources += [AURORA / name for name in '''
        lib/dolphin/gd/GDGeometry.cpp lib/dolphin/gd/GDTev.cpp
        lib/dolphin/gd/GDIndirect.cpp lib/gx/dl.cpp lib/gx/attr_fmt.cpp
        '''.split()]
    math_sources = [NATIVE / 'tests/gx_pica_math_test.cpp', NATIVE / 'runtime/gx_pica_math.cpp']
    before = EVIDENCE / 'before/Source/platforms/3ds/gx_command_draw.cpp'
    inputs = set(sources + math_sources + [before, Path(__file__).resolve()])
    inputs.update(NATIVE.glob('*.hpp'))
    inputs.update((NATIVE / 'runtime').glob('*.hpp'))
    inputs.add(NATIVE / 'tests/gx_rigid_reference.hpp')

    def hashes():
        return {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in sorted(inputs)}

    receipt = {'status': 'running', 'input_sha256': hashes(), 'commands': [],
               'origin': 'Host ASan/UBSan with GPU substitutes; no ARM build, emulator or hardware',
               'limits': ['No PICA raster output or performance evidence',
                          'Guest log captures only 16 of 36 Kakariko vertices and no texture q values; scene advance requires parent rerun']}
    report = OUT / 'results.json'

    def run(name, command, expect_failure=False):
        receipt['commands'].append({'name': name, 'argv': list(map(str, command))})
        result = subprocess.run(list(map(str, command)), capture_output=True, text=True,
                                timeout=90, env={**os.environ,
                                    'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1',
                                    'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})
        output = result.stdout + result.stderr
        (OUT / (name + '.log')).write_text(output)
        if expect_failure:
            assert result.returncode != 0 and 'Assertion failed' in output and 'c.culled==1' in output, output
            assert 'ERROR: AddressSanitizer' not in output and 'runtime error:' not in output, output
        else:
            result.check_returncode()
        return result.returncode

    common = ['clang++', '-std=c++20', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1',
              '-I' + str(AURORA / 'include'), '-I' + str(NATIVE),
              '-Wall', '-Wextra', '-Werror', '-Wno-switch', '-O1', '-g',
              '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    try:
        run('adapter-build', [*common, *sources, '-o', OUT / 'adapter'])
        run('adapter-test', [OUT / 'adapter'])
        run('math-build', [*common, *math_sources, '-o', OUT / 'math'])
        run('math-test', [OUT / 'math'])
        old_sources = [before if p == NATIVE / 'gx_command_draw.cpp' else p for p in sources]
        run('before-build', [*common, *old_sources, '-o', OUT / 'before'])
        receipt['before_expected_failure_returncode'] = run('before-test', [OUT / 'before'], True)
        assert hashes() == receipt['input_sha256'], 'Inputs changed during verification'
        receipt.update(status='pass', input_unchanged=True)
    except (OSError, subprocess.SubprocessError, AssertionError) as error:
        receipt.update(status='fail', error=str(error))
    report.write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({key: value for key, value in receipt.items()
                      if key not in ('commands', 'input_sha256')}, indent=2))
    return int(receipt['status'] != 'pass')


if __name__ == '__main__':
    raise SystemExit(main())
