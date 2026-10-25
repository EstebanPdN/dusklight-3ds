#!/usr/bin/env python3
"""Inspect retained original model dependencies; not a final executable link."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[2]
BIN=Path('/opt/devkitpro/devkitARM/bin')

def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inventory',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    data=json.loads(args.inventory.read_text());objects=[]
    for item in data['native_object_receipts']:
        path=ROOT/item['path'];assert digest(path)==item['sha256'];objects.append(path)
    for unit in data['units']:
        if unit['returncode']==0:
            assert digest(ROOT/'Source'/unit['path'])==unit['source_sha256']
            objects.append(ROOT/unit['object'])
    archive=out/'current.a'
    subprocess.run([str(BIN/'arm-none-eabi-ar'),'rcs',str(archive),*map(str,objects)],check=True)
    report={'origin':'ARM relocatable section GC; no runtime libraries or executable acceptance',
            'inventory':str(args.inventory),'inventory_sha256':digest(args.inventory),
            'objects':[{'path':str(p.relative_to(ROOT)),'sha256':digest(p)} for p in objects],
            'probes':[]}
    roots={'model_load':['_ZN22J3DModelLoaderDataBase4loadEPKvm'],
           'model_update':['_ZN8J3DModel14entryModelDataEP12J3DModelDatamm','_ZN8J3DModel6updateEv']}
    definitions=subprocess.check_output([str(BIN/'arm-none-eabi-nm'),'--defined-only',str(archive)],text=True)
    for name,symbols in roots.items():
        for symbol in symbols:assert symbol in definitions
        obj=out/(name+'.o');command=[str(BIN/'arm-none-eabi-ld'),'-r','--gc-sections','-Map',str(out/(name+'.map'))]
        for symbol in symbols:command+=['-u',symbol]
        command += [str(archive),'-o',str(obj)]
        result=subprocess.run(command,capture_output=True,text=True)
        (out/(name+'.log')).write_text(result.stdout+result.stderr);result.check_returncode()
        raw=subprocess.check_output([str(BIN/'arm-none-eabi-nm'),'-u',str(obj)],text=True)
        undefined=[line.split()[-1] for line in raw.splitlines() if line.strip()]
        names=subprocess.check_output([str(BIN/'arm-none-eabi-c++filt'),*undefined],text=True).splitlines()
        report['probes'].append({'name':name,'roots':symbols,'command':command,'exit':result.returncode,
                                'retained_undefined':[{'symbol':s,'name':n} for s,n in zip(undefined,names)]})
        print(name,len(undefined),'retained undefined symbols (includes libraries)')
    report['limitations']='Counts include C/C++/platform runtime and unselected utility objects; are not implementation-task counts. Unsupported texture-pointer/TLUT paths still panic. No GX execution or gameplay proven.'
    (out/'results.json').write_text(json.dumps(report,indent=2)+'\n')

if __name__=='__main__':main()
