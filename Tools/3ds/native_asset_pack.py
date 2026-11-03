"""Assembly and canonical original model/image identities for native banks."""
import hashlib
import struct
import zlib
from native_material_pack import integer
from native_mesh_pack import MAX_MODEL_PARTS

MAX_BYTES=16<<20

def identity(source):
    if not isinstance(source,bytes) or not 32<=len(source)<=16<<20 or source[:8] not in (b'J3D2bmd2',b'J3D2bmd3',b'J3D2bdl3',b'J3D2bdl4') or struct.unpack_from('>I',source,8)[0]!=len(source):
        raise ValueError('Original source extent/header')
    return hashlib.sha256(source).digest()

def resolve_external_images(source, resources):
    """Resolve original TEX1 names exactly as the game: ASCII-fold(name+'.bti').

    Input keys are actual archive basenames, never guessed extensionless aliases.
    The converter receives its existing label-keyed table only after resolution.
    """
    from j3d_native_geometry import sections, span, u16, u32, need, Unsupported
    from j3d_native_textures import archive_key, texture_name, texture_role
    need(isinstance(resources,dict) and len(resources)<=4096,'External resource table bound')
    original={}
    for key,value in resources.items():
        need(isinstance(key,str) and 0<len(key)<=512 and archive_key(key) not in original,
             'Ambiguous/invalid original resource key')
        original[archive_key(key)]=value
    block=sections(source).get('TEX1')
    need(block is not None,'Missing TEX1 block')
    count,base=u16(block,8),u32(block,12)
    need(count<=4096,'TEX1 texture header count');span(block,base,count*32)
    resolved={}
    for index in range(count):
        name=texture_name(block,index)
        if texture_role(name):continue
        if u32(block,base+index*32+28):continue
        names=u32(block,16)
        need(names>=20 and u16(block,names)==count and name is not None,'TEX1 name table count/offset')
        label=name.decode('shift_jis','strict')
        key=archive_key(label+'.bti')
        if key not in original:raise Unsupported('Named external texture requires native resource binding: '+label)
        resolved[archive_key(label)]=original[key]
    return resolved

def image_identity(bti):
    """DUSKIMG1: original BE header except offsets, LE lengths, pixels, palette."""
    from j3d_native_textures import BLOCKS
    if not isinstance(bti,bytes) or not 32<=len(bti)<=16<<20:raise ValueError('Image resource extent')
    header=bytearray(bti[:32]);kind=header[0]
    if kind not in BLOCKS:raise ValueError('Image format')
    width,height=struct.unpack_from('>2H',header,2);levels=header[24]
    if not 0<width<=1024 or not 0<height<=1024 or not 1<=levels<=8:raise ValueError('Image/mip domain')
    bw,bh,bs=BLOCKS[kind]
    size=sum(((max(1,width>>n)+bw-1)//bw)*((max(1,height>>n)+bh-1)//bh)*bs for n in range(levels))
    offset=struct.unpack_from('>i',header,28)[0]
    if offset<32 or offset+size>len(bti):raise ValueError('Image payload bounds')
    image=bti[offset:offset+size];palette=b''
    if kind in (8,9,10):
        entries=struct.unpack_from('>H',header,10)[0];at=struct.unpack_from('>i',header,12)[0]
        if not 0<entries<=16384 or header[9]>2 or at<32 or at+entries*2>len(bti):raise ValueError('Image palette bounds')
        palette=bti[at:at+entries*2]
    header[12:16]=bytes(4);header[28:32]=bytes(4)
    return hashlib.sha256(b'DUSKIMG1'+header+struct.pack('<2I',len(image),len(palette))+image+palette).digest()

def source_image_identity(bti, metadata):
    from native_texture_pack import FRAMEBUFFER_COLOR
    role=metadata.get('role')
    if role is None:return image_identity(bti)
    if role!=FRAMEBUFFER_COLOR or bti is not None:raise ValueError('Invalid source image role')
    return hashlib.sha256(b'DUSKROLE1framebuffer-color').digest()

def image_context(source, external_images=None, *, declaration=None):
    """DUSKCTX1: LE table count then ordered canonical per-image SHA256s."""
    from j3d_native_textures import source_images
    images=[source_image_identity(bti,metadata) for _,bti,metadata in source_images(source,external_images,declaration=declaration)]
    if len(images)>4096:raise ValueError('Image table bound')
    return hashlib.sha256(b'DUSKCTX1'+struct.pack('<I',len(images))+b''.join(images)).digest()

def filename(source, context):
    if not isinstance(context,bytes) or len(context)!=32:raise ValueError('Native image context')
    return identity(source).hex()+'-'+context.hex()+'.dna'

def compile_asset(source,meshes,images,materials,plan,shapes,*,image_context,external_context_required=False):
    """Banks must be immutable outputs of the same source conversion.

    This generic container checks native bank signatures/lengths and binds their
    exact bytes; conversion provenance must also be retained by the caller.
    The console performs the full existing native bank validation before upload.
    """
    digest=identity(source)
    if type(external_context_required) is not bool:raise ValueError('Native texture-context flag')
    if not isinstance(image_context,bytes) or len(image_context)!=32:raise ValueError('Native image context')
    for bank,magic,optional in [(meshes,b'DUSKPN01',False),(images,b'DUSKTX01',True),(materials,b'DUSKMT01',False),(plan,b'DUSKML01',False)]:
        if optional and bank==b'':continue
        if not isinstance(bank,bytes) or len(bank)<32 or bank[:8]!=magic or struct.unpack_from('<I',bank,12)[0]!=len(bank):raise ValueError('Native bank header/extent')
    count=struct.unpack_from('<I',plan,16)[0]
    if not isinstance(shapes,list) or not 1<=count<=MAX_MODEL_PARTS or len(shapes)!=count:raise ValueError('Native shape ownership count')
    shape_bytes=b''.join(struct.pack('<I',integer(s,4095)) for s in shapes)
    header=bytearray(160);header[:8]=b'DUSKNA02';struct.pack_into('<4I',header,8,2,0,len(source),count);header[24:56]=digest
    struct.pack_into('<I',header,96,int(external_context_required));header[100:132]=image_context
    payload=bytearray()
    for n,bank in enumerate([meshes,images,materials,plan,shape_bytes]):
        struct.pack_into('<2I',header,56+n*8,160+len(payload),len(bank));payload.extend(bank);payload.extend(bytes((-len(payload))%16))
    size=160+len(payload)
    if size>MAX_BYTES:raise ValueError('Native asset staging bound')
    struct.pack_into('<I',header,12,size);struct.pack_into('<I',header,156,zlib.crc32(header[:156]+payload))
    return bytes(header+payload)
