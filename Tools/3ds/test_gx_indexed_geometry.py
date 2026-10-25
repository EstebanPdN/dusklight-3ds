#!/usr/bin/env python3
"""Verify indexed GPU arena, prepared packing, native compilation and shader assembly."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
DEVKITPRO = Path(os.environ.get('DEVKITPRO') or next((p for p in (Path.home()/'devkitpro',Path('/opt/devkitpro')) if p.is_dir()), '/opt/devkitpro'))
OUT = ROOT / 'Diagnostics/implementation-19/indexed-geometry'


def main():
    global OUT
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("--output",type=Path,default=OUT)
    OUT=parser.parse_args().output
    OUT.mkdir(parents=True, exist_ok=True)
    native = ROOT/'Source/platforms/3ds'
    aurora = ROOT/'Source/extern/aurora'
    sources = [native/'tests/gx_gpu_geometry_test.cpp', native/'runtime/gx_gpu_geometry.cpp', native/'runtime/gx_geometry_pack.cpp']
    headers = [native/'runtime/gx_gpu_geometry.hpp',native/'runtime/gx_geometry_pack.hpp',native/'runtime/gx_geometry.hpp',
               native/'runtime/gx_vertex_transform.hpp',native/'runtime/gx_pica_material.hpp',native/'runtime/gx_fog_pass.hpp']
    inputs = [*sources,*headers,Path(__file__).resolve(),native/'gx_geometry_pica.cpp',native/'gx_geometry_pica.hpp',
              native/'gx_pica.cpp',native/'gx_color.v.pica',native/'gx_rigid.v.pica',native/'main.cpp',native/'CMakeLists.txt',
              native/'tests/gx_geometry_flush_test.cpp',native/'runtime/gpu_sync_ctru.cpp',native/'runtime/gx_framebuffer_ctru.cpp']
    installed_headers = [(DEVKITPRO/'libctru/include')/h for h in ['3ds/types.h','3ds/gpu/enums.h','c3d/types.h','c3d/buffers.h','c3d/attribs.h','c3d/base.h','c3d/renderqueue.h']]
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    common = ['-std=c++20', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1',
              '-I'+str(aurora/'include'), '-I'+str(DEVKITPRO/'libctru/include'), '-Wall', '-Wextra', '-Werror']
    arm = [str(DEVKITPRO/'devkitARM/bin/arm-none-eabi-g++'),*common,'-O2','-g','-march=armv6k','-mtune=mpcore',
           '-mfloat-abi=hard','-mfpu=vfp','-mtp=soft','-D__3DS__','-fstack-usage']
    commands = [['clang++', *common, '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                 *map(str,sources), '-o', str(OUT/'test')]]
    commands.append(['clang++',*common,'-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                     str(native/'gx_geometry_pica.cpp'),str(native/'runtime/gx_gpu_geometry.cpp'),
                     str(native/'tests/gx_geometry_flush_test.cpp'),'-o',str(OUT/'flush-test')])
    for name in ['runtime/gx_gpu_geometry.cpp','runtime/gx_geometry_pack.cpp','gx_geometry_pica.cpp','gx_pica.cpp']:
        commands.append([*arm,'-c',str(native/name),'-o',str(OUT/(Path(name).stem+'-arm.o'))])
    commands.append([str(DEVKITPRO/'tools/bin/picasso'),'-o',str(OUT/'gx_color.shbin'),str(native/'gx_color.v.pica')])
    commands.append([str(DEVKITPRO/'tools/bin/picasso'),'-o',str(OUT/'gx_rigid.shbin'),str(native/'gx_rigid.v.pica')])
    receipt = {'status':'running', 'commands':commands, 'input_sha256':hashes, 'installed_header_sha256':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in installed_headers},
               'origin':'host ASan/UBSan execution and ARM compilation; no GPU or console execution',
               'limits':['host validates arena ownership and packing only; real indexed GPU path tested separately in Azahar',
                         'three UV outputs assemble but multi-unit sampling and projective support are not GPU-validated',
                         'no physical console timing or game execution']}
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
        flush_result=subprocess.run([str(OUT/'flush-test')],capture_output=True,text=True,timeout=60,
                                    env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
        (OUT/'flush-test.log').write_text(flush_result.stdout+flush_result.stderr)
        print(flush_result.stdout+flush_result.stderr,end='');flush_result.check_returncode()
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
