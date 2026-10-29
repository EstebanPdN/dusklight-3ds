"""DEFAULT-OFF Mac asset experiment: native ETC with explicit byte-quality gates.

No runtime conversion and no automatic content admission. A bank hash and image
ID set are required; a native material audit must still precede game activation.
"""
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import zlib
from native_texture_pack import compile_images,extent,FORMATS,MAX_FILE
import texture_lab_formats as codec

def images_from_pack(data):
    if not isinstance(data,bytes) or not 32<=len(data)<=MAX_FILE:raise ValueError('Native image bank extent')
    magic,version,size,count,table,tableBytes,checksum=struct.unpack('<8s6I',data[:32])
    if magic!=b'DUSKTX01' or version!=1 or size!=len(data) or not 1<=count<=64 or table!=32 or tableBytes!=count*32 or len(data)<32+tableBytes:raise ValueError('Native image bank header')
    if zlib.crc32(data[:28]+data[32:32+tableBytes])!=checksum:raise ValueError('Native image directory CRC')
    images=[];names=list(FORMATS)
    for n in range(count):
        identifier,width,height,kind,levels,offset,size,checksum=struct.unpack_from('<8I',data,32+n*32)
        if kind>=len(names):raise ValueError('Native image format')
        sizes=extent(width,height,levels,names[kind])
        if sum(sizes)!=size or offset>len(data) or size>len(data)-offset:raise ValueError('Native image payload extent')
        payload=data[offset:offset+size]
        if zlib.crc32(payload)!=checksum:raise ValueError('Native image payload CRC')
        chunks=[];at=0
        for size in sizes:chunks.append(payload[at:at+size]);at+=size
        images.append({'id':identifier,'width':width,'height':height,'format':names[kind],'levels':chunks})
    # Canonical rewrite also checks sorted IDs, overlaps, gaps, padding and
    # trailing bytes, rather than trusting mutually inconsistent directories.
    if compile_images(images)!=data:raise ValueError('Noncanonical native image pack')
    return images

def untile_rgba8(data,width,height):
    if len(data)!=width*height*4:raise ValueError('RGBA8 native mip extent')
    out=bytearray(len(data))
    for y in range(height):
        for x in range(width):
            at=((y//8*(width//8)+x//8)*64+codec.morton(x,y))*4
            out[(y*width+x)*4:(y*width+x+1)*4]=data[at:at+4][::-1]
    return bytes(out)

def compile_profile(data,encoder,scratch,allowed_ids,expected_sha256,*,max_rgb_error=24,max_rgb_mse=16):
    digest=hashlib.sha256(data).hexdigest()
    if digest!=expected_sha256:raise ValueError('Image admission bank hash mismatch')
    if type(max_rgb_error) is not int or not 0<=max_rgb_error<=255 or type(max_rgb_mse) not in (int,float) or not math.isfinite(max_rgb_mse) or not 0<=max_rgb_mse<=65535:raise ValueError('Compression quality policy')
    images=images_from_pack(data)
    if not isinstance(allowed_ids,set) or any(type(x) is not int for x in allowed_ids) or not allowed_ids.issubset({i['id'] for i in images}):raise ValueError('Unknown native image admission ID')
    compiled=[];rows=[];encoder_used=False
    for image in images:
        output=image;identifier=image['id'];row={'id':identifier,'source_format':image['format'],'source_bytes':sum(map(len,image['levels'])),'status':'not_admitted'}
        if identifier in allowed_ids and image['format'] in ('rgba8','rgb565'):
            reference=[(untile_rgba8(level,image['width']>>n,image['height']>>n) if image['format']=='rgba8' else
                codec.decode16(level,image['width']>>n,image['height']>>n,'RGB565')) for n,level in enumerate(image['levels'])]
            alpha={a for rgba in reference for a in rgba[3::4]}
            if any(a%17 for a in alpha):row['status']='retained_alpha_precision'
            else:
                fmt='ETC1' if alpha=={255} else 'ETC1A4';levels=[];metrics=[]
                work=scratch/f'image-{identifier:08x}';work.mkdir(mode=0o700,exist_ok=False)
                encoder_used=True
                for n,rgba in enumerate(reference):
                    w,h=image['width']>>n,image['height']>>n
                    encoded,_=codec.encode_etc(rgba,w,h,fmt,encoder,work)
                    decoded=codec.decode_etc(encoded,w,h,fmt)
                    if decoded[3::4]!=rgba[3::4]:raise ValueError('Compressed native alpha changed')
                    errors=[int(a)-int(b) for i,(a,b) in enumerate(zip(rgba,decoded)) if i%4!=3]
                    metrics.append({'max_rgb_error':max(map(abs,errors)),'rgb_mse':sum(e*e for e in errors)/len(errors),'alpha_exact':True})
                    levels.append(encoded)
                row['metrics']=metrics
                if all(m['max_rgb_error']<=max_rgb_error and m['rgb_mse']<=max_rgb_mse for m in metrics):
                    output={**image,'format':fmt.lower(),'levels':levels};row['status']='admitted_experiment'
                else:row['status']='retained_rgb_quality'
        compiled.append(output);row.update(native_format=output['format'],native_bytes=sum(map(len,output['levels'])));rows.append(row)
    packed=compile_images(compiled)
    return packed,{'source_sha256':digest,'native_sha256':hashlib.sha256(packed).hexdigest(),'encoder_sha256':hashlib.sha256(Path(encoder).read_bytes()).hexdigest() if encoder_used else None,
        'images':rows,'allowed_ids':sorted(allowed_ids),'max_rgb_error':max_rgb_error,'max_rgb_mse':max_rgb_mse,
        'image_storage_only':True,'experimental':True,'default_enabled':False,'material_roles_validated':False,'rendering_ready':False,
        'scope':'Stored-texel alpha exact and explicit RGB error bounds. No filtering, source material comparison, rendering, physical or performance acceptance.'}

def build_encoder(root,upstream,output):
    # Only rg_etc1.cpp/header are linked, under their embedded zlib notice.
    # tex3ds application code is not copied, linked or included in the game.
    sources=[root/'Tools/3ds/texture_lab_etc_host.cpp',upstream/'source/rg_etc1.cpp',upstream/'include/rg_etc1.h']
    command=['clang++','-std=c++11','-O2','-DNDEBUG','-I'+str(upstream/'include'),*map(str,sources[:2]),'-o',str(output)]
    result=subprocess.run(command,text=True,capture_output=True,timeout=90);output.with_suffix('.build.log').write_text(result.stdout+result.stderr);result.check_returncode()
    output.with_suffix('.build.json').write_text(json.dumps({'command':command,'source_sha256':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
        'binary_sha256':hashlib.sha256(output.read_bytes()).hexdigest(),'license':'rg_etc1 zlib notice in the pinned upstream header'},indent=2)+'\n')
