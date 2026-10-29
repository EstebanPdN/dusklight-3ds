#!/usr/bin/env python3
"""Native shader algebra and assembled ABI checks. No GPU/emulator execution."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import re
import struct

ROOT=Path(__file__).resolve().parents[2]
SHADER=ROOT/'Source/platforms/3ds/native/scene.v.pica'
RIGID_SHADER=ROOT/'Source/platforms/3ds/native/scene_rigid.v.pica'
AXIS={x:i for i,x in enumerate('xyzw')}
AXIS.update(s=0,t=1,p=2,q=3)

def f32(x):return struct.unpack('<f',struct.pack('<f',x))[0]

def execute(source,vertices,uniforms):
    """Execute this source's small instruction subset with host float32 math.

    This is deliberately NOT a PICA float24/rasterization emulator. It checks
    the actual shader algebra, source/destination masks, palette and UV ABI.
    """
    constants=[float(x) for x in re.search(r'\.constf k\(([^)]+)\)',source)[1].split(',')]
    body=source.split('.proc main',1)[1]
    lines=[x.split(';',1)[0].strip() for x in body.splitlines()]
    lines=[x for x in lines if x]
    registers={f'r{i}':[0.]*4 for i in range(16)}
    registers.update({f'v{i}':list(v) for i,v in enumerate(vertices)})
    registers.update(k=constants,a0=[0]*4)
    registers.update({name:[0.]*4 for name in ['outpos','outcolor','outtex0','outtex1','outtex2']})
    jumps={};stack=[]
    for i,line in enumerate(lines):
        if line.startswith('ifc '):stack.append(i)
        elif line=='.end' and stack:jumps[stack.pop()]=i
    assert not stack
    pattern=r'(u\[[^\]]+\]|[a-z][a-z0-9]*)(?:\.([xyzwstpq]+))?'
    def reference(token):
        name,swizzle=re.fullmatch(pattern,token).groups()
        if name.startswith('u['):
            expression=name[2:-1]
            index=int(registers['a0'][0])+int(expression[5:]) if expression.startswith('a0.x+') else int(expression)
            assert 0<=index<len(uniforms)
            values=uniforms[index]
        else:values=registers[name]
        swizzle=swizzle or 'xyzw'
        if len(swizzle)==1:swizzle*=4
        assert len(swizzle)==4
        return [values[AXIS[c]] for c in swizzle]
    def store(token,value):
        name,mask=re.fullmatch(pattern,token).groups()
        for c in mask or 'xyzw':registers[name][AXIS[c]]=f32(value[AXIS[c]])
    pc=0;condition=False
    while pc<len(lines):
        line=lines[pc];pc+=1
        if line=='end':break
        if line=='.end':continue
        op,args=line.split(' ',1);args=[x.strip() for x in args.split(',')]
        if op=='ifc':
            assert args==['cmp.x']
            if not condition:pc=jumps[pc-1]+1
            continue
        if op=='cmp':
            a,b=reference(args[0])[0],reference(args[3])[0]
            assert args[1]==args[2] and args[1] in ['lt','gt']
            condition=a<b if args[1]=='lt' else a>b
            continue
        a=reference(args[1]);b=reference(args[2]) if len(args)>2 else None
        if op in ['mov','mova']:value=a
        elif op=='add':value=[f32(x+y) for x,y in zip(a,b)]
        elif op=='mul':value=[f32(x*y) for x,y in zip(a,b)]
        elif op in ['dp3','dp4']:
            dot=0.
            for i in range(int(op[-1])):dot=f32(dot+f32(a[i]*b[i]))
            value=[dot]*4
        elif op=='rsq':value=[f32(1/math.sqrt(x)) for x in a]
        elif op=='max':value=[max(x,y) for x,y in zip(a,b)]
        elif op=='min':value=[min(x,y) for x,y in zip(a,b)]
        else:raise AssertionError('Unexpected native instruction '+line)
        store(args[0],value)
    return registers

def fixtures():
    rng=random.Random(139)
    for case in range(264):
        rows=[[0.]*4 for _ in range(57)]
        for n in range(24):
            rows[n]=[rng.uniform(-1,1) for _ in range(4)]
            rows[24+n]=[rng.uniform(-1,1) for _ in range(3)]+[0.]
        for n in range(4):rows[48+n]=[rng.uniform(-1,1) for _ in range(4)]
        rows[51][3]=2
        rows[52]=[rng.uniform(0,.25) for _ in range(3)]+[float(case%2)]
        for n in [53,55]:
            direction=[rng.uniform(-1,1) for _ in range(3)];length=math.sqrt(sum(x*x for x in direction))
            rows[n]=[x/length for x in direction]+[0.]
            rows[n+1]=[rng.uniform(0,.5) for _ in range(3)]+[0.]
        cuts=sorted([0,255]+[rng.randrange(256) for _ in range(3)])
        weights=[cuts[i+1]-cuts[i] for i in range(4)] if case>=8 else [255 if i==case//2 else 0 for i in range(4)]
        vertex=[[rng.uniform(-4,4) for _ in range(3)]+[1.],
                [rng.uniform(-1,1) for _ in range(3)]+[0.],
                [rng.uniform(-2,2),rng.uniform(-2,2),0.,1.],
                [rng.uniform(-2,2),rng.uniform(-2,2),0.,1.],
                [rng.uniform(-2,2),rng.uniform(-2,2),0.,1.],
                [float(rng.randrange(256)) for _ in range(4)],
                [float(3*rng.randrange(8)) for _ in range(4)],list(map(float,weights))]
        if case==7:vertex[1]=[0.]*4
        if case==5:vertex[1]=[1.9517822265625,-2.,0.,0.]
        rows=[[f32(x) for x in row] for row in rows];vertex=[[f32(x) for x in row] for row in vertex]
        # Independent mathematical skinning and directional Lambert model.
        position=[0.]*3;normal=[0.]*3
        for influence,weight in zip(vertex[6],weights):
            for r in range(3):
                position[r]+=weight/255*sum(a*b for a,b in zip(rows[int(influence)+r],vertex[0]))
                normal[r]+=weight/255*sum(a*b for a,b in zip(rows[24+int(influence)+r][:3],vertex[1][:3]))
        expectedPosition=[sum(a*b for a,b in zip(row,position+[1.])) for row in rows[48:52]]
        light=rows[52][:3].copy();length=math.sqrt(sum(x*x for x in normal))
        if length>1e-6:
            for n in [53,55]:
                factor=max(0.,sum(a*b/length for a,b in zip(normal,rows[n][:3])))
                light=[a+factor*b for a,b in zip(light,rows[n+1][:3])]
        expectedColor=[x/255 for x in vertex[5]]
        if case%2:expectedColor[:3]=[c*min(1,l) for c,l in zip(expectedColor[:3],light)]
        yield vertex,rows,expectedPosition,expectedColor

def algebra(source):
    total=0
    for v,u,position,color in fixtures():
        actual=execute(source,v,u)
        for key,want in [('outpos',position),('outcolor',color)]:
            for x,y in zip(actual[key],want):assert abs(x-y)<=2e-5*max(1,abs(y)),(total,key,x,y)
        for n in range(3):assert actual[f'outtex{n}'][:2]==v[n+2][:2]
        assert actual['outtex0'][2]==1
        total+=1
    return total

def binary_abi(path,uniform_count=57):
    data=path.read_bytes();assert data[:4]==b'DVLB'
    count,offset=struct.unpack_from('<II',data,4);assert count==1 and data[offset:offset+4]==b'DVLE'
    dvlp=12;assert data[dvlp:dvlp+4]==b'DVLP'
    instructionCount=struct.unpack_from('<I',data,dvlp+12)[0];assert 0<instructionCount<=512
    inputMask=struct.unpack_from('<H',data,offset+16)[0];assert inputMask==255
    uniformOffset,uniformCount,symbolOffset=struct.unpack_from('<III',data,offset+48)
    uniforms={}
    for n in range(uniformCount):
        symbol,start,end=struct.unpack_from('<IHH',data,offset+uniformOffset+8*n)
        at=offset+symbolOffset+symbol;name=data[at:data.index(b'\0',at)].decode();uniforms[name]=(start,end)
    assert uniforms['u']==(16,15+uniform_count),uniforms
    constantOffset,constantCount=struct.unpack_from('<II',data,offset+24)
    constants=[struct.unpack_from('<HH4I',data,offset+constantOffset+20*n) for n in range(constantCount)]
    outputOffset,outputCount=struct.unpack_from('<II',data,offset+40)
    outputs=[struct.unpack_from('<HHB',data,offset+outputOffset+8*n) for n in range(outputCount)]
    assert {(x[0],x[2]) for x in outputs}=={(0,15),(2,15),(3,3),(4,4),(5,3),(6,3)},outputs
    return {'instructions':instructionCount,'input_mask':inputMask,'uniform_entries':uniforms,'constants':constants,'outputs':outputs}

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--shader-binary',type=Path,required=True);parser.add_argument('--rigid-binary',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    source=SHADER.read_text();cases=algebra(source);detected=[]
    for name,old,new in [('weight-scale','mul r3.x, k.z, v7.x','mul r3.x, k.y, v7.x'),
                         ('normal-palette','u[a0.x+24]','u[a0.x+0]'),
                         ('uv2-source','mov outtex2.xy, v4','mov outtex2.xy, v3'),
                         ('color-scale','mul r10, k.z, v5','mov r10, v5')]:
        assert old in source
        try:algebra(source.replace(old,new))
        except AssertionError:detected.append(name)
        else:raise AssertionError('Undetected shader mutation '+name)
    rigid=RIGID_SHADER.read_text();rigidCases=0
    for v,u,_,_ in fixtures():
        v[7]=[255.,0.,0.,0.]
        full=execute(source,v,u);special=execute(rigid,v,u)
        for name in ['outpos','outcolor','outtex0','outtex1','outtex2']:
            assert struct.pack('<4f',*full[name])==struct.pack('<4f',*special[name]),name
        rigidCases+=1
    generalAbi,rigidAbi=binary_abi(args.shader_binary),binary_abi(args.rigid_binary)
    assert generalAbi['constants']==rigidAbi['constants'] and generalAbi['outputs']==rigidAbi['outputs']
    # Conservative absolute-value range bounds, including headroom for finite
    # float24 rounding. This establishes no source rendering/pixel equivalence.
    viewBound=2*(3*(2**20)*(2**15)+2**20)
    clipBound=2*(3*viewBound*(2**15)+2**20)
    normalSquaredBound=2*3*(2*3*2*256)**2
    assert clipBound<2**62 and normalSquaredBound<2**62
    result={'status':'pass','host_float32_algebra_cases':cases,'detected_mutants':detected,'assembled_abi':generalAbi,
            'shader_sha256':hashlib.sha256(SHADER.read_bytes()).hexdigest(),'binary_sha256':hashlib.sha256(args.shader_binary.read_bytes()).hexdigest(),
            'rigid_exact_host_cases':rigidCases,'rigid_assembled_abi':rigidAbi,
            'conservative_abs_intermediate_bounds':{'view':viewBound,'clip':clipBound,'normal_squared':normalSquaredBound,'finite_margin_reference':2**62},
            'rigid_source_sha256':hashlib.sha256(RIGID_SHADER.read_bytes()).hexdigest(),'rigid_binary_sha256':hashlib.sha256(args.rigid_binary.read_bytes()).hexdigest(),
            'limits':'Source algebra interpreter + assembled ABI only. No PICA float24, hardware timing, pixels, texture filtering or raster acceptance.'}
    (args.output/'shader-receipt.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))

if __name__=='__main__':main()
