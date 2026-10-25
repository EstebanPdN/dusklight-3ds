#!/usr/bin/env python3
"""Host ASan/UBSan half-CMPR validation with independent block/pixel/hash oracle."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import test_texture_half as prior
import audit_cmpr_half_candidates as audit

ROOT, NATIVE = prior.ROOT, prior.NATIVE


def fixture(w, h, mips, mode='opaque', offset=39):
    header = bytearray(offset);header[0] = 14
    struct.pack_into('>HH', header, 2, w, h);header[24] = mips
    struct.pack_into('>I', header, 28, offset)
    payload = bytearray()
    for k in range(mips):
        lw,lh = max(1,w >> k),max(1,h >> k)
        for ty in range((lh+7)//8):
            for tx in range((lw+7)//8):
                for sub in range(4):
                    n = ty*((lw+7)//8)*4+tx*4+sub+k*23
                    # Cover both endpoint orderings, equal endpoints, all four
                    # selectors when opaque, and unused transparent palette 3.
                    a,b = ((0xf800 | (n % 256)),(0x001f+(n % 32))) if n % 3 == 0 else ((n*13) % 65536,(n*13) % 65536 if n % 3 == 1 else 65535)
                    selectors = []
                    for y in range(4):
                        row = 0
                        for x in range(4):
                            value = (x+2*y+n) % (4 if a > b and mode != 'padding' else 3)
                            if mode == 'padding' and (tx*8+(sub % 2)*4+x >= lw or ty*8+(sub//2)*4+y >= lh): value=3
                            row |= value << (6-2*x)
                        selectors.append(row)
                    if mode == 'padding': a,b=0,0
                    payload.extend(struct.pack('>HH4B',a,b,*selectors))
    if mode.startswith('alpha'):
        k = int(mode[5:]);at = sum(((max(1,w>>i)+7)//8)*((max(1,h>>i)+7)//8)*32 for i in range(k))
        payload[at:at+4] = bytes(4);payload[at+4] |= 0xc0
    return header+payload


def decode_oracle(image):
    """Independent row-major logical texels, then separate PICA Morton writer."""
    w,h = struct.unpack_from('>HH', image, 2);at = struct.unpack_from('>I', image, 28)[0]
    levels=[]
    # Bit replication is the exact pinned decoder contract, rather than
    # normalized rounded RGB. Explicitly expand endpoints independently.
    def rgb(v):
        r,g,b=v>>11,(v>>5)&63,v&31
        return ((r<<3)|(r>>2),(g<<2)|(g>>4),(b<<3)|(b>>2))
    for k in range(image[24]):
        lw,lh=max(1,w>>k),max(1,h>>k);bw=(lw+7)//8
        pixels=[None]*(lw*lh)
        for y in range(lh):
            for x in range(lw):
                block=at+((y//8)*bw+x//8)*32+((y%8)//4*2+(x%8)//4)*8
                a,b=struct.unpack_from('>HH',image,block);pa,pb=rgb(a),rgb(b)
                index=(image[block+4+y%4]>>(6-2*(x%4)))&3
                if index < 2: color=(pa if index == 0 else pb)+(255,)
                elif a > b:
                    left,right=(pa,pb) if index == 2 else (pb,pa)
                    color=tuple((5*c+3*d)//8 for c,d in zip(left,right))+(255,)
                else: color=tuple((c+d)//2 for c,d in zip(pa,pb))+(0 if index == 3 else 255,)
                pixels[y*lw+x]=color
        levels.append((lw,lh,pixels));at+=bw*((lh+7)//8)*32
    return levels,at


def pica(levels):
    output=bytearray()
    for w,h,pixels in levels:
        pw,ph=max(8,1<<(w-1).bit_length()),max(8,1<<(h-1).bit_length())
        target=bytearray(pw*ph*4)
        for y in range(ph):
            for x in range(pw):
                morton=sum(((x>>k)&1)<<(2*k) | ((y>>k)&1)<<(2*k+1) for k in range(3))
                at=(((y//8)*(pw//8)+x//8)*64+morton)*4
                r,g,b,a=pixels[min(y,h-1)*w+min(x,w-1)]
                target[at:at+4]=bytes((a,b,g,r))
        output.extend(target)
    return output


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);p.add_argument('--private-disc',type=Path);args=p.parse_args()
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    inputs=[Path(__file__).resolve(),ROOT/'Tools/3ds/test_texture_half.py',ROOT/'Tools/3ds/audit_cmpr_half_candidates.py']
    inputs += [NATIVE/n for n in ('runtime/texture_gx.cpp','runtime/texture_gx.hpp','gx_texture_pica.cpp','gx_texture_pica.hpp',
                                  'tests/texture_half_cmpr_test.cpp','tests/texture_half_backend_test.cpp',
                                  'runtime/copy_lifecycle_probe.cpp','runtime/copy_lifecycle_probe.hpp')]
    inputs += [NATIVE/n for n in ('runtime/fused_color.cpp','runtime/fused_color.hpp','runtime/gx_copy_pixels.cpp','runtime/gx_copy_pixels.hpp','runtime/gx_profile.hpp','tests/host_fused_copy_off.hpp')]
    before={str(f):prior.sha(f) for f in inputs}
    receipt={'status':'running','started_at':datetime.datetime.now(datetime.timezone.utc).isoformat(),'source_sha256':before,'commands':[],'oracle_cases':[],
             'limits':['Host only; no PICA execution, runtime/story/visual acceptance or measured performance.',
                       'Synthetic sources only. Startup manifest remains empty.']}
    def run(name,cmd,timeout=90):
        cmd=list(map(str,cmd));receipt['commands'].append({'name':name,'argv':cmd})
        result=subprocess.run(cmd,capture_output=True,text=True,timeout=timeout,env=prior.ENV)
        (out/(name+'.log')).write_text(result.stdout+result.stderr);result.check_returncode();print(name+': PASS',flush=True);return result.stdout
    try:
        common=['clang++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer']
        run('build',[*common,NATIVE/'runtime/texture_gx.cpp',NATIVE/'tests/texture_half_cmpr_test.cpp','-o',out/'cmpr'])
        run('focused',[out/'cmpr'])
        cases=[(16,16,2,'opaque'),(32,16,2,'opaque'),(64,128,4,'opaque'),(512,512,7,'opaque'),(1024,512,7,'opaque')]
        cases += [(32,32,3,'alpha'+str(k)) for k in range(3)]
        cases += [(w,h,1,'padding') for w,h in ((1,1),(3,5),(7,9),(9,17),(15,6))]
        cases += [(16,16,1,'opaque'),(8,8,1,'opaque')]
        for case,(w,h,mips,mode) in enumerate(cases):
            image=fixture(w,h,mips,mode);levels,end=decode_oracle(image)
            opaque=all(pixel[3]==255 for _,_,pixels in levels for pixel in pixels)
            metadata=audit.texture_record(image,0,len(image),{'original_path':'synthetic'})
            assert [l['transparent_logical_texels'] for l in metadata['levels']]==[sum(p[3]==0 for p in pixels) for _,_,pixels in levels]
            for n in (0,31,len(image)-1):
                try: audit.texture_record(image[:n],0,n,{})
                except (ValueError,IndexError,struct.error): pass
                else: raise AssertionError('audit accepted truncated input')
            eligible=opaque and min(w,h)>=16 and mips>=2 and not(w&(w-1) or h&(h-1)) and min(w>>(mips-1),h>>(mips-1))>=8
            digest=hashlib.sha256(bytes([14])+struct.pack('>HHB',w,h,mips)+image[39:end]).hexdigest()
            assert metadata['canonical_sha256']==digest and metadata['mechanically_eligible']==eligible
            if mode=='padding': assert opaque
            src=out/f'case-{case}.bti';src.write_bytes(image)
            for half in (False,True):
                for reference in (False,True):
                    name=f'case-{case}-half{int(half)}-ref{int(reference)}';dest=out/(name+'.pica')
                    plan=json.loads(run(name,[out/'cmpr',src,dest,int(reference),int(half)]))
                    accepted=not half or eligible
                    assert plan['accepted']==accepted and plan['hashed'] and plan['fingerprint']==digest
                    if accepted:
                        expected=pica(levels[int(half):]);assert dest.read_bytes()==expected
                        assert (plan['width'],plan['height'],plan['mips'],plan['source_mip'],plan['bytes'])==(w>>int(half),h>>int(half),mips-int(half),int(half),len(expected))
                    else: assert dest.read_bytes()==bytes([0x69])*17
                    assert src.read_bytes()==image
                    receipt['oracle_cases'].append({'name':name,'input_sha256':prior.sha(src),'output_sha256':prior.sha(dest),'logical_opaque':opaque,'plan':plan})
        # Canonical aliases ignore relocation/header/sampler/trailing bytes, but
        # include discarded base, dimensions, format and all provided mip data.
        image=fixture(32,32,3);canonical=image
        variants=[]
        alias=image[:32]+bytearray(32)+image[39:];alias[31]=64;alias[6]=2;alias[20]=5;variants.append(('relocated',alias,True))
        alias=image+bytearray(13);variants.append(('trailing',alias,True))
        for at in (39,551,679):
            alias=image.copy();alias[at]^=1;variants.append((f'changed-{at}',alias,False))
        alias=image.copy();alias[1]=1;variants.append(('header-alpha',alias,True))
        baseline=hashlib.sha256(bytes([14])+struct.pack('>HHB',32,32,3)+canonical[39:]).hexdigest()
        for name,image,same in variants:
            src=out/(name+'.bti');dest=out/(name+'.pica');src.write_bytes(image)
            plan=json.loads(run(name,[out/'cmpr',src,dest,0,1]));assert (plan['fingerprint']==baseline)==same
            if name=='header-alpha': assert plan['accepted']
            receipt['oracle_cases'].append({'name':name,'input_sha256':prior.sha(src),'output_sha256':prior.sha(dest),'plan':plan})
        if args.private_disc:
            receipt['actual_source_cases']=[]
            disc=args.private_disc.resolve();disc_before=disc.stat()
            with disc.open('rb') as stream:
                files,identity=audit.base.resource.read_fst(stream,disc_before.st_size)
                receipt['private_disc']={**identity,'bytes':disc_before.st_size,'whole_disc_sha256':None}
                # Three real background models, bounded independently of the
                # wider metadata census. Hash every exact stored/model input.
                for stage,room,names in [('F_SP103',1,{'I_Kusa2','I_yaneki'}),
                                         ('F_SP00',0,{'M_kusa','C_IwaKabe_Bokusou'}),
                                         ('F_SP109',0,{'H_wall04'})]:
                    path=f'/res/Stage/{stage}/R{room:02d}_00.arc';offset,size=files[path]
                    assert size <= audit.base.resource.MAX_ARCHIVE
                    stored=audit.base.resource.read_exact(stream,offset,size)
                    archive=audit.base.resource.decompress(stored)
                    model=next(raw for member,_,_,raw in audit.base.archive_members(archive) if member=='/bmdr/model.bmd')
                    at=32;found=set()
                    for _ in range(audit.base.resource.u32(model,12)):
                        size=audit.base.resource.u32(model,at+4)
                        if model[at:at+4]==b'TEX1':
                            count=audit.base.u16(model,at+8);headers=at+audit.base.resource.u32(model,at+12);strings=at+audit.base.resource.u32(model,at+16)
                            for index in range(count):
                                name=audit.base.name_at(model,strings+audit.base.u16(model,strings+6+index*4))
                                if name not in names: continue
                                header=headers+index*32
                                row=audit.texture_record(model,header,at+size,{'original_path':path+'!/bmdr/model.bmd'})
                                assert row['mechanically_eligible']
                                bti=bytearray(model[header:header+32]);struct.pack_into('>I',bti,28,32)
                                bti.extend(model[row['pixel_offset']:row['pixel_offset']+row['source_payload_bytes']])
                                levels,end=decode_oracle(bti);assert end==len(bti)
                                assert all(p[3]==255 for _,_,pixels in levels for p in pixels)
                                entry={'original_path':row['original_path'],'texture_name':name,'texture_index':index,
                                       'canonical_sha256':row['canonical_sha256'],'stored_archive_sha256':hashlib.sha256(stored).hexdigest(),
                                       'model_sha256':hashlib.sha256(model).hexdigest(),'normalized_bti_sha256':hashlib.sha256(bti).hexdigest(),
                                       'original_dimensions':[row['original_width'],row['original_height']],'mips':row['original_mips'],'outputs':[]}
                                for half in (False,True):
                                    command=[str(out/'cmpr'),'--pipe',str(int(half))]
                                    result=subprocess.run(command,input=bti,capture_output=True,timeout=90,env=prior.ENV)
                                    result.check_returncode();expected=pica(levels[int(half):]);assert result.stdout==expected
                                    entry['outputs'].append({'half':half,'bytes':len(expected),'pica_sha256':hashlib.sha256(expected).hexdigest(),'argv':command})
                                receipt['actual_source_cases'].append(entry);found.add(name)
                        at+=size
                    assert found==names
            disc_after=disc.stat();assert (disc_before.st_size,disc_before.st_mtime_ns)==(disc_after.st_size,disc_after.st_mtime_ns)
            receipt['private_disc']['size_mtime_unchanged']=True
            print('actual source in-memory full/half oracles: PASS',flush=True)
        run('existing-half-and-all-format-regressions',['python3',ROOT/'Tools/3ds/test_texture_half.py','--output',out/'regressions'],300)
        assert json.loads((out/'regressions/results.json').read_text())['status']=='pass'
        assert before=={str(f):prior.sha(f) for f in inputs}
        receipt.update(status='pass',inputs_unchanged=True,binary_sha256=prior.sha(out/'cmpr'))
    except (OSError,subprocess.SubprocessError,AssertionError,ValueError,KeyError) as e:
        receipt.update(status='fail',error=repr(e));print(repr(e),flush=True)
    finally:
        receipt['finished_at']=datetime.datetime.now(datetime.timezone.utc).isoformat()
        (out/'results.json').write_text(json.dumps(receipt,indent=2)+'\n')
    return receipt['status']!='pass'

if __name__=='__main__': raise SystemExit(main())
