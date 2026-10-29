#!/usr/bin/env python3
"""Mac-only TEX1 image preparation. Samplers/materials/texgen remain separate."""
import hashlib
from pathlib import Path
import struct
import subprocess
from j3d_native_geometry import sections,span,u16,u32,need,Unsupported
from native_texture_pack import compile_images,extent,MAX_FILE

BLOCKS={0:(8,8,32),1:(8,4,32),2:(8,4,32),3:(4,4,32),4:(4,4,32),5:(4,4,32),6:(4,4,64),8:(8,8,32),9:(8,4,32),10:(4,4,32),14:(8,8,32)}

def archive_key(name):
    # JKRArchive::CArcName lowercases source bytes before archive lookup.
    # ASCII casing is explicit; Unicode casefold would merge unrelated names.
    return ''.join(chr(ord(c)+32) if 'A'<=c<='Z' else c for c in name)

def source_images(data,external_images=None):
    external={}
    if external_images is not None:
        need(isinstance(external_images,dict) and len(external_images)<=4096,'External resource table bound')
        for name,resource in external_images.items():
            need(isinstance(name,str) and 0<len(name)<=512 and archive_key(name) not in external,'Ambiguous/invalid external resource name')
            external[archive_key(name)]=resource
    block=sections(data).get('TEX1')
    need(block is not None,'Missing TEX1 block');span(block,0,20)
    count=u16(block,8);base=u32(block,12)
    need(count<=4096,'TEX1 texture header count')
    if not count:return # Valid source model with no texture resources.
    need(base>=20,'TEX1 texture header offset');span(block,base,count*32)
    for index in range(count):
        at=base+index*32;owner=block;minimum=base+count*32
        header=bytearray(span(owner,at,32));link={}
        if u32(header,28)==0:
            names=u32(block,16);label=f'index {index}'
            if names:
                need(names>=20 and u16(block,names)==count,'TEX1 name table count/offset')
                nameAt=names+u16(block,names+4+index*4+2)
                raw=span(block,nameAt,min(512,len(block)-nameAt));end=raw.find(b'\0');need(end>=0,'TEX1 name terminator')
                label=raw[:end].decode('shift_jis','backslashreplace')
            if archive_key(label) not in external:raise Unsupported('Named external texture requires native resource binding: '+label)
            resource=external[archive_key(label)]
            need(isinstance(resource,bytes) and 32<=len(resource)<=16<<20,'External texture resource extent')
            link={'external_name':label,'external_resource_sha256':hashlib.sha256(resource).hexdigest(),'external_placeholder_sha256':hashlib.sha256(header).hexdigest()}
            owner=resource;at=0;minimum=32;header=bytearray(resource[:32])
            need(u32(header,28)!=0,'Unresolved external texture resource')
        kind=header[0]
        if kind not in BLOCKS:raise Unsupported('Texture encoding '+str(kind))
        width,height=u16(header,2),u16(header,4);levels=header[24]
        need(0<width<=1024 and 0<height<=1024 and 1<=levels<=8,'TEX1 image/mip domain')
        bw,bh,bs=BLOCKS[kind];sizes=[((max(1,width>>n)+bw-1)//bw)*((max(1,height>>n)+bh-1)//bh)*bs for n in range(levels)]
        imageOffset=at+struct.unpack_from('>i',header,28)[0]
        need(imageOffset>=minimum,'Image overlaps source headers')
        payload=span(owner,imageOffset,sum(sizes));palette=b''
        if kind in (8,9,10):
            paletteOffset=at+struct.unpack_from('>i',header,12)[0]
            need(paletteOffset>=minimum,'Palette overlaps source headers')
            palette=span(owner,paletteOffset,2*u16(header,10))
            colors=u16(header,10);cursor=0
            # A missing palette entry is an invalid asset, never a successful
            # transparent fallback inherited from the diagnostic decoder.
            for level,size in enumerate(sizes):
                w,h=max(1,width>>level),max(1,height>>level);wide=(w+bw-1)//bw
                for y in range(h):
                    for x in range(w):
                        cell=cursor+((y//bh)*wide+x//bw)*bs
                        pixel=(y%bh)*bw+x%bw
                        value=(payload[cell+pixel//2]>>(4 if pixel%2==0 else 0))&15 if kind==8 else payload[cell+pixel] if kind==9 else u16(payload,cell+pixel*2)&16383
                        need(value<colors,'Selected palette index outside declared entries')
                cursor+=size
        struct.pack_into('>I',header,12,32 if palette else 0)
        struct.pack_into('>I',header,28,32+len(palette))
        yield index,bytes(header)+palette+payload,{**link,'source_format':kind,'width':width,'height':height,'mip_levels':levels,
            'source_mip_bytes':sum(sizes),'source_header_sha256':hashlib.sha256(span(owner,at,32)).hexdigest(),
            'source_sampler_bytes_hex':span(owner,at+6,2).hex()+span(owner,at+16,12).hex()}

def convert(data,decoder,scratch,external_images=None):
    """All image storage must convert; refuses the whole bank on any failure."""
    images=[];rows=[];nativeBytes=32
    for index,bti,metadata in source_images(data,external_images):
        if index>=64:raise Unsupported('TEX1 bank exceeds 64 native resident image slots')
        # Do not drop small mip tails, pad logical dimensions, or silently
        # rebase UVs. These require a future explicitly converted material.
        sizes=extent(metadata['width'],metadata['height'],metadata['mip_levels'],'rgb565' if metadata['source_format']==4 else 'rgba8')
        nativeBytes+=32+sum(sizes)
        need(nativeBytes<=MAX_FILE,'Converted image bank exceeds 64 MiB')
        source=scratch/f'image-{index:04}.bti';target=scratch/f'image-{index:04}.native'
        with source.open('xb') as file:file.write(bti)
        result=subprocess.run([str(decoder),str(source),str(target)],text=True,capture_output=True,timeout=15)
        if result.returncode:raise Unsupported(f'Offline image {index} decoder refused ({result.returncode})')
        raw=target.read_bytes();need(len(raw)>=24,'Native compiler intermediate header')
        magic,width,height,kind,levels,size=struct.unpack('<6I',raw[:24])
        need(magic==0x31474d49 and width==metadata['width'] and height==metadata['height'] and levels==metadata['mip_levels'] and kind<=1 and size==len(raw)-24,'Native compiler intermediate contract')
        fmt='rgb565' if kind else 'rgba8';sizes=extent(width,height,levels,fmt);need(sum(sizes)==size,'Native compiler mip bytes')
        chunks=[];cursor=24
        for size in sizes:chunks.append(raw[cursor:cursor+size]);cursor+=size
        images.append({'id':index+1,'width':width,'height':height,'format':fmt,'levels':chunks})
        rows.append({'id':index+1,**metadata,'normalized_bti_sha256':hashlib.sha256(bti).hexdigest(),
            'native_format':fmt,'native_mip_bytes':sum(sizes),'native_sha256':hashlib.sha256(raw[24:]).hexdigest()})
    packed=compile_images(images) if images else None
    return packed,{'source_sha256':hashlib.sha256(data).hexdigest(),'native_sha256':hashlib.sha256(packed).hexdigest() if packed else None,'images':rows,
        'native_image_pack_needed':bool(images),
        'image_storage_converted':True,'samplers_converted':False,'materials_converted':False,'texgen_converted':False,'rendering_ready':False,
        'scope':'Original bounded decoder reused only by the Mac asset compiler. Native output contains tiled images, never source texture headers or commands.'}

def build_decoder(root,output):
    command=['clang++','-std=c++20','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer',
        str(root/'Tools/3ds/compile_native_texture.cpp'),str(root/'Source/platforms/3ds/runtime/texture_gx.cpp'),'-o',str(output)]
    result=subprocess.run(command,text=True,capture_output=True,timeout=90)
    output.with_suffix('.build.log').write_text(result.stdout+result.stderr);result.check_returncode();return command
