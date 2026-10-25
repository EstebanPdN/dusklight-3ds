#!/usr/bin/env python3
"""Host sanitizer checks for actual native copy, framebuffer and SD save logic."""
import argparse,hashlib,json,subprocess,os
from pathlib import Path
root=Path(__file__).resolve().parents[2];src=root/'Source'
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);p.add_argument('--suite',choices=['pixels','storage'],action='append');a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
common=['clang++','-std=c++20','-DTARGET_PC=1','-DDUSK_TARGET_3DS=1','-I'+str(src/'extern/aurora/include'),'-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']
suites={'pixels':['gx_framebuffer.cpp','gx_copy_pixels.cpp','texture_gx.cpp','gx_pica_math.cpp'],'storage':['card.cpp','aram.cpp']}
rec={'origin':'Host ASan/UBSan actual implementation; no GPU/NDSP/New3DS acceptance','status':'running','suites':{}}
for name,units in suites.items():
    if a.suite and name not in a.suite:continue
    paths=[src/'platforms/3ds/runtime'/u for u in units]+[src/'platforms/3ds/tests'/('framebuffer_copy_test.cpp' if name=='pixels' else 'card_aram_test.cpp')]
    dest=out/name;dest.mkdir();cmd=common+[str(x) for x in paths]+['-o',str(dest/'test')]
    headers=[src/'platforms/3ds/tests/gx_copy_reference.hpp',src/'platforms/3ds/runtime/gx_copy_pixels.hpp',src/'platforms/3ds/runtime/gx_profile.hpp'] if name=='pixels' else []
    case={'commands':[cmd],'sha256':{str(x.relative_to(root)):hashlib.sha256(x.read_bytes()).hexdigest() for x in [*paths,*headers,Path(__file__).resolve()]}}
    try:
        with(dest/'build.log').open('w')as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=90)
        r=subprocess.run([str(dest/'test')],cwd=dest,capture_output=True,text=True,timeout=30,env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
        (dest/'test.log').write_text(r.stdout+r.stderr);print(name+': '+r.stdout+r.stderr);r.check_returncode();case['status']='pass'
    except (OSError,subprocess.SubprocessError)as e:case.update(status='fail',error=str(e));print((dest/'build.log').read_text())
    rec['suites'][name]=case
rec['status']='pass' if all(x['status']=='pass'for x in rec['suites'].values())else'fail';(out/'results.json').write_text(json.dumps(rec,indent=2)+'\n');raise SystemExit(rec['status']!='pass')
