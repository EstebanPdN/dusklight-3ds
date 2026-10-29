"""Mac-only assembly of source-identified native resource banks; no GX output."""
import hashlib
import struct
import zlib
from native_material_pack import integer

MAX_BYTES=16<<20

def identity(source):
    if not isinstance(source,bytes) or not 32<=len(source)<=16<<20 or source[:8] not in (b'J3D2bmd2',b'J3D2bmd3',b'J3D2bdl3',b'J3D2bdl4') or struct.unpack_from('>I',source,8)[0]!=len(source):
        raise ValueError('Original source extent/header')
    return hashlib.sha256(source).digest()

def compile_asset(source,meshes,images,materials,plan,shapes,*,external_context_required=False):
    """Banks must be immutable outputs of the same source conversion.

    This generic container checks native bank signatures/lengths and binds their
    exact bytes; conversion provenance must also be retained by the caller.
    The console performs the full existing native bank validation before upload.
    """
    digest=identity(source)
    if type(external_context_required) is not bool:raise ValueError('Native texture-context flag')
    for bank,magic,optional in [(meshes,b'DUSKPN01',False),(images,b'DUSKTX01',True),(materials,b'DUSKMT01',False),(plan,b'DUSKML01',False)]:
        if optional and bank==b'':continue
        if not isinstance(bank,bytes) or len(bank)<32 or bank[:8]!=magic or struct.unpack_from('<I',bank,12)[0]!=len(bank):raise ValueError('Native bank header/extent')
    count=struct.unpack_from('<I',plan,16)[0]
    if not isinstance(shapes,list) or not 1<=count<=64 or len(shapes)!=count:raise ValueError('Native shape ownership count')
    shape_bytes=b''.join(struct.pack('<I',integer(s,4095)) for s in shapes)
    header=bytearray(128);header[:8]=b'DUSKNA01';struct.pack_into('<4I',header,8,1,0,len(source),count);header[24:56]=digest
    struct.pack_into('<I',header,96,int(external_context_required))
    payload=bytearray()
    for n,bank in enumerate([meshes,images,materials,plan,shape_bytes]):
        struct.pack_into('<2I',header,56+n*8,128+len(payload),len(bank));payload.extend(bank);payload.extend(bytes((-len(payload))%16))
    size=128+len(payload)
    if size>MAX_BYTES:raise ValueError('Native asset staging bound')
    struct.pack_into('<I',header,12,size);struct.pack_into('<I',header,124,zlib.crc32(header[:124]+payload))
    return bytes(header+payload)
