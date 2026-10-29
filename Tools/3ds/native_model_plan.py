"""Mac native model-plan writer; no original hierarchy/source enums in output."""
import struct,zlib
from native_material_pack import integer,enum
WRAPS=['clamp','repeat','mirror'];FILTERS=['nearest','linear']
REQUIREMENTS={'poses':1,'texgen':2,'lighting':4,'fog':8,'ordering':16,'colors':32}
MAX_FILE=48+64*64+64*48

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
    if not isinstance(parts,list) or not 1<=len(parts)<=64 or not isinstance(uniforms,list) or not 1<=len(uniforms)<=64:raise ValueError('Native model-plan counts')
    if not isinstance(requirements,list) or len(set(requirements))!=len(requirements) or any(r not in REQUIREMENTS for r in requirements):raise ValueError('Native model-plan input requirements')
    if any(not isinstance(u,dict) or set(u)!={'material','anchor','palette'} for u in uniforms):raise ValueError('Native uniform slot fields')
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
        if set(slot)!={'material','anchor','palette'}:raise ValueError('Native uniform slot fields')
        material=integer(slot['material'],0xffffffff);anchor=integer(slot['anchor'],4096);palette=slot['palette']
        if not material or not anchor or not isinstance(palette,list) or not 1<=len(palette)<=8:raise ValueError('Native uniform slot domain')
        palette=[integer(p,65535) for p in palette]
        if not all(palette):raise ValueError('Zero native pose-matrix ID')
        slots.extend(struct.pack('<12I',material,anchor,len(palette),*(palette+[0]*(8-len(palette))),0))
    size=48+len(prefix)+len(slots);mask=sum(REQUIREMENTS[r] for r in requirements)
    header=struct.pack('<8s9I',b'DUSKML01',1,size,len(parts),len(uniforms),mask,48,48+len(prefix),len(slots),0)
    payload=prefix+slots
    return header+struct.pack('<I',zlib.crc32(header+payload))+payload
