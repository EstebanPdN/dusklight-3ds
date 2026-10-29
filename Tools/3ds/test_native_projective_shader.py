#!/usr/bin/env python3
"""Native position->STQ source algebra and binary ABI. Never GPU execution."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from test_native_shader import execute,fixtures,binary_abi

ROOT=Path(__file__).resolve().parents[2]
NATIVE=ROOT/'Source/platforms/3ds/native'

def inputs():
    for case,(v,u,_,_) in enumerate(fixtures()):
        uv=[[1.,0.,0.,0.],[0.,1.,0.,0.],[.5,.25,-.125,2.],[0.,1.,.25,0.],[1.,0.,.125,0.],[-.25,.5,0.,0.],[0.,float(case%2),float((case//2)%2),0.]]
        rows=[[.5,.25,-.125,1.],[-.25,1.,.5,-2.],[.125,-.25,.5,.25]]
        posed=bool(case%2)
        if case==0:rows[2]=[0.,0.,0.,1.]
        if case==1:rows[2]=[0.,0.,0.,-1.]
        if case==2:rows[2]=[1.,0.,0.,0.];v[0][0]=0.;posed=False
        yield v,u+uv+rows+[[float(posed),0.,0.,0.]],rows,posed

def algebra(source,rigid=False):
    total=0;signs={'positive':0,'negative':0,'zero':0}
    base=(NATIVE/('scene_rigid_uv.v.pica' if rigid else 'scene_uv.v.pica')).read_text()
    for v,u,rows,posed in inputs():
        if rigid:v[7]=[255.,0.,0.,0.]
        previous=execute(base,v,u[:64]);actual=execute(source,v,u)
        for name in ['outpos','outcolor','outtex1','outtex2']:assert struct.pack('<4f',*actual[name])==struct.pack('<4f',*previous[name]),name
        # Independent affine point and weighted-position equations; never divide
        # on CPU or infer what a PICA sampler does at signed/zero Q.
        point=v[0][:3]+[1.]
        if posed:
            point=[sum(weight/255*sum(u[int(bone)+r][c]*v[0][c] for c in range(4)) for bone,weight in zip(v[6],v[7])) for r in range(3)]+[1.]
        wanted=[sum(a*b for a,b in zip(row,point)) for row in rows]
        for a,b in zip(actual['outtex0'][:3],wanted):assert abs(a-b)<=2e-4*max(1,abs(b)),(total,a,b)
        q=actual['outtex0'][2];signs['positive' if q>0 else 'negative' if q<0 else 'zero']+=1;total+=1
    assert all(signs.values()),signs
    return total,signs

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build-dir',type=Path,required=True);p.add_argument('--output',type=Path,required=True);args=p.parse_args()
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True);general=(NATIVE/'scene_projective.v.pica').read_text();rigid=(NATIVE/'scene_rigid_projective.v.pica').read_text()
    full,signs=algebra(general);special,_=algebra(rigid,True);detected=[]
    for name,old,new in [('projective-q','dp4 outtex0.z, u[66], r14','mov outtex0.z, k.y'),
        ('position-source','cmp u[67].x, gt, gt','cmp u[67].x, lt, lt'),('affine-w','mov r14.w, k.y','mov r14.w, k.x'),
        ('projective-t','dp4 outtex0.y, u[65]','dp4 outtex0.y, u[64]')]:
        assert old in general
        try:algebra(general.replace(old,new))
        except AssertionError:detected.append(name)
        else:raise AssertionError('Undetected projective shader mutation '+name)
    exact=0
    for v,u,_,_ in inputs():
        v[7]=[255.,0.,0.,0.];a,b=execute(general,v,u),execute(rigid,v,u)
        for name in ['outpos','outcolor','outtex0','outtex1','outtex2']:assert struct.pack('<4f',*a[name])==struct.pack('<4f',*b[name]),name
        exact+=1
    abi={name:binary_abi(args.build_dir/('native_'+name+'.shbin'),68) for name in ['scene_projective','scene_rigid_projective']}
    previous=binary_abi(args.build_dir/'native_scene.shbin')
    for row in abi.values():assert row['constants']==previous['constants'] and row['outputs']==previous['outputs']
    view_bound=2*(3*(2**20)*(2**15)+2**20);projected_bound=2*(3*view_bound*(2**15)+2**20);assert projected_bound<2**62
    files=[NATIVE/name for name in ['scene_projective.v.pica','scene_rigid_projective.v.pica','scene_uv.v.pica','scene_rigid_uv.v.pica']]+[ROOT/'Tools/3ds/test_native_shader.py',Path(__file__).resolve()]
    (out/'receipt.json').write_text(json.dumps({'status':'pass','weighted_algebra_cases':full,'rigid_algebra_cases':special,'rigid_exact_host_cases':exact,
        'q_output_sign_cases':signs,'mutants_detected':detected,'assembled_abi':abi,'conservative_abs_projected_bound':projected_bound,
        'source_sha256':{str(f.relative_to(ROOT)):hashlib.sha256(f.read_bytes()).hexdigest() for f in files},
        'scope':'Finite host float32 position->STQ equations only. Signed/near-zero Q sampling, PICA float24/derivatives/filtering, pixels and physical behavior unproved.'},indent=2)+'\n')
    print(f'PASS {full+special} position->STQ cases, {exact} rigid parity cases, four mutations and native projective ABI')

if __name__=='__main__':main()
