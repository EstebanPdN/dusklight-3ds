#!/usr/bin/env python3
"""Independent binary fixtures for the Mac-only J3D native geometry compiler."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct
import j3d_native_geometry as convert
import native_mesh_pack as pack

ROOT=Path(__file__).resolve().parents[2]

def word(data,at,value):struct.pack_into('>I',data,at,value)
def half(data,at,value):struct.pack_into('>H',data,at,value)
def append(block,alignment,payload):
    block.extend(bytes((-len(block))%alignment));offset=len(block);block.extend(payload);return offset
def finish(block,kind):
    block.extend(bytes((-len(block))%32));block[:4]=kind;word(block,4,len(block));return bytes(block)

def model(primitive=0x98,groups=2,color_type=0,normal_count=0,shape_mode=3):
    # Source fields are serialized independently from the converter, following
    # J3DVertexBlock/J3DShapeInitData/J3DShapeMtxInitData in the original headers.
    v=bytearray(64)
    formats=[(9,1,3,2),(10,normal_count,1,0),(11,int(color_type>=3),color_type,0),(13,1,3,8),(14,1,4,0)]
    raw=b''.join(struct.pack('>IIIB3x',*f) for f in formats)+struct.pack('>I12x',255)
    word(v,8,append(v,16,raw))
    positions=[(-8,0,0),(8,0,0),(8,8,0),(-8,8,0)]
    word(v,12,append(v,16,b''.join(struct.pack('>3h',*p) for p in positions)))
    word(v,16,append(v,16,bytes([0,0,64])))
    colors=[0xf800,0x07e0,0x001f,0xffff]
    color_payload=b''.join(struct.pack('>H',c) for c in colors) if color_type in (0,3) else bytes([255,0,0,255]*4)
    word(v,24,append(v,16,color_payload))
    uv=[(-128,0),(256,0),(256,320),(-128,320)]
    word(v,32,append(v,16,b''.join(struct.pack('>2h',*p) for p in uv)))
    v=finish(v,b'VTX1')
    d=bytearray(20);half(d,8,3);word(d,12,append(d,4,bytes(3)));word(d,16,append(d,4,struct.pack('>3H',0,1,2)));d=finish(d,b'DRW1')
    j=bytearray(24);half(j,8,3);j=finish(j,b'JNT1')
    s=bytearray(44);half(s,8,1)
    init=bytearray(40);init[0]=shape_mode;half(init,2,groups)
    word(s,12,append(s,4,init));word(s,16,append(s,2,struct.pack('>H',0)))
    desc=[(0,1),(9,3),(10,2),(11,2),(13,3),(14,1)]
    word(s,24,append(s,4,b''.join(struct.pack('>II',*item) for item in reversed(desc))+struct.pack('>II',255,0)))
    word(s,28,append(s,2,struct.pack('>4H',0,1,65535,2)))
    display=bytearray();draws=[]
    for group in range(groups):
        start=len(display);display.extend(struct.pack('>BH',primitive,4 if primitive!=0x90 else 3))
        for i in range(4 if primitive!=0x90 else 3):
            selector=3 if i in (1,2) else 0
            display.extend(struct.pack('>BHBBH2f',selector,i,0,i,i,float(i)+.25,float(group)+.5))
        display.extend(bytes((-len(display))%32));draws.append((len(display)-start,start))
    word(s,32,append(s,32,display))
    word(s,36,append(s,4,b''.join(struct.pack('>HHI',0,2,2*g) for g in range(groups))))
    word(s,40,append(s,4,b''.join(struct.pack('>II',*draw) for draw in draws)));s=finish(s,b'SHP1')
    blocks=[v,d,j,s];header=bytearray(32);header[:8]=b'J3D2bmd3';word(header,8,32+sum(map(len,blocks)));word(header,12,len(blocks))
    return bytes(header)+b''.join(blocks)

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    source=model();document,native,metadata=convert.convert(source)
    assert metadata['geometry_attribute_streams_complete'] and not metadata['rendering_ready'] and not metadata['materials_converted'] and not metadata['animations_converted']
    assert len(document['meshes'])==2 and [m['draw_matrix_palette'] for m in metadata['meshes']]==[[0,1],[0,2]]
    for group,mesh in enumerate(document['meshes']):
        assert mesh['indices']==[0,1,2,2,1,3]
        assert mesh['vertices'][0]['position']==[-2,0,0]
        assert mesh['vertices'][0]['normal']==[0,0,1]
        assert mesh['vertices'][0]['uv'][0]==[-.5,0]
        assert mesh['vertices'][0]['uv'][1]==[.25,group+.5]
        assert mesh['vertices'][0]['color']==[255,0,0,255]
        assert mesh['vertices'][1]['bones']==[1,0,0,0]
    assert native==pack.compile_pack(document) and native==convert.convert(source)[1]
    assert convert.topology(0x80,4)==[(0,1,2),(2,3,0)]
    assert convert.topology(0xa0,4)==[(0,1,2),(0,2,3)]
    # All supported numeric/color paths have independently expected bytes.
    arrays=convert.Arrays(convert.sections(source)['VTX1'])
    for kind,raw,want in [(0,b'\xf8\x00',[255,0,0,255]),(0,b'\xa5\x14',[165,162,165,255]),(1,b'\x11\x22\x33',[17,34,51,255]),(2,b'\x11\x22\x33\x00',[17,34,51,255]),
                          (3,b'\xf1\x08',[255,17,0,136]),(4,b'\xff\x00\x00',[255,195,0,0]),(5,b'\x11\x22\x33\x44',[17,34,51,68])]:
        arrays.formats[11]=(int(kind>=3),kind,0);assert arrays.decode(11,raw)==want,(kind,arrays.decode(11,raw),want)
    refused=0
    for i in range(len(source)):
        try:convert.convert(source[:i])
        except (ValueError,struct.error,UnicodeDecodeError):refused+=1
        else:raise AssertionError('Truncated model accepted')
    for raw in [model(normal_count=1),model(shape_mode=1),model(primitive=0xb8)]:
        try:convert.convert(raw)
        except convert.Unsupported:refused+=1
        else:raise AssertionError('Unsupported source rendered as success')
    bad=bytearray(source);word(bad,8,len(bad)+1)
    try:convert.convert(bad)
    except ValueError:refused+=1
    else:raise AssertionError('Contradictory file size')
    (out/'fixture.bmd').write_bytes(source);(out/'fixture.dnp').write_bytes(native)
    (out/'fixture.native.json').write_text(json.dumps(document,indent=2)+'\n')
    files=[ROOT/'Tools/3ds'/name for name in ['j3d_native_geometry.py','native_mesh_pack.py','test_j3d_native_geometry.py']]
    receipt={'status':'pass','refused':refused,'native_sha256':hashlib.sha256(native).hexdigest(),'source_sha256':hashlib.sha256(source).hexdigest(),
             'tool_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},'metadata':metadata,
             'scope':'Synthetic binary fixtures and offline source conversion only; no game, GPU, materials or animation acceptance.'}
    (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print('PASS J3D native geometry fixtures, format oracles, matrix inheritance and',refused,'refusals')

if __name__=='__main__':main()
