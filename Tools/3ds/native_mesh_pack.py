#!/usr/bin/env python3
"""Prepare version1 native mesh packs on the Mac; no source-command payloads."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import zlib

MAGIC=b'DUSKPN01'
VERTEX=struct.Struct('<12f12B')
MAX_FILE=64<<20

def align16(n):return (n+15)&~15

def integer(value,low,high):
    if type(value) is not int or not low<=value<=high:raise ValueError('Integer outside native bounds')
    return value

def vector(value,count,bound,label='attribute'):
    if not isinstance(value,list) or len(value)!=count:raise ValueError('Native vector length')
    result=[]
    for x in value:
        if type(x) not in (int,float) or not math.isfinite(x) or abs(x)>bound or (x and abs(x)<2**-62):raise ValueError(f'Native {label} outside float domain: {x!r}')
        result.append(struct.unpack('<f',struct.pack('<f',x))[0])
    return result

def mesh_bytes(mesh):
    if not isinstance(mesh,dict) or set(mesh)-{'id','vertices','indices'}:raise ValueError('Unknown mesh field')
    identifier=integer(mesh['id'],1,0xffffffff);vertices=mesh['vertices'];indices=mesh['indices']
    if not isinstance(vertices,list) or not 1<=len(vertices)<=65536:raise ValueError('Native vertex count')
    if not isinstance(indices,list) or not 1<=len(indices)<=65535 or len(indices)%3:raise ValueError('Native triangle index count')
    vertex_data=bytearray()
    for v in vertices:
        if not isinstance(v,dict) or set(v)-{'position','normal','uv','color','bones','weights'}:raise ValueError('Unknown native vertex field')
        position=vector(v['position'],3,1<<20,'position');normal=vector(v.get('normal',[0,0,1]),3,2,'normal')
        uv=v.get('uv',[[0,0],[0,0],[0,0]])
        if not isinstance(uv,list) or len(uv)!=3:raise ValueError('Three native UV pairs required')
        coordinates=[x for pair in uv for x in vector(pair,2,32768,'UV')]
        color=v.get('color',[255]*4);bones=v.get('bones',[0]*4);weights=v.get('weights',[255,0,0,0])
        if any(not isinstance(a,list) or len(a)!=4 for a in [color,bones,weights]):raise ValueError('Native color/influence length')
        color=[integer(x,0,255) for x in color];rows=[3*integer(x,0,7) for x in bones];weights=[integer(x,0,255) for x in weights]
        if sum(weights)!=255:raise ValueError('Native weights must sum to255')
        vertex_data.extend(VERTEX.pack(*position,*normal,*coordinates,*color,*rows,*weights))
    index_data=struct.pack('<'+'H'*len(indices),*[integer(i,0,len(vertices)-1) for i in indices])
    return identifier,len(vertices),len(indices),bytes(vertex_data),index_data

def compile_pack(document):
    if not isinstance(document,dict) or set(document)!={'schema','meshes'} or document['schema']!='dusklight-native-mesh/1':raise ValueError('Native input schema')
    meshes=document['meshes']
    if not isinstance(meshes,list) or not 1<=len(meshes)<=64:raise ValueError('Native pack mesh count')
    compiled=sorted(mesh_bytes(m) for m in meshes);ids=[m[0] for m in compiled]
    if len(set(ids))!=len(ids):raise ValueError('Duplicate native mesh ID')
    cursor=32+len(meshes)*32;table=bytearray();payload=bytearray()
    for identifier,nv,ni,vertices,indices in compiled:
        vo=cursor;io=align16(vo+len(vertices));end=align16(io+len(indices))
        if end>MAX_FILE:raise ValueError('Native pack exceeds64MiB')
        table.extend(struct.pack('<8I',identifier,nv,ni,vo,io,len(vertices),len(indices),zlib.crc32(vertices+indices)))
        payload.extend(vertices);payload.extend(bytes(io-vo-len(vertices)));payload.extend(indices);payload.extend(bytes(end-io-len(indices)));cursor=end
    prefix=struct.pack('<8s5I',MAGIC,1,cursor,len(meshes),32,len(table))
    result=prefix+struct.pack('<I',zlib.crc32(prefix+table))+table+payload
    assert len(result)==cursor
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('input',type=Path);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    # Private output uses the same pinned-ancestor/no-follow/untracked guard as
    # owner bundles and saves. No overwrite of an existing file is permitted.
    from run_azahar_smoke import card_private_parent
    with args.input.open('rb') as stream:
        source=stream.read(32*1024*1024+1)
    if len(source)>32*1024*1024:parser.error('Input JSON exceeds32MiB')
    packed=compile_pack(json.loads(source))
    with card_private_parent(args.output,create=True) as (parent,name,path):
        fd=os.open(name,os.O_CREAT|os.O_EXCL|os.O_WRONLY|os.O_NOFOLLOW,0o600,dir_fd=parent)
        try:
            with os.fdopen(fd,'wb') as stream:
                if stream.write(packed)!=len(packed):raise OSError('Short native pack write')
                stream.flush();os.fsync(stream.fileno())
        except BaseException:
            os.unlink(name,dir_fd=parent);raise
    print(json.dumps({'output':str(path),'bytes':len(packed),'sha256':hashlib.sha256(packed).hexdigest(),'source_sha256':hashlib.sha256(source).hexdigest(),
                      'scope':'Native mesh ABI only. This is not an ISO/BMD/J3D importer or complete native game pack.'},indent=2))

if __name__=='__main__':main()
