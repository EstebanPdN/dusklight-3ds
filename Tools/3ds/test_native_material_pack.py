#!/usr/bin/env python3
"""Native material binary contracts with actual parser under sanitizers."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import native_material_pack as pack
from j3d_native_materials import compile_material
from test_native_material_raster_lookup import declaration,fog_declaration

ROOT=Path(__file__).resolve().parents[2]

def fixture():
    return [{'id':3,'images':[17,0,0],'requires_fog':True,'blend':'custom',
        'custom_blend':{'color_source':'sourceAlpha','color_destination':'oneMinusSourceAlpha','alpha_source':'sourceAlpha','alpha_destination':'oneMinusSourceAlpha'},
        'stages':[{'rgb':['texture0','primary','primary'],'alpha':['texture0','primary','primary'],'rgb_function':'modulate','alpha_function':'modulate'}]},
        {'id':11,'stages':[{'rgb':['primary']*3,'alpha':['primary']*3}],
         'fog_values':[n/255 for n in range(256)],'fog_color':0x12345678}]

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    records=fixture();packed=pack.compile_materials(records);assert packed==pack.compile_materials(fixture());(out/'fixture.dnm').write_bytes(packed)
    lookup=declaration();lookup_record,_=compile_material(lookup,allow_normalized_arithmetic=True,preserve_color_parameters=True)
    (out/'lookup.dnm').write_bytes(pack.compile_materials([lookup_record]))
    lookup['stages'][0]['alpha']=(4,7,7,7);lookup['stages'][1]['alpha']=(0,7,7,7)
    lookup['states']['fog']=(2,1,1,64)+tuple(bytes(40))
    fog_record,_=compile_material(lookup,allow_normalized_arithmetic=True,preserve_color_parameters=True)
    (out/'fog-lookup.dnm').write_bytes(pack.compile_materials([fog_record]))
    texture_fog=[]
    for unit in range(3):
        source=fog_declaration(unit,projective=unit>0);source['id']=unit+1
        record,_=compile_material(source,allow_normalized_arithmetic=True,preserve_color_parameters=True,synthetic_image_base=12)
        texture_fog.append(record)
    source=declaration();source['id']=4;source['stages'][0]['rgb']=(10,15,15,15);source['stages'][0]['alpha']=(5,7,7,7);source['states']['fog']=lookup['states']['fog']
    record,_=compile_material(source,allow_normalized_arithmetic=True,preserve_color_parameters=True,synthetic_image_base=12);texture_fog.append(record)
    (out/'texture-fog.dnm').write_bytes(pack.compile_materials(texture_fog))
    for index,change in [(0,lambda x:x.update(images=[0,0,0])),(0,lambda x:x['stages'][0].update(alpha=['texture0']*3)),
        (1,lambda x:x.update(fog_carrier='texture0')),(3,lambda x:x.update(fog_carrier='texture1')),
        (2,lambda x:x.update(fog_values=[0]*256)),(0,lambda x:x['stages'][-1].update(alpha=['primary']*3))]:
        bad=copy.deepcopy(texture_fog[index]);change(bad)
        try:pack.compile_materials([bad])
        except ValueError:pass
        else:raise AssertionError('Fog texture collision, missing binding or alpha loss accepted')
    rejected=0
    for change in [lambda x:x[0].update(id=11),lambda x:x[0].update(images=[0,0,0]),lambda x:x[0].update(blend='source'),
        lambda x:x[0]['stages'][0].update(rgb_function='unknown'),lambda x:x[0]['stages'][0].update(alpha_function='dot3'),
        lambda x:x[0]['stages'][0].update(rgb=['previous']*3),lambda x:x[1]['fog_values'].__setitem__(7,float('nan')),
        lambda x:x[0].update(requires_fog=1),lambda x:x[0].update(custom_blend={'color_source':'source-register'})]:
        bad=copy.deepcopy(records);change(bad)
        try:pack.compile_materials(bad)
        except ValueError:rejected+=1
        else:raise AssertionError('Invalid native material accepted')
    native=ROOT/'Source/platforms/3ds';sources=[native/'native/contracts.cpp',native/'native/pack.cpp',native/'native/material_pack.cpp',native/'tests/native_material_pack_test.cpp']
    command=['clang++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(native),*map(str,sources),'-o',str(out/'test')]
    result=subprocess.run(command,text=True,capture_output=True,timeout=90);(out/'build.log').write_text(result.stdout+result.stderr);result.check_returncode()
    result=subprocess.run([str(out/'test'),str(out/'fixture.dnm'),str(out/'lookup.dnm'),str(out/'fog-lookup.dnm'),str(out/'texture-fog.dnm')],text=True,capture_output=True,timeout=30);(out/'run.log').write_text(result.stdout+result.stderr);result.check_returncode();print(result.stdout,end='')
    files=sources+[native/'native/material_pack.hpp',ROOT/'Tools/3ds/native_material_pack.py',Path(__file__).resolve()]
    (out/'receipt.json').write_text(json.dumps({'status':'pass','invalid_native_inputs':rejected,'source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},'command':command,
        'scope':'Native fragment-state parsing and required native fog binding. No source visual equivalence, rendering or physical acceptance.'},indent=2)+'\n')

if __name__=='__main__':main()
