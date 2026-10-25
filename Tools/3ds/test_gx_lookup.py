#!/usr/bin/env python3
"""Check bounded live texture lookup hints under full-table collision and reuse."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);out=p.parse_args().output;out.mkdir(parents=True,exist_ok=False)
sources=[root/'Source/platforms/3ds'/s for s in ['runtime/gx_state.cpp','runtime/resource_archive.cpp','tests/gx_lookup_test.cpp']]
cmd=['clang++','-std=c++20','-O2','-g','-DDUSK_GAME_STARTUP=1','-DDUSK_GX_LOOKUP_TEST=1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer',*map(str,sources),'-o',str(out/'test')]
r={'status':'running','origin':'actual native registry, host ASan/UBSan; no hardware acceptance','command':cmd,'sha256':{str(s.relative_to(root)):hashlib.sha256(s.read_bytes()).hexdigest() for s in [*sources,Path(__file__).resolve()]}}
try:
 with(out/'build.log').open('w')as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=90)
 q=subprocess.run([str(out/'test')],capture_output=True,text=True,timeout=60,env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
 (out/'test.log').write_text(q.stdout+q.stderr);print(q.stdout+q.stderr);q.check_returncode();r['status']='pass'
except subprocess.SubprocessError as e:r.update(status='fail',error=str(e));print((out/'build.log').read_text())
(out/'results.json').write_text(json.dumps(r,indent=2)+'\n');raise SystemExit(r['status']!='pass')
