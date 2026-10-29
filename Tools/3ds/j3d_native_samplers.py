"""Mac-only source sampler controls -> native PICA descriptor constants.

Maps the modeled original SDK's quantized LOD controls, not source commands.
Derivative, edge behavior and physical filtering equivalence remain unproved.
"""
import hashlib,math,struct
from j3d_native_geometry import need,Unsupported

def f32(x):return struct.unpack('<f',struct.pack('<f',x))[0]

def sampler(header,resident_levels,*,allow_sdk_sampler_model=False):
    if not allow_sdk_sampler_model:raise Unsupported('Source sampler model requires explicit selection; physical filtering is unproved')
    need(isinstance(header,bytes) and len(header)>=32,'Source sampler header extent')
    need(type(resident_levels) is int and 1<=resident_levels<=8,'Native resident mip count')
    s,t=header[6:8];enabled,edge,clamp,anisotropy,minimum,magnify,first,last,levels=header[16:25]
    need(s<3 and t<3 and minimum<6 and magnify<2 and enabled<2 and edge<2 and clamp<2 and anisotropy<3,'Source sampler control domain')
    need(levels==resident_levels,'Source/native resident mip count mismatch')
    if edge or clamp or anisotropy:raise Unsupported('Source edge LOD, bias clamp or anisotropy requires an explicit native policy')
    raw_bias=struct.unpack_from('>h',header,26)[0]
    # J3D multiplies the signed hundredths by float32(1/100); the SDK then
    # clamps and writes the low signed8-bit, five-fractional-bit bias field.
    # Spell out truncation/byte encoding rather than float-to-u8 C undefined
    # behavior on negative values. Native PICA's eight fractional bits retain
    # the modeled field exactly (multiply its signed value by8).
    bias=f32(f32(raw_bias)*f32(1/100))
    bias=f32(-4) if bias<-4 else f32(3.99) if bias>=4 else bias
    source_field=math.trunc(f32(bias*32))&255
    signed=source_field if source_field<128 else source_field-256
    if minimum<2:lo=hi=0
    else:
        lo,hi=min(first,80),min(last,80)
        if lo%8 or hi%8:raise Unsupported('Fractional source mip clamps cannot be represented by the native integer mip interval')
        lo//=8;hi//=8
        if lo>hi or hi>=resident_levels:raise Unsupported('Source sampler mip interval exceeds native resident storage')
    wraps=['clamp','repeat','mirror'];filters=['nearest','linear']
    native={'wrap_s':wraps[s],'wrap_t':wraps[t],'magnify':filters[magnify],'minify':filters[minimum&1],
            'mipmap':filters[int(minimum>=4)],'first_level':lo,'last_level':hi,'lod_bias':signed*8}
    return native,{'source_sampler_sha256':hashlib.sha256(header[6:8]+header[16:28]).hexdigest(),'source_bias_field':source_field,
        'original_bias_hundredths':raw_bias,'native_bias_exact_to_modeled_sdk_field':True,'source_sampler_model':'J3D float32 scale and original SDK LOD fields',
        'filtering_gpu_validated':False,'rendering_ready':False}
