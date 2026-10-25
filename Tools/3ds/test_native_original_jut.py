#!/usr/bin/env python3
"""Check actual original JUT console and panic owners with host OS backends."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
 out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);s=ROOT/'Source'
 units=['platforms/3ds/tests/native_original_jut_test.cpp','libs/JSystem/src/JUtility/JUTException.cpp','libs/JSystem/src/JUtility/JUTConsole.cpp','libs/JSystem/src/JSupport/JSUList.cpp']
 headers=['libs/JSystem/include/JSystem/JUtility/JUTException.h','libs/JSystem/include/JSystem/JUtility/JUTConsole.h','include/global.h']
 inputs=[Path(__file__).resolve(),*[s/x for x in units+headers]];hashes={str(x.relative_to(ROOT)):digest(x)for x in inputs}
 flags=['-std=c++20','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-Wl,-dead_strip']
 flags+=['-D'+x for x in ['TARGET_PC=1','DUSK_TARGET_3DS=1','DUSK_BUILDING_GAME=1','VERSION=0','PARTIAL_DEBUG=1','AVOID_UB=1','MTX_USE_PS=1']]
 flags+=['-I'+str(s/x)for x in ['include','libs/JSystem/include','extern/aurora/include','extern/aurora/include/dolphin','sdk/include','src']]
 flags+=['-include',str(s/'extern/aurora/include/dolphin/types.h'),'-include',str(s/'include/global.h'),'-Wno-multichar','-Wno-deprecated-declarations','-Wno-invalid-noreturn']
 cmd=['clang++',*flags,*[str(s/x)for x in units],'-o',str(out/'test')]
 rec={'status':'running','command':cmd,'input_sha256':hashes,'origin':'Actual original JUT owners, host OSReport/OSPanic backends; no native fatal UI, exception worker or gameplay execution'}
 try:
  with(out/'build.log').open('w')as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=90)
  r=subprocess.run([str(out/'test')],text=True,capture_output=True,timeout=30,env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
  (out/'test.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='');r.check_returncode()
  assert hashes=={str(x.relative_to(ROOT)):digest(x)for x in inputs};rec.update(status='pass',input_unchanged=True)
 except(OSError,subprocess.SubprocessError,AssertionError)as e:rec.update(status='fail',error=str(e));print((out/'build.log').read_text())
 (out/'results.json').write_text(json.dumps(rec,indent=2)+'\n');return int(rec['status']!='pass')
if __name__=='__main__':raise SystemExit(main())
