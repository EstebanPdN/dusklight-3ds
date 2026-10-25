#!/usr/bin/env python3
"""Exercise original game-clock phase logic with the actual native Aurora time ABI."""
import argparse,hashlib,json,os,shlex,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
out=a.output;out.mkdir(parents=True,exist_ok=False)
n=ROOT/'Source/platforms/3ds';s=ROOT/'Source'
sources=[s/'src/dusk/game_clock.cpp',n/'runtime/game_clock_native.cpp',n/'runtime/os_sync.cpp',n/'tests/host_primitives.cpp',n/'tests/game_clock_native_test.cpp']
headers=[s/'src/dusk/game_clock.h',s/'extern/aurora/include/aurora/time.hpp',s/'extern/aurora/include/dolphin/os/OSMutex.h',n/'runtime/primitives.hpp']
inputs=[Path(__file__).resolve(),*sources,*headers];hashes={str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest()for x in inputs}
common=['-std=c++20','-DTARGET_PC=1','-DDUSK_TARGET_3DS=1','-I'+str(s/'src'),'-I'+str(s/'extern/aurora/include'),'-Wall','-Wextra','-Werror']
commands=[['clang++',*common,'-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',*map(str,sources),'-o',str(out/'test')]]
arm=['/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++',*common,'-O2','-march=armv6k','-mtune=mpcore','-mfloat-abi=hard','-mfpu=vfp','-mtp=soft','-D__3DS__','-fstack-usage']
for src in sources[:2]: commands.append([*arm,'-c',str(src),'-o',str(out/(src.stem+'-arm.o'))])
r={'status':'running','origin':'Host deterministic OS ticks, production native time/OS mutex and original simulation algorithm; ARM compile; no gameplay/hardware','input_sha256':hashes,'commands':commands}
report=out/'results.json';report.write_text(json.dumps(r,indent=2)+'\n')
try:
 with (out/'build.log').open('w')as log:
  for cmd in commands: subprocess.run(cmd,check=True,stdout=log,stderr=subprocess.STDOUT,timeout=90)
 run=subprocess.run([str(out/'test')],capture_output=True,text=True,timeout=30,env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
 (out/'test.log').write_text(run.stdout+run.stderr);print(run.stdout+run.stderr,end='');run.check_returncode()
 assert hashes=={str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest()for x in inputs}
 r.update(status='pass',input_unchanged=True,arm_stack_usage={p.name:p.read_text()for p in out.glob('*.su')})
except (OSError,subprocess.SubprocessError,AssertionError)as e:
 r.update(status='fail',error=str(e));print((out/'build.log').read_text())
finally:report.write_text(json.dumps(r,indent=2)+'\n')
raise SystemExit(r['status']!='pass')
