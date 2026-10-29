"""Mac-only native fragment-state writer. Never emits source command packets."""
import math
import struct
import zlib

SOURCES=['primary','texture0','texture1','texture2','constant','previous','buffer']
FUNCTIONS=['replace','modulate','add','addSigned','interpolate','subtract','dot3','multiplyAdd','addMultiply']
RGB_OPS=['color','oneMinusColor','alpha','oneMinusAlpha','red','oneMinusRed','green','oneMinusGreen','blue','oneMinusBlue']
ALPHA_OPS=['alpha','oneMinusAlpha','red','oneMinusRed','green','oneMinusGreen','blue','oneMinusBlue']
COMPARE=['never','always','equal','notEqual','less','lessEqual','greater','greaterEqual']
BLENDS=['opaque','alpha','additive','custom'];CULL=['none','front','back']
EQUATIONS=['add','subtract','reverseSubtract','minimum','maximum']
FACTORS=['zero','one','sourceColor','oneMinusSourceColor','destinationColor','oneMinusDestinationColor','sourceAlpha','oneMinusSourceAlpha','destinationAlpha','oneMinusDestinationAlpha']
MAX_FILE=32+64*(32+1248)

def enum(value,values):
    if type(value) is not str or value not in values:raise ValueError('Unknown native material enum')
    return values.index(value)

def integer(value,limit):
    if type(value) is not int or not 0<=value<=limit:raise ValueError('Native material integer domain')
    return value

def boolean(value):
    if type(value) is not bool:raise ValueError('Native material boolean domain')
    return value

def stage_bytes(stage,mask,index):
    allowed={'rgb','alpha','rgb_operand','alpha_operand','rgb_function','alpha_function','rgb_scale','alpha_scale','constant','constant_bindings'}
    if not isinstance(stage,dict) or set(stage)-allowed:raise ValueError('Native stage fields')
    result=bytearray(32)
    for field,offset,values,default in [('rgb',0,SOURCES,['primary']*3),('alpha',3,SOURCES,['primary']*3),('rgb_operand',6,RGB_OPS,['color']*3),('alpha_operand',9,ALPHA_OPS,['alpha']*3)]:
        items=stage.get(field,default)
        if not isinstance(items,list) or len(items)!=3:raise ValueError('Native stage operand count')
        result[offset:offset+3]=bytes(enum(x,values) for x in items)
    for lane,field in enumerate(['rgb_function','alpha_function']):
        function=enum(stage.get(field,'replace'),FUNCTIONS)
        if lane and function==6:raise ValueError('Alpha dot3 unsupported')
        result[12+lane]=function
        arity=1 if function==0 else 3 if function in (4,7,8) else 2
        for source in result[lane*3:lane*3+arity]:
            if index==0 and source==5:raise ValueError('First stage previous undefined')
            if 1<=source<=3 and not mask&(1<<(source-1)):raise ValueError('Native stage missing texture binding')
    result[14]=integer(stage.get('rgb_scale',0),2);result[15]=integer(stage.get('alpha_scale',0),2)
    struct.pack_into('<I',result,16,integer(stage.get('constant',0xffffffff),0xffffffff))
    bindings=stage.get('constant_bindings',[0]*4)
    if not isinstance(bindings,list) or len(bindings)!=4:raise ValueError('Native constant binding count')
    result[20:24]=bytes(integer(x,32) for x in bindings)
    return bytes(result)

def material_bytes(record):
    allowed={'id','images','stages','blend','custom_blend','cull','depth','alpha','depth_test','depth_write','color_write','alpha_write','alpha_reference','buffer_rgb_mask','buffer_alpha_mask','buffer_color','fog_color','fog_values','fog_depth_flip','requires_fog','initial_snapshot_only'}
    if not isinstance(record,dict) or set(record)-allowed:raise ValueError('Native material fields')
    identifier=integer(record['id'],0xffffffff)
    if not identifier:raise ValueError('Zero native material ID')
    images=record.get('images',[0,0,0]);stages=record['stages']
    if not isinstance(images,list) or len(images)!=3 or not isinstance(stages,list) or not 1<=len(stages)<=6:raise ValueError('Native material binding/stage count')
    images=[integer(x,0xffffffff) for x in images];mask=sum((1<<n) for n,x in enumerate(images) if x)
    result=bytearray(32);result[:6]=bytes([len(stages),mask,enum(record.get('blend','opaque'),BLENDS),enum(record.get('cull','back'),CULL),enum(record.get('depth','greaterEqual'),COMPARE),enum(record.get('alpha','always'),COMPARE)])
    for n,(field,default) in enumerate([('depth_test',True),('depth_write',True),('color_write',True),('alpha_write',True)]):
        if boolean(record.get(field,default)):result[6]|=1<<n
    fog=record.get('fog_values');result[6]|=16 if fog is not None else 0
    if boolean(record.get('fog_depth_flip',False)):result[6]|=32
    result[7]=integer(record.get('alpha_reference',0),255);result[8]=integer(record.get('buffer_rgb_mask',0),15);result[9]=integer(record.get('buffer_alpha_mask',0),15)
    struct.pack_into('<II',result,12,integer(record.get('buffer_color',0),0xffffffff),integer(record.get('fog_color',0),0xffffffff))
    blend=record.get('custom_blend',{})
    fields=['color','alpha','color_source','color_destination','alpha_source','alpha_destination']
    if not isinstance(blend,dict) or set(blend)-set(fields):raise ValueError('Native blend fields')
    for n,field in enumerate(fields):result[20+n]=enum(blend.get(field,'add' if n<2 else 'one' if n%2==0 else 'zero'),EQUATIONS if n<2 else FACTORS)
    for n,stage in enumerate(stages):result.extend(stage_bytes(stage,mask,n))
    if fog is not None:
        if not isinstance(fog,list) or len(fog)!=256 or any(type(x) not in (int,float) or not math.isfinite(x) or not 0<=x<=1 or (x and x<2**-62) for x in fog):raise ValueError('Native fog LUT domain')
        result.extend(struct.pack('<256f',*fog))
    requirements=int(boolean(record.get('requires_fog',False)))|int(boolean(record.get('initial_snapshot_only',False)))<<1
    return identifier,images,bytes(result),requirements

def compile_materials(records):
    if not isinstance(records,list) or not 1<=len(records)<=64:raise ValueError('Native material bank count')
    rows=sorted(material_bytes(m) for m in records)
    if len({r[0] for r in rows})!=len(rows):raise ValueError('Duplicate native material ID')
    cursor=32+len(rows)*32;table=bytearray();payload=bytearray()
    for identifier,images,data,requirements in rows:
        table.extend(struct.pack('<8I',identifier,cursor,len(data),zlib.crc32(data),*images,requirements));payload.extend(data);cursor+=len(data)
    if cursor>MAX_FILE:raise ValueError('Native material bank size')
    version=2 if any(any(s.get('constant_bindings',[])) for m in records for s in m['stages']) else 1
    prefix=struct.pack('<8s5I',b'DUSKMT01',version,cursor,len(rows),32,len(table))
    return prefix+struct.pack('<I',zlib.crc32(prefix+table))+table+payload
