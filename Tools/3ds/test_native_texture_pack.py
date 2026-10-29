#!/usr/bin/env python3
"""Native image contract checks. No emulator or GPU execution."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import native_texture_pack as pack

ROOT=Path(__file__).resolve().parents[2]

def fixture():
    return [{'id':i+1,'width':16,'height':16,'format':fmt,'levels':[bytes((j*17+i)&255 for j in range(size)) for size in pack.extent(16,16,2,fmt)]}
        for i,fmt in reversed(list(enumerate(pack.FORMATS)))]

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    images=fixture();packed=pack.compile_images(images);assert packed==pack.compile_images(fixture());(out/'fixture.dnt').write_bytes(packed)
    rejected=0
    for change in [lambda x:x[0].update(id=1),lambda x:x[0].update(id=True),lambda x:x[0].update(width=4),lambda x:x[0].update(height=9),
        lambda x:x[0].update(format='source'),lambda x:x[0].update(levels=[b'']),lambda x:x[0].update(levels=[]),lambda x:x[0].update(unknown=1)]:
        bad=copy.deepcopy(images);change(bad)
        try:pack.compile_images(bad)
        except ValueError:rejected+=1
        else:raise AssertionError('Malformed native image accepted')
    # Independent known-address pattern, including both tile axes and byte order.
    rgba=bytes(c for y in range(16) for x in range(16) for c in (x,y,x^y,255-x))
    tiled=pack.tile_rgba8(rgba,16,16)
    for x,y,at in [(0,0,0),(1,0,4),(0,1,8),(7,7,252),(8,0,256),(0,8,512),(15,15,1020)]:
        assert tiled[at:at+4]==bytes((255-x,x^y,y,x))
    native=ROOT/'Source/platforms/3ds';sources=[native/'native/contracts.cpp',native/'native/pack.cpp',native/'native/texture_pack.cpp',native/'tests/native_texture_pack_test.cpp']
    command=['clang++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(native),*map(str,sources),'-o',str(out/'test')]
    run=subprocess.run(command,text=True,capture_output=True,timeout=90);(out/'build.log').write_text(run.stdout+run.stderr);run.check_returncode()
    run=subprocess.run([str(out/'test'),str(out/'fixture.dnt')],text=True,capture_output=True,timeout=30);(out/'run.log').write_text(run.stdout+run.stderr);run.check_returncode();print(run.stdout,end='')
    files=sources+[native/'native/texture_pack.hpp',ROOT/'Tools/3ds/native_texture_pack.py',Path(__file__).resolve()]
    (out/'receipt.json').write_text(json.dumps({'status':'pass','invalid_native_sources_refused':rejected,'known_morton_addresses':7,
        'source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},'command':command,
        'scope':'Synthetic native storage, no source sampler equivalence, PICA execution or speed claim.'},indent=2)+'\n')

if __name__=='__main__':main()
