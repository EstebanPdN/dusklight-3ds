#!/usr/bin/env python3
"""Capture native font batches and compare exact original quad output."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[2]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);out=p.parse_args().output;out.mkdir(parents=True,exist_ok=True)
    inputs=[ROOT/'Source/platforms/3ds/runtime/font_batch.cpp',ROOT/'Source/platforms/3ds/runtime/font_batch.hpp',ROOT/'Source/platforms/3ds/tests/font_batch_test.cpp',Path(__file__).resolve(),ROOT/'Source/libs/JSystem/include/JSystem/JUtility/JUTFont.h',ROOT/'Source/libs/JSystem/src/JUtility/JUTResFont.cpp',ROOT/'Source/libs/JSystem/src/JUtility/JUTCacheFont.cpp',ROOT/'Source/libs/JSystem/src/J2DGraph/J2DPrint.cpp']
    hashes={str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs}
    common=['-std=c++20','-O2','-g','-DTARGET_PC=1','-DDUSK_TARGET_3DS=1','-I'+str(ROOT/'Source/extern/aurora/include'),'-Wall','-Wextra','-Werror']
    commands=[['clang++',*common,'-fsanitize=address,undefined','-fno-omit-frame-pointer',str(inputs[0]),str(inputs[2]),'-o',str(out/'test')],['/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++',*common,'-march=armv6k','-mtune=mpcore','-mfloat-abi=hard','-mfpu=vfp','-mtp=soft','-D__3DS__','-fstack-usage','-c',str(inputs[0]),'-o',str(out/'font-batch-arm.o')]]
    report={'status':'running','origin':'host captured GX output plus ARM compilation; no raster/hardware acceptance','input_sha256':hashes,'commands':commands}
    try:
        with (out/'build.log').open('w') as log:
            for command in commands:subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
        run=subprocess.run([str(out/'test')],capture_output=True,text=True,timeout=60,env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
        (out/'test.log').write_text(run.stdout+run.stderr);print(run.stdout+run.stderr,end='');run.check_returncode()
        assert hashes=={str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs}
        report.update(status='pass',inputs_unchanged=True)
    except (subprocess.SubprocessError,AssertionError) as e:
        report.update(status='fail',error=str(e));print((out/'build.log').read_text())
    (out/'results.json').write_text(json.dumps(report,indent=2)+'\n');return int(report['status']!='pass')
if __name__=='__main__':raise SystemExit(main())
