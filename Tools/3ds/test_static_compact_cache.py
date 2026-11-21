#!/usr/bin/env python3
"""Check the native static geometry cache: compact-palette entries equal the
uncached decode byte for byte, a compact record's replay placement equals the
slow path's, and the stage-change clear protocol."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'Diagnostics/alpha7/static-compact-cache'

def main():
    global OUT
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,default=OUT)
    OUT=parser.parse_args().output.resolve()
    OUT.mkdir(parents=True,exist_ok=True)
    source=ROOT/'Source/platforms/3ds'
    test=source/'tests/gx_static_compact_cache_test.cpp'
    compiled=[test,source/'runtime/gx_vertex_decode.cpp',source/'runtime/gx_rigid_decode.cpp',
              source/'runtime/gx_vertex_transform.cpp',source/'runtime/gx_pica_math.cpp']
    inputs=[*compiled,Path(__file__).resolve(),source/'runtime/gx_compact_cache.hpp',source/'runtime/gx_bytes_equal.hpp',
            source/'runtime/gx_rigid_vertex.hpp',source/'runtime/gx_vertex_decode.hpp',source/'runtime/gx_gpu_geometry.hpp',
            source/'runtime/gx_vertex_warp.hpp',source/'runtime/gx_indirect.hpp']
    common=['-std=c++20','-DTARGET_PC=1','-DDUSK_TARGET_3DS=1',
            '-I'+str(ROOT/'Source/extern/aurora/include'),'-Wall','-Wextra','-Werror']
    arm=[str(Path(os.environ.get('DEVKITARM',Path.home()/'devkitpro/devkitARM'))/'bin/arm-none-eabi-g++'),*common,'-O2','-march=armv6k',
         '-mtune=mpcore','-mfloat-abi=hard','-mfpu=vfp','-mtp=soft','-fno-short-enums','-D__3DS__','-fstack-usage']
    commands=[['clang++',*common,'-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
               *map(str,compiled),'-o',str(OUT/'test')],
              # ARM: the 36-byte key assertion and the 3DS-only clear report.
              [*arm,'-c',str(source/'runtime/gx_rigid_decode.cpp'),'-o',str(OUT/'rigid-decode-arm.o')],
              [*arm,'-c',str(test),'-o',str(OUT/'static-cache-test-arm.o')]]
    hashes={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    report={'status':'running','origin':'synthetic host execution (ASan/UBSan) and ARM compilation only',
            'input_sha256':hashes,'commands':commands,
            'limits':['no GPU submission; resident pool bytes are compared in host memory',
                      'the compact record replay steps (restart, add in palette order, compare both row maps) are restated '
                      'from gx_command_draw.cpp nativeReplay, not executed from that file']}
    path=OUT/'results.json';path.write_text(json.dumps(report)+'\n')
    try:
        with (OUT/'build.log').open('w') as log:
            for command in commands:subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
        run=subprocess.run([str(OUT/'test')],capture_output=True,text=True,timeout=120,
                           env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1',
                                'UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
        (OUT/'test.log').write_text(run.stdout+run.stderr);print(run.stdout+run.stderr,end='');run.check_returncode()
        assert hashes=={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        report.update(status='pass',inputs_unchanged=True,arm_stack_usage={p.name:p.read_text() for p in OUT.glob('*.su')})
    except (OSError,subprocess.SubprocessError,AssertionError) as error:
        report.update(status='fail',error=str(error));print((OUT/'build.log').read_text())
    finally:path.write_text(json.dumps(report,indent=2)+'\n')
    return int(report['status']!='pass')

if __name__=='__main__':raise SystemExit(main())
