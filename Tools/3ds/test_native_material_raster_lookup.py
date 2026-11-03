#!/usr/bin/env python3
"""Fragment-space dual raster transport, alpha, S10 lanes and source fog.

These are mathematical material declarations, not fabricated game assets.
Optional --inventory exercises the original local PAL inputs as well.
"""
import argparse
import copy
import json
import math
from pathlib import Path
import random
import struct
from j3d_native_materials import compile_material,convert,Unsupported
from j3d_material_source import materials
from j3d_native_geometry import sections
from native_material_pack import compile_materials
from test_j3d_native_materials import native_eval,source_fixture


def declaration():
    def stage(rgb,alpha,raster,slot=255,coordinate=255):
        return {'rgb':rgb,'alpha':alpha,'rgb_op':(0,0,0,1,0),'alpha_op':(0,0,0,1,0),
            'raster':raster,'texture_slot':slot,'coordinate':coordinate,'raster_swap':(0,1,2,3),
            'texture_swap':(0,1,2,3),'konst_color_selector':0,'konst_alpha_selector':0}
    return {'id':1,'textures':[None]*6+[4,None],'generators':[(1,4,60,0),(1,5,60,0),(1,6,60,0)],
        'registers':[[-1024,-1,256,1023]]*4,'konst':[[255]*4]*4,
        'stages':[stage((15,10,8,15),(7,5,4,7),4,6,2),stage((15,0,10,15),(7,0,5,7),5)],
        'channel_count':2,'channels':[(1,1,255,2,1,0,255,255),(0,1,0,0,2,0,255,255)]*2,
        'states':{'blend':(0,1,0,0),'depth':(1,3,1,0),'cull':2,'alpha':(7,0,0,7,0),
            'fog':tuple(bytes(44)),'early_depth':0,'dither':1}}

def fog_declaration(texture_count,projective=False):
    source=declaration();source['stages']=source['stages'][:1];stage=source['stages'][0]
    source['states']['fog']=(2,1,1,64)+tuple(bytes(40))
    source['textures']=[4,5,6]+[None]*5
    stage['texture_slot']=stage['coordinate']=0
    if projective:source['generators'][0]=(0,0,60,0)
    if not texture_count:stage['rgb']=(10,15,15,15);stage['alpha']=(5,7,7,7)
    for n in range(1,texture_count):
        extra=copy.deepcopy(stage);extra['rgb']=(15,0,8,15);extra['alpha']=(7,0,4,7)
        extra['texture_slot']=extra['coordinate']=n;source['stages'].append(extra)
    return source



def compile_one(source,**kwargs):
    return compile_material(source,allow_normalized_arithmetic=True,preserve_color_parameters=True,**kwargs)


def wire(record,parameters):
    result=copy.deepcopy(record)
    for stage in result['stages']:
        values=[stage['constant']>>(8*c)&255 for c in range(4)]
        for c,selector in enumerate(stage.get('constant_bindings',[0]*4)):
            if selector:values[c]=parameters[(selector-1)//4][(selector-1)%4]
        stage['constant']=sum(v<<(8*c) for c,v in enumerate(values))
    return result


def sample_native(image,u,v):
    # PICA T starts at the bottom; native Morton rows retain authored top-to-bottom order.
    width,height=image['width'],image['height']
    x,y=u*width-.5,(1-v)*height-.5;ix,iy=math.floor(x),math.floor(y);fx,fy=x-ix,y-iy
    data=bytes.fromhex(image['data_hex']);result=[0.0]*4
    for dx,wx in [(0,1-fx),(1,fx)]:
        for dy,wy in [(0,1-fy),(1,fy)]:
            tx,ty=max(0,min(width-1,ix+dx)),max(0,min(height-1,iy+dy))
            texel=((ty//8)*(width//8)+tx//8)*64+sum((((tx%8)>>bit)&1)<<(2*bit)|(((ty%8)>>bit)&1)<<(2*bit+1) for bit in range(3))
            for c in range(4):result[c]+=wx*wy*data[4*texel+3-c]/255
    return result

def bilinear(image,rgba):
    a,b=image['texgen']['components']
    return sample_native(image,(.5+rgba[a])/8,1-(.5+rgba[b])/8)



def lookup_samples(receipt,secondary,texture,fog_factor=0):
    images={image['key']:image for image in receipt['synthetic_images']}
    def synthetic_sample(key):
        image=images[key]
        return sample_native(image,.5,(.5+255*fog_factor)/256) if image['kind']=='fog_factor' else bilinear(image,secondary)
    return {(b['image'],b['coordinate']):synthetic_sample(b['synthetic']) if b.get('synthetic') else texture
        for b in receipt['texture_bindings']}


def indirect_source(enabled,count,settings):
    source=source_fixture();block=bytearray(sections(source)['MAT3']);at=len(block);block.extend(bytes(312))
    struct.pack_into('>I',block,24,at);block[at:at+2]=bytes((enabled,count))
    block[at+120:at+129]=bytes(settings);struct.pack_into('>I',block,4,len(block))
    result=bytearray(source[:-len(sections(source)['MAT3'])]+block);struct.pack_into('>I',result,8,len(result));return bytes(result)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--inventory',type=Path);args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    source=declaration();record,receipt=compile_one(source,synthetic_image_base=12)
    (out/'lookup.dnm').write_bytes(compile_materials([record]));rng=random.Random(158);checked=0
    # Fixed native Morton offsets match the original BTI decoder's row order:
    # (0,0)=0, (0,1)=2, (1,0)=1. Sampling supplies the PICA T inversion.
    rg,ba=[bytes.fromhex(image['data_hex']) for image in receipt['synthetic_images']]
    assert rg[0:4]==bytes((0,0,0,0)) and rg[8:12]==bytes((0,0,255,0)) and rg[4:8]==bytes((0,0,0,255))
    assert ba[0:4]==bytes((0,0,0,0)) and ba[8:12]==bytes((255,0,0,0)) and ba[4:8]==bytes((0,255,0,0))
    # The unequal colors make interpolate(a*b) observably wrong; both products
    # must happen after independently interpolating the two RGBA inputs.
    bary=(.2,.3,.5);vertices0=[[rng.random() for _ in range(4)] for _ in range(3)]
    vertices1=[[rng.random() for _ in range(4)] for _ in range(3)]
    for case in range(1024):
        r0=[sum(bary[v]*vertices0[v][c] for v in range(3)) for c in range(4)]
        r1=[sum(bary[v]*vertices1[v][c] for v in range(3)) for c in range(4)]
        texture=[rng.random() for _ in range(4)];samples=lookup_samples(receipt,r1,texture)
        actual=native_eval(record,receipt['texture_bindings'],samples,r0)
        expected=[texture[c]*r0[c]*r1[c] for c in range(4)]
        assert max(abs(a-b) for a,b in zip(actual,expected))<1e-12,(actual,expected)
        premult=[texture[c]*sum(bary[v]*vertices0[v][c]*vertices1[v][c] for v in range(3)) for c in range(4)]
        if case==0:assert max(abs(a-b) for a,b in zip(actual,premult))>1e-3
        bad=copy.deepcopy(record);bad['buffer_alpha_mask']=0
        assert abs(native_eval(bad,receipt['texture_bindings'],samples,r0)[3]-expected[3])>1e-12
        bad=copy.deepcopy(record);bad['buffer_rgb_mask']=0
        assert max(abs(a-b) for a,b in zip(native_eval(bad,receipt['texture_bindings'],samples,r0)[:3],expected[:3]))>1e-12
        vertices0=[[rng.random() for _ in range(4)] for _ in range(3)];vertices1=[[rng.random() for _ in range(4)] for _ in range(3)];checked+=1
    assert receipt['texture_bindings'][0]['texture_slot']==6 and receipt['texture_bindings'][0]['texture_generator']==2
    assert receipt['raster_read_masks']==[15,15] and receipt['light_mask']==255
    # When raster1 is used immediately, stage0's capture must already be visible
    # at stage2. Updating the same value at stage1 is observably one stage late.
    early=copy.deepcopy(source);early['stages'][0]['raster']=5;early['stages'][1]['raster']=4
    early_record,early_meta=compile_one(early);r0=[.2,.4,.6,.8];r1=[.7,.5,.3,.1];texture=[.9,.6,.3,.7]
    samples=lookup_samples(early_meta,r1,texture);expected=[texture[c]*r0[c]*r1[c] for c in range(4)]
    assert max(abs(a-b) for a,b in zip(native_eval(early_record,early_meta['texture_bindings'],samples,r0),expected))<1e-12
    early_record['buffer_rgb_mask']=early_record['buffer_alpha_mask']=2
    assert max(abs(a-b) for a,b in zip(native_eval(early_record,early_meta['texture_bindings'],samples,r0),expected))>1e-3
    # No original texture: lookup occupies units0/1 instead of1/2.
    untextured=copy.deepcopy(source);untextured['stages'][0]['rgb']=(10,15,15,15);untextured['stages'][0]['alpha']=(5,7,7,7)
    bare,bare_meta=compile_one(untextured);r0=[.2,.4,.6,.8];r1=[.7,.5,.3,.1]
    assert max(abs(a-b) for a,b in zip(native_eval(bare,bare_meta['texture_bindings'],lookup_samples(bare_meta,r1,[0]*4),r0),[a*b for a,b in zip(r0,r1)]))<1e-12
    # Native fog never consumes source alpha, and must retain range metadata.
    fogged=copy.deepcopy(source);fogged['stages'][0]['alpha']=(4,7,7,7);fogged['stages'][1]['alpha']=(0,7,7,7)
    fogged['states']['fog']=(2,1,1,64)+tuple(bytes(40));fog_record,fog_meta=compile_one(fogged)
    fog_color=[31,97,173,221];fog_factor=.37;r0=[.3,.5,.8,fog_factor];r1=[.7,.2,.4,.9];tex=[.9,.8,.6,.55]
    resolved=wire(fog_record,[[0]*4]*7+[fog_color]);actual=native_eval(resolved,fog_meta['texture_bindings'],lookup_samples(fog_meta,r1,tex),r0)
    expected=[tex[c]*r0[c]*r1[c]*(1-fog_factor)+fog_color[c]/255*fog_factor for c in range(3)]+[tex[3]]
    assert max(abs(a-b) for a,b in zip(actual,expected))<1e-12
    assert fog_meta['source_fog']['range_enabled'] and fog_meta['raster_read_masks']==[7,7]
    (out/'fog-lookup.dnm').write_bytes(compile_materials([fog_record]))
    # Spare texture carriers must preserve every primary component, including
    # alpha, and may not steal a projected source texture's unit0/Q.
    for count in range(3):
        textured=fog_declaration(count,projective=count>0)
        fog_record,fog_meta=compile_one(textured,synthetic_image_base=12)
        assert fog_meta['fog_carrier']=='texture'+str(count)
        assert fog_record['images']==[5,6][:count]+[15]+[0]*(2-count)
        if count:assert fog_meta['texture_bindings'][0]['projective']
        ramp=bytes.fromhex(fog_meta['synthetic_images'][-1]['data_hex'])
        assert len(ramp)==8192 and [ramp[n] for n in (0,8,256,8104)]==[255,254,247,0]
        for factor in (0,1/510,.37,254.5/255,1):
            r0=[.2,.6,.8,.73];tex=[.9,.7,.4,.41]
            samples=lookup_samples(fog_meta,[0]*4,tex,factor)
            actual=native_eval(wire(fog_record,[[0]*4]*7+[fog_color]),fog_meta['texture_bindings'],samples,r0)
            expected=[r0[c]*tex[c]**count*(1-factor)+fog_color[c]/255*factor for c in range(3)]+[r0[3]*tex[3]**count]
            assert max(abs(a-b) for a,b in zip(actual,expected))<1e-12,(count,factor,actual,expected)
        compile_materials([fog_record])
    # The two raster lookup units and fog can coexist only without an original
    # texture. Their +1/+2/+3 IDs never collapse when one helper is absent.
    untextured['states']['fog']=fogged['states']['fog']
    combined,combined_meta=compile_one(untextured,synthetic_image_base=12)
    assert combined['images']==[13,14,15] and combined_meta['fog_carrier']=='texture2'
    assert [image['kind'] for image in combined_meta['synthetic_images']]==['raster_color','raster_color','fog_factor']
    r0=[.2,.4,.6,.8];r1=[.7,.5,.3,.1];factor=.37
    got=native_eval(wire(combined,[[0]*4]*7+[fog_color]),combined_meta['texture_bindings'],lookup_samples(combined_meta,r1,[0]*4,factor),r0)
    expected=[r0[c]*r1[c]*(1-factor)+fog_color[c]/255*factor for c in range(3)]+[r0[3]*r1[3]]
    assert max(abs(a-b) for a,b in zip(got,expected))<1e-12
    compile_materials([combined])
    full=fog_declaration(3)
    exhausted=fog_declaration(0)
    for _ in range(6):
        stage=copy.deepcopy(exhausted['stages'][0]);stage['rgb']=(15,0,10,15);stage['alpha']=(7,0,5,7);exhausted['stages'].append(stage)
    for bad,reason,options in [(full,'occupy all three',{}),(dict(source,states=fogged['states']),'occupy all three',{}),
        (exhausted,'six-stage budget',{}),(fog_declaration(0),'uint32',{'synthetic_image_base':0xfffffffd}),
        (fog_declaration(1),'overlaps original',{'synthetic_image_base':0})]:
        try:compile_one(bad,**options)
        except Unsupported as error:assert reason in str(error),(reason,str(error))
        else:raise AssertionError('Unavailable fog resources accepted')
    # Only actual initial parameter lanes are validated. A/B/C read low bytes;
    # D reads signed values and cannot blindly truncate -1 or256 to a byte.
    s10=declaration();s10['stages']=s10['stages'][:1];s=s10['stages'][0];s['rgb']=(2,15,15,15);s['alpha']=(1,7,7,7)
    for value in (-1024,-1,0,255,256,1023):
        s10['registers']=[[value]*4,[-32768]*4,[32767]*4,[0]*4];packed,meta=compile_one(s10)
        got=native_eval(wire(packed,[[value&255]*4]+[[0]*4]*7),[],{},[0]*4)
        assert max(abs(c-(value&255)/255) for c in got)<1e-12
        assert meta['parameter_read_masks']==[15,0,0,0,0,0,0] and meta['parameter_wrap_masks']==[15,0,0,0,0,0,0]
    s['rgb']=(15,15,15,2);s['alpha']=(7,7,7,1)
    for value in (-1024,-1,256,1023):
        s10['registers'][0]=[value]*4
        try:compile_one(s10)
        except Unsupported as error:assert 'signed D' in str(error)
        else:raise AssertionError('Signed D silently truncated')
    for value in (0,255):
        s10['registers'][0]=[value]*4;packed,meta=compile_one(s10)
        got=native_eval(wire(packed,[[value]*4]+[[0]*4]*7),[],{},[0]*4)
        assert got==[value/255]*4 and meta['parameter_wrap_masks']==[0]*7
    s['rgb']=(14,15,15,15);s['alpha']=(6,7,7,7)
    for selector,byte in enumerate((255,223,191,159,128,96,64,32)):
        s['konst_color_selector']=s['konst_alpha_selector']=selector
        packed,meta=compile_one(s10)
        assert native_eval(packed,[],{},[0]*4)==[byte/255]*4
    # Actual table semantics: enabled alone, stage count and bias alone are not
    # a coordinate effect. A matrix is, even if its TEV source was unchanged.
    inactive=list(materials(indirect_source(1,1,(0,0,7,0,0,0,0,0,0))))[0]
    assert 'error' not in inactive and inactive['indirect']['stage_count']==1
    active=list(materials(indirect_source(1,1,(0,0,7,1,0,0,0,0,0))))[0]
    assert 'Active indirect TEV stage 0' in active['error']
    original_results=[]
    if args.inventory:
        for entry in json.loads(args.inventory.read_text()):
            if entry['kind']!='model':continue
            _,result=convert(Path(entry['path']).read_bytes(),allow_normalized_arithmetic=True,preserve_color_parameters=True,allow_native_temporaries=True)
            original_results.append({'archive':entry['archive'],'name':entry['name'],'materials':result['materials']})
    (out/'receipt.json').write_text(json.dumps({'status':'pass','fragment_samples':checked,'original_inputs':original_results,
        'scope':'Normalized fragment mathematics, exact lookup bytes and lane boundaries; not GX integer rounding or physical PICA validation.'},indent=2)+'\n')
    print(f'PASS {checked} dual-raster fragment products, independent alpha masks, source textures, fog and S10 boundaries')

if __name__=='__main__':main()
