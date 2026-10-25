#!/usr/bin/env python3
"""Check bounded native vertex decoding on host sanitizers and ARM compiler."""
import hashlib
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'Diagnostics/implementation-08/vertex'

def main():
    global OUT
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,default=OUT)
    OUT=parser.parse_args().output
    OUT.mkdir(parents=True,exist_ok=True)
    source=ROOT/'Source/platforms/3ds'
    inputs=[source/'runtime/gx_vertex_decode.cpp',source/'runtime/gx_vertex_decode.hpp',
            source/'tests/gx_vertex_decode_test.cpp',Path(__file__).resolve(),source/'runtime/gx_rigid_decode.cpp',
            source/'runtime/gx_rigid_vertex.hpp',source/'runtime/gx_gpu_geometry.hpp',source/'runtime/gx_raster_color.hpp',source/'tests/gx_rigid_reference.hpp',source/'runtime/gx_vertex_transform.cpp',source/'runtime/gx_pica_math.cpp',source/'runtime/gx_compact_cache.hpp',source/'runtime/gx_bytes_equal.hpp']
    common=['-std=c++20','-O1','-g','-DTARGET_PC=1','-DDUSK_TARGET_3DS=1',
            '-I'+str(ROOT/'Source/extern/aurora/include'),'-Wall','-Wextra','-Werror']
    commands=[['clang++',*common,'-fsanitize=address,undefined','-fno-omit-frame-pointer',
               str(inputs[0]),str(inputs[2]),str(inputs[4]),str(source/'runtime/gx_vertex_transform.cpp'),str(source/'runtime/gx_pica_math.cpp'),'-o',str(OUT/'test')],
              [str(Path(os.environ.get('DEVKITARM',Path.home()/'devkitpro/devkitARM'))/'bin/arm-none-eabi-g++'),*common,'-O2','-march=armv6k',
               '-mtune=mpcore','-mfloat-abi=hard','-mfpu=vfp','-mtp=soft','-D__3DS__','-fstack-usage',
               '-c',str(inputs[0]),'-o',str(OUT/'vertex-arm.o')]]
    commands.append([*commands[1][:-4],'-c',str(inputs[4]),'-o',str(OUT/'rigid-decode-arm.o')])
    hashes={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    report={'status':'running','origin':'synthetic host execution and ARM compilation only',
            'input_sha256':hashes,'commands':commands,'limits':['no GPU submission or lighting/matrix evaluation',
              'arrays are borrowed spans with caller-owned lifetime','plans are prepared once from validated layouts']}
    path=OUT/'results.json';path.write_text(json.dumps(report)+'\n')
    try:
        with (OUT/'build.log').open('w') as log:
            for command in commands:subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
        run=subprocess.run([str(OUT/'test')],capture_output=True,text=True,timeout=60,
                           env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1',
                                'UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
        (OUT/'test.log').write_text(run.stdout+run.stderr);print(run.stdout+run.stderr,end='');run.check_returncode()
        assert hashes=={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        report.update(status='pass',inputs_unchanged=True,arm_sha256=hashlib.sha256((OUT/'vertex-arm.o').read_bytes()).hexdigest())
    except (subprocess.SubprocessError,AssertionError) as error:
        report.update(status='fail',error=str(error));print((OUT/'build.log').read_text())
    finally:path.write_text(json.dumps(report,indent=2)+'\n')
    return int(report['status']!='pass')

if __name__=='__main__':raise SystemExit(main())
