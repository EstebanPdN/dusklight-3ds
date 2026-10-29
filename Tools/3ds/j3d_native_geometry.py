#!/usr/bin/env python3
"""Offline J3D geometry -> native meshes. Never used by the 3DS renderer."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
from native_mesh_pack import compile_pack,VERTEX

class Unsupported(ValueError):pass

def need(value,message):
    if not value:raise ValueError(message)

def span(data,offset,size):
    need(0<=offset<=len(data) and 0<=size<=len(data)-offset,'J3D span outside block')
    return data[offset:offset+size]

def u16(data,offset):return struct.unpack('>H',span(data,offset,2))[0]
def u32(data,offset):return struct.unpack('>I',span(data,offset,4))[0]

def sections(data):
    need(32<=len(data)<=64<<20 and data[:8] in (b'J3D2bmd3',b'J3D2bdl4'),'Unsupported J3D model header')
    need(u32(data,8)==len(data) and 1<=u32(data,12)<=16,'J3D file size/block count')
    result={};at=32
    for _ in range(u32(data,12)):
        kind=span(data,at,4).decode('ascii');size=u32(data,at+4)
        need(size>=8 and kind not in result,'J3D block size/duplicate')
        result[kind]=span(data,at,size);at+=size
    need(at==len(data),'Unexpected J3D trailing bytes')
    need(all(k in result for k in ['VTX1','SHP1','DRW1','JNT1']),'Missing J3D geometry/pose block')
    return result

class Arrays:
    def __init__(self,data):
        self.data=data;span(data,0,64);self.formats={}
        at=u32(data,8)
        need(64<=at<=len(data)-16,'Vertex format offset outside data region')
        for _ in range(32):
            attr=u32(data,at)
            if attr==255:break
            need(attr not in self.formats,'Duplicate vertex format')
            self.formats[attr]=(u32(data,at+4),u32(data,at+8),span(data,at+12,1)[0]);at+=16
        else:raise ValueError('Unterminated vertex formats')
        pointers=[u32(data,x) for x in range(12,64,4)]
        need(all(not p or 64<=p<=len(data) for p in pointers),'Vertex array pointer outside data region')
        self.offsets={9:pointers[0],10:pointers[1],11:pointers[3],12:pointers[4],**{13+i:pointers[5+i] for i in range(8)}}
        self.ends={p:min([x for x in pointers if x>p]+[len(data)]) for p in pointers if p}

    def layout(self,attr):
        if attr not in self.formats:raise ValueError('Missing used vertex format')
        count,kind,fraction=self.formats[attr]
        if attr==9:need(count in (0,1),'Position component count');components=2+count
        elif attr==10:
            if count!=0:raise Unsupported('NBT/three-index normal encoding needs native tangent support')
            if kind not in (1,3,4):raise Unsupported('Unsigned source normal encoding')
            components=3
        elif attr==11:
            need(count in (0,1) and kind<=5,'Color format/count')
            if count!=int(kind>=3):raise Unsupported('Source color component/encoding mismatch')
            return (2,3,4,2,3,4)[kind]
        elif 13<=attr<=15:need(count in (0,1),'UV component count');components=1+count
        else:raise Unsupported('Native vertex ABI does not preserve attribute '+str(attr))
        need(kind<=4 and fraction<=31,'Numeric vertex format')
        return components*(1,1,2,2,4)[kind]

    def decode(self,attr,raw):
        count,kind,fraction=self.formats[attr]
        if attr==11:
            if kind==0:
                word=int.from_bytes(raw,'big');r=(word>>11)&31;g=(word>>5)&63;b=word&31;return [(r<<3)|(r>>2),(g<<2)|(g>>4),(b<<3)|(b>>2),255]
            if kind in (1,2):return list(raw[:3])+[255]
            if kind==3:return [((int.from_bytes(raw,'big')>>shift)&15)*17 for shift in [12,8,4,0]]
            if kind==4:return [(((int.from_bytes(raw,'big')>>shift)&63)<<2)|((int.from_bytes(raw,'big')>>shift)&63)>>4 for shift in [18,12,6,0]]
            return list(raw)
        components=(3 if attr==10 else 2+count if attr==9 else 1+count)
        result=list(struct.unpack('>'+('BbHhf'[kind])*components,raw))
        scale=1 if kind==4 else 2**-(6 if attr==10 and kind==1 else 14 if attr==10 and kind==3 else fraction)
        result=[x*scale for x in result]
        need(all(math.isfinite(x) for x in result),'Nonfinite source attribute')
        return result+[0.]*(3-len(result) if attr==9 else 2-len(result) if attr>=13 else 0)

    def fetch(self,attr,index):
        size=self.layout(attr);offset=self.offsets.get(attr,0)
        need(offset>0 and offset in self.ends,'Missing used vertex array')
        start=offset+index*size;need(start+size<=self.ends[offset],'Vertex array index outside bounded array')
        return self.decode(attr,span(self.data,start,size))

def descriptor(block,at,arrays):
    result={};seen=set()
    for _ in range(32):
        attr,kind=u32(block,at),u32(block,at+4);at+=8
        if attr==255:break
        need(attr not in seen and 0<=attr<=20 and kind in (0,1,2,3),'Vertex descriptor duplicate/type')
        seen.add(attr)
        if kind==0:continue
        if attr==0:need(kind==1,'Indexed matrix selector unsupported')
        else:arrays.layout(attr)
        result[attr]=kind
    else:raise ValueError('Unterminated vertex descriptor')
    need(9 in result,'Shape lacks position')
    return sorted(result.items())

def topology(primitive,count):
    if primitive==0x90:
        need(count>=3 and count%3==0,'Triangle vertex count');return [(i,i+1,i+2) for i in range(0,count,3)]
    if primitive==0x80:
        need(count>=4 and count%4==0,'Quad vertex count');return [t for i in range(0,count,4) for t in [(i,i+1,i+2),(i+2,i+3,i)]]
    if primitive in (0x98,0xa0):
        need(count>=3,'Strip/fan vertex count')
        return [(i-2,i-1,i) if i%2==0 else (i-1,i-2,i) for i in range(2,count)] if primitive==0x98 else [(0,i-1,i) for i in range(2,count)]
    raise Unsupported('Non-triangle primitive/state command '+hex(primitive))

def packets(data,desc,arrays,matrixSlots):
    vertices=[];indices=[];palette=[];lookup={};at=0;packetCount=0
    while at<len(data):
        op=data[at];at+=1
        if op==0:continue
        if op&7:raise Unsupported('Nonzero source vertex format slot')
        count=u16(data,at);at+=2;triangles=topology(op,count);remap=[]
        for _ in range(count):
            value={'position':[0,0,0],'normal':[0,0,1],'uv':[[0,0] for _ in range(3)],'color':[255]*4};selector=0
            for attr,kind in desc:
                if attr==0:selector=span(data,at,1)[0];at+=1;continue
                if kind==1:
                    n=arrays.layout(attr);decoded=arrays.decode(attr,span(data,at,n));at+=n
                else:
                    n=kind-1;index=int.from_bytes(span(data,at,n),'big');at+=n;decoded=arrays.fetch(attr,index)
                if attr==9:value['position']=decoded
                elif attr==10:value['normal']=decoded
                elif attr==11:value['color']=decoded
                else:value['uv'][attr-13]=decoded
            need(selector%3==0 and selector//3<len(matrixSlots),'Invalid position matrix selector')
            sourceMatrix=matrixSlots[selector//3];need(sourceMatrix is not None,'Unresolved inherited matrix slot')
            if sourceMatrix not in palette:palette.append(sourceMatrix)
            if len(palette)>8:raise Unsupported('Group needs more than eight native matrices')
            local=palette.index(sourceMatrix);value['bones']=[local,0,0,0];value['weights']=[255,0,0,0]
            uv=[x for pair in value['uv'] for x in pair]
            key=VERTEX.pack(*value['position'],*value['normal'],*uv,*value['color'],local*3,0,0,0,255,0,0,0)
            if key not in lookup:lookup[key]=len(vertices);vertices.append(value)
            remap.append(lookup[key])
        indices.extend(remap[i] for triangle in triangles for i in triangle);packetCount+=1
        need(len(vertices)<=65536 and len(indices)<=65535,'Native mesh chunk exceeds bounds')
    return vertices,indices,palette,packetCount

def convert(data):
    blocks=sections(data);arrays=Arrays(blocks['VTX1']);shape=blocks['SHP1'];draw=blocks['DRW1']
    nDraw=u16(draw,8);need(nDraw>0,'No draw matrices')
    flags=span(draw,u32(draw,12),nDraw);indices=span(draw,u32(draw,16),nDraw*2);jointCount=u16(blocks['JNT1'],8)
    envelopeCount=u16(blocks['EVP1'],8) if 'EVP1' in blocks else 0;drawMap=[]
    for i,flag in enumerate(flags):
        index=u16(indices,2*i);need(flag in (0,1) and index<(envelopeCount if flag else jointCount),'Draw matrix pose reference')
        drawMap.append({'weighted_envelope':bool(flag),'pose_index':index})
    nShape=u16(shape,8);need(0<nShape<=4096,'Shape count bound')
    init,remap,descBase,mtxTable,display,mtxInit,drawInit=[u32(shape,x) for x in [12,16,24,28,32,36,40]]
    meshes=[];bindings=[];empty=[];totalPackets=0;totalGroups=0;totalDisplayBytes=0;totalVertices=0
    for sourceShape in range(nShape):
        index=u16(shape,remap+sourceShape*2);need(index<nShape,'Shape remap bound');record=init+index*40
        mode=span(shape,record,1)[0]
        if mode not in (0,3):raise Unsupported(f'Shape {sourceShape}: billboard mode {mode} requires native billboard pipeline')
        groups=u16(shape,record+2);totalGroups+=groups;need(0<groups<=4096 and totalGroups<=4096,'Matrix group count/work bound');desc=descriptor(shape,descBase+u16(shape,record+4),arrays)
        matrixFirst,drawFirst=u16(shape,record+6),u16(shape,record+8);slots=[None]*10
        for group in range(groups):
            m=mtxInit+(matrixFirst+group)*8;use,count,first=u16(shape,m),u16(shape,m+2),u32(shape,m+4)
            if mode==0:slots[0]=use
            else:
                need(1<=count<=10,'Source matrix group count')
                for slot in range(count):
                    value=u16(shape,mtxTable+2*(first+slot))
                    if value!=65535:slots[slot]=value
            d=drawInit+(drawFirst+group)*8;size,offset=u32(shape,d),u32(shape,d+4)
            totalDisplayBytes+=size;need(totalDisplayBytes<=128<<20,'Repeated geometry work bound')
            try:v,ix,palette,packetCount=packets(span(shape,display+offset,size),desc,arrays,slots)
            except (ValueError,struct.error) as error:raise type(error)(f'Shape {sourceShape} group {group}: {error}') from error
            need(all(i<nDraw for i in palette),'Position matrix outside DRW1')
            totalVertices+=len(v);need(totalVertices<=512000,'Converted native vertex work bound')
            totalPackets+=packetCount
            if not ix:empty.append({'shape':sourceShape,'group':group,'reason':'empty source geometry'});continue
            identifier=len(meshes)+1;need(identifier<=64,'Native pack has more than64mesh chunks')
            meshes.append({'id':identifier,'vertices':v,'indices':ix})
            bindings.append({'mesh':identifier,'source_shape':sourceShape,'source_group':group,'source_mode':mode,'draw_matrix_palette':palette})
    need(meshes,'No triangle geometry')
    document={'schema':'dusklight-native-mesh/1','meshes':meshes}
    packed=compile_pack(document) # Native numeric domains and all attribute contracts.
    manifest={'schema':'j3d-native-geometry/1','source_sha256':hashlib.sha256(data).hexdigest(),'source_blocks':list(blocks),
              'geometry_attribute_streams_complete':True,'rendering_ready':False,'materials_converted':False,'textures_converted':False,'animations_converted':False,'texgen_converted':False,
              'shape_count':nShape,'source_packets':totalPackets,'meshes':bindings,'empty_groups':empty,'draw_matrices':drawMap,
              'native_pack_sha256':hashlib.sha256(packed).hexdigest(),'native_pack_bytes':len(packed),
              'limits':'Geometry only. Pose bindings require native animation integration; material assignment, texgen, lighting and visual equivalence are unverified.'}
    return document,packed,manifest

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('input',type=Path);parser.add_argument('--output-prefix',type=Path,required=True);args=parser.parse_args()
    with args.input.open('rb') as stream:
        before=os.fstat(stream.fileno());data=stream.read((64<<20)+1);after=os.fstat(stream.fileno())
    need((before.st_dev,before.st_ino,before.st_size,before.st_mtime_ns,before.st_ctime_ns)==(after.st_dev,after.st_ino,after.st_size,after.st_mtime_ns,after.st_ctime_ns),'Source changed during read')
    document,packed,manifest=convert(data)
    from run_azahar_smoke import card_private_parent
    outputs=[(args.output_prefix.with_suffix('.dnp'),packed),(args.output_prefix.with_suffix('.native.json'),(json.dumps(document,separators=(',',':'))+'\n').encode()),
             (args.output_prefix.with_suffix('.receipt.json'),(json.dumps(manifest,indent=2)+'\n').encode())]
    created=[]
    try:
        for path,payload in outputs:
            with card_private_parent(path,create=True) as (parent,name,actual):
                pinned=os.dup(parent)
                try:fd=os.open(name,os.O_CREAT|os.O_EXCL|os.O_WRONLY|os.O_NOFOLLOW,0o600,dir_fd=parent)
                except BaseException:os.close(pinned);raise
                info=os.fstat(fd);created.append((pinned,name,info.st_dev,info.st_ino))
                with os.fdopen(fd,'wb') as stream:
                    need(stream.write(payload)==len(payload),'Short native output write');stream.flush();os.fsync(stream.fileno())
    except BaseException:
        for parent,name,device,inode in created:
            try:
                info=os.stat(name,dir_fd=parent,follow_symlinks=False)
                if (info.st_dev,info.st_ino)==(device,inode):os.unlink(name,dir_fd=parent)
            except FileNotFoundError:pass
        raise
    finally:
        for parent,_,_,_ in created:os.close(parent)
    print(json.dumps(manifest,indent=2))

if __name__=='__main__':main()
