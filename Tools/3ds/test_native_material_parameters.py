#!/usr/bin/env python3
"""Native parameter wiring, versioned pack and offline source specialization."""
from pathlib import Path
import argparse,copy,hashlib,json,random,struct,subprocess
from native_material_pack import compile_materials
from j3d_native_materials import compile_material,Unsupported
from j3d_material_source import materials
from test_j3d_native_materials import source_fixture,source_eval,native_eval
ROOT=Path(__file__).resolve().parents[2]

def fixture():
    return [{'id':7,'stages':[{'rgb':['constant','primary','primary'],'alpha':['constant','primary','primary'],'constant':0xa1b2c3d4,'constant_bindings':[1,6,0,32]}]}]

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',required=True,type=Path);args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    records=fixture();packed=compile_materials(records);assert struct.unpack_from('<I',packed,8)[0]==2 and packed==compile_materials(records);(out/'fixture.dnm').write_bytes(packed)
    literal=copy.deepcopy(records);literal[0]['stages'][0]['constant_bindings']=[0]*4;assert struct.unpack_from('<I',compile_materials(literal),8)[0]==1
    rejected=0
    for bad in [[33,0,0,0],[-1,0,0,0],[True,0,0,0],[0,0,0],None]:
        records=fixture();records[0]['stages'][0]['constant_bindings']=bad
        try:compile_materials(records)
        except ValueError:rejected+=1
        else:raise AssertionError('Invalid native parameter writer input admitted')
    source=list(materials(source_fixture()))[0]
    # A real-source-shaped one-stage equation uses dynamic RGB K0/C0 and K3.a.
    # Remove the branch that intentionally still needs a native temporary.
    original=copy.deepcopy(source);s=original['stages'][0];original['stages']=[s]
    s['rgb']=(14,8,10,2);s['rgb_op']=(0,0,2,1,0);s['konst_color_selector']=12
    s['alpha']=(7,6,4,7);s['alpha_op']=(0,0,0,1,0);s['konst_alpha_selector']=31
    record,receipt=compile_material(original,allow_normalized_arithmetic=True,preserve_color_parameters=True)
    assert not record['initial_snapshot_only'] and receipt['runtime_constant_updates_supported'] and not receipt['procedural_color_updates_supported']
    assert record['stages'][0]['constant_bindings']==[13,14,15,28]
    assert record['stages'][1]['constant_bindings'][:3]==[1,2,3]
    samples_checked=0;rng=random.Random(148)
    for _ in range(2048):
        dynamic=copy.deepcopy(original)
        dynamic['registers']=[[rng.randrange(256) for _ in range(4)] for _ in range(3)]
        dynamic['konst']=[[rng.randrange(256) for _ in range(4)] for _ in range(4)]
        colors=dynamic['registers']+dynamic['konst']+[[0]*4]
        # Independently wire native bytes, then evaluate native normalized ops.
        resolved=copy.deepcopy(record)
        for stage in resolved['stages']:
            values=[stage['constant']>>(8*c)&255 for c in range(4)]
            for c,selector in enumerate(stage.get('constant_bindings',[0]*4)):
                if selector:values[c]=colors[(selector-1)//4][(selector-1)%4]
            stage['constant']=sum(value<<(8*c) for c,value in enumerate(values))
        samples={(b['image'],b['coordinate']):[rng.randrange(256)/255 for _ in range(4)] for b in receipt['bindings']};primary=[rng.randrange(256)/255 for _ in range(4)]
        a,b=source_eval(dynamic,samples,primary),native_eval(resolved,receipt['bindings'],samples,primary)
        assert max(abs(x-y) for x,y in zip(a,b))<1e-12,(a,b);samples_checked+=1
    # Preserving parameters must reject the old branched fixture, rather than
    # specialize its current zero/one values to bypass temporary allocation.
    for bad in [source,dict(original,registers=[[-1,0,0,0]]+original['registers'][1:])]:
        try:compile_material(bad,allow_normalized_arithmetic=True,preserve_color_parameters=True)
        except Unsupported:rejected+=1
        else:raise AssertionError('Unsupported live source admitted')
    try:compile_material(original,allow_normalized_arithmetic=True,preserve_color_parameters=True,specialize_initial_constants=True)
    except Unsupported:rejected+=1
    else:raise AssertionError('Conflicting parameter policies admitted')
    (out/'source-receipt.json').write_text(json.dumps({'record':record,'receipt':receipt,'normalized_samples':samples_checked},indent=2)+'\n')
    native=ROOT/'Source/platforms/3ds';files=[native/'native/contracts.cpp',native/'native/pack.cpp',native/'native/material_pack.cpp',native/'tests/native_material_parameters_test.cpp']
    command=['clang++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(native),*map(str,files),'-o',str(out/'test')]
    r=subprocess.run(command,text=True,capture_output=True,timeout=90);(out/'build.log').write_text(r.stdout+r.stderr);r.check_returncode()
    r=subprocess.run([str(out/'test'),str(out/'fixture.dnm')],text=True,capture_output=True,timeout=30);(out/'run.log').write_text(r.stdout+r.stderr);r.check_returncode();print(r.stdout,end='')
    files += [native/'native/contracts.hpp',native/'native/material_pack.hpp',Path(__file__).resolve(),*[ROOT/'Tools/3ds'/x for x in ['native_material_pack.py','j3d_native_materials.py','j3d_material_source.py','test_j3d_native_materials.py']]]
    (out/'receipt.json').write_text(json.dumps({'status':'pass','invalid_inputs':rejected,'normalized_samples':samples_checked,'source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},'scope':'Host native byte wiring and normalized equations only; no PICA execution, GX integer equivalence or gameplay acceptance.'},indent=2)+'\n')
    print(f'PASS {samples_checked} independently wired dynamic source/native equations, unsigned domains and explicit temporary-allocation refusal')
if __name__=='__main__':main()
