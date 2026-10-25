#!/usr/bin/env python3
"""Exercise original JUT/JKR with native mapping and synthetic HID snapshots."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
 out=args.output.resolve();out.mkdir(parents=True,exist_ok=False);s=ROOT/'Source'
 sources=['platforms/3ds/tests/native_pad_test.cpp','platforms/3ds/tests/host_primitives.cpp','platforms/3ds/runtime/bootstrap.cpp','platforms/3ds/runtime/os_sync.cpp','platforms/3ds/runtime/os_arena.cpp','platforms/3ds/runtime/os_time.cpp','platforms/3ds/runtime/pad_mapping.cpp','platforms/3ds/runtime/pad_clamp.cpp','platforms/3ds/runtime/pad_bootstrap.cpp','libs/JSystem/src/JUtility/JUTGamePad.cpp','src/helpers/string.cpp','libs/JSystem/src/JSupport/JSUList.cpp']
 sources += ['libs/JSystem/src/JKernel/'+x+'.cpp'for x in ['JKRHeap','JKRExpHeap','JKRSolidHeap','JKRDisposer']]
 sources += ['src/m_Do/m_Do_controller_pad.cpp']
 headers=['platforms/3ds/runtime/pad_mapping.hpp','platforms/3ds/runtime/pad_bootstrap.hpp','libs/JSystem/include/JSystem/JUtility/JUTGamePad.h','extern/aurora/include/dolphin/pad.h','include/m_Do/m_Do_controller_pad.h','include/global.h']
 inputs=[Path(__file__).resolve(),*[s/p for p in sources+headers]];hashes={str(p.relative_to(ROOT)):digest(p)for p in inputs}
 flags=['-std=c++20','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-Wl,-dead_strip']
 flags += ['-D'+x for x in ['TARGET_PC=1','DUSK_TARGET_3DS=1','DUSK_BUILDING_GAME=1','VERSION=0','PARTIAL_DEBUG=1','AVOID_UB=1','MTX_USE_PS=1','DUSK_TEST_KERNEL=1']]
 flags += ['-I'+str(s/p) for p in ['include','libs/JSystem/include','extern/aurora/include','extern/aurora/include/dolphin','sdk/include','src']]
 flags += ['-include',str(s/'extern/aurora/include/dolphin/types.h'),'-include',str(s/'include/global.h'),'-Wno-multichar','-Wno-deprecated-declarations','-Wno-invalid-noreturn']
 cmd=['clang++',*flags,*[str(s/p)for p in sources],'-o',str(out/'test')]
 receipt={'status':'running','origin':'Host synthetic HID snapshots, real JUT/JKR and native mapping/clamp; not physical HID or gameplay','command':cmd,'input_sha256':hashes}
 try:
  with (out/'build.log').open('w')as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
  run=subprocess.run([str(out/'test')],capture_output=True,text=True,timeout=30,env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
  (out/'test.log').write_text(run.stdout+run.stderr);print(run.stdout+run.stderr,end='');run.check_returncode()
  assert hashes=={str(p.relative_to(ROOT)):digest(p)for p in inputs}
  receipt.update(status='pass',input_unchanged=True)
 except (OSError,subprocess.SubprocessError,AssertionError)as e:
  receipt.update(status='fail',error=str(e));print((out/'build.log').read_text())
 (out/'results.json').write_text(json.dumps(receipt,indent=2)+'\n')
 return int(receipt['status']!='pass')
if __name__=='__main__':raise SystemExit(main())
