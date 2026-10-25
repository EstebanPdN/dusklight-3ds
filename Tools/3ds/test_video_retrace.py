#!/usr/bin/env python3
"""Test native retrace/sleep contracts with a synthetic event source and real queues."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True);a=parser.parse_args()
 out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);s=ROOT/'Source'
 units=['platforms/3ds/tests/video_retrace_test.cpp','platforms/3ds/runtime/video_retrace.cpp','platforms/3ds/runtime/os_sync.cpp','platforms/3ds/tests/host_primitives.cpp']
 headers=['platforms/3ds/runtime/video_retrace.hpp','include/helpers/native_frame_wait.h','platforms/3ds/runtime/clock.hpp','extern/aurora/include/dolphin/vi.h','extern/aurora/include/dolphin/os/OSMessage.h']
 inputs=[Path(__file__).resolve(),*[s/x for x in units+headers]];hashes={str(x.relative_to(ROOT)):digest(x)for x in inputs}
 common=['-std=c++20','-DTARGET_PC=1','-DDUSK_TARGET_3DS=1','-I'+str(s/'extern/aurora/include'),'-I'+str(s/'include'),'-Wall','-Wextra','-Werror','-Wno-invalid-noreturn']
 commands=[['clang++',*common,'-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',*[str(s/x)for x in units],'-o',str(out/'test')]]
 arm=['/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++',*common,'-O2','-g','-march=armv6k','-mtune=mpcore','-mfloat-abi=hard','-mfpu=vfp','-mtp=soft','-D__3DS__','-fstack-usage']
 commands.append([*arm,'-c',str(s/'platforms/3ds/runtime/video_retrace.cpp'),'-o',str(out/'video-retrace-arm.o')])
 rec={'status':'running','origin':'Synthetic host IRQ/sleep source, actual native adapter and original OS queue; not GSP execution, full video or game acceptance','commands':commands,'input_sha256':hashes}
 try:
  with(out/'build.log').open('w')as log:
   for cmd in commands:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=90)
  r=subprocess.run([str(out/'test')],capture_output=True,text=True,timeout=30,env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
  (out/'test.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='');r.check_returncode()
  assert hashes=={str(x.relative_to(ROOT)):digest(x)for x in inputs};rec.update(status='pass',input_unchanged=True)
 except(OSError,subprocess.SubprocessError,AssertionError)as e:rec.update(status='fail',error=str(e));print((out/'build.log').read_text())
 (out/'results.json').write_text(json.dumps(rec,indent=2)+'\n');return int(rec['status']!='pass')
if __name__=='__main__':raise SystemExit(main())
