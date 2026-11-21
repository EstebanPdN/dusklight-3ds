#!/usr/bin/env python3
"""Check the second-core worker's portable parts: register bank deltas against
a real command processor (every changed word is logged), and the shape queue
in deferred mode and with a consumer thread (order, exact bank, the game
thread taking over what the worker has not drawn), the threaded part also
under ThreadSanitizer."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'Diagnostics/alpha10/gx-worker'

def main():
    global OUT
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,default=OUT)
    OUT=parser.parse_args().output.resolve()
    OUT.mkdir(parents=True,exist_ok=True)
    native=ROOT/'Source/platforms/3ds';aurora=ROOT/'Source/extern/aurora'
    compiled=[native/'tests/gx_worker_test.cpp',native/'runtime/gx_worker.cpp',native/'runtime/gx_commands.cpp',
              native/'runtime/gx_vertex_decode.cpp',native/'runtime/gd_native.cpp',
              aurora/'lib/dolphin/gd/GDGeometry.cpp',aurora/'lib/gx/dl.cpp',aurora/'lib/gx/attr_fmt.cpp']
    inputs=[*compiled,Path(__file__).resolve(),native/'runtime/gx_worker.hpp',native/'runtime/gx_bank_delta.hpp',
            native/'runtime/gx_commands.hpp',native/'runtime/gx_target_extent.hpp']
    common=['-std=c++20','-DTARGET_PC=1','-DDUSK_TARGET_3DS=1','-I'+str(aurora/'include'),'-Wall','-Wextra','-Werror',
            '-fno-strict-aliasing']
    arm=[str(Path(os.environ.get('DEVKITARM',Path.home()/'devkitpro/devkitARM'))/'bin/arm-none-eabi-g++'),*common,'-O2','-march=armv6k',
         '-mtune=mpcore','-mfloat-abi=hard','-mfpu=vfp','-mtp=soft','-fno-short-enums','-D__3DS__','-fstack-usage']
    commands=[['clang++',*common,'-Wno-switch','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
               *map(str,compiled),'-o',str(OUT/'test')],
              ['clang++',*common,'-Wno-switch','-O1','-g','-fsanitize=thread',*map(str,compiled),'-o',str(OUT/'test-tsan')],
              [*arm,'-c',str(native/'runtime/gx_worker.cpp'),'-o',str(OUT/'gx-worker-arm.o')],
              [*arm,'-c',str(native/'runtime/gx_commands.cpp'),'-o',str(OUT/'gx-commands-arm.o')]]
    runs=[[str(OUT/'test')],[str(OUT/'test-tsan'),'threads']]
    hashes={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    report={'status':'running','origin':'synthetic host execution (ASan/UBSan, ThreadSanitizer) and ARM compilation only',
            'input_sha256':hashes,'commands':commands,'runs':runs,
            'limits':['host threads and condition variables stand in for the core 2 thread and its kernel events',
                      'the draw adapter (records and display lists of real shapes) is not part of this test',
                      'no GPU, no cache: cross-core visibility on the console is not shown here']}
    path=OUT/'results.json';path.write_text(json.dumps(report)+'\n')
    try:
        with (OUT/'build.log').open('w') as log:
            for command in commands:subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=300)
        text=''
        for run in runs:
            done=subprocess.run(run,capture_output=True,text=True,timeout=600,
                                env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1',
                                     'UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1','TSAN_OPTIONS':'halt_on_error=1'})
            text+=done.stdout+done.stderr;print(done.stdout+done.stderr,end='');done.check_returncode()
        (OUT/'test.log').write_text(text)
        assert hashes=={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        report.update(status='pass',inputs_unchanged=True,arm_stack_usage={p.name:p.read_text() for p in OUT.glob('*.su')})
    except (OSError,subprocess.SubprocessError,AssertionError) as error:
        report.update(status='fail',error=str(error));print((OUT/'build.log').read_text()[-6000:])
    finally:path.write_text(json.dumps(report,indent=2)+'\n')
    print('test_gx_worker:',report['status'])
    return int(report['status']!='pass')

if __name__=='__main__':raise SystemExit(main())
