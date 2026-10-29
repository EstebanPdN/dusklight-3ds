#!/usr/bin/env python3
"""Source hierarchy/sampler model and native metadata contracts; no graphics."""
from pathlib import Path
import argparse,copy,hashlib,json,os,random,struct,subprocess
from j3d_native_geometry import sections,Unsupported
from j3d_native_model import assemble,hierarchy
from j3d_native_samplers import sampler
from native_model_plan import compile_plan
from test_j3d_native_geometry import model as geometry_fixture
from test_j3d_native_materials import source_fixture as material_fixture
ROOT=Path(__file__).resolve().parents[2]

def rebuild(blocks):
    return struct.pack('>8sII16x',b'J3D2bmd3',32+sum(map(len,blocks)),len(blocks))+b''.join(blocks)

def information(commands):
    block=bytearray(24);block[:4]=b'INF1';struct.pack_into('>I',block,20,24)
    block.extend(b''.join(struct.pack('>HH',kind,value) for kind,value in commands));block.extend(bytes((-len(block))%32));struct.pack_into('>I',block,4,len(block));return bytes(block)

def fixture():
    blocks=sections(geometry_fixture());blocks['MAT3']=sections(material_fixture())['MAT3']
    commands=[(16,0),(1,0),(17,0),(1,0),(18,0),(2,0),(16,1),(1,0),(2,0),(16,2),(1,0),(2,0),(2,0),(0,0)]
    blocks['INF1']=information(commands);tex=bytearray(84);tex[:4]=b'TEX1';struct.pack_into('>H',tex,8,2);struct.pack_into('>I',tex,12,20)
    for index in range(2):
        h=bytearray(32);h[0]=14;struct.pack_into('>HH',h,2,16,16);h[6:8]=bytes([index+1,1]);h[16]=not index;h[20:25]=bytes([5 if index==0 else 1,1,0,8 if index==0 else 0,2]);struct.pack_into('>h',h,26,-60 if index==0 else 0)
        struct.pack_into('>I',h,28,len(tex)-(20+index*32));tex[20+index*32:52+index*32]=h;tex.extend(bytes(160))
    tex.extend(bytes((-len(tex))%32));struct.pack_into('>I',tex,4,len(tex));blocks['TEX1']=bytes(tex)
    return rebuild(list(blocks.values()))

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);args=p.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    source=fixture();plan,native,receipt=assemble(source,allow_normalized_arithmetic=True,allow_sdk_sampler_model=True)
    assert len(plan['parts'])==2 and [u['palette'] for u in plan['uniforms']]==[[1,2],[1,3]]
    assert plan['parts'][0]['samplers'][0]['wrap_s']=='mirror' and plan['parts'][0]['samplers'][1]['lod_bias']==-152 and plan['parts'][0]['samplers'][1]['last_level']==1
    assert native==compile_plan(plan) and not receipt['rendering_ready'];(out/'fixture.dnl').write_bytes(native);(out/'plan.json').write_text(json.dumps(plan,indent=2)+'\n')
    # Independently serialized hierarchy demonstrates prepend order and the
    # parent argument's ownership, rather than a naive last-declaration walk.
    b=sections(source);joint=bytearray(b['JNT1']);struct.pack_into('>H',joint,8,3);shape=bytearray(b['SHP1']);struct.pack_into('>H',shape,8,3);mat=bytearray(b['MAT3']);struct.pack_into('>H',mat,8,3)
    b.update(JNT1=bytes(joint),SHP1=bytes(shape),MAT3=bytes(mat),INF1=information([(16,0),(1,0),(17,0),(18,0),(16,1),(1,0),(17,1),(18,1),(2,0),(17,2),(18,2),(2,0),(0,0)]))
    tree=hierarchy(b);assert tree['entries']==[(0,2,2),(0,0,0),(1,1,1)] and tree['parents']=={0:None,1:0}
    rejected=0
    for commands in [[(1,0),(0,0)],[(2,0),(0,0)],[(16,0),(1,0),(0,0)],[(16,3),(0,0)],[(99,0),(0,0)],[(16,0),(16,0),(0,0)],[(17,0),(0,0)],[(16,0),(1,0),(18,0),(2,0),(0,0)],[(16,0),(1,0),(17,0),(18,0),(18,1),(2,0),(0,0)]]:
        bad=dict(b,INF1=information(commands))
        try:hierarchy(bad)
        except ValueError:rejected+=1
        else:raise AssertionError('Invalid source hierarchy admitted')
    for change in [lambda x:x['parts'][0].update(mesh=0),lambda x:x['parts'][0].update(uniform_slot=64),lambda x:x['parts'][0].update(material=99),lambda x:x['uniforms'][0].update(palette=[0]),lambda x:x['parts'][0]['samplers'][0].update(lod_bias=4096),lambda x:x['requirements'].append('unknown'),lambda x:x['uniforms'][0].pop('material')]:
        bad=copy.deepcopy(plan);change(bad)
        try:compile_plan(bad)
        except ValueError:rejected+=1
        else:raise AssertionError('Invalid native model plan admitted')
    # Every raw bias value is checked against an independent single-precision
    # C++ source-control oracle. No unsigned float-cast UB is used in either.
    header=bytearray(32);header[6:8]=bytes((1,2));header[16]=1;header[20:25]=bytes((5,1,0,8,2));lines=[]
    filters=0
    for mode in range(6):
        for mag in range(2):
            for s in range(3):
                for t in range(3):
                    candidate=bytearray(header);candidate[6:8]=bytes((s,t));candidate[20:22]=bytes((mode,mag));native_sampler,_=sampler(bytes(candidate),2,allow_sdk_sampler_model=True)
                    assert native_sampler['wrap_s']==['clamp','repeat','mirror'][s] and native_sampler['wrap_t']==['clamp','repeat','mirror'][t]
                    assert native_sampler['magnify']==['nearest','linear'][mag] and native_sampler['minify']==['nearest','linear'][mode%2]
                    assert native_sampler['mipmap']==('linear' if mode in (4,5) else 'nearest') and native_sampler['last_level']==(0 if mode in (0,1) else 1);filters+=1
    for bias in range(-32768,32768):
        struct.pack_into('>h',header,26,bias);s,_=sampler(bytes(header),2,allow_sdk_sampler_model=True);lines.append(str(s['lod_bias']))
    (out/'bias.txt').write_text('\n'.join(lines)+'\n')
    for field,value in [(17,1),(18,1),(19,1),(22,1),(23,9),(23,24),(20,6)]:
        bad=bytearray(header);bad[field]=value
        try:sampler(bytes(bad),2,allow_sdk_sampler_model=True)
        except ValueError:rejected+=1
        else:raise AssertionError('Unconverted source sampler admitted')
    sources=[ROOT/'Source/platforms/3ds'/n for n in ['native/contracts.cpp','native/pack.cpp','native/model_plan.cpp','tests/native_model_plan_test.cpp']]
    command=['clang++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(ROOT/'Source/platforms/3ds/tests/native_engine_stubs'),'-I'+str(ROOT/'Source/platforms/3ds'),'-I'+str(Path(os.environ.get('DEVKITPRO',Path.home()/'devkitpro'))/'libctru/include'),*map(str,sources),'-o',str(out/'test')]
    r=subprocess.run(command,capture_output=True,text=True,timeout=90);(out/'build.log').write_text(r.stdout+r.stderr);r.check_returncode();r=subprocess.run([str(out/'test'),str(out/'fixture.dnl'),str(out/'bias.txt')],capture_output=True,text=True,timeout=30);(out/'run.log').write_text(r.stdout+r.stderr);r.check_returncode();print(r.stdout,end='')
    files=sources+[ROOT/'Tools/3ds'/n for n in ['j3d_native_model.py','j3d_native_samplers.py','native_model_plan.py','test_native_model_plan.py']]+[ROOT/'Source/platforms/3ds/native/model_plan.hpp']
    (out/'receipt.json').write_text(json.dumps({'status':'pass','source_and_writer_refusals':rejected,'bias_values':65536,'filter_wrap_cases':filters,'source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},'scope':'Native associations, metadata and modeled source controls, not GPU/filtering/gameplay acceptance.'},indent=2)+'\n')
if __name__=='__main__':main()
