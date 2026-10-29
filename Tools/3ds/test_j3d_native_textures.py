#!/usr/bin/env python3
"""Mac compiler checks against independent constant/coordinate image fixtures."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import j3d_native_textures as compiler
from native_texture_pack import tile_rgba8

ROOT=Path(__file__).resolve().parents[2]

def model(textures):
    block=bytearray(32+len(textures)*32);block[:4]=b'TEX1';struct.pack_into('>H',block,8,len(textures));struct.pack_into('>I',block,12,32)
    for n,(kind,levels,palette) in enumerate(textures):
        at=32+n*32;header=bytearray(32);header[0]=kind;struct.pack_into('>HH',header,2,16,16);header[24]=len(levels)
        if palette:
            header[8]=1;header[9]=1;struct.pack_into('>H',header,10,len(palette)//2)
            struct.pack_into('>i',header,12,len(block)-at);block.extend(palette)
        struct.pack_into('>i',header,28,len(block)-at);block.extend(b''.join(levels));block[at:at+32]=header
    block.extend(bytes((-len(block))%32));struct.pack_into('>I',block,4,len(block))
    blocks=[b'VTX1'+struct.pack('>I',8),b'SHP1'+struct.pack('>I',8),b'DRW1'+struct.pack('>I',8),b'JNT1'+struct.pack('>I',8),bytes(block)]
    return struct.pack('>8sII16x',b'J3D2bmd3',32+sum(map(len,blocks)),len(blocks))+b''.join(blocks)

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True);decoder=out/'image-compiler';command=compiler.build_decoder(ROOT,decoder)
    # Known constant source cells, not produced by the native converter.
    cases=[(0,b'\xaa'*32,(170,170,170,170),b''),(1,b'\x7b'*32,(123,123,123,123),b''),
        (2,b'\xa3'*32,(51,51,51,170),b''),(3,b'\xa1\x38'*16,(56,56,56,161),b''),
        (4,b'\xf8\x00'*16,(255,0,0,255),b''),(5,b'\x7a\xbc'*16,(170,187,204,255),b''),
        (6,b'\xff\x11'*16+b'\x22\x33'*16,(17,34,51,255),b''),
        (8,b'\x11'*32,(255,0,0,255),b'\0\0\xf8\0'),(9,b'\x01'*32,(255,0,0,255),b'\0\0\xf8\0'),
        (10,b'\x00\x01'*16,(255,0,0,255),b'\0\0\xf8\0'),(14,(b'\xf8\0\x07\xe0'+bytes(4))*4,(255,0,0,255),b'')]
    count=0
    for kind,cell,expected,palette in cases:
        bw,bh,_=compiler.BLOCKS[kind];levels=[cell*((w//bw)*(w//bh)) for w in (16,8)]
        source=model([(kind,levels,palette)]);scratch=out/f'format-{kind}';scratch.mkdir(exist_ok=False)
        packed,receipt=compiler.convert(source,decoder,scratch);assert receipt['image_storage_converted'] and not receipt['rendering_ready']
        at=struct.unpack_from('<I',packed,52)[0];payload=packed[at:]
        expectedBytes=(b'\0\xf8'*(256+64) if kind==4 else bytes(expected[::-1])*(256+64))
        assert payload==expectedBytes,(kind,hashlib.sha256(payload).hexdigest());count+=1
    # Independent nonconstant source planar cells exercise address/orientation.
    rgba=bytes(c for y in range(16) for x in range(16) for c in (x*7,y*11,(x^y)*13,255-x-y));planar=bytearray()
    for by in range(0,16,4):
        for bx in range(0,16,4):
            pixels=[rgba[((by+y)*16+bx+x)*4:((by+y)*16+bx+x+1)*4] for y in range(4) for x in range(4)]
            planar.extend(b''.join(bytes((p[3],p[0])) for p in pixels)+b''.join(bytes((p[1],p[2])) for p in pixels))
    scratch=out/'coordinates';scratch.mkdir(exist_ok=False)
    packed,_=compiler.convert(model([(6,[bytes(planar)],b'')]),decoder,scratch);assert packed[64:]==tile_rgba8(rgba,16,16)
    empty,receipt=compiler.convert(model([]),decoder,scratch)
    assert empty is None and not receipt['images'] and not receipt['native_image_pack_needed'] and receipt['image_storage_converted']
    external=bytearray(model([(6,[bytes(planar)],b'')]))
    # Add a valid original JUTNameTab pointing at a stage resource; the source
    # placeholder's complete descriptor is replaced, as in setupStageTexture.
    texAt=64;names=len(external)-texAt
    external.extend(struct.pack('>HHHH',1,0xffff,0,8)+b'stageRock\0');external.extend(bytes((-len(external))%32))
    struct.pack_into('>I',external,8,len(external));struct.pack_into('>I',external,texAt+4,len(external)-texAt)
    struct.pack_into('>I',external,texAt+16,names);external[texAt+32:texAt+64]=bytes(32)
    _,resource,_=next(compiler.source_images(model([(6,[bytes(planar)],b'')])))
    try:list(compiler.source_images(bytes(external)))
    except compiler.Unsupported:pass
    else:raise AssertionError('Missing stage resource accepted')
    scratch=out/'external';scratch.mkdir(exist_ok=False)
    resolved,receipt=compiler.convert(bytes(external),decoder,scratch,{'STAGEROCK':resource})
    assert resolved==packed and receipt['images'][0]['external_name']=='stageRock'
    try:list(compiler.source_images(bytes(external),{'stageRock':resource,'STAGEROCK':resource}))
    except ValueError:pass
    else:raise AssertionError('Ambiguous archive names accepted')
    rejected=0;valid=model([(8,[b'\x11'*128],b'\0\0\xf8\0')]);tex=64 # Four empty 8-byte blocks follow the file header.
    for field,value,size in [(tex+12,0xffffffff,4),(tex+8,4097,2),(tex+32,7,1),(tex+32+28,0,4),(tex+32+2,1025,2),(tex+32+10,1,2)]:
        bad=bytearray(valid);bad[field:field+size]=value.to_bytes(size,'big')
        try:list(compiler.source_images(bytes(bad)))
        except ValueError:rejected+=1
        else:raise AssertionError('Invalid source image accepted')
    for n in range(len(valid)):
        try:list(compiler.source_images(valid[:n]))
        except ValueError:rejected+=1
        else:raise AssertionError('Truncated source accepted')
    files=[ROOT/'Tools/3ds'/name for name in ['compile_native_texture.cpp','j3d_native_textures.py','native_texture_pack.py','j3d_native_geometry.py','test_j3d_native_textures.py']]
    files+=[ROOT/'Source/platforms/3ds/runtime'/name for name in ['texture_gx.cpp','texture_gx.hpp']]
    (out/'receipt.json').write_text(json.dumps({'status':'pass','constant_format_cases':count,'coordinate_texels':256,'source_rejections':rejected,
        'source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},'build_command':command,
        'scope':'Offline image storage and existing decoder contract. No original sampler, filtering, material, pixels or hardware acceptance.'},indent=2)+'\n')
    print(f'PASS {count} source formats, independent coordinate image and {rejected} source refusals')

if __name__=='__main__':main()
