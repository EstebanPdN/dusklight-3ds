#!/usr/bin/env python3
"""Check ARM archive admission and allocated-relocation filtering in the probe."""
import argparse, hashlib, json, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
compiler=Path('/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++')
sources={
 'root': '.section .text.root,"ax",%progbits\n.global dusk_probe_root\ndusk_probe_root:\n bl dusk_probe_provider\n bl dusk_probe_missing\n bx lr\n',
 'provider': '.section .text.provider,"ax",%progbits\n.global dusk_probe_provider\ndusk_probe_provider:\n bx lr\n.section .text.unused,"ax",%progbits\n.global dusk_probe_unused\ndusk_probe_unused:\n bl dusk_probe_unreachable\n bx lr\n'}
units=[];commands=[]
for name,source in sources.items():
 path=out/(name+'.S');path.write_text(source);obj=out/(name+'.o')
 cmd=[str(compiler),'-march=armv6k','-c',str(path),'-o',str(obj)]
 subprocess.run(cmd,check=True,capture_output=True);commands.append(cmd)
 units.append({'object':str(obj.relative_to(ROOT)),'command':cmd})
fixture=out/'inventory.json';fixture.write_text(json.dumps({'origin':'synthetic ARM assembly test fixture, no game data','units':units,'native_object_receipts':[]},indent=2)+'\n')
probe=ROOT/'Tools/3ds/probe_environment_link.py'
cmd=['python3',str(probe),'--inventory',str(fixture),'--output',str(out/'valid'),'--root','dusk_probe_root']
r=subprocess.run(cmd,capture_output=True,text=True);commands.append(cmd);(out/'valid.log').write_text(r.stdout+r.stderr);assert r.returncode==0
report=json.loads((out/'valid/results.json').read_text());assert report['defined_roots']==['dusk_probe_root']
assert report['retained_undefined']=={'dusk_probe_missing':'dusk_probe_missing'}
cmd=['python3',str(probe),'--inventory',str(fixture),'--output',str(out/'missing'),'--root','dusk_probe_root','--root','dusk_probe_absent']
r=subprocess.run(cmd,capture_output=True,text=True);commands.append(cmd);(out/'missing.log').write_text(r.stdout+r.stderr);assert r.returncode==2
report=json.loads((out/'missing/results.json').read_text());assert report['exit']==2
assert report['missing_roots']==['dusk_probe_absent']
result={'status':'pass','origin':'synthetic ARM relocatable link; no execution or gameplay',
 'checks':['requested root and transitive archive provider admitted','reachable missing provider retained','unreachable missing provider excluded','absent root fails explicitly instead of false clean result'],
 'commands':commands,'input_sha256':{str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest() for x in [Path(__file__).resolve(),probe,*out.glob('*.S')]}}
(out/'results.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS: archive roots, transitive providers, retained-only references and absent-root rejection')
