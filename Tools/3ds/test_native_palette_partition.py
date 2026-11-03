#!/usr/bin/env python3
"""Ordered source geometry and v3 plan metadata, with an actual C++ reader.

Run after integration: python Tools/3ds/test_native_palette_partition.py --output Build/native-palette-partition
No game resources or emulator are used.
"""
import argparse
import copy
import os
from pathlib import Path
import struct
import subprocess
import zlib
import j3d_native_geometry as geometry
from native_mesh_pack import compile_pack
from native_model_plan import compile_plan
from test_j3d_native_geometry import append,finish,half,word

ROOT=Path(__file__).resolve().parents[2]
SLOTS=[9,2,7,0,8,1,6,3,5,4]
TRIANGLES=[[(i,0) for i in t] for t in [(0,1,2),(2,1,0),(2,3,0),(4,3,2),(4,5,6),(7,8,9),(0,1,2)]]+[
    [(0,0),(0,1),(0,2)]]
ATTRIBUTES=[0,9,10,11,12,13,14,15]
READ_MASKS=[15,10,5,1,2,4,8]
WRAP_MASKS=[1,2,0,0,0,0,0]
SIGNED_MASKS=[10,8,5,0,0,0,0]

def attributes(slot,variant):
    # Distinct normal, UV and both color streams, including color-only seams.
    return {'position':[slot/4,1.,-1.],'normal':[0.,.5,1.],
            'uv':[[slot/256,-.5],[slot+.25,1.5],[-slot/256,.25]],
            'color':[slot,23,45,67],'color1':[89,variant,123,145]}

def corner_bytes(slot,variant):
    v=attributes(slot,variant)
    return struct.pack('>B3h3b8B2h2f2h',slot*3,slot,4,-4,0,32,64,
                       *v['color'],*v['color1'],slot,-128,slot+.25,1.5,-slot,64)

def fixture():
    v=bytearray(64)
    formats=[(9,1,3,2),(10,0,1,0),(11,1,5,0),(12,1,5,0),(13,1,3,8),(14,1,4,0),(15,1,3,8)]
    word(v,8,append(v,16,b''.join(struct.pack('>IIIB3x',*f) for f in formats)+struct.pack('>I12x',255)))
    v=finish(v,b'VTX1')
    d=bytearray(20);half(d,8,10)
    word(d,12,append(d,4,bytes([0,1]*5)))
    word(d,16,append(d,2,struct.pack('>10H',*[i//2 for i in range(10)])));d=finish(d,b'DRW1')
    j=bytearray(24);half(j,8,5);j=finish(j,b'JNT1')
    e=bytearray(24);half(e,8,5);e=finish(e,b'EVP1')
    s=bytearray(44);half(s,8,1);init=bytearray(40);init[0]=3;half(init,2,2)
    word(s,12,append(s,4,init));word(s,16,append(s,2,bytes(2)))
    word(s,24,append(s,4,b''.join(struct.pack('>II',a,1) for a in ATTRIBUTES)+struct.pack('>II',255,0)))
    word(s,28,append(s,2,struct.pack('>20H',*SLOTS,65535,4,*([65535]*8))))
    display=bytearray();draws=[]
    for triangles in (TRIANGLES,TRIANGLES[:2]):
        start=len(display)
        # Multiple source packets must not reset partition or ordering state.
        for triangle in triangles:
            display.extend(struct.pack('>BH',0x90,3))
            display.extend(b''.join(corner_bytes(*c) for c in triangle))
        draws.append((len(display)-start,start))
    word(s,32,append(s,32,display))
    word(s,36,append(s,4,struct.pack('>HHIHHI',0,10,0,0,10,10)))
    word(s,40,append(s,4,b''.join(struct.pack('>II',*d) for d in draws)));s=finish(s,b'SHP1')
    blocks=[v,d,j,e,s]
    return struct.pack('>8sII16x',b'J3D2bmd3',32+sum(map(len,blocks)),len(blocks))+b''.join(blocks)

def signature(vertex,source):
    return (struct.pack('<12f8B',*vertex['position'],*vertex['normal'],
                        *(x for uv in vertex['uv'] for x in uv),*vertex['color'],*vertex['color1']),source)

def check_geometry():
    document,bank,manifest=geometry.convert(fixture())
    expected=[]
    for slots,triangles in ((SLOTS,TRIANGLES),([SLOTS[0],4,*SLOTS[2:]],TRIANGLES[:2])):
        expected.extend(signature(attributes(s,v),slots[s]) for triangle in triangles for s,v in triangle)
    observed=[];groups=[]
    for mesh,binding in zip(document['meshes'],manifest['meshes']):
        palette=binding['draw_matrix_palette'];groups.append(binding['source_group'])
        assert binding['source_shape']==0 and binding['source_mode']==3
        assert binding['source_attributes']==ATTRIBUTES and 1<=len(palette)<=4
        vertices=mesh['vertices']
        assert len({signature(v,palette[v['bones'][0]]) for v in vertices})==len(vertices)
        for index in mesh['indices']:
            v=vertices[index]
            assert v['bones'][1:]==[0,0,0] and v['weights']==[255,0,0,0]
            observed.append(signature(v,palette[v['bones'][0]]))
    assert observed==expected, 'Triangle order, winding, attributes or draw identities changed'
    assert groups==sorted(groups) and groups.count(0)>1 and groups.count(1)==1
    first=document['meshes'][0]
    assert first['indices'][:6]==[0,1,2,2,1,0], 'Shared triangle vertices were not deduplicated'
    assert manifest['draw_matrices']==[{'weighted_envelope':bool(i%2),'pose_index':i//2} for i in range(10)]
    assert compile_pack(document)==bank
    # Decode the actual native bank, not only the intermediate document.
    actual=[]
    for n,binding in enumerate(manifest['meshes']):
        identifier,nv,ni,vo,io,vb,ib,_=struct.unpack_from('<8I',bank,32+n*32)
        assert (identifier,nv,ni,vb,ib)==(document['meshes'][n]['id'],len(document['meshes'][n]['vertices']),len(document['meshes'][n]['indices']),nv*64,ni*2)
        for i in struct.unpack_from('<'+'H'*ni,bank,io):
            at=vo+i*64;rows=bank[at+56:at+60]
            assert rows[0] in (0,3,6,9) and rows[1:]==bytes(3)
            assert bank[at+60:at+64]==bytes([255,0,0,0])
            actual.append((bank[at:at+56],binding['draw_matrix_palette'][rows[0]//3]))
    assert actual==expected
    # One strip/fan packet can itself cross several palette boundaries.
    arrays=geometry.Arrays(geometry.sections(fixture())['VTX1'])
    for op,triangles in ((0x98,[(0,1,2),(2,1,3),(2,3,4),(4,3,5),(4,5,6),(6,5,7),(6,7,8),(8,7,9)]),
                         (0xa0,[(0,i-1,i) for i in range(2,10)])):
        packet=struct.pack('>BH',op,10)+b''.join(corner_bytes(i,0) for i in range(10))
        v,ix,p,count=geometry.packets(packet,[(a,1) for a in ATTRIBUTES],arrays,SLOTS)
        chunks=geometry.partition(v,ix,p)
        assert count==1 and all(len(p)<=4 for _,_,p in chunks)
        assert [signature(v[i],p[v[i]['bones'][0]]) for v,ix,p in chunks for i in ix]==[
            signature(attributes(i,0),SLOTS[i]) for triangle in triangles for i in triangle]
    # A long same-palette group splits at the index bound, without losing one.
    vertices=first['vertices'][:3];indices=[0,1,2]*21846
    chunks=geometry.partition(vertices,indices,manifest['meshes'][0]['draw_matrix_palette'])
    assert [len(ix) for _,ix,_ in chunks]==[65535,3]
    assert [signature(v[i],p[v[i]['bones'][0]]) for v,ix,p in chunks for i in ix]==[signature(vertices[i],SLOTS[i]) for i in indices]
    bad=copy.deepcopy(document);bad['meshes'][0]['vertices'][0]['bones'][0]=4
    try:compile_pack(bad)
    except ValueError:pass
    else:raise AssertionError('Fifth native matrix selector admitted')

def plan_fixture(metadata=True):
    uniform={'material':5,'anchor':3,'palette':[10,3,8,1]}
    if metadata:uniform.update(texture_slots=[7,1,255],texture_generators=[2,0,255],source_attributes=sum(1<<a for a in ATTRIBUTES),
                               channel_read_mask=5,parameter_read_masks=READ_MASKS,parameter_wrap_masks=WRAP_MASKS,
                               parameter_signed_masks=SIGNED_MASKS,source_shape=4095,billboard_mode=2,
                               source_stage_count=4,source_texgen_count=3)
    return {'schema':'dusklight-native-model-plan/1','requirements':[],
            'parts':[{'mesh':m,'material':5,'uniform_slot':0,'sampler_mask':3,'samplers':[{}, {}, {}],'pass':'alpha'} for m in (9,3,7)],'uniforms':[uniform]}

READER=r'''#include "native/model_plan.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>
using namespace dusk::pica;
unsigned word(const std::vector<std::byte>& d,unsigned at){unsigned v=0;for(unsigned n=0;n<4;++n)v|=std::to_integer<unsigned>(d[at+n])<<(8*n);return v;}
void put(std::vector<std::byte>& d,unsigned at,unsigned v){for(unsigned n=0;n<4;++n)d[at+n]=std::byte(v>>(8*n));}
void repair(std::vector<std::byte>& d){put(d,44,crc32(std::span(d).first(44),std::span(d).subspan(48)));}
int main(int argc,char** argv){
    assert(argc==4);auto plan=std::make_unique<ModelPlan>();
    for(unsigned mode=0;mode<2;++mode){
        std::ifstream file(argv[mode+1],std::ios::binary);assert(file.good());
        std::vector<char> raw((std::istreambuf_iterator<char>(file)),{});std::vector<std::byte> bytes(raw.size());std::memcpy(bytes.data(),raw.data(),raw.size());
        assert(ModelPlan::open(bytes,*plan)==PackError::none);
        assert(plan->size()==3&&plan->uniformCount()==1);const auto* u=plan->uniform(0);
        assert(u&&u->material==5&&u->anchor==3&&u->paletteCount==4);
        assert((u->palette[0]==10&&u->palette[1]==3&&u->palette[2]==8&&u->palette[3]==1));
        assert(u->sourceAttributes==(mode?0u:65025u));
        assert(u->channelReadMask==(mode?15:5)&&u->sourceShape==(mode?0:4095)&&u->billboardMode==(mode?0:2));
        assert(u->sourceStageCount==(mode?0:4)&&u->sourceTexGenCount==(mode?0:3));
        const unsigned reads[7]={15,10,5,1,2,4,8},wraps[7]={1,2,0,0,0,0,0},signs[7]={10,8,5,0,0,0,0};
        for(unsigned p=0;p<7;++p){
            assert(u->parameterReadMasks[p]==(mode?0:reads[p]));
            assert(u->parameterWrapMasks[p]==(mode?0:wraps[p]));
            assert(u->parameterSignedMasks[p]==(mode?0:signs[p]));
        }
        const unsigned slots[3]={7,1,255},generators[3]={2,0,255},meshes[3]={9,3,7};
        for(unsigned n=0;n<3;++n){
            assert(u->textureSlots[n]==(mode?255:slots[n])&&u->textureGenerators[n]==(mode?255:generators[n]));
            PartBinding p;unsigned pass;assert(plan->entry(n,p,pass)&&p.mesh==meshes[n]&&pass==1);
        }
        const auto offset=word(bytes,32);
        auto rejected=[&](std::vector<std::byte> bad){repair(bad);assert(ModelPlan::open(bad,*plan)!=PackError::none&&plan->size()==0&&plan->uniformCount()==0);};
        for(unsigned version:{0u,1u,2u,3u,5u}){auto bad=bytes;put(bad,8,version);rejected(bad);}
        for(unsigned count:{0u,5u,8u}){auto bad=bytes;put(bad,offset+8,count);rejected(bad);}
        for(unsigned at:{28u,29u,30u,31u,32u,33u}){auto bad=bytes;bad[offset+at]=std::byte{8};rejected(bad);}
        for(unsigned at:{34u,35u,62u,63u,74u,75u,76u,77u,78u,79u}){auto bad=bytes;bad[offset+at]=std::byte{1};rejected(bad);}
        {auto bad=bytes;bad[offset+72]=std::byte{17};rejected(bad);}
        {auto bad=bytes;bad[offset+73]=std::byte{9};rejected(bad);}
        {auto bad=bytes;bad[offset+72]=std::byte{0};bad[offset+73]=std::byte{1};rejected(bad);}
        for(unsigned at=40;at<62;++at){auto bad=bytes;bad[offset+at]=std::byte{16};rejected(bad);}
        for(unsigned p=0;p<7;++p)for(unsigned lane=1;lane<=8;lane<<=1){
            auto base=bytes;std::fill(base.begin()+offset+41,base.begin()+offset+62,std::byte{});
            {auto bad=base;bad[offset+48+p]=std::byte(lane);rejected(bad);}
            {auto bad=base;bad[offset+55+p]=std::byte(lane);rejected(bad);}
            {auto bad=base;bad[offset+41+p]=bad[offset+48+p]=bad[offset+55+p]=std::byte(lane);rejected(bad);}
            if(p>=3){
                auto bad=base;bad[offset+41+p]=bad[offset+48+p]=std::byte(lane);rejected(bad);
                bad=base;bad[offset+41+p]=bad[offset+55+p]=std::byte(lane);rejected(bad);
            }
        }
        for(unsigned shape:{4096u,0xffffffffu}){auto bad=bytes;put(bad,offset+64,shape);rejected(bad);}
        for(unsigned billboard:{3u,0xffffffffu}){auto bad=bytes;put(bad,offset+68,billboard);rejected(bad);}
        for(unsigned shape:{0u,4095u})for(unsigned billboard=0;billboard<=2;++billboard)for(unsigned channel=0;channel<=15;++channel){
            auto valid=bytes;put(valid,offset+64,shape);put(valid,offset+68,billboard);valid[offset+40]=std::byte(channel);repair(valid);
            assert(ModelPlan::open(valid,*plan)==PackError::none);
            assert(plan->uniform(0)->sourceShape==shape&&plan->uniform(0)->billboardMode==billboard&&plan->uniform(0)->channelReadMask==channel);
        }
        {auto bad=bytes;bad[offset+28]=std::byte(mode?7:255);rejected(bad);}
        {auto bad=bytes;put(bad,offset+8,3);rejected(bad);}
        for(unsigned size=0;size<bytes.size();++size)assert(ModelPlan::open(std::span(bytes).first(size),*plan)!=PackError::none&&plan->size()==0);
        for(unsigned at=0;at<bytes.size();++at){auto bad=bytes;bad[at]^=std::byte{1};assert(ModelPlan::open(bad,*plan)!=PackError::none);}
    }
    std::ifstream file(argv[3],std::ios::binary);assert(file.good());
    std::vector<char> raw((std::istreambuf_iterator<char>(file)),{});std::vector<std::byte> bytes(raw.size());std::memcpy(bytes.data(),raw.data(),raw.size());
    assert(bytes.size()==48+64*64+64*80&&ModelPlan::open(bytes,*plan)==PackError::none&&plan->size()==64&&plan->uniformCount()==64);
    for(unsigned n=0;n<64;++n){PartBinding part;unsigned pass;assert(plan->entry(n,part,pass)&&part.uniformSlot==n&&plan->uniform(n)->sourceShape==n);}
    for(unsigned at=0;at<bytes.size();++at){auto bad=bytes;bad[at]^=std::byte{1};assert(ModelPlan::open(bad,*plan)!=PackError::none);}
    bytes.push_back(std::byte{});put(bytes,12,bytes.size());repair(bytes);assert(ModelPlan::open(bytes,*plan)==PackError::header);
}
'''

def check_plans(out):
    plan=plan_fixture();bank=compile_plan(plan);offset=48+3*64
    assert struct.unpack_from('<8s9I',bank)==(b'DUSKML01',4,offset+80,3,1,0,48,offset,80,0)
    expected=struct.pack('<7I6B2xI',5,3,4,10,3,8,1,7,1,255,2,0,255,65025)
    expected+=bytes([5,*READ_MASKS,*WRAP_MASKS,*SIGNED_MASKS])+bytes(2)+struct.pack('<II2B6x',4095,2,4,3)
    assert bank[offset:]==expected
    assert struct.unpack_from('<I',bank,44)[0]==zlib.crc32(bank[:44]+bank[48:])
    for field,value in [('palette',[1,2,3,4,5]),('texture_slots',[0,8,255]),('texture_slots',[0,1]),
                        ('texture_generators',[0,True,255]),('texture_generators',[2,0,7]),
                        ('texture_slots',[255,1,255]),('source_attributes',-1),('source_attributes',1<<32),('source_attributes',True)]:
        bad=copy.deepcopy(plan);bad['uniforms'][0][field]=value
        try:compile_plan(bad)
        except ValueError:pass
        else:raise AssertionError(f'Malformed {field} admitted: {value!r}')
    invalid=[('channel_read_mask',v) for v in (-1,16,True,1.0,None)]
    invalid += [(field,v) for field in ('source_shape','billboard_mode') for v in (-1,True,1.0,None)]
    invalid += [('source_shape',4096),('billboard_mode',3)]
    invalid += [('source_stage_count',v) for v in (-1,0,17,True,1.0,None)]
    invalid += [('source_texgen_count',v) for v in (-1,9,True,1.0,None)]
    for field in ('parameter_read_masks','parameter_wrap_masks','parameter_signed_masks'):
        invalid += [(field,v) for v in (None,True,0,[0]*6,[0]*8)]
        for index in range(7):
            for value in (-1,16,True,1.0,None):
                masks=[0]*7;masks[index]=value;invalid.append((field,masks))
    for field,value in invalid:
        bad=plan_fixture();bad['uniforms'][0][field]=value
        try:compile_plan(bad)
        except ValueError:pass
        else:raise AssertionError(f'Malformed {field} admitted: {value!r}')
    for field in ('source_stage_count','source_texgen_count'):
        bad=plan_fixture();del bad['uniforms'][0][field]
        try:compile_plan(bad)
        except ValueError:pass
        else:raise AssertionError(f'Missing {field} admitted')
    for index in range(7):
        for lane in (1,2,4,8):
            cases=[(0,lane,0),(0,0,lane),(lane,lane,lane)]
            if index>=3:cases.extend([(lane,lane,0),(lane,0,lane)])
            for read,wrap,sign in cases:
                bad=plan_fixture(False);u=bad['uniforms'][0]
                for field,value in zip(('parameter_read_masks','parameter_wrap_masks','parameter_signed_masks'),(read,wrap,sign)):
                    u[field]=[0]*7;u[field][index]=value
                try:compile_plan(bad)
                except ValueError:pass
                else:raise AssertionError(f'Invalid read/wrap/signed lanes admitted: {index}, {read}, {wrap}, {sign}')
    for shape in (0,4095):
        for billboard in range(3):
            valid=plan_fixture();valid['uniforms'][0].update(source_shape=shape,billboard_mode=billboard)
            assert struct.unpack_from('<II',compile_plan(valid),offset+64)==(shape,billboard)
    (out/'metadata.dnl').write_bytes(bank);(out/'defaults.dnl').write_bytes(compile_plan(plan_fixture(False)))
    maximum=plan_fixture()
    maximum['parts']=[dict(maximum['parts'][0],uniform_slot=n) for n in range(64)]
    maximum['uniforms']=[dict(maximum['uniforms'][0],source_shape=n) for n in range(64)]
    maximum_bank=compile_plan(maximum);assert len(maximum_bank)==48+64*(64+80)
    (out/'maximum.dnl').write_bytes(maximum_bank)
    (out/'reader.cpp').write_text(READER)
    native=ROOT/'Source/platforms/3ds/native';sdk=Path(os.environ.get('DEVKITPRO',Path.home()/'devkitpro'))
    command=['clang++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer',
             '-I'+str(native.parent),'-I'+str(native.parent/'tests/native_engine_stubs'),'-I'+str(sdk/'libctru/include'),
             str(native/'contracts.cpp'),str(native/'pack.cpp'),str(native/'model_plan.cpp'),str(out/'reader.cpp'),'-o',str(out/'reader')]
    subprocess.run(command,check=True,timeout=90)
    subprocess.run([str(out/'reader'),str(out/'metadata.dnl'),str(out/'defaults.dnl'),str(out/'maximum.dnl')],check=True,timeout=30)

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    check_geometry();check_plans(out)
    print('PASS ordered four-matrix partitions, complete attributes, source identities and v4 metadata/lane/billboard refusals')

if __name__=='__main__':main()
