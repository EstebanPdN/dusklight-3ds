"""Native model-plan v4 writer; earlier binaries are deliberately unsupported.

The 48-byte header and 64-byte part records precede 80-byte uniform records.
Uniform offsets: material/anchor/count at 0/4/8, four palette u32s at 12,
texture_slots/generators (3*u8 each) at 28/31, source_attributes u32 at 36,
channel_read_mask u8 at 40, seven RGBA lane masks each for parameter reads,
wraps and signed reads at 41/48/55, source_shape/billboard_mode u32s at 64/68.
Bytes 72/73 contain source TEV-stage/texgen counts; 0/0 means no source proof.
Bytes 34..35, 62..63 and 74..79 are zero; all integers are little-endian.
Wrap/signed lanes are disjoint subsets of reads, only for registers 0..2.
Library-authored bindings default to texture sentinels, all channel reads,
zero parameter masks, source_attributes, source_shape and billboard_mode.
Source-derived assemblers must supply actual metadata, not these defaults.
"""
import struct,zlib
from native_material_pack import integer,enum
from native_mesh_pack import MAX_MODEL_PARTS
WRAPS=['clamp','repeat','mirror'];FILTERS=['nearest','linear']
REQUIREMENTS={'poses':1,'texgen':2,'lighting':4,'fog':8,'ordering':16,'colors':32}
MAX_FILE=48+MAX_MODEL_PARTS*(64+80)

def sampler_bytes(s):
    fields={'wrap_s','wrap_t','magnify','minify','mipmap','first_level','last_level','lod_bias'}
    if not isinstance(s,dict) or set(s)-fields:raise ValueError('Native sampler fields')
    a=bytearray(12)
    for n,field in enumerate(['wrap_s','wrap_t','magnify','minify','mipmap']):a[n]=enum(s.get(field,'clamp' if n<2 else 'linear' if n<4 else 'nearest'),WRAPS if n<2 else FILTERS)
    a[5]=integer(s.get('first_level',0),7);a[6]=integer(s.get('last_level',0),7)
    bias=s.get('lod_bias',0)
    if type(bias) is not int or not -4096<=bias<=4095 or a[5]>a[6]:raise ValueError('Native sampler interval/bias')
    struct.pack_into('<h',a,8,bias);return bytes(a)

def compile_plan(plan):
    if not isinstance(plan,dict) or set(plan)!={'schema','parts','uniforms','requirements'} or plan['schema']!='dusklight-native-model-plan/1':raise ValueError('Native model-plan schema')
    parts,uniforms,requirements=plan['parts'],plan['uniforms'],plan['requirements']
    if not isinstance(parts,list) or not 1<=len(parts)<=MAX_MODEL_PARTS or not isinstance(uniforms,list) or not 1<=len(uniforms)<=MAX_MODEL_PARTS:raise ValueError('Native model-plan counts')
    if not isinstance(requirements,list) or any(type(r) is not str or r not in REQUIREMENTS for r in requirements) or len(set(requirements))!=len(requirements):raise ValueError('Native model-plan input requirements')
    required={'material','anchor','palette'};fields=required|{'texture_slots','texture_generators','source_attributes','channel_read_mask','parameter_read_masks','parameter_wrap_masks','parameter_signed_masks','source_shape','billboard_mode','source_stage_count','source_texgen_count'}
    if any(not isinstance(u,dict) or not required<=set(u)<=fields for u in uniforms):raise ValueError('Native uniform slot fields')
    prefix=bytearray();slots=bytearray();used=set()
    for index,p in enumerate(parts):
        if not isinstance(p,dict) or set(p)!={'mesh','material','uniform_slot','sampler_mask','samplers','pass'}:raise ValueError('Native part fields')
        mesh,material=integer(p['mesh'],0xffffffff),integer(p['material'],0xffffffff);slot=integer(p['uniform_slot'],len(uniforms)-1);mask=integer(p['sampler_mask'],7)
        if not mesh or not material or material!=uniforms[slot]['material']:raise ValueError('Native part material/slot binding')
        sampling=p['samplers']
        if not isinstance(sampling,list) or len(sampling)!=3:raise ValueError('Native part sampler count')
        prefix.extend(struct.pack('<6I',mesh,material,slot,mask,enum(p['pass'],['opaque','alpha']),index))
        prefix.extend(b''.join(sampler_bytes(s) for s in sampling));prefix.extend(bytes(4));used.add(slot)
    if used!=set(range(len(uniforms))):raise ValueError('Unreferenced native uniform slot')
    for slot in uniforms:
        material=integer(slot['material'],0xffffffff);anchor=integer(slot['anchor'],4096);palette=slot['palette']
        if not material or not anchor or not isinstance(palette,list) or not 1<=len(palette)<=4:raise ValueError('Native uniform slot domain')
        palette=[integer(p,65535) for p in palette]
        if not all(palette):raise ValueError('Zero native pose-matrix ID')
        bindings=[]
        for field in ('texture_slots','texture_generators'):
            values=slot.get(field,[255]*3)
            if not isinstance(values,list) or len(values)!=3:raise ValueError('Native texture binding count')
            values=[integer(v,255) for v in values]
            if any(v>7 and v!=255 for v in values):raise ValueError('Native texture binding domain')
            bindings.append(values)
        if any((s==255)!=(g==255) for s,g in zip(*bindings)):raise ValueError('Incomplete native texture binding')
        attributes=integer(slot.get('source_attributes',0),0xffffffff)
        channel=integer(slot.get('channel_read_mask',15),15)
        masks=[]
        for field in ('parameter_read_masks','parameter_wrap_masks','parameter_signed_masks'):
            values=slot.get(field,[0]*7)
            if not isinstance(values,list) or len(values)!=7:raise ValueError('Native parameter mask count')
            masks.append([integer(v,15) for v in values])
        for p,(read,wrap,sign) in enumerate(zip(*masks)):
            if wrap&~read or sign&~read or wrap&sign or (p>=3 and (wrap or sign)):raise ValueError('Native parameter lane masks')
        shape=integer(slot.get('source_shape',0),4095);billboard=integer(slot.get('billboard_mode',0),2)
        has_program='source_stage_count' in slot
        if has_program!=('source_texgen_count' in slot):raise ValueError('Incomplete source program counts')
        stages=integer(slot.get('source_stage_count',0),16);texgens=integer(slot.get('source_texgen_count',0),8)
        if has_program and not stages:raise ValueError('Invalid source program stage count')
        slots.extend(struct.pack('<7I6B2xI22B2xII2B6x',material,anchor,len(palette),*(palette+[0]*(4-len(palette))),
                                 *bindings[0],*bindings[1],attributes,channel,*masks[0],*masks[1],*masks[2],shape,billboard,stages,texgens))
    size=48+len(prefix)+len(slots);mask=sum(REQUIREMENTS[r] for r in requirements)
    header=struct.pack('<8s9I',b'DUSKML01',4,size,len(parts),len(uniforms),mask,48,48+len(prefix),len(slots),0)
    payload=prefix+slots
    return header+struct.pack('<I',zlib.crc32(header+payload))+payload
