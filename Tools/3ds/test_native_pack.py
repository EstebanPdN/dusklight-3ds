#!/usr/bin/env python3
"""Cross-language native pack verification; synthetic assets only, no emulator."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import native_mesh_pack as pack

ROOT=Path(__file__).resolve().parents[2]

def fixture():
    vertices=[{'position':[float(i&1),float(i>>1),0]} for i in range(4)]
    vertices[0].update(bones=[0,2,0,0],weights=[128,127,0,0])
    return {'schema':'dusklight-native-mesh/1','meshes':[{'id':17,'vertices':vertices[:3],'indices':[0,1,2]},
                                                      {'id':3,'vertices':vertices,'indices':[0,1,2,2,1,3]}]}

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    document=fixture();result=pack.compile_pack(document);assert result==pack.compile_pack(fixture())
    (out/'fixture.json').write_text(json.dumps(document,indent=2)+'\n');(out/'fixture.pica').write_bytes(result)
    rejected=0
    for change in [lambda d:d.update(schema='bad'),lambda d:d['meshes'][0].update(id=3),lambda d:d['meshes'][0].update(id=0),
                   lambda d:d['meshes'][0].update(indices=[0,1,99]),lambda d:d['meshes'][0]['vertices'][0].update(position=[float('nan'),0,0]),
                   lambda d:d['meshes'][0]['vertices'][0].update(weights=[1,0,0,0]),lambda d:d['meshes'][0]['vertices'][0].update(bones=[8,0,0,0]),
                   lambda d:d['meshes'][0]['vertices'][0].update(uv=[[0,0]]),lambda d:d['meshes'][0]['vertices'][0].update(color=[True,0,0,0])]:
        mutated=copy.deepcopy(document);change(mutated)
        try:pack.compile_pack(mutated)
        except ValueError:rejected+=1
        else:raise AssertionError('Invalid native source accepted')
    native=ROOT/'Source/platforms/3ds'
    sources=[native/'native/contracts.cpp',native/'native/pack.cpp',native/'tests/native_pack_test.cpp']
    command=['clang++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer',
             '-I'+str(native),*map(str,sources),'-o',str(out/'test')]
    built=subprocess.run(command,text=True,capture_output=True,timeout=90);(out/'build.log').write_text(built.stdout+built.stderr);built.check_returncode()
    run=subprocess.run([str(out/'test'),str(out/'fixture.pica')],text=True,capture_output=True,timeout=30)
    (out/'run.log').write_text(run.stdout+run.stderr);run.check_returncode();print(run.stdout,end='')
    files=sources+[native/'native/contracts.hpp',native/'native/pack.hpp',ROOT/'Tools/3ds/native_mesh_pack.py',Path(__file__).resolve()]
    receipt={'status':'pass','python_invalid_sources_refused':rejected,'fixture_sha256':hashlib.sha256(result).hexdigest(),'fixture_bytes':len(result),
             'source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},'command':command,
             'scope':'Synthetic native mesh data. No original-game conversion, GPU execution or runtime performance measured.'}
    (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')

if __name__=='__main__':main()
