#!/usr/bin/env python3
"""Offline native ETC admission and exact-alpha regression checks."""
import argparse
import hashlib
import json
from pathlib import Path
import native_texture_pack as pack
import native_texture_compression as compression

ROOT=Path(__file__).resolve().parents[2]

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--codec-source',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True);encoder=out/'encoder';compression.build_encoder(ROOT,args.codec_source.resolve(),encoder)
    images=[]
    for identifier,alpha in [(1,255),(2,0),(3,128)]:
        levels=[]
        for width in (16,8):
            rgba=bytes(c for y in range(width) for x in range(width) for c in (100,120,140,255 if identifier==2 and (3*x+y)%5 else alpha))
            levels.append(pack.tile_rgba8(rgba,width,width))
        images.append({'id':identifier,'width':16,'height':16,'format':'rgba8','levels':levels})
    original=pack.compile_images(images);digest=hashlib.sha256(original).hexdigest()
    assert pack.compile_images(compression.images_from_pack(original))==original
    same,receipt=compression.compile_profile(original,out/'missing-codec',out,set(),digest)
    assert same==original and not receipt['default_enabled'] and receipt['encoder_sha256'] is None
    scratch=out/'admitted';scratch.mkdir(exist_ok=False)
    converted,receipt=compression.compile_profile(original,encoder,scratch,{1,2,3},digest)
    result=compression.images_from_pack(converted)
    assert [r['format'] for r in result]==['etc1','etc1a4','rgba8']
    assert receipt['images'][2]['status']=='retained_alpha_precision' and result[2]==images[2]
    assert sum(map(len,result[0]['levels']))*8==sum(map(len,images[0]['levels']))
    assert sum(map(len,result[1]['levels']))*4==sum(map(len,images[1]['levels']))
    # Direct PICA block and alpha-nibble addresses, independently enumerated
    # rather than only trusting encoder/decoder mutual agreement.
    for bx,by in [(0,0),(4,0),(0,4),(4,4),(8,0),(0,8),(12,12)]:
        block=((by//8)*2+bx//8)*4+((by%8)//4)*2+(bx%8)//4
        word=int.from_bytes(result[1]['levels'][0][block*16:block*16+8],'little')
        expected=sum((15 if (3*(bx+x)+by+y)%5 else 0)<<(4*(x*4+y)) for y in range(4) for x in range(4))
        assert word==expected
    assert not receipt['rendering_ready'] and not receipt['material_roles_validated']
    # A color tuple with channel differences1 cannot be exactly represented by
    # ETC1's quantized base channels plus one shared modifier.
    poor=pack.compile_images([{'id':1,'width':8,'height':8,'format':'rgba8','levels':[pack.tile_rgba8(bytes((1,2,3,255))*64,8,8)]}])
    scratch=out/'strict';scratch.mkdir(exist_ok=False)
    retained,strict=compression.compile_profile(poor,encoder,scratch,{1},hashlib.sha256(poor).hexdigest(),max_rgb_error=0,max_rgb_mse=0)
    assert retained==poor and strict['images'][0]['status']=='retained_rgb_quality'
    for name,limit,mse in [('maximum',0,65535),('mean-square',255,0)]:
        scratch=out/name;scratch.mkdir(exist_ok=False)
        retained,_=compression.compile_profile(poor,encoder,scratch,{1},hashlib.sha256(poor).hexdigest(),max_rgb_error=limit,max_rgb_mse=mse)
        assert retained==poor
    refused=0
    for ids,expected,limit in [({1},'bad',24),({4},digest,24),({True},digest,24),({1},digest,-1)]:
        try:compression.compile_profile(original,encoder,out,ids,expected,max_rgb_error=limit)
        except ValueError:refused+=1
        else:raise AssertionError('Invalid image policy accepted')
    for n in range(len(original)):
        try:compression.images_from_pack(original[:n])
        except ValueError:refused+=1
        else:raise AssertionError('Truncated native bank accepted')
    files=[ROOT/'Tools/3ds'/name for name in ['native_texture_compression.py','native_texture_pack.py','texture_lab_formats.py','texture_lab_etc_host.cpp','test_native_texture_compression.py']]
    (out/'receipt.json').write_text(json.dumps({'status':'pass','refusals':refused,'compression_receipt':receipt,'strict_quality_receipt':strict,
        'source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},'scope':'Stored bytes and admission only; not GPU, sampler or material acceptance.'},indent=2)+'\n')
    print(f'PASS default OFF, bank-bound admission, ETC1/ETC1A4 exact alpha, quality rejection and {refused} invalid inputs')

if __name__=='__main__':main()
