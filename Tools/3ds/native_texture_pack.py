"""Offline native image pack writer. No source-console formats in the output."""
import struct
import zlib

FORMATS={'rgba8':0,'rgb565':1,'etc1':2,'etc1a4':3}
MAX_FILE=64<<20

def extent(width,height,levels,kind):
    if any(type(x) is not int for x in (width,height,levels)):raise ValueError('Image dimensions/levels must be integers')
    if any(x<8 or x>1024 or x&(x-1) for x in (width,height)) or not 1<=levels<=8 or kind not in FORMATS:raise ValueError('Native image layout')
    sizes=[]
    for n in range(levels):
        w,h=width>>n,height>>n
        if min(w,h)<8:raise ValueError('Native mip below 8 texels')
        sizes.append(w*h*4 if kind=='rgba8' else w*h*2 if kind=='rgb565' else w*h//2 if kind=='etc1' else w*h)
    return sizes

def tile_rgba8(rgba,width,height):
    extent(width,height,1,'rgba8')
    if len(rgba)!=width*height*4:raise ValueError('RGBA source extent')
    result=bytearray(len(rgba))
    for y in range(height):
        for x in range(width):
            morton=sum(((x>>b)&1)<<(2*b)|((y>>b)&1)<<(2*b+1) for b in range(3))
            at=((y//8*(width//8)+x//8)*64+morton)*4
            pixel=rgba[(y*width+x)*4:(y*width+x+1)*4]
            result[at:at+4]=pixel[::-1] # Native little-endian ABGR bytes.
    return bytes(result)

def compile_images(images):
    """Each image has id,width,height,format,levels (already tiled bytes)."""
    if not isinstance(images,list) or not 1<=len(images)<=64:raise ValueError('Native image count')
    rows=[]
    for image in images:
        if set(image)!={'id','width','height','format','levels'}:raise ValueError('Native image fields')
        identifier=image['id'];levels=image['levels']
        if type(identifier) is not int or not 1<=identifier<=0xffffffff:raise ValueError('Native image ID')
        if not isinstance(levels,list):raise ValueError('Native image levels')
        sizes=extent(image['width'],image['height'],len(levels),image['format'])
        if any(not isinstance(data,bytes) or len(data)!=size for data,size in zip(levels,sizes)):raise ValueError('Native mip payload extent')
        rows.append((identifier,image,b''.join(levels)))
    rows.sort(key=lambda x:x[0])
    if len({x[0] for x in rows})!=len(rows):raise ValueError('Duplicate native image ID')
    cursor=32+len(rows)*32;table=bytearray();payload=bytearray()
    for identifier,image,data in rows:
        end=(cursor+len(data)+15)&~15
        if end>MAX_FILE:raise ValueError('Native image pack exceeds 64 MiB')
        table.extend(struct.pack('<8I',identifier,image['width'],image['height'],FORMATS[image['format']],len(image['levels']),cursor,len(data),zlib.crc32(data)))
        payload.extend(data);payload.extend(bytes(end-cursor-len(data)));cursor=end
    prefix=struct.pack('<8s5I',b'DUSKTX01',1,cursor,len(rows),32,len(table))
    return prefix+struct.pack('<I',zlib.crc32(prefix+table))+table+payload
