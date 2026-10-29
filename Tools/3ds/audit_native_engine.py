#!/usr/bin/env python3
"""Seal the standalone ARM native engine and check its dependency boundary."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

ROOT=Path(__file__).resolve().parents[2]

def sha(path):
    with path.open('rb') as stream:
        digest=hashlib.sha256()
        for block in iter(lambda:stream.read(1024*1024),b''):digest.update(block)
        return digest.hexdigest()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();build=args.build_dir.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    sdk=Path(os.environ.get('DEVKITPRO',Path.home()/'devkitpro'))
    archive=build/'libdusk_pica_engine.a'
    files=sorted(p for p in (ROOT/'Source/platforms/3ds/native').iterdir() if p.suffix in ['.cpp','.hpp','.pica'])
    # Incremental build timestamps are not provenance. Reassemble each source
    # to detect a stale shader after restores that preserved older mtimes.
    for source in (ROOT/'Source/platforms/3ds/native').glob('*.v.pica'):
        name='native_'+source.name.removesuffix('.v.pica')+'.shbin'
        fresh=out/('verified-'+name)
        subprocess.run([str(sdk/'tools/bin/picasso'),'-o',str(fresh),str(source)],check=True,capture_output=True,timeout=20)
        assert fresh.read_bytes()==(build/name).read_bytes(),'Stale generated shader: '+name
        symbol=name.removesuffix('.shbin')+'_shader'
        obj=out/(symbol+'.obj');obj.write_bytes(subprocess.check_output([str(sdk/'devkitARM/bin/arm-none-eabi-ar'),'p',str(archive),symbol+'.cpp.obj'],timeout=20))
        embedded=out/('embedded-'+name)
        subprocess.run([str(sdk/'devkitARM/bin/arm-none-eabi-objcopy'),'--dump-section','.rodata.'+symbol+'='+str(embedded),str(obj)],check=True,capture_output=True,timeout=20)
        assert embedded.read_bytes()==fresh.read_bytes(),'Stale embedded shader in archive: '+symbol
    forbidden=re.compile(r'#include[^\n]*(?:gx_|dolphin|aurora|JSystem)|\b(?:CommandProcessor|CommandStream|drawGxCommand|planPicaMaterial|decodeMaterial)\b')
    for file in files:assert not forbidden.search(file.read_text()),file
    nm=sdk/'devkitARM/bin/arm-none-eabi-nm'
    undefined=subprocess.check_output([str(nm),'-u',str(archive)],text=True)
    for name in ['GXBegin','GXLoad','drawGxCommand','CommandProcessor','CommandStream','planPicaMaterial','decodeMaterial','aurora']:
        assert name not in undefined,name
    (out/'undefined-symbols.log').write_text(undefined)
    entries=json.loads((build/'compile_commands.json').read_text());commands=[]
    for entry in entries:
        if 'CMakeFiles/dusk_pica_engine.dir/' not in entry['command']:continue
        args=shlex.split(entry['command']);assert '-O2' in args and '-include' not in args
        assert not any('DUSK_TARGET_3DS' in x or 'TARGET_PC' in x or 'dolphin' in x for x in args)
        commands.append(entry)
    expected=len(list((ROOT/'Source/platforms/3ds/native').glob('*.cpp')))+len(list((ROOT/'Source/platforms/3ds/native').glob('*.v.pica')))
    assert len(commands)==expected
    stacks=[]
    for path in (build/'CMakeFiles/dusk_pica_engine.dir').rglob('*.su'):
        for row in path.read_text().splitlines():
            function,size,kind=row.rsplit('\t',2);stacks.append({'function':function,'bytes':int(size),'kind':kind})
    assert stacks and all(x['kind']=='static' for x in stacks)
    sdkFiles=[sdk/'libctru/include'/p for p in ['citro3d.h','c3d/base.h','c3d/texture.h','c3d/texenv.h','c3d/renderqueue.h','c3d/fog.h','c3d/effect.h','3ds/gpu/enums.h','3ds/gpu/shbin.h','3ds/gpu/shaderProgram.h']]
    sdkFiles+=[sdk/'libctru/lib/libcitro3d.a',sdk/'libctru/lib/libctru.a',sdk/'tools/bin/picasso']
    receipt={'status':'pass','boundary':'Native module and artifact compilation only. Source game model packets dispatch natively; asset/input migration, UI/effects and graphical acceptance remain incomplete.',
             'source_sha256':{str(p.relative_to(ROOT)):sha(p) for p in files},
             'artifacts_sha256':{str(p.relative_to(ROOT)):sha(p) for p in [archive,*[build/('native_'+p.name.removesuffix('.v.pica')+'.shbin') for p in (ROOT/'Source/platforms/3ds/native').glob('*.v.pica')]]},
             'sdk_sha256':{str(p):sha(p) for p in sdkFiles},'compile_commands':commands,
             'max_individual_engine_stack_bytes':max(x['bytes'] for x in stacks),
             'stack_limits':'Compiler local engine frames only, excluding call-chain totals, SDK, caller and interrupts.',
             'runtime_executed':False,'hardware_validated':False,'performance_validated':False}
    (out/'audit.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(f'PASS native dependency boundary, {expected} optimized ARM translation units and artifact/SDK pins')

if __name__=='__main__':main()
