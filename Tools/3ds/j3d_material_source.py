"""Bounded Mac-only MAT3 declaration reader, independent of runtime GX state."""
import struct
import hashlib
from j3d_native_geometry import sections,span,u16,u32,need,Unsupported

DEFAULT_STAGE=bytes.fromhex('040a0f0f00000000010005070700000000010000')

class Tables:
    def __init__(self,block):
        self.block=block;span(block,0,132)
        self.pointers=[u32(block,n) for n in range(12,132,4)]
        need(all(p==0 or 132<=p<=len(block) for p in self.pointers),'MAT3 table pointer')
    def get(self,field,index,size):
        at=u32(self.block,field);need(at>=132,'Missing used MAT3 table')
        end=min([p for p in self.pointers if p>at]+[len(self.block)])
        need(index>=0 and at+index*size+size<=end,'MAT3 table index crosses its region')
        return span(self.block,at+index*size,size)

def materials(data,*,declaration=None):
    if declaration is not None:
        yield from checked_declaration(data,declaration)['materials']
        return
    block=sections(data).get('MAT3');need(block is not None,'Missing MAT3')
    tables=Tables(block);count=u16(block,8);need(count<=4096,'Material count bound')
    for identifier in range(count):
        try:
            record=tables.get(12,u16(tables.get(16,identifier,2),0),332)
            stageIndex=record[4]
            if stageIndex==255:raise Unsupported('Unspecified initial TEV stage count')
            stageCount=tables.get(88,stageIndex,1)[0];need(1<=stageCount<=16,'Source TEV stage count')
            indirect=u32(block,24);indirect_info={'enabled':False,'stage_count':0,'stages':[]}
            if indirect:
                raw=tables.get(24,identifier,312);need(raw[0]<2 and raw[1]<=4,'Source indirect header')
                settings=[tuple(raw[120+n*12:129+n*12]) for n in range(stageCount)]
                indirect_info={'enabled':bool(raw[0]),'stage_count':raw[1],'stages':settings}
                if raw[0]:
                    for n,setting in enumerate(settings):
                        # Format/bias without a matrix cannot perturb coordinates.
                        # Wrapping, accumulated offsets and bump alpha still can.
                        if any(setting[k] for k in (3,4,5,6,8)):
                            raise Unsupported(f'Active indirect TEV stage {n}: count={raw[1]}, settings={setting}; coordinate/bump lowering unavailable')
            regs=[];konst=[]
            for n in range(4):
                index=u16(record,220+n*2)
                regs.append((0,0,0,0) if index==65535 else struct.unpack('>4h',tables.get(80,index,8)))
                index=u16(record,148+n*2);konst.append((255,255,255,255) if index==65535 else tuple(tables.get(84,index,4)))
            # Source helpers load register0..2 from these indices. PREV starts
            # from the fourth slot only when explicitly referenced before write.
            stages=[]
            for n in range(stageCount):
                index=u16(record,228+n*2);raw=DEFAULT_STAGE if index==65535 else tables.get(92,index,20)
                index=u16(record,188+n*2);order=bytes((255,255,255,255)) if index==65535 else tables.get(76,index,4)
                index=u16(record,260+n*2);swap=bytes(4) if index==65535 else tables.get(96,index,4)
                swizzles=[]
                for selected in [swap[0],swap[1]]:
                    need(selected<4,'Source swap selector');table=u16(record,292+selected*2)
                    swizzles.append((0,1,2,3) if table==65535 else tuple(tables.get(100,table,4)))
                need(all(c<4 for s in swizzles for c in s),'Source swap channel')
                stages.append({'rgb':tuple(raw[1:5]),'rgb_op':tuple(raw[5:10]),'alpha':tuple(raw[10:14]),'alpha_op':tuple(raw[14:19]),
                    'coordinate':order[0],'texture_slot':order[1],'raster':order[2],'raster_swap':swizzles[0],'texture_swap':swizzles[1],
                    'konst_color_selector':record[156+n],'konst_alpha_selector':record[172+n]})
            textures=[]
            for n in range(8):
                index=u16(record,132+n*2);textures.append(None if index==65535 else u16(tables.get(72,index,2),0))
            states={}
            for label,field,index,size in [('blend',112,u16(record,328),4),('alpha',108,u16(record,326),8),('fog',104,u16(record,324),44),('depth',116,record[6],4)]:
                if index==(255 if label=='depth' else 65535):raise Unsupported('Default '+label+' state needs explicit source default compilation')
                states[label]=tuple(tables.get(field,index,size))
            cull=record[1]
            if cull==255:raise Unsupported('Unspecified source cull state')
            states['cull']=u32(tables.get(28,cull,4),0)
            states['early_depth']=0 if record[5]==255 else tables.get(120,record[5],1)[0]
            states['dither']=1 if record[7]==255 else tables.get(124,record[7],1)[0]
            need(states['early_depth']<2 and states['dither']<2,'Source early-depth/dither flag')
            generatorIndex=record[3];generatorCount=0 if generatorIndex==255 else tables.get(52,generatorIndex,1)[0]
            need(generatorCount<=8,'Source texgen count');generators=[]
            for n in range(generatorCount):
                index=u16(record,40+n*2);generators.append((1,4+n,60,0) if index==65535 else tuple(tables.get(56,index,4)))
            channelIndex=record[2];channelCount=0 if channelIndex==255 else tables.get(36,channelIndex,1)[0]
            need(channelCount<=2,'Source color channel count');channels=[]
            for n in range(4):
                index=u16(record,12+n*2)
                channels.append((0,0,0,2,2,0,255,255) if index==65535 else tuple(tables.get(40,index,8)))
            yield {'id':identifier+1,'source_material':identifier,'stages':stages,'registers':regs,'konst':konst,'textures':textures,'states':states,'generators':generators,
                'channel_count':channelCount,'channels':channels,'indirect':indirect_info,
                'texture_slots':list(enumerate(textures)),'generator_mappings':list(enumerate(generators))}
        except (ValueError,struct.error) as error:
            yield {'id':identifier+1,'source_material':identifier,'error':str(error)}

def input_declaration(data,external_images=None,*,loader_recipe=None,warp_resource=None,warp_selector=None):
    """One ordered image/material input shared by every native-bank producer.

    The caller authenticates exact archive selectors. This only models the
    source-backed loaderBasicBmd/addWarpMaterial -> initModel/offWarpMaterial
    transition; original geometry and source bytes are never rewritten.
    Image entries retain source indices; a framebuffer-color role has no BTI
    and carries its role in the entry metadata instead of source pixels.
    """
    from j3d_native_textures import source_images,normalize_image
    images=list(source_images(data,external_images));declarations=list(materials(data))
    receipt={'loader_recipe':loader_recipe,'source_sha256':hashlib.sha256(data).hexdigest(),
             'original_image_count':len(images),'material_deltas':[]}
    if loader_recipe is None:
        need(warp_resource is None and warp_selector is None,'Warp resource without loader recipe')
    else:
        need(loader_recipe=='bmwr-warp-off','Unsupported readied loader recipe')
        need(isinstance(warp_resource,bytes) and isinstance(warp_selector,dict),'Missing readied warp selector/resource')
        need(32<=len(warp_resource)<=16<<20,'Readied warp resource extent')
        need(hashlib.sha256(warp_resource).hexdigest()==warp_selector.get('source_sha256'),'Stale readied warp resource hash')
        bti,metadata=normalize_image(warp_resource)
        image=len(images)
        appended={'selector':dict(warp_selector),'texture_index':image,'native_image_id':image+1,
                  'resource_sha256':hashlib.sha256(warp_resource).hexdigest(),
                  'producer':'dRes_info_c::loaderBasicBmd/addWarpMaterial'}
        images.append((image,bti,{**metadata,'readied_append':appended,
                                 'external_resource_sha256':appended['resource_sha256']}))
        receipt.update(appended_warp=appended,
                       source_chain=['dRes_info_c::loaderBasicBmd','addWarpMaterial',
                                     'daAlink_c::initModel','dRes_info_c::offWarpMaterial'],
                       geometry_policy='Original geometry; inactive warp DL/VCD expansion not emitted',
                       runtime_channel_producer={
                           'channel':'rgb0','input':'g_env_light.light_mask_type',
                           'operation':'source light mask AND selected mask; other values leave source mask unchanged',
                           'masks':{1:4,2:12,3:13,4:15,5:31,6:63,7:127},
                           'snapshot_selected':False},
                       sampler_policy='Original construction sampler data; later eye mip LOD edits are runtime sampler updates, not image identity')
        for material in declarations:
            need('error' not in material,'Readied source material refused: '+str(material.get('error')))
            generators=len(material['generators']);stages=len(material['stages'])
            need(generators<4 and stages<4,'BMWR warp loader requires fewer than four active texgens/TEV stages')
            need(material['textures'][3] in (None,65535),'BMWR warp loader requires unused texture slot 3')
            states=material['states'];alpha=states['alpha']
            delta={'id':material['id'],'alpha':{'original':alpha,'readied':(4,128,0,3,255)+alpha[5:]},
                   'early_depth':{'original':states['early_depth'],'readied':0},
                   'active_stage_count':stages,'active_generator_count':generators,
                   'inactive_warp':{
                       'active':False,'texture_slot':3,'texture_index':image,'native_image_id':image+1,
                       'generator_index':generators,'generator':(0,0,30+generators*3,0),
                       'texture_matrix_source':'Source/src/d/d_resorce.cpp::l_texMtxInfo',
                       'stage_index':stages,'order':(generators,3,255,0),
                       'stage_info_hex':'050f08000f0000000100070400070000000100'},
                   'preserved_states':['depth','blend','cull','fog','dither'],
                   'runtime_channel_producer':receipt['runtime_channel_producer']}
            states['alpha']=delta['alpha']['readied'];states['early_depth']=0
            material['readied_delta']=delta
            receipt['material_deltas'].append(delta)
    receipt['image_count']=len(images)
    receipt['mathematical_helper_first_native_id']=len(images)+1
    return {'source_sha256':receipt['source_sha256'],'images':tuple(images),
            'materials':tuple(declarations),'receipt':receipt}

def checked_declaration(data,declaration):
    need(isinstance(declaration,dict) and declaration.get('source_sha256')==hashlib.sha256(data).hexdigest(),
         'Readied declaration source hash mismatch')
    from j3d_native_textures import texture_name,texture_role
    block=sections(data)['TEX1'];count=u16(block,8);images=declaration['images']
    for index in range(max(count,len(images))):
        name=texture_name(block,index) if index<count else None
        role=texture_role(name)
        actual,bti,metadata=images[index] if index<len(images) else (None,None,{})
        declared=metadata.get('role')
        if role is None and declared is None:continue
        need(role is not None and role==declared and actual==index and bti is None and
             metadata=={'role':role,'source_name':name.decode('ascii'),'width':0,'height':0,'mip_levels':1},
             'Readied declaration source image role mismatch')
    return declaration
