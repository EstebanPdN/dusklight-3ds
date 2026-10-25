#!/usr/bin/env python3
"""Resolve retained original environment dependencies; never claim runtime from this probe."""
import argparse, hashlib, json, re, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def run(c): return subprocess.run(list(map(str,c)),capture_output=True,text=True,check=True).stdout
def refs(tool,obj):
    lines=run([tool,'-h',obj]).splitlines(); allocated=set()
    for i,line in enumerate(lines[:-1]):
        f=line.split()
        if f and f[0].isdigit() and 'ALLOC' in lines[i+1]:allocated.add(f[1])
    section=None; found=set(); text=run([tool,'-r',obj])
    for line in text.splitlines():
        match=re.match(r'RELOCATION RECORDS FOR \[(.+)\]:',line)
        if match:section=match[1];continue
        f=line.split()
        if section in allocated and len(f)==3 and f[1].startswith('R_ARM_'):found.add(re.sub(r'[+-]0x[0-9a-fA-F]+$','',f[2]))
    return found,text
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--inventory',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--root',action='append',help='Explicit retained symbol; repeat to override the default environment roots');a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=False)
inventory=json.loads(a.inventory.read_text());tool=Path(inventory['units'][0]['command'][0]).parent
objects=[ROOT/u['object'] for u in inventory['units'] if u['object']]+[ROOT/u['path'] for u in inventory['native_object_receipts']]
# Preserve exact inputs and consume only newly compiled/currently recorded objects.
receipts={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in objects}
archive=a.output/'context.a';run([tool/'arm-none-eabi-ar','rcs',archive,*objects])
roots=['_Z15dKy_tevstr_initP12dKy_tevstr_cah','_ZN18dScnKy_env_light_c11setLight_bgEP12dKy_tevstr_cP10GXColorS10S3_PfS4_','_ZN18dScnKy_env_light_c25setLightTevColorType_MAJIEP12J3DModelDataP12dKy_tevstr_c']
if a.root:roots=a.root
obj=a.output/'context.o';command=[tool/'arm-none-eabi-ld','-r','--gc-sections']
# Register the roots before scanning an archive. ld processes archive admission
# at its command-line position; late -u roots can leave existing providers
# unresolved in a relocatable link. Group rescanning also admits cyclic edges.
for root in roots:command+=['-u',root]
command+=['--start-group',archive,'--end-group','-o',obj]
link=subprocess.run(list(map(str,command)),capture_output=True,text=True);(a.output/'link.log').write_text(link.stdout+link.stderr)
result={'origin':'Relocatable link only; original constructors and gameplay NOT executed','command':list(map(str,command)),'exit':link.returncode,'objects_sha256':receipts}
if link.returncode==0:
    defined={x.split()[-1] for x in run([tool/'arm-none-eabi-nm','--defined-only',obj]).splitlines() if x.split()}
    result['requested_roots']=roots
    result['defined_roots']=[root for root in roots if root in defined]
    result['missing_roots']=[root for root in roots if root not in defined]
    if result['missing_roots']:
        result['exit']=2
        (a.output/'results.json').write_text(json.dumps(result,indent=2)+'\n')
        print('Probe did not admit requested roots: '+', '.join(result['missing_roots']))
        raise SystemExit(2)
    references,relocations=refs(tool/'arm-none-eabi-objdump',obj);(a.output/'relocations.txt').write_text(relocations)
    undefined={x.split()[-1] for x in run([tool/'arm-none-eabi-nm','-u',obj]).splitlines() if x.split()}
    retained=sorted(undefined&references);names=run([tool/'arm-none-eabi-c++filt',*retained]).splitlines()
    result['retained_undefined']=dict(zip(retained,names));print('Retained unresolved:',len(retained));print('\n'.join(names))
(a.output/'results.json').write_text(json.dumps(result,indent=2)+'\n')
raise SystemExit(link.returncode)
