#!/usr/bin/env python3
"""Verify actual native archive/texture lifetime code with host sanitizers."""
import argparse,hashlib,json,subprocess,os
from pathlib import Path
root=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
native=root/'Source/platforms/3ds'
sources=[native/'runtime'/n for n in ['texture_resources.cpp','gx_state.cpp','resource_archive.cpp','texture_gx.cpp']]+[native/'tests/texture_resources_test.cpp']
headers=[native/'runtime'/n for n in ['texture_resources.hpp','gx_state.hpp','resource_archive.hpp','texture_gx.hpp']]
cmd=['clang++','-std=c++20','-DTARGET_PC=1','-DDUSK_TARGET_3DS=1','-DDUSK_GAME_STARTUP=1','-I'+str(root/'Source/extern/aurora/include'),'-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',*map(str,sources),'-o',str(out/'test')]
rec={'origin':'Host ASan/UBSan actual resource bounds, storage, texture registry and decoder; substitutes OS exclusion/thread identity and GPU completion/deletion only','command':cmd,'sha256':{str(x.relative_to(root)):hashlib.sha256(x.read_bytes()).hexdigest()for x in sources+headers},'status':'running'}
try:
    with(out/'build.log').open('w')as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=90)
    r=subprocess.run([str(out/'test')],capture_output=True,text=True,timeout=30,env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
    (out/'test.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr);r.check_returncode();rec['status']='pass'
except(OSError,subprocess.SubprocessError)as e:rec.update(status='fail',error=str(e));print((out/'build.log').read_text())
(out/'results.json').write_text(json.dumps(rec,indent=2)+'\n');raise SystemExit(rec['status']!='pass')
