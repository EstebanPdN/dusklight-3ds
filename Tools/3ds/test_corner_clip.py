#!/usr/bin/env python3
"""Corner non-coverage proof versus exact integer feasibility and old outcodes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import subprocess

ROOT=Path(__file__).resolve().parents[2]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    native=ROOT/'Source/platforms/3ds'
    runtime=native/'runtime'
    sources=[native/'tests/gx_corner_clip_test.cpp',runtime/'gx_pica_math.cpp']
    inputs=[*sources,runtime/'gx_pica_math.hpp',Path(__file__).resolve()]
    hashes={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    flags=['clang++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror',
           '-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(runtime)]
    env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'}
    commands=[]
    def run(command,name,expect_success=True):
        commands.append(list(map(str,command)))
        result=subprocess.run(command,capture_output=True,text=True,timeout=90,env=env)
        (out/name).write_text(result.stdout+result.stderr)
        if expect_success:result.check_returncode()
        return result
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    run([*flags,*sources,'-o',out/'test'],'build.log')
    run([out/'test'],'test.log')
    # This control substitutes only the preceding same-plane proof. It must
    # refuse the captured corner case; a generic compiler failure is not a pass.
    current=sources[1].read_text();begin=current.index('bool picaTriangleOutsideSides(')
    end=current.index('bool picaTransform(',begin)
    old=current[:begin]+'''bool picaTriangleOutsideSides(const std::array<float,4>& a,
        const std::array<float,4>& b,const std::array<float,4>& c){
        return (picaClipSideMask(a)&picaClipSideMask(b)&picaClipSideMask(c))!=0;
    }
'''+current[end:]
    (out/'same-plane-control.cpp').write_text(old)
    run([*flags,sources[0],out/'same-plane-control.cpp','-o',out/'control'],'control-build.log')
    control=run([out/'control'],'control.log',False)
    assert control.returncode!=0 and 'picaTriangleOutsideSides(live[0],live[1],live[2])' in control.stderr
    assert hashes=={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    result={'status':'pass','commands':commands,'input_sha256':hashes,
            'negative_control_returncode':control.returncode,
            'scope':'Host ASan/UBSan; exact integer feasibility for 20000 deterministic triangles; no native GPU acceptance'}
    (out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    print((out/'test.log').read_text(),end='')


if __name__=='__main__':main()
