#!/usr/bin/env python3
"""Reopen a lab run, validate every actual PICA payload, and retain receipts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

from texture_compression_lab import ROOT, digest, save_json, decode_mips
import analyze_texture_dump as analyzer
import texture_lab_formats as f


def validate(output):
    output=output.resolve()
    receipt=json.loads((output/'receipts.json').read_text())
    census=json.loads((output/'census.json').read_text())
    actual=json.loads((output/'actual-results.json').read_text())
    for p,h in receipt['protected_after'].items():
        if digest(Path(p))!=h: raise ValueError('Protected input hash changed: '+p)
    for p,h in receipt['lab_sha256'].items():
        if digest(Path(p))!=h: raise ValueError('Lab code changed since run: '+p)
    command=['python3','-m','unittest','discover','-s','Tools/3ds','-p','test_texture_compression_lab.py','-v']
    tests=subprocess.run(command,cwd=ROOT,capture_output=True,text=True,timeout=90)
    (output/'tests.log').write_text(tests.stdout+tests.stderr)
    tests.check_returncode()
    decoder,encoder=Path(receipt['decoder']),Path(receipt['encoder'])
    sources={}
    for inp in census['inputs']:
        _,_,records=analyzer.read_snapshot(Path(inp['path']))
        for r,bti in records: sources[(inp['scene'],r['index'])]=(r,bti)
    checked=levels_count=etc_blocks=0; exact=[]
    with tempfile.TemporaryDirectory(prefix='texture-lab-verify-') as temp:
        scratch=Path(temp)
        for sample in actual:
            r=sample['record']; directory=output/sample['directory']
            source_record,bti=sources[(r['scene'],r['index'])]
            source_levels=decode_mips(source_record,bti,decoder,scratch)
            original,w,h=source_levels[0]
            raw=(directory/'original.rgba').read_bytes()
            if raw!=struct.pack('<II',w,h)+original: raise ValueError('Original RGBA mismatch')
            for v in sample['variants']:
                path=directory/v['pica_data']; data=path.read_bytes()
                if len(data)!=v['encoded_bytes'] or digest(path)!=v['sha256']: raise ValueError('PICA receipt mismatch')
                levels=source_levels if v['scale']==1 else [f.half_box(p,lw,lh) for p,lw,lh in source_levels[:v['mips']]]
                offset=0; base=None
                for n,(reference,lw,lh) in enumerate(levels):
                    size=f.allocation(lw,lh,1,f.FORMATS[v['format']]); chunk=data[offset:offset+size]; offset+=size
                    if v['format'].startswith('ETC'):
                        decoded=f.decode_etc(chunk,lw,lh,v['format'])
                        # Reopen the real emitted PICA words, independently
                        # unpack their RGB with upstream's C++ decoder, then
                        # compare each in-bounds pixel's RGB against Python.
                        step=16 if v['format']=='ETC1A4' else 8
                        blocks=b''.join(chunk[i+step-8:i+step][::-1] for i in range(0,len(chunk),step))
                        inp,out=scratch/'verify.etc',scratch/'verify.rgba'; inp.write_bytes(blocks)
                        subprocess.run([str(encoder),'decode',str(inp),str(out)],check=True,capture_output=True,timeout=15)
                        oracle=out.read_bytes()
                        for block,(bx,by) in enumerate(f.block_positions(lw,lh)):
                            for y in range(min(4,lh-by)):
                                for x in range(min(4,lw-bx)):
                                    a=((by+y)*lw+bx+x)*4; b=block*64+(y*4+x)*4
                                    if decoded[a:a+3]!=oracle[b:b+3]: raise ValueError('Independent ETC1 decoder disagreement')
                        etc_blocks+=len(blocks)//8
                    else: decoded=f.decode16(chunk,lw,lh,v['format'])
                    if f.metrics(reference,decoded)!=v['per_mip_codec_only'][n]: raise ValueError('Mip metrics mismatch')
                    if n==0: base=decoded
                    levels_count+=1
                if offset!=len(data): raise ValueError('Mip payload trailing bytes')
                compared=f.expand_nearest(base,v['width'],v['height'],w,h) if v['scale']==2 else base
                if f.metrics(original,compared)!=v['base_vs_original']: raise ValueError('Original-grid metrics mismatch')
                if v.get('exact_gx565_all_mips'): exact.append(sample['directory'])
                checked+=1
    # Concrete environment dependency/exec receipts, not a guessed build blocker.
    dependency=subprocess.run(['pkg-config','--modversion','Magick++'],capture_output=True,text=True,timeout=15)
    try:
        installed=subprocess.run([str(Path(os.environ.get('DEVKITPRO', '/opt/devkitpro'))/'tools/bin/tex3ds'),'--version'],capture_output=True,text=True,timeout=5)
        installed_error={'returncode':installed.returncode,'stderr':installed.stderr}
    except OSError as error:
        installed_error={'errno':error.errno,'message':str(error)}
    validation={'test_command':command,'unit_tests_returncode':tests.returncode,
                'actual_payloads_verified':checked,'actual_mip_payloads_verified':levels_count,
                'actual_etc_blocks_compared_with_independent_upstream_decoder':etc_blocks,
                'exact_gx565_all_mips_samples':exact,'protected_hashes_unchanged':True,
                'magick_dependency_probe':{'command':['pkg-config','--modversion','Magick++'],
                                           'returncode':dependency.returncode,'stdout':dependency.stdout,'stderr':dependency.stderr},
                'installed_tex3ds_exec_probe':installed_error,
                'output_sha256':{str(p.relative_to(output)):digest(p) for p in output.rglob('*') if p.is_file() and p.name!='validation.json'}}
    save_json(output/'validation.json',validation)
    print(json.dumps({k:v for k,v in validation.items() if k!='output_sha256'},indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__); parser.add_argument('output',type=Path)
    validate(parser.parse_args().output)
