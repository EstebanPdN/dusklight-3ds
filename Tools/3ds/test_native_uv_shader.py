#!/usr/bin/env python3
"""Native affine UV shader algebra/ABI. No GPU or emulator execution."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from test_native_shader import execute,fixtures,binary_abi

ROOT=Path(__file__).resolve().parents[2]
NATIVE=ROOT/'Source/platforms/3ds/native'

def algebra(source,rigid=False):
    total=0
    for case,(v,u,_,_) in enumerate(fixtures()):
        if rigid:v[7]=[255.,0.,0.,0.]
        reference=execute((NATIVE/('scene_rigid.v.pica' if rigid else 'scene.v.pica')).read_text(),v,u)
        rows=[];enabled=[];expected=[]
        for unit in range(3):
            channel=(case+unit)%3
            first=[(case%7-3)*.25,unit*.5,.125*(case%5),float(channel)]
            second=[unit*.25,-.5,(case%3-1)*.375,0.]
            on=bool(case&(1<<unit));enabled.append(float(on));rows.extend([first,second])
            expected.append([sum(row[n]*[v[channel+2][0],v[channel+2][1],1][n] for n in range(3)) for row in [first,second]] if on else v[unit+2][:2])
        uniforms=u+rows+[enabled+[0.]];actual=execute(source,v,uniforms)
        for name in ['outpos','outcolor']:assert struct.pack('<4f',*actual[name])==struct.pack('<4f',*reference[name]),(case,name)
        for unit in range(3):
            for a,b in zip(actual[f'outtex{unit}'][:2],expected[unit]):assert abs(a-b)<=2e-5*max(1,abs(b)),(case,unit,a,b)
        assert actual['outtex0'][2]==1
        total+=1
    return total

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build-dir',type=Path,required=True);p.add_argument('--output',type=Path,required=True);args=p.parse_args()
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    general=(NATIVE/'scene_uv.v.pica').read_text();rigid=(NATIVE/'scene_rigid_uv.v.pica').read_text()
    full=algebra(general);special=algebra(rigid,True);detected=[]
    for name,old,new in [('channel-two','mov r14.xy, v4','mov r14.xy, v3'),('translation','mov r14.z, k.y','mov r14.z, k.x'),
        ('second-row','dp3 outtex1.y, u[60]','dp3 outtex1.y, u[59]'),('unit-enable','cmp u[63].z','cmp u[63].x')]:
        assert old in general
        try:algebra(general.replace(old,new))
        except AssertionError:detected.append(name)
        else:raise AssertionError('Undetected UV shader mutation '+name)
    exact=0
    for v,u,_,_ in fixtures():
        v[7]=[255.,0.,0.,0.];extra=[[1.,.25,-.125,2.],[.5,1.,.25,0.]]*3+[[1.,1.,1.,0.]]
        a,b=execute(general,v,u+extra),execute(rigid,v,u+extra)
        for name in ['outpos','outcolor','outtex0','outtex1','outtex2']:assert struct.pack('<4f',*a[name])==struct.pack('<4f',*b[name]),name
        exact+=1
    abi={}
    for name,count in [('scene',57),('scene_rigid',57),('scene_uv',64),('scene_rigid_uv',64)]:
        abi[name]=binary_abi(args.build_dir/('native_'+name+'.shbin'),count)
    for name in abi:assert abi[name]['constants']==abi['scene']['constants'] and abi[name]['outputs']==abi['scene']['outputs']
    # Worst finite affine magnitude with twofold headroom, no pixel-fidelity claim.
    uv_bound=2*(2*32768*32768+32768);assert uv_bound<2**62
    files=[NATIVE/name for name in ['scene.v.pica','scene_rigid.v.pica','scene_uv.v.pica','scene_rigid_uv.v.pica']]+[ROOT/'Tools/3ds/test_native_shader.py',Path(__file__).resolve()]
    (out/'receipt.json').write_text(json.dumps({'status':'pass','weighted_algebra_cases':full,'rigid_algebra_cases':special,'rigid_exact_host_cases':exact,
        'mutations_detected':detected,'assembled_abi':abi,'conservative_abs_uv_bound':uv_bound,
        'source_sha256':{str(f.relative_to(ROOT)):hashlib.sha256(f.read_bytes()).hexdigest() for f in files},
        'scope':'Host float32 source algebra and assembled ABI. No PICA float24, texture filtering, images, timing or physical acceptance.'},indent=2)+'\n')
    print(f'PASS {full+special} affine/channel cases, {exact} rigid parity cases, four mutations and four assembled shader ABIs')

if __name__=='__main__':main()
