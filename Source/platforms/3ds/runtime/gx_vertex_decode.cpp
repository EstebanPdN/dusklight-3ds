#include "gx_vertex_decode.hpp"
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace dusk::native::gx {
namespace {
bool fail(VertexDecodeError value, VertexDecodeError* error) { if(error)*error=value;return false; }
u32 integer(std::span<const u8> data,bool little) {
    // The original indexed arrays are frequently unaligned native floats.
    // memcpy retains alignment/alias safety and lowers to bounded loads; do
    // not rebuild every 16/32-bit component in a byte loop on ARM11.
    if(data.size()==1)return data[0];
    if(data.size()==2){std::uint16_t value;std::memcpy(&value,data.data(),2);
        return little==(std::endian::native==std::endian::little)?value:__builtin_bswap16(value);}
    if(data.size()==4){u32 value;std::memcpy(&value,data.data(),4);
        return little==(std::endian::native==std::endian::little)?value:__builtin_bswap32(value);}
    u32 value=0;
    if(little) for(size_t i=data.size();i>0;--i)value=(value<<8)|data[i-1];
    else for(u8 byte:data)value=(value<<8)|byte;
    return value;
}
float scalar(std::span<const u8> data,u8 type,bool little,float scale) {
    const u32 value=integer(data,little);
    switch(type) {
    case GX_U8:case GX_U16:return static_cast<float>(value)*scale;
    case GX_S8:return static_cast<float>(value>=128?static_cast<s32>(value)-256:static_cast<s32>(value))*scale;
    case GX_S16:return static_cast<float>(value>=32768?static_cast<s32>(value)-65536:static_cast<s32>(value))*scale;
    case GX_F32:return std::bit_cast<float>(value);
    default:return 0; // Unreachable for a successfully constructed plan.
    }
}
void color(std::span<const u8> data,u8 type,bool little,float* out) {
    if(type==GX_RGB8 || type==GX_RGBX8 || type==GX_RGBA8) {
        for(unsigned i=0;i<3;++i)out[i]=data[i]/255.0f;
        out[3]=type==GX_RGBA8?data[3]/255.0f:1.0f;return;
    }
    const u32 v=integer(data,little);
    if(type==GX_RGB565) {
        out[0]=((v>>11)&31)/31.0f;out[1]=((v>>5)&63)/63.0f;out[2]=(v&31)/31.0f;out[3]=1;
    } else {
        const unsigned bits=type==GX_RGBA4?4:6, mask=(1u<<bits)-1;
        for(unsigned i=0;i<4;++i)out[i]=((v>>((3-i)*bits))&mask)/static_cast<float>(mask);
    }
}
}
bool planVertexDecode(const aurora::gx::dl::VtxLayout& layout,
                      std::span<const GXVtxAttrFmtList> formats,VertexDecodePlan& output,VertexDecodeError* error) {
    output={};if(error)*error=VertexDecodeError::none;
    VertexDecodePlan plan{};unsigned cursor=0;
    for(unsigned attr=0;attr<GX_VA_MAX_ATTR;++attr) {
        const auto& in=layout.attrs[attr];if(in.type==GX_NONE)continue;
        if(attr>GX_VA_TEX7 || in.offset!=cursor || in.type>GX_INDEX16 || in.type<GX_DIRECT)
            return fail(VertexDecodeError::layout,error);
        auto& a=plan.attributes[plan.count++];a.attribute=attr;a.offset=in.offset;a.descriptor=in.type;
        if(attr<=GX_VA_TEX7MTXIDX) {
            if(in.type!=GX_DIRECT || in.size!=1)return fail(VertexDecodeError::layout,error);
            a.components=a.scalarSize=a.packedSize=1;a.type=GX_U8;
        } else {
            const GXVtxAttrFmtList* fmt=nullptr;
            for(const auto& f:formats)if(f.attr==attr){if(fmt)return fail(VertexDecodeError::format,error);fmt=&f;}
            if(!fmt || fmt->frac>31)return fail(VertexDecodeError::format,error);
            a.type=static_cast<u8>(fmt->type);
            unsigned bytes=0;
            if(attr==GX_VA_CLR0 || attr==GX_VA_CLR1) {
                if(fmt->cnt!=GX_CLR_RGB && fmt->cnt!=GX_CLR_RGBA)return fail(VertexDecodeError::format,error);
                switch(fmt->type) {
                case GX_RGB565:case GX_RGBA4:bytes=2;break;
                case GX_RGB8:case GX_RGBA6:bytes=3;break;
                case GX_RGBX8:case GX_RGBA8:bytes=4;break;
                default:return fail(VertexDecodeError::format,error);
                }
                a.scalarSize=bytes;a.components=1;
            } else {
                switch(fmt->type) {
                case GX_U8:case GX_S8:a.scalarSize=1;break;
                case GX_U16:case GX_S16:a.scalarSize=2;break;
                case GX_F32:a.scalarSize=4;break;
                default:return fail(VertexDecodeError::format,error);
                }
                if(attr==GX_VA_POS) {
                    if(fmt->cnt!=GX_POS_XY && fmt->cnt!=GX_POS_XYZ)return fail(VertexDecodeError::format,error);
                    a.components=fmt->cnt==GX_POS_XY?2:3;
                } else if(attr==GX_VA_NRM) {
                    if(fmt->cnt!=GX_NRM_XYZ && fmt->cnt!=GX_NRM_NBT && fmt->cnt!=GX_NRM_NBT3)
                        return fail(VertexDecodeError::format,error);
                    a.components=fmt->cnt==GX_NRM_XYZ?3:9;
                    a.nbt3=fmt->cnt==GX_NRM_NBT3 && in.type!=GX_DIRECT;
                } else {
                    if(fmt->cnt!=GX_TEX_S && fmt->cnt!=GX_TEX_ST)return fail(VertexDecodeError::format,error);
                    a.components=fmt->cnt==GX_TEX_S?1:2;
                }
                const unsigned fraction=attr==GX_VA_NRM?(a.scalarSize==1?6:a.scalarSize==2?14:0):fmt->frac;
                a.scale=fmt->type==GX_F32?1.0f:std::ldexp(1.0f,-static_cast<int>(fraction));
                bytes=a.scalarSize*a.components;
            }
            const unsigned packedSize=in.type==GX_DIRECT?bytes:(in.type==GX_INDEX8?1:2)*(a.nbt3?3:1);
            if(in.size!=packedSize)return fail(VertexDecodeError::layout,error);
            a.packedSize=packedSize;
        }
        cursor+=a.packedSize;
    }
    if(!cursor || cursor!=layout.stride || layout.attrs[GX_VA_POS].type==GX_NONE)
        return fail(VertexDecodeError::layout,error);
    plan.stride=cursor;output=plan;return true;
}
bool decodeVertex(const VertexDecodePlan& plan,std::span<const u8> packed,
                  const VertexArrays& arrays,DecodedVertex& output,VertexDecodeError* error) {
    output={};if(error)*error=VertexDecodeError::none;
    if(!plan.stride || plan.count>plan.attributes.size())return fail(VertexDecodeError::layout,error);
    if(packed.size()<plan.stride)return fail(VertexDecodeError::packedBounds,error);
    DecodedVertex value{};
    for(unsigned i=0;i<plan.count;++i) {
        const auto& a=plan.attributes[i];
        // Plans are generated by planVertexDecode, not loaded from resources.
        const auto record=packed.subspan(a.offset,a.packedSize);
        const unsigned attr=a.attribute;value.present|=1u<<attr;
        if(attr<=GX_VA_TEX7MTXIDX){value.matrix[attr]=record[0];continue;}
        auto fetch=[&](unsigned slice,unsigned length,std::span<const u8>& bytes,bool& little)->bool {
            const size_t within=a.nbt3?slice*3*a.scalarSize:0;
            if(a.descriptor==GX_DIRECT){bytes=record;little=false;return true;}
            const unsigned width=a.descriptor==GX_INDEX8?1:2;
            const auto index=integer(record.subspan(a.nbt3?slice*width:0,width),false);
            const auto& array=arrays[attr];
            // Use wide arithmetic even on ARM32 before checking the bound.
            const std::uint64_t offset=std::uint64_t(index)*array.stride+within;
            if(offset>array.bytes.size() || length>array.bytes.size()-offset)return false;
            bytes=array.bytes.subspan(static_cast<size_t>(offset),length);little=array.littleEndian;return true;
        };
        if(attr==GX_VA_CLR0 || attr==GX_VA_CLR1) {
            std::span<const u8> bytes;bool little;
            if(!fetch(0,a.scalarSize,bytes,little))return fail(VertexDecodeError::arrayBounds,error);
            color(bytes,a.type,little,value.color[attr-GX_VA_CLR0]);continue;
        }
        if(attr==GX_VA_NRM)value.normalCount=a.components/3;
        float* dest=attr==GX_VA_POS?value.position:attr==GX_VA_NRM?value.normal[0]:value.texcoord[attr-GX_VA_TEX0];
        const unsigned slices=a.nbt3?3:1,components=a.nbt3?3:a.components;
        for(unsigned slice=0;slice<slices;++slice) {
            std::span<const u8> bytes;bool little;
            if(!fetch(slice,components*a.scalarSize,bytes,little))return fail(VertexDecodeError::arrayBounds,error);
            for(unsigned c=0;c<components;++c) {
                const float v=scalar(bytes.subspan(c*a.scalarSize,a.scalarSize),a.type,little,a.scale);
                if(!std::isfinite(v))return fail(VertexDecodeError::nonfinite,error);
                // Address each normal slice separately; do not index across C++ subarrays.
                if(attr==GX_VA_NRM)value.normal[(slice*components+c)/3][c%3]=v;
                else dest[c]=v;
            }
        }
    }
    output=value;return true;
}
}
