#!/usr/bin/env python3
"""Verify the original-command draw adapter with bounded host GPU substitutes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'Diagnostics/implementation-22/command-draw'


def main():
    global OUT
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path);args=parser.parse_args()
    if args.output:OUT=args.output
    OUT.mkdir(parents=True, exist_ok=True)
    native = ROOT/'Source/platforms/3ds'
    aurora = ROOT/'Source/extern/aurora'
    sources = [native/'runtime/gx_texture_commands.cpp',native/'gx_command_draw.cpp',native/'runtime/gx_gpu_geometry.cpp',native/'runtime/gx_material.cpp',native/'runtime/gx_pica_material.cpp',native/'runtime/gx_raster_plan.cpp',native/'runtime/gx_raster_color.cpp',native/'runtime/gd_native.cpp',aurora/'lib/dolphin/gd/GDGeometry.cpp',aurora/'lib/dolphin/gd/GDTev.cpp',aurora/'lib/dolphin/gd/GDIndirect.cpp',native/'tests/gx_command_draw_test.cpp',native/'runtime/gx_vertex_transform.cpp',native/'runtime/gx_geometry.cpp',native/'runtime/gx_pica_math.cpp',
               native/'runtime/gx_commands.cpp',native/'runtime/gx_stream.cpp',native/'runtime/gx_vertex_decode.cpp',native/'runtime/gx_rigid_decode.cpp',
               native/'runtime/gx_transform_commands.cpp',native/'runtime/gx_vertex_commands.cpp',native/'runtime/gx_lighting_commands.cpp',native/'runtime/gx_light_objects.cpp',
               native/'runtime/gx_tev_commands.cpp',native/'gx_api.cpp',native/'gx_command_api.cpp',native/'runtime/gx_state.cpp',native/'runtime/resource_archive.cpp',
               aurora/'lib/gx/dl.cpp',aurora/'lib/gx/attr_fmt.cpp']
    headers = [native/'runtime/gx_texture_commands.hpp',native/'gx_command_draw.hpp',native/'gx_geometry_pica.hpp',native/'gx_texture_pica.hpp',native/'gx_pica.hpp',native/'runtime/gx_gpu_geometry.hpp',native/'runtime/gx_raster_plan.hpp',native/'runtime/gx_raster_color.hpp',native/'runtime/gx_pica_material.hpp',native/'runtime/gx_material.hpp',native/'runtime/gx_geometry.hpp',native/'runtime/gx_vertex_transform.hpp',native/'runtime/gx_vertex_decode.hpp',native/'runtime/gx_commands.hpp',
               native/'runtime/gx_pica_math.hpp',native/'runtime/gx_state.hpp',native/'runtime/gx_stream.hpp']
    headers.append(native/'runtime/gx_copy_setup.hpp')
    headers.append(native/'runtime/gx_batch_cache.hpp')
    headers.extend([native/'runtime/gx_fog.hpp',native/'runtime/gx_fog_pass.hpp'])
    headers.append(native/'runtime/gx_indirect.hpp')
    headers.append(native/'runtime/gx_early_alpha.hpp')
    headers.append(native/'runtime/gx_material_liveness.hpp')
    headers.append(native/'runtime/gx_profile.hpp')
    headers.append(native/'runtime/gx_rigid_vertex.hpp')
    headers.append(native/'runtime/gx_clip_geometry.hpp')
    headers.append(native/'runtime/gx_camera_clip.hpp')
    headers.extend([native/'runtime/gx_i4_quarter_bake67.hpp',native/'runtime/gx_i4_bake_cache68.hpp'])
    headers.append(native/'tests/gx_rigid_reference.hpp')
    inputs = [*sources, *headers, Path(__file__).resolve()]
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    common = ['-std=c++20', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1',
              '-I'+str(aurora/'include'), '-Wall', '-Wextra', '-Werror','-Wno-switch']
    devkitpro = Path(os.environ.get('DEVKITPRO') or
                    next((p for p in (Path.home()/'devkitpro', Path('/opt/devkitpro')) if p.is_dir()), '/opt/devkitpro'))
    arm = [str(devkitpro/'devkitARM/bin/arm-none-eabi-g++'),*common,'-O2','-g','-march=armv6k','-mtune=mpcore',
           '-mfloat-abi=hard','-mfpu=vfp','-mtp=soft','-D__3DS__','-fstack-usage']
    commands = [['clang++', *common, '-O1', '-g','-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                 *map(str,sources), '-o', str(OUT/'test')]]
    for name in ['runtime/gx_texture_commands.cpp','runtime/gx_raster_plan.cpp','runtime/gx_raster_color.cpp','gx_command_draw.cpp']:
        commands.append([*arm,'-c',str(native/name),'-o',str(OUT/(Path(name).stem+'-arm.o'))])
    receipt = {'status':'running', 'commands':commands, 'input_sha256':hashes,
               'origin':'host ASan/UBSan execution and ARM compilation; no GPU or console execution',
               'limits':['Host substitutes GPU reserve/bind/submit calls; actual command/geometry/material/state code executes; no GPU or original scene',
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
                       arm_stack_usage={p.name:p.read_text() for p in OUT.glob('*.su')})
    except (OSError, subprocess.SubprocessError, AssertionError) as error:
        receipt.update(status='fail', error=str(error))
        print((OUT/'build.log').read_text())
    finally:
        report.write_text(json.dumps(receipt,indent=2)+'\n')
    return int(receipt['status'] != 'pass')

if __name__ == '__main__':
    raise SystemExit(main())
