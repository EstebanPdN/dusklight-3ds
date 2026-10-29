#!/usr/bin/env python3
"""Independent source fixtures and normalized fragment-equation checks."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import random
import struct
from j3d_native_materials import convert,compile_material,alpha_test,Unsupported
from j3d_material_source import materials

ROOT=Path(__file__).resolve().parents[2]

def source_fixture():
    block=bytearray(132);block[:4]=b'MAT3';struct.pack_into('>H',block,8,1)
    record=bytearray(b'\xff'*332);record[0]=1;record[1]=record[3]=record[4]=record[6]=0
    for base,values in [(132,[0,1]),(148,[0,0,1,1]),(188,[0,1,2]),(220,[0,1,1,0]),(228,[0,1,2]),(260,[0,0,0]),(292,[0]),(324,[0,0,0])]:
        for n,value in enumerate(values):struct.pack_into('>H',record,base+n*2,value)
    for n,value in enumerate([0,17,12]):record[156+n]=value
    for n,value in enumerate([0,28,31]):record[172+n]=value
    for n in range(2):struct.pack_into('>H',record,40+n*2,n)
    def table(field,data):
        block.extend(bytes((-len(block))%4));struct.pack_into('>I',block,field,len(block));block.extend(data)
    def stage(rgb,rgbop,alpha,alphaop):return bytes([255,*rgb,*rgbop,*alpha,*alphaop,255])
    table(12,record);table(16,bytes(2));table(28,struct.pack('>I',2));table(52,b'\2');table(56,bytes((1,4,60,255,0,0,33,255)));table(72,struct.pack('>2H',0,1))
    table(76,bytes((0,0,4,255,1,1,255,255,255,255,255,255)))
    table(80,struct.pack('>8h',0,0,0,255,255,255,255,255));table(84,bytes((0,0,0,255,255,255,255,255)));table(88,b'\3')
    table(92,b''.join([stage([15,8,10,15],[0,0,0,1,0],[7,5,4,7],[0,0,0,1,0]),stage([15,12,14,8],[0,0,0,1,3],[7,7,7,0],[0,0,0,0,0]),stage([14,6,0,2],[0,0,2,1,0],[7,6,0,7],[0,0,0,0,0])]))
    table(96,bytes(4));table(100,bytes((0,1,2,3)));table(104,bytes(44));table(108,bytes((7,0,1,7,0,255,255,255)));table(112,bytes((0,1,0,3)));table(116,bytes((1,3,1,255)))
    block.extend(bytes((-len(block))%32));struct.pack_into('>I',block,4,len(block))
    blocks=[tag+struct.pack('>I',8) for tag in [b'VTX1',b'SHP1',b'DRW1',b'JNT1']]+[bytes(block)]
    return struct.pack('>8sII16x',b'J3D2bmd3',32+sum(map(len,blocks)),5)+b''.join(blocks)

def compare(code,a,b):
    if code==0:return False
    if code==1:return a<b
    if code==2:return a==b
    if code==3:return a<=b
    if code==4:return a>b
    if code==5:return a!=b
    if code==6:return a>=b
    return True

def source_eval(material,samples,primary):
    rgb=[None]+[[v/255 for v in x[:3]] for x in material['registers'][:3]];alpha=[None]+[x[3]/255 for x in material['registers'][:3]]
    def k(sel,scalar):
        if sel<8:value=[1,7/8,3/4,5/8,1/2,3/8,1/4,1/8][sel];return value if scalar else [value]*3
        if sel<16:return [x/255 for x in material['konst'][sel-12][:3]]
        value=material['konst'][(sel-16)%4][(sel-16)//4]/255;return value if scalar else [value]*3
    for stage in material['stages']:
        slot=stage['texture_slot'];image=material['textures'][slot] if slot<8 else None
        tex=samples.get((image,stage['coordinate']),(0,0,0,0));t=[tex[i] for i in stage['texture_swap']];ras=[primary[i] for i in stage['raster_swap']]
        def color(arg):
            if arg<8:return [alpha[arg//2]]*3 if arg%2 else rgb[arg//2]
            if arg==8:return t[:3]
            if arg==9:return [t[3]]*3
            if arg==10:return ras[:3]
            if arg==11:return [ras[3]]*3
            if arg==12:return [1]*3
            if arg==13:return [128/255]*3
            if arg==14:return k(stage['konst_color_selector'],False)
            return [0]*3
        def av(arg):return alpha[arg] if arg<4 else t[3] if arg==4 else ras[3] if arg==5 else k(stage['konst_alpha_selector'],True) if arg==6 else 0
        values=[color(x) for x in stage['rgb']];a,b,c,d=values;op,bias,scale,clamp,dest=stage['rgb_op']
        new=[(z+(1 if op==0 else -1)*(x*(1-f)+y*f))*(2**scale) for x,y,f,z in zip(a,b,c,d)]
        if clamp:new=[min(1,max(0,x)) for x in new]
        a,b,c,d=[av(x) for x in stage['alpha']];op,bias,scale,clamp,adest=stage['alpha_op'];anew=(d+(1 if op==0 else -1)*(a*(1-c)+b*c))*(2**scale)
        if clamp:anew=min(1,max(0,anew))
        rgb[dest],alpha[adest]=new,anew
    return rgb[0]+[alpha[0]]

def native_eval(record,bindings,samples,primary):
    textures={n:samples[(b['image'],b['coordinate'])] for n,b in enumerate(bindings)};previous=[0]*4
    buffer=[0]*4;pending=[(record.get('buffer_color',0)>>(8*c)&255)/255 for c in range(4)]
    for index,stage in enumerate(record['stages']):
        constant=[(stage['constant']>>(8*n)&255)/255 for n in range(4)]
        def inputs(lane):
            result=[]
            for source,operand in zip(stage[lane],stage[lane+'_operand']):
                value=primary if source=='primary' else previous if source=='previous' else buffer if source=='buffer' else constant if source=='constant' else textures[int(source[-1])]
                complement=operand.startswith('oneMinus');channel=operand[8:].lower() if complement else operand
                part=value[:3] if channel=='color' else [value[{'red':0,'green':1,'blue':2,'alpha':3}[channel]]]*(1 if lane=='alpha' else 3)
                result.append([1-x for x in part] if complement else part)
            return result
        output=[]
        for lane in ['rgb','alpha']:
            a,b,c=inputs(lane);fn=stage[lane+'_function'];values=[]
            for x,y,z in zip(a,b,c):
                value=x if fn=='replace' else x*y if fn=='modulate' else x+y if fn=='add' else x-y if fn=='subtract' else x*z+y*(1-z)
                values.append(min(1,max(0,value*(1<<stage[lane+'_scale']))))
            output.extend(values)
        previous=output;buffer=pending.copy()
        if record.get('buffer_rgb_mask',0)&(1<<index):pending[:3]=output[:3]
        if record.get('buffer_alpha_mask',0)&(1<<index):pending[3]=output[3]
    return previous

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    source=source_fixture();(out/'fixture.bmd').write_bytes(source)
    missing,receipt=convert(source);assert missing is None and receipt['materials'][0]['status']=='refused'
    blocked,metadata=convert(source,allow_normalized_arithmetic=True);assert blocked is None and metadata['materials'][0]['status']=='refused'
    packed,receipt=convert(source,allow_normalized_arithmetic=True,specialize_initial_constants=True);record=receipt['records'][0];bindings=receipt['materials'][0]['bindings'];original=list(materials(source))[0]
    assert len(record['stages'])==2 and record['images']==[2,1,0] and bindings[0]['projective'] and not receipt['rendering_ready'];(out/'fixture.dnm').write_bytes(packed)
    rng=random.Random(147);pixels=0
    for _ in range(1024):
        samples={(0,0):[rng.randrange(256)/255 for _ in range(4)],(1,1):[rng.randrange(256)/255 for _ in range(4)]};primary=[rng.randrange(256)/255 for _ in range(4)]
        a,b=source_eval(original,samples,primary),native_eval(record,bindings,samples,primary);assert max(abs(x-y) for x,y in zip(a,b))<1e-12;pixels+=1
    contextual=copy.deepcopy(original);contextual['states']['blend']=(1,2,3,3)
    converted,_=compile_material(contextual,allow_normalized_arithmetic=True,specialize_initial_constants=True)
    assert converted['custom_blend']['color_source']=='destinationColor' and converted['custom_blend']['color_destination']=='oneMinusSourceColor'
    assert converted['custom_blend']['alpha_source']=='destinationAlpha' and converted['custom_blend']['alpha_destination']=='oneMinusSourceAlpha'
    contextual['states']['blend']=(3,1,1,3);converted,_=compile_material(contextual,allow_normalized_arithmetic=True,specialize_initial_constants=True)
    assert converted['custom_blend']['color']=='reverseSubtract' and converted['custom_blend']['alpha']=='reverseSubtract'
    refused=0
    for field,values in [('alpha_op',(0,0,1,0,0)),('rgb_op',(0,1,0,1,0)),('rgb_op',(8,0,0,1,0))]:
        bad=copy.deepcopy(original);bad['stages'][2][field]=values
        try:compile_material(bad,allow_normalized_arithmetic=True,specialize_initial_constants=True)
        except Unsupported:refused+=1
        else:raise AssertionError('Unsupported source arithmetic accepted')
    available=set()
    for code in range(8):
        for ref in range(256):available.add(sum(int(compare(code,v,ref))<<v for v in range(256)))
    predicates=0
    for _ in range(256):
        a,b,op,ra,rb=rng.randrange(8),rng.randrange(8),rng.randrange(4),rng.randrange(256),rng.randrange(256)
        mask=0
        for v in range(256):
            x,y=compare(a,v,ra),compare(b,v,rb);answer=(x and y) if op==0 else (x or y) if op==1 else (x!=y) if op==2 else (x==y);mask|=int(answer)<<v
        try:name,ref=alpha_test((a,ra,op,b,rb))
        except Unsupported:assert mask not in available
        else:
            names=['never','less','equal','lessEqual','greater','notEqual','greaterEqual','always'];assert sum(int(compare(names.index(name),v,ref))<<v for v in range(256))==mask
        predicates+=1
    for n in range(len(source)):
        try:convert(source[:n],allow_normalized_arithmetic=True,specialize_initial_constants=True)
        except ValueError:refused+=1
        else:raise AssertionError('Truncated source accepted')
    files=[ROOT/'Tools/3ds'/name for name in ['j3d_material_source.py','j3d_native_materials.py','native_material_pack.py','j3d_native_geometry.py','test_j3d_native_materials.py']]
    (out/'receipt.json').write_text(json.dumps({'status':'pass','normalized_equation_samples':pixels,'alpha_truth_predicates':predicates,'refusals':refused,
        'source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},
        'scope':'Normalized fragment equations and alpha predicate classification; no source integer precision, GPU sampling or visual acceptance.'},indent=2)+'\n')
    print(f'PASS {pixels} normalized fragment equations, {predicates} alpha predicate classifications and {refused} source refusals')

if __name__=='__main__':main()
