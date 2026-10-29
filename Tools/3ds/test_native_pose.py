#!/usr/bin/env python3
"""Native pose contracts and offline matrices, never emulator/GPU execution."""
from pathlib import Path
import argparse,copy,hashlib,json,os,struct,subprocess,shutil
from native_pose_pack import compile_pose
ROOT=Path(__file__).resolve().parents[2]
def identity():return [[1.,0.,0.,0.],[0.,1.,0.,0.],[0.,0.,1.,0.]]
def fixture():
    a=identity();a[0][0]=2;a[0][3]=10;b=identity();b[1][3]=5;c=[[0.,-1.,0.,0.],[1.,0.,0.,0.],[0.,0.,1.,0.]]
    first=identity();first[0][3]=-2;second=identity();second[1][3]=3
    terms=[{'node':2,'weight':.25,'inverse_bind':first},{'node':3,'weight':.5,'inverse_bind':second}]
    return {'schema':'dusklight-native-pose/1','nodes':[{'parent':0,'anchor':1,'local':a},{'parent':1,'anchor':2,'local':b},{'parent':0,'anchor':3,'local':c}],
            'matrices':[{'node':2,'normal':'inverse_transpose'},{'terms':terms,'normal':'inverse_transpose'},{'terms':copy.deepcopy(terms),'normal':'linear'}]}
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);p.add_argument('--negative-controls',action='store_true');args=p.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    native=ROOT/'Source/platforms/3ds';sources=[native/'native/contracts.cpp',native/'native/pack.cpp',native/'native/pose.cpp',native/'tests/native_pose_test.cpp'];files=sources+[native/'native/pose.hpp',ROOT/'Tools/3ds/native_pose_pack.py',ROOT/'Tools/3ds/j3d_native_pose.py',Path(__file__).resolve()]
    pins={str(f.relative_to(ROOT)):hashlib.sha256(f.read_bytes()).hexdigest() for f in files};packed=compile_pose(fixture());(out/'fixture.dps').write_bytes(packed);rejected=0
    for change in [lambda d:d.update(schema='unknown'),lambda d:d['nodes'][0].update(parent=1),lambda d:d['nodes'][1].update(anchor=1),lambda d:d['nodes'][0]['local'][0].__setitem__(0,float('nan')),lambda d:d['nodes'][0]['local'][0].__setitem__(0,2**-70),lambda d:d['matrices'][0].update(node=4),lambda d:d['matrices'][0].update(normal='stale'),lambda d:d['matrices'][1]['terms'][0].update(weight=True),lambda d:d['matrices'][1]['terms'][0].update(node=0),lambda d:d['matrices'][1].update(terms=[])]:
        doc=copy.deepcopy(fixture());change(doc)
        try:compile_pose(doc)
        except ValueError:rejected+=1
        else:raise AssertionError('Malformed native pose accepted')
    command=['clang++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-ffp-contract=off','-I'+str(native),*map(str,sources),'-o',str(out/'native-pose-test')]
    build=subprocess.run(command,text=True,capture_output=True,timeout=90);(out/'build.log').write_text(build.stdout+build.stderr);build.check_returncode()
    run=subprocess.run([str(out/'native-pose-test'),str(out/'fixture.dps')],text=True,capture_output=True,timeout=30,env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
    (out/'run.log').write_text(run.stdout+run.stderr);run.check_returncode();print(run.stdout,end='')
    controls=[]
    if args.negative_controls:
        mutations=[('parent-ignored','compose(parent?world[parent-1]:root,local,world[n])','compose(parent?root:root,local,world[n])'),
            ('normal-policy-ignored','if(mode==NormalMode::inverseTranspose)','if(mode==NormalMode::inverseTranspose||mode==NormalMode::linear)'),
            ('inverse-bind-ignored','compose(world[word(bytes,term)-1],inverse,product)','compose(world[word(bytes,term)-1],(void(inverse),identity()),product)'),
            ('stale-success-view','out={};if(!count','if(!count'),
            ('singular-normal-fallback','if(!std::isfinite(determinant)||determinant==0)return false;','if(!std::isfinite(determinant)||determinant==0){out=PosePack::identity();return true;}')]
        for name,old,new in mutations:
            dest=out/'mutants'/name;dest.mkdir(parents=True,exist_ok=True);source=dest/'pose.cpp';text=(native/'native/pose.cpp').read_text();assert text.count(old)==1;source.write_text(text.replace(old,new));cmd=command.copy();cmd.insert(1,'-I'+str(native/'native'));cmd[cmd.index(str(native/'native/pose.cpp'))]=str(source);cmd[-1]=str(dest/'test');build=subprocess.run(cmd,text=True,capture_output=True,timeout=90);(dest/'build.log').write_text(build.stdout+build.stderr);build.check_returncode()
            result=subprocess.run([str(dest/'test'),str(out/'fixture.dps')],text=True,capture_output=True,timeout=30);(dest/'run.log').write_text(result.stdout+result.stderr);assert result.returncode!=0,'Undetected native pose mutation '+name;controls.append({'name':name,'detected':True,'exit':result.returncode})
    assert pins=={str(f.relative_to(ROOT)):hashlib.sha256(f.read_bytes()).hexdigest() for f in files}
    (out/'receipt.json').write_text(json.dumps({'status':'pass','scope':'Native CPU pose parameter contracts, no vertex skinning or GPU execution','writer_refusals':rejected,'negative_controls':controls,'source_sha256':pins,'command':command},indent=2)+'\n')
if __name__=='__main__':main()
