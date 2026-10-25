#!/usr/bin/env python3
"""Test native command storage with the actual J3D GD and display-list objects."""
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT/'Diagnostics/implementation-07/gd'


def main():
    OUT.mkdir(parents=True,exist_ok=True)
    native=ROOT/'Source/platforms/3ds'; aurora=ROOT/'Source/extern/aurora'
    common=['-std=c++20','-O1','-g','-DTARGET_PC=1','-DDUSK_TARGET_3DS=1',
            '-fsanitize=address,undefined','-fno-omit-frame-pointer','-Wno-invalid-noreturn']
    includes=['Source/include','Source/libs/JSystem/include','Source/extern/aurora/include',
              'Source/extern/aurora/include/dolphin','Source/sdk/include','Source/src']
    common += ['-I'+str(ROOT/p) for p in includes]
    game=[*common,'-DDUSK_BUILDING_GAME=1','-DVERSION=0','-DPARTIAL_DEBUG=1','-DAVOID_UB=1','-DMTX_USE_PS=1',
          '-include',str(aurora/'include/dolphin/types.h'),'-include',str(ROOT/'Source/include/global.h'),
          '-Wno-multichar','-Wno-deprecated-declarations']
    original=[ROOT/'Source/libs/JSystem/src/J3DGraphBase/J3DGD.cpp',
              ROOT/'Source/libs/JSystem/src/J3DGraphBase/J3DPacket.cpp',native/'tests/gd_packet_test.cpp']
    sources=[native/'runtime/gd_native.cpp',aurora/'lib/dolphin/gd/GDGeometry.cpp',
             aurora/'lib/gx/dl.cpp',aurora/'lib/gx/attr_fmt.cpp',native/'tests/gd_native_test.cpp',native/'runtime/gx_state.cpp',native/'runtime/resource_archive.cpp']
    objects=[OUT/(p.stem+'-host.o') for p in original]
    commands=[['clang++',*game,'-c',str(s),'-o',str(o)] for s,o in zip(original,objects)]
    commands.append(['clang++',*common,*map(str,sources),*map(str,objects),'-Wl,-dead_strip','-o',str(OUT/'test')])
    arm='/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++'
    flags=['-std=c++20','-O2','-g','-march=armv6k','-mtune=mpcore','-mfloat-abi=hard','-mfpu=vfp',
           '-mtp=soft','-D__3DS__','-DTARGET_PC=1','-DDUSK_TARGET_3DS=1','-I'+str(aurora/'include')]
    commands.append([arm,*flags,'-Wall','-Wextra','-Werror','-c',str(sources[0]),'-o',str(OUT/'gd-native-arm.o')])
    commands.append([arm,*flags,'-c',str(sources[1]),'-o',str(OUT/'gd-geometry-arm.o')])
    headers=[aurora/'include/dolphin/gd/GDBase.h',aurora/'include/dolphin/os.h',
             ROOT/'Source/libs/JSystem/include/JSystem/J3DGraphBase/J3DPacket.h']
    inputs=[*original,*sources,*headers,Path(__file__).resolve()]
    hashes={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    receipt={'status':'running','origin':'host ASan/UBSan execution of original J3D/GD methods; ARM compilation only',
             'commands':commands,'input_sha256':hashes,
             'limits':['no native GD execution on ARM/hardware','no display-list GPU executor',
                       'GD callers own buffers and must not share one list concurrently',
                       'unsized/physical arrays and unregistered texture-pointer/TLUT operations fail explicitly']}
    report=OUT/'results.json';report.write_text(json.dumps(receipt)+'\n')
    try:
        with (OUT/'build.log').open('w') as log:
            for cmd in commands:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=90)
        run=subprocess.run([str(OUT/'test')],capture_output=True,text=True,timeout=60,
                           env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1',
                                'UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
        (OUT/'test.log').write_text(run.stdout+run.stderr);print(run.stdout+run.stderr,end='');run.check_returncode()
        assert hashes=={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        receipt.update(status='pass',input_unchanged=True)
    except (subprocess.SubprocessError,AssertionError) as error:
        receipt.update(status='fail',error=str(error));print((OUT/'build.log').read_text())
    finally:report.write_text(json.dumps(receipt,indent=2)+'\n')
    return int(receipt['status']!='pass')

if __name__=='__main__':raise SystemExit(main())
