#!/usr/bin/env python3
"""Check native packets (Source/platforms/3ds/runtime/native_packets.hpp)
against the original path: synthetic room-model material packets run through
a captured command stream and the second-core queue's mirror processor, and
through a plain stream; every shape must be drawn in order under the same
bank (all words but the two epochs the seal moves on purpose) and bindings,
the worker's epochs must never go down and must move with every word they
cover, and the game bank must end equal. Scenarios: shipped packets, the
queue's verify mode, the native packet verify mode (no mismatch allowed),
packets off, random register traffic (shipped and verified), and a consumer
thread (also under ThreadSanitizer). The runtime sources are also compiled
for the ARM11."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'Diagnostics/alpha12/native-packets'
SCENARIOS=['shipped','direct','direct-verify','threads-direct','queue-verify','verify','rooms','rooms-verify','off','fuzz','fuzz-verify','threads','threads-verify','threads-fuzz']

def main():
    global OUT
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,default=OUT)
    OUT=parser.parse_args().output.resolve()
    OUT.mkdir(parents=True,exist_ok=True)
    native=ROOT/'Source/platforms/3ds';aurora=ROOT/'Source/extern/aurora'
    compiled=[native/'tests/native_packets_test.cpp',native/'runtime/native_packets.cpp',native/'runtime/gx_stream.cpp',
              native/'runtime/gx_worker.cpp',native/'runtime/gx_commands.cpp',native/'runtime/gx_vertex_decode.cpp',
              native/'runtime/gd_native.cpp',aurora/'lib/dolphin/gd/GDGeometry.cpp',aurora/'lib/gx/dl.cpp',aurora/'lib/gx/attr_fmt.cpp']
    inputs=[*compiled,Path(__file__).resolve(),native/'runtime/native_packets.hpp',native/'runtime/gx_stream.hpp',
            native/'runtime/gx_worker.hpp',native/'runtime/gx_bank_delta.hpp',native/'runtime/gx_commands.hpp',
            native/'runtime/gx_target_extent.hpp']
    common=['-std=c++20','-DTARGET_PC=1','-DDUSK_TARGET_3DS=1','-I'+str(aurora/'include'),'-Wall','-Wextra','-Werror',
            '-fno-strict-aliasing']
    devkit=Path(os.environ.get('DEVKITARM',Path.home()/'devkitpro/devkitARM'))
    arm=[str(devkit/'bin/arm-none-eabi-g++'),*common,'-O3','-march=armv6k','-mtune=mpcore','-mfloat-abi=hard','-mfpu=vfp',
         '-mtp=soft','-fno-short-enums','-D__3DS__','-fstack-usage']
    commands=[['clang++',*common,'-Wno-switch','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
               *map(str,compiled),'-o',str(OUT/'test')],
              ['clang++',*common,'-Wno-switch','-O1','-g','-fsanitize=thread',*map(str,compiled),'-o',str(OUT/'test-tsan')],
              [*arm,'-c',str(native/'runtime/native_packets.cpp'),'-o',str(OUT/'native-packets-arm.o')],
              [*arm,'-c',str(native/'runtime/gx_stream.cpp'),'-o',str(OUT/'gx-stream-arm.o')],
              [*arm,'-c',str(native/'runtime/gx_worker.cpp'),'-o',str(OUT/'gx-worker-arm.o')]]
    runs=[[str(OUT/'test'),name] for name in SCENARIOS]+[[str(OUT/'test-tsan'),name] for name in ('threads','threads-fuzz','threads-direct')]
    hashes={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    report={'status':'running','origin':'synthetic host execution (ASan/UBSan, ThreadSanitizer) and ARM compilation only',
            'input_sha256':hashes,'commands':commands,'runs':runs,
            'limits':['synthetic packets shaped like J3D room material packets; the J3D code itself is not compiled here '
                      '(its calls into the stream are proven at run time by native-packets.txt v)',
                      'host threads stand in for core 2; no GPU, no cache: cross-core visibility on the console is not shown here',
                      'the draw adapter is the test\'s: it notes banks and bindings, it does not plan PICA draws']}
    path=OUT/'results.json';path.write_text(json.dumps(report)+'\n')
    try:
        with (OUT/'build.log').open('w') as log:
            for command in commands:subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=600)
        text=''
        for run in runs:
            done=subprocess.run(run,capture_output=True,text=True,timeout=900,
                                env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1',
                                     'UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1','TSAN_OPTIONS':'halt_on_error=1'})
            line=done.stdout+done.stderr;text+=line;print(line,end='');done.check_returncode()
        (OUT/'test.log').write_text(text)
        assert hashes=={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        report.update(status='pass',inputs_unchanged=True,arm_stack_usage={p.name:p.read_text() for p in OUT.glob('*.su')})
    except (OSError,subprocess.SubprocessError,AssertionError) as error:
        report.update(status='fail',error=str(error));print((OUT/'build.log').read_text()[-6000:])
    finally:path.write_text(json.dumps(report,indent=2)+'\n')
    print('test_native_packets:',report['status'])
    return int(report['status']!='pass')

if __name__=='__main__':raise SystemExit(main())
