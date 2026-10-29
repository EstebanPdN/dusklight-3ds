#!/usr/bin/env python3
"""Offline native buffer allocation, normalized equations and refusal controls."""
from pathlib import Path
import argparse,copy,hashlib,json,random
from j3d_native_materials import compile_material,schedule_buffer,leaf,calc,Unsupported
from j3d_material_source import materials
from native_material_pack import compile_materials
from test_j3d_native_materials import source_fixture,source_eval,native_eval
ROOT=Path(__file__).resolve().parents[2]

def wire(record,parameters):
    result=copy.deepcopy(record)
    for stage in result['stages']:
        values=[stage['constant']>>(8*c)&255 for c in range(4)]
        for c,selector in enumerate(stage.get('constant_bindings',[0]*4)):
            if selector:values[c]=parameters[(selector-1)//4][(selector-1)%4]
        stage['constant']=sum(v<<(8*c) for c,v in enumerate(values))
    return result

def plan_record(sequence,alpha):
    stages=[]
    for item in sequence:
        lane='alpha' if alpha else 'rgb';s={'rgb':['primary']*3,'alpha':['primary']*3,'rgb_operand':['color']*3,'alpha_operand':['alpha']*3,'rgb_function':'replace','alpha_function':'replace','rgb_scale':0,'alpha_scale':0,'constant':0xffffffff}
        s[lane]=item['sources'];s[lane+'_operand']=item['operands'];s[lane+'_function']=item['function'];s[lane+'_scale']=item['scale']
        values=[255]*4;bindings=[0]*4
        if item['constant'] is not None:
            if alpha:values[3]=item['constant'][0]
            else:values[:3]=item['constant']
        if item['binding'] is not None:
            if alpha:bindings[3]=item['binding'][0]
            else:bindings[:3]=item['binding']
        s['constant']=sum(v<<(8*c) for c,v in enumerate(values));s['constant_bindings']=bindings;stages.append(s)
    return {'id':9,'images':[1,2,0],'stages':stages,'buffer_'+('alpha' if alpha else 'rgb')+'_mask':sum(1<<n for n,item in enumerate(sequence) if item['buffer_update'])}

def evaluate(expr,params,textures,primary,alpha):
    if expr[0]=='const':return [float(v) for v in expr[1]]
    if expr[0]=='leaf':
        source,channel,complement=expr[1:];value=params[source[1]] if source[0]=='parameter' else textures[source[1:]] if source[0]=='texture' else primary
        result=value[:3] if channel=='color' else [value[{'red':0,'green':1,'blue':2,'alpha':3}[channel]]]*(1 if alpha else 3)
        return [1-v for v in result] if complement else result
    fn,args,scale=expr[1:];values=[evaluate(a,params,textures,primary,alpha) for a in args];out=[]
    for c in range(1 if alpha else 3):
        a=[v[c] for v in values];v=a[0] if fn=='replace' else a[0]*a[1] if fn=='modulate' else a[0]+a[1] if fn=='add' else a[0]-a[1] if fn=='subtract' else a[0]*a[2]+a[1]*(1-a[2]);out.append(max(0,min(1,v*(1<<scale))))
    return out

def fixture():
    source=list(materials(source_fixture()))[0]
    record,receipt=compile_material(source,allow_normalized_arithmetic=True,preserve_color_parameters=True,allow_native_temporaries=True)
    return source,record,receipt

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    original,record,receipt=fixture();assert len(record['stages'])==4 and record['buffer_rgb_mask']==1 and not record['initial_snapshot_only']
    (out/'source-native.dnm').write_bytes(compile_materials([record]));(out/'source-receipt.json').write_text(json.dumps({'record':record,'receipt':receipt},indent=2)+'\n')
    rng=random.Random(149);pixels=0;mask_differences=0;distinct=set()
    for case in range(4096):
        dynamic=copy.deepcopy(original)
        ceiling=[16,64,256,256][case%4]
        dynamic['registers']=[[rng.randrange(ceiling) for c in range(4)] for n in range(3)];dynamic['konst']=[[rng.randrange(ceiling) for c in range(4)] for n in range(4)]
        parameters=dynamic['registers']+dynamic['konst']+[[0]*4];resolved=wire(record,parameters)
        samples={(b['image'],b['coordinate']):[rng.randrange(ceiling)/255 for c in range(4)] for b in receipt['bindings']};primary=[rng.randrange(256)/255 for c in range(4)]
        a,b=source_eval(dynamic,samples,primary),native_eval(resolved,receipt['bindings'],samples,primary)
        assert max(abs(x-y) for x,y in zip(a,b))<1e-12,(case,a,b);pixels+=1;distinct.add(tuple(round(c*255) for c in b))
        bad=copy.deepcopy(resolved);bad['buffer_rgb_mask']=2
        mask_differences+=max(abs(x-y) for x,y in zip(a,native_eval(bad,receipt['bindings'],samples,primary)))>1e-10
    assert len(distinct)>100 and mask_differences>100,'Saturated fixtures failed to observe buffer errors'
    # Independent expression families exercise color and scalar-alpha lanes,
    # subtraction order, three parameter inputs, shared subexpressions and
    # load/hold decisions without a source-material-specific recipe.
    property_cases=0;bindings=[{'image':0,'coordinate':0},{'image':1,'coordinate':1}]
    for alpha in [False,True]:
        channel='alpha' if alpha else 'color';a,b,c=[leaf(('parameter',n),channel) for n in range(3)];t,u=[leaf(('texture',n,n),channel) for n in range(2)]
        left=calc('modulate',(t,a));right=calc('modulate',(u,b))
        expressions=[calc('add',(left,right)),calc('subtract',(left,right)),('calc','interpolate',(a,b,c),0),('calc','interpolate',(left,right,c),0),calc('add',(left,left)),calc('modulate',(a,b))]
        for expression in expressions:
            sequence=schedule_buffer(expression,{(0,0):0,(1,1):1},alpha);assert len(sequence)<=6
            packed=plan_record(sequence,alpha);compile_materials([packed])
            for case in range(128):
                params=[[rng.randrange(256) for c in range(4)] for n in range(8)];textures={(n,n):[rng.randrange(256)/255 for c in range(4)] for n in range(2)};primary=[rng.randrange(256)/255 for c in range(4)]
                expected=evaluate(expression,[[c/255 for c in color] for color in params],textures,primary,alpha)
                got=native_eval(wire(packed,params),bindings,textures,primary)[3:] if alpha else native_eval(wire(packed,params),bindings,textures,primary)[:3]
                assert max(abs(x-y) for x,y in zip(expected,got))<1e-12,(expression,sequence,expected,got);property_cases+=1
    # A strictly sequential eight-stage dependency cannot be made six stages
    # by silently dropping arithmetic. Bounded resource refusal is required.
    expression=leaf(('texture',0,0),'color')
    for n in range(8):expression=('calc','modulate',(expression,leaf(('parameter',n),'color')),0)
    try:schedule_buffer(expression,{(0,0):0})
    except Unsupported:pass
    else:raise AssertionError('Over-budget graph admitted')
    files=[Path(__file__).resolve(),*[ROOT/'Tools/3ds'/n for n in ['j3d_native_materials.py','test_j3d_native_materials.py','native_material_pack.py','j3d_material_source.py']]]
    result={'status':'pass','dynamic_source_samples':pixels,'property_samples':property_cases,'distinct_outputs':len(distinct),'shifted_mask_differences':mask_differences,'source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},'scope':'Offline normalized arithmetic and buffer model only; no PICA execution or source integer-precision acceptance.'}
    (out/'receipt.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
