#!/usr/bin/env python3
"""Compile/run native contracts and backend with SDK substitutes; never Azahar."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil

ROOT=Path(__file__).resolve().parents[2]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--negative-controls',action='store_true')
    args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    sdk=Path(os.environ.get('DEVKITPRO',Path.home()/'devkitpro'))
    native=ROOT/'Source/platforms/3ds'
    sources=[native/'native/contracts.cpp',native/'native/renderer.cpp',native/'native/quad_batch.cpp',native/'native/pack.cpp',native/'native/texture_pack.cpp',native/'native/material_pack.cpp',native/'native/model.cpp',native/'native/model_plan.cpp',native/'native/scene.cpp',native/'native/pose.cpp',native/'native/game_draw.cpp',native/'native/game_source.cpp',native/'native/asset_pack.cpp',native/'native/game_assets.cpp',native/'native/camera.cpp',native/'tests/native_engine_test.cpp']
    files=sources+[native/'native/contracts.hpp',native/'native/renderer.hpp',native/'native/quad_batch.hpp',native/'native/pack.hpp',native/'native/texture_pack.hpp',native/'native/scene.v.pica',native/'native/scene_rigid.v.pica',native/'tests/native_engine_stubs/citro3d.h',Path(__file__).resolve(),ROOT/'Tools/3ds/native_mesh_pack.py',ROOT/'Tools/3ds/test_native_pack.py',ROOT/'Tools/3ds/native_texture_pack.py',ROOT/'Tools/3ds/test_native_texture_pack.py']
    files.extend([native/'native/scene_uv.v.pica',native/'native/scene_rigid_uv.v.pica'])
    files.extend([native/'native/scene_projective.v.pica',native/'native/scene_rigid_projective.v.pica'])
    files.extend([native/'native/material_pack.hpp',ROOT/'Tools/3ds/native_material_pack.py',ROOT/'Tools/3ds/test_native_material_pack.py'])
    files.extend([ROOT/'Tools/3ds'/name for name in ['j3d_native_materials.py','j3d_material_source.py','test_j3d_native_materials.py','j3d_native_geometry.py']])
    files.append(ROOT/'Tools/3ds/test_native_material_parameters.py')
    files.append(ROOT/'Tools/3ds/test_native_material_temporaries.py')
    files.extend([native/'native/game_draw.hpp',native/'native/game_draw_api.hpp',ROOT/'Tools/3ds/native_game_packet_fixture.py',ROOT/'Source/libs/JSystem/src/J3DGraphBase/J3DPacket.cpp',native/'native/model.hpp',native/'native/scene.hpp',native/'native/pose.hpp',ROOT/'Tools/3ds/native_pose_pack.py',ROOT/'Tools/3ds/test_native_pose.py'])
    files.extend([native/'native'/p for p in ['game_source.hpp','asset_pack.hpp','game_assets.hpp','model_plan.hpp']])
    files.extend([ROOT/'Tools/3ds'/p for p in ['native_asset_pack.py','native_model_plan.py']])
    files.extend([ROOT/'Tools/3ds/native_game_source_fixture.py',native/'tests/native_game_assets_cases.hpp',ROOT/'Source/src/d/d_resorce.cpp',ROOT/'Source/libs/JSystem/src/J3DGraphAnimator/J3DModel.cpp'])
    files.extend([native/'native/camera.hpp',ROOT/'Tools/3ds/native_camera_fixture.py',native/'tests/native_camera_cases.hpp',ROOT/'Source/libs/dolphin/src/mtx/mtx44.c',ROOT/'Source/src/m_Do/m_Do_graphic.cpp'])
    files.extend(p for p in [ROOT/'Upstream/extracted/citro3d-1.7.1/source/maths'/n for n in ['mtx_persptilt.c','mtx_orthotilt.c']] if p.is_file())
    before={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
    from test_native_pack import fixture
    from native_mesh_pack import compile_pack
    fixture_path=out/'native-fixture.pica';fixture_path.write_bytes(compile_pack(fixture()))
    from test_native_texture_pack import fixture as texture_fixture
    from native_texture_pack import compile_images
    texture_path=out/'native-texture-fixture.dnt';texture_path.write_bytes(compile_images(texture_fixture()))
    from test_native_material_pack import fixture as material_fixture
    from native_material_pack import compile_materials
    from test_native_material_parameters import fixture as parameters_fixture
    model_stage={'rgb':['texture0','constant','primary'],'alpha':['texture0','constant','primary'],'rgb_function':'modulate','alpha_function':'modulate','constant_bindings':[1,2,3,4]}
    model_records=[{'id':23,'images':[1,0,0],'stages':[model_stage]}, {'id':29,'stages':[{}]},
        {'id':37,'images':[1,0,0],'stages':[model_stage],'initial_snapshot_only':True},
        {'id':41,'images':[1,0,0],'stages':[model_stage],'requires_fog':True},
        {'id':43,'images':[99,0,0],'stages':[model_stage]},
        {'id':47,'stages':[{}],'depth_test':False,'depth_write':False}]
    material_path=out/'native-material-fixture.dnm';material_path.write_bytes(compile_materials(material_fixture()+parameters_fixture()+model_records))
    from test_j3d_native_materials import source_fixture
    from j3d_native_materials import convert as compile_source_materials
    source_material_path=out/'source-compiled-material-fixture.dnm';source_material_path.write_bytes(compile_source_materials(source_fixture(),allow_normalized_arithmetic=True,specialize_initial_constants=True)[0])
    from test_native_material_temporaries import fixture as buffered_fixture
    buffered_path=out/'native-buffered-material-fixture.dnm';buffered_path.write_bytes(compile_materials([buffered_fixture()[1]]))
    from test_native_pose import fixture as pose_fixture
    from native_pose_pack import compile_pose
    pose_path=out/'native-pose-fixture.dps';pose_path.write_bytes(compile_pose(pose_fixture()))
    from native_game_packet_fixture import generate
    (out/'native_game_packet_fixture.hpp').write_text(generate())
    from native_game_source_fixture import generate as source_fixture_header
    (out/'native_game_source_fixture.hpp').write_text(source_fixture_header())
    from native_camera_fixture import generate as camera_header
    camera_text,camera_sdk=camera_header();(out/'native_camera_fixture.hpp').write_text(camera_text)
    from native_asset_pack import compile_asset,identity
    from native_model_plan import compile_plan
    asset_dir=out/'asset-fixture';asset_dir.mkdir(exist_ok=True)
    for size in [32,55,56,63,64,65,97,128,1024,4096]:
        source=bytearray((n*17+3)%256 for n in range(size));source[:8]=b'J3D2bmd3';source[8:12]=size.to_bytes(4,'big');source=bytes(source)
        (asset_dir/(str(size)+'.source')).write_bytes(source);(asset_dir/(str(size)+'.sha256')).write_bytes(hashlib.sha256(source).digest())
    source=(asset_dir/'97.source').read_bytes()
    base={'mesh':3,'material':23,'uniform_slot':0,'sampler_mask':1,'samplers':[{}, {}, {}],'pass':'opaque'}
    plan={'schema':'dusklight-native-model-plan/1','requirements':['poses','colors'],'parts':[base,{**base,'mesh':17,'uniform_slot':1},{**base,'material':29,'sampler_mask':0}],
          'uniforms':[{'material':23,'anchor':1,'palette':[1,2,3]},{'material':23,'anchor':1,'palette':[1,2,3]}]}
    # The untextured third part uses a distinct slot/material, matching offline ownership.
    plan['parts'][2]['uniform_slot']=2;plan['uniforms'].append({'material':29,'anchor':1,'palette':[1,2,3]})
    packed=compile_asset(source,fixture_path.read_bytes(),texture_path.read_bytes(),material_path.read_bytes(),compile_plan(plan),[5,5,8])
    (asset_dir/(identity(source).hex()+'.dna')).write_bytes(packed)
    command=['clang++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer',
             '-I'+str(out),'-I'+str(native/'tests/native_engine_stubs'),'-I'+str(native),'-I'+str(sdk/'libctru/include'),*map(str,sources),'-o',str(out/'native-engine-test')]
    receipt={'scope':'Host SDK substitutes, not PICA execution, hardware correctness or performance','status':'running','command':command,'camera_sdk_source_oracle':camera_sdk}
    try:
        built=subprocess.run(command,text=True,capture_output=True,timeout=90)
        (out/'host-build.log').write_text(built.stdout+built.stderr);built.check_returncode()
        run=subprocess.run([str(out/'native-engine-test'),str(fixture_path),str(texture_path),str(material_path),str(source_material_path),str(buffered_path),str(pose_path),str(asset_dir)],text=True,capture_output=True,timeout=30,
            env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
        (out/'host-run.log').write_text(run.stdout+run.stderr);run.check_returncode();print(run.stdout,end='')
        if args.negative_controls:
            controls=[('early-retirement','renderer.cpp','    if(!C3D_FrameBegin(C3D_FRAME_NONBLOCK))','    collect();\n    if(!C3D_FrameBegin(C3D_FRAME_NONBLOCK))'),
                      ('stale-generation','renderer.hpp','++s.generation;','(void)0;'),
                      ('clear-reordering','renderer.cpp','    if(emitted)C3D_FrameSplit(GX_CMDLIST_FLUSH);','    (void)emitted;'),
                      ('projective-owned-mutation','renderer.cpp','            sampler=&view;','            *sampler=view;'),
                      ('missing-material-parameters','renderer.cpp','    if((u.material.suppliedMask&p.parameterMask)!=p.parameterMask)return fail(Error::invalidAsset);','    (void)p.parameterMask;'),
                      ('parameter-channel-alias','renderer.cpp','>>(8*(source%4))','>>(8*(source/4%4))'),
                      ('sampler-resident-levels','renderer.cpp','        if((u.samplerMask&(1u<<n))&&!validateSampler(u.samplers[n],bound[n]->maxLevel+1))return fail(Error::invalidAsset);','        (void)n;'),
                      ('model-parameter-preflight','model.cpp','        if(u.samplerMask||!validateUniforms(u,uniformBones[n])||(u.material.suppliedMask&uniformParameters[n])!=uniformParameters[n])return reject(Error::invalidAsset);','        (void)u;'),
                      ('scene-preflight','scene.cpp','        if(error!=Error::none)return {0,error};','        (void)error;'),
                      ('scene-tie-order','scene.cpp','        return a<b;','        return a>b;'),
                      ('scene-partial-result','scene.cpp','return {submitted,item.model->error()};','return {unsigned(items.size()),Error::none};'),
                      ('game-owned-shapes','game_draw.cpp','vacant->shapes[n]=std::uint16_t(shapes[n]);','vacant->shapes[n]=0;'),
                      ('game-stale-before-producer','game_draw.cpp','if(!model.loaded||model.renderer!=renderer||model.revision!=binding->revision)return reject(Error::invalidHandle);','if(!model.loaded||model.renderer!=renderer)return reject(Error::invalidHandle);'),
                      ('game-ignore-reentry','game_draw.cpp','if(submitting){invalidated=true;return reject(Error::protocol);}','if(submitting)return reject(Error::protocol);'),
                      ('game-partial-success','game_draw.cpp','failure=result.error;return result;','failure=result.error;return {count,Error::none};'),
                      ('game-source-dispatch','native_game_packet_fixture.hpp','        drawNativeGameShape(*packet);','        (void)packet;'),
                      ('game-source-state-handoff','native_game_packet_fixture.hpp','    dusk::native::invalidatePicaCommandState();','    (void)0;'),
                      ('source-extent','game_source.cpp','if(declared!=original.size())return false;','(void)declared;'),
                      ('asset-source-identity','asset_pack.cpp','std::memcmp(b.data()+24,expected.sha256.data(),32)','false'),
                      ('asset-input-admission','game_assets.cpp','const auto bindings=pack.plan.bindings(inputs);','const auto bindings=pack.plan.bindings(63);'),
                      ('asset-shape-ownership','game_assets.cpp','asset->shapes=pack.shapes;','asset->shapes={};'),
                      ('asset-shared-lifetime','game_assets.cpp','if(!asset->references){','if(asset->references<=1){'),
                      ('asset-environment-alias','game_assets.cpp','||!sameEnvironment(*asset)',''),
                      ('asset-external-context','game_assets.cpp','if(pack.requiresTextureContext)return fail(GameAssetError::textureContext);','(void)pack.requiresTextureContext;'),
                      ('source-single-matrix-mode','native_game_source_fixture.hpp','mModelData->checkFlag(0x110) ||','false ||'),
                      ('model-resource-sharing','model.hpp','        for(unsigned n=0;n<count;++n)if(resources[n].id==id)return int(n);','        (void)resources;'),
                      ('model-partial-result','model.cpp','return reject(r.error(),submitted);','return {partCount,Error::none};')]
            receipt['negative_controls']=[]
            for name,file,old,new in controls:
                variant=out/'mutants'/name;directory=variant/'native';directory.mkdir(parents=True,exist_ok=True)
                for p in (native/'native').glob('*'):
                    if p.suffix in ('.hpp','.cpp'):shutil.copy2(p,directory/p.name)
                shutil.copy2(out/'native_game_packet_fixture.hpp',directory/'native_game_packet_fixture.hpp')
                shutil.copy2(out/'native_game_source_fixture.hpp',directory/'native_game_source_fixture.hpp')
                path=directory/file;text=path.read_text();assert text.count(old)==1;path.write_text(text.replace(old,new))
                variant_command=command.copy();variant_command.insert(1,'-I'+str(variant))
                variant_command.insert(2,'-I'+str(directory))
                for i,arg in enumerate(variant_command):
                    if arg in map(str,sources[:-1]):variant_command[i]=str(directory/Path(arg).name)
                variant_command[-1]=str(variant/'test')
                built=subprocess.run(variant_command,text=True,capture_output=True,timeout=90)
                (variant/'build.log').write_text(built.stdout+built.stderr);built.check_returncode()
                result=subprocess.run([str(variant/'test'),str(fixture_path),str(texture_path),str(material_path),str(source_material_path),str(buffered_path),str(pose_path),str(asset_dir)],text=True,capture_output=True,timeout=30,
                    env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
                (variant/'run.log').write_text(result.stdout+result.stderr)
                assert result.returncode!=0,'Undetected mutation '+name
                receipt['negative_controls'].append({'name':name,'exit':result.returncode,'detected':True})
        assert before=={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},'Inputs changed during verification'
        receipt['status']='pass'
    except (OSError,subprocess.SubprocessError,AssertionError) as error:
        receipt.update(status='fail',error=str(error));raise
    finally:
        receipt['source_sha256']=before
        (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')

if __name__=='__main__':main()
