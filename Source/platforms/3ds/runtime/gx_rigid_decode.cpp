#include "gx_vertex_decode.hpp"
#include "gx_raster_color.hpp"
#include "gx_gpu_geometry.hpp"
#include "gx_profile.hpp"
#include "gx_rigid_vertex.hpp"
#include "gx_compact_cache.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#if defined(__3DS__)
extern "C" void OSReport(const char*,...);
#endif
namespace dusk::native::gx {
StaticGeometryHint staticGeometryHint{};
namespace {
StaticCompactCache* activeStaticCache=nullptr;
}
StaticCompactCache* staticCompactCache(){return activeStaticCache;}
void setStaticCompactCache(StaticCompactCache* cache){activeStaticCache=cache;}
}
// Called by J3DShape around each display list of a static model (generation
// nonzero) and after it (zero). Batches inside one list are numbered by the
// command processor in submission order.
extern "C" void duskNativeStaticGeometry(std::uint32_t generation,const void* list){
    auto& hint=dusk::native::gx::staticGeometryHint;
    if(!generation&&hint.list&&dusk::native::gx::staticListEnded)dusk::native::gx::staticListEnded(hint.list,hint.batch);
    hint={generation,generation?list:nullptr,0};
}
namespace dusk::native::gx {
namespace {
// Keep arbitrary GX array alignment legal, while exposing naturally aligned
// native arrays to the compiler. ARM11 otherwise assembles these tiny copies
// one byte at a time even for the original aligned position/normal streams.
template<unsigned Bytes,unsigned Alignment>
inline void copyNative(void* destination,const void* source){
    if(reinterpret_cast<std::uintptr_t>(source)%Alignment==0)
        std::memcpy(destination,__builtin_assume_aligned(source,Alignment),Bytes);
    else std::memcpy(destination,source,Bytes);
}
inline bool boundedPosition(float value){
    static_assert(sizeof(float)==sizeof(std::uint32_t)&&std::numeric_limits<float>::is_iec559);
    // IEEE magnitudes are ordered as unsigned bits. This rejects infinity and
    // every NaN as well as |value| > 1e7, without a VFP comparison per scalar.
    return (std::bit_cast<std::uint32_t>(value)&0x7fffffffu)<=std::bit_cast<std::uint32_t>(1e7f);
}
// Bounds are proved against fresh live spans for each draw, never cached across
// mutations. Once index <= limit, native-width multiplication cannot overflow:
// index*stride <= bytes.size()-elementBytes. Zero stride repeats element zero.
struct IndexedGather {
    const u8* data{};
    std::size_t stride{};
    unsigned offset{},limit{},size{};
    bool wide{};
    bool prepare(unsigned packedOffset,bool index16,const VertexArrayView& array,unsigned elementBytes){
        if(array.bytes.size()<elementBytes)return false;
        const auto last=array.stride?(array.bytes.size()-elementBytes)/array.stride:std::size_t(65535);
        *this={array.bytes.data(),array.stride,packedOffset,unsigned(std::min<std::size_t>(last,65535)),elementBytes,index16};
        return true;
    }
    const u8* read(const u8* packed)const{
        const auto* record=packed+offset;
        const unsigned index=wide?(unsigned(record[0])<<8)|record[1]:record[0];
        return index<=limit?data+std::size_t(index)*stride:nullptr;
    }
};
// Native PICA engine: GX attribute formats converted once into the compact
// shader formats (float position, s16 normal/UV with a uniform scale, RGBA8).
unsigned colorBytes(unsigned type){
    switch(type){case GX_RGB565:case GX_RGBA4:return 2;case GX_RGB8:case GX_RGBA6:return 3;case GX_RGBX8:case GX_RGBA8:return 4;}
    return 0;
}
void readColor(unsigned type,const u8* s,std::uint8_t* o){
    if(type==GX_RGBA8){std::memcpy(o,s,4);return;}
    if(type==GX_RGB8||type==GX_RGBX8){o[0]=s[0];o[1]=s[1];o[2]=s[2];o[3]=255;return;}
    u32 v;
    if(type==GX_RGBA6)v=u32(s[0])|(u32(s[1])<<8)|(u32(s[2])<<16);
    else{std::uint16_t h;std::memcpy(&h,s,2);v=h;}
    if(type==GX_RGB565){
        o[0]=std::uint8_t((((v>>11)&31)*255+15)/31);o[1]=std::uint8_t((((v>>5)&63)*255+31)/63);
        o[2]=std::uint8_t(((v&31)*255+15)/31);o[3]=255;return;
    }
    const unsigned bits=type==GX_RGBA4?4:6,mask=(1u<<bits)-1;
    for(unsigned i=0;i<4;++i)o[i]=std::uint8_t((((v>>((3-i)*bits))&mask)*255+mask/2)/mask);
}
template<class Attribute>
bool acceptPosition(const Attribute& a){
    return a.components==3&&(a.type==GX_F32||a.type==GX_S16||a.type==GX_U16||a.type==GX_S8||a.type==GX_U8);
}
template<class Attribute>
bool readPosition(const Attribute& a,const u8* s,float* o){
    for(unsigned c=0;c<3;++c){
        switch(a.type){
        case GX_F32:std::memcpy(o+c,s+4*c,4);break;
        case GX_S16:{std::int16_t v;std::memcpy(&v,s+2*c,2);o[c]=float(v)*a.scale;break;}
        case GX_U16:{std::uint16_t v;std::memcpy(&v,s+2*c,2);o[c]=float(v)*a.scale;break;}
        case GX_S8:o[c]=float(std::int8_t(s[c]))*a.scale;break;
        default:o[c]=float(s[c])*a.scale;break;
        }
        if(!boundedPosition(o[c]))return false;
    }
    return true;
}
// Normal: S16/S8 keep raw integers with the original scale; F32 uses 1/16384.
template<class Attribute>
bool acceptNormal(const Attribute& a){
    return a.components==3&&(a.type==GX_S16||a.type==GX_S8||a.type==GX_F32);
}
template<class Attribute>
float normalScale(const Attribute& a){return a.type==GX_F32?1.f/16384.f:a.scale;}
template<class Attribute>
bool readNormal(const Attribute& a,const u8* s,std::int16_t* o){
    if(a.type==GX_S16){copyNative<6,2>(o,s);return true;}
    for(unsigned c=0;c<3;++c){
        if(a.type==GX_S8)o[c]=std::int8_t(s[c]);
        else{float v;std::memcpy(&v,s+4*c,4);v*=16384.f;if(!std::isfinite(v)||std::abs(v)>32767.f)return false;o[c]=std::int16_t(std::lround(v));}
    }
    return true;
}
// Texture coordinate: S16/S8/U8 raw with original scale; U16 when it fits;
// F32 as 1/1024 fixed point (|uv| < 32), otherwise the caller falls back.
template<class Attribute>
bool acceptTexcoord(const Attribute& a){
    return a.components==2&&(a.type==GX_S16||a.type==GX_S8||a.type==GX_U8||a.type==GX_U16||a.type==GX_F32);
}
template<class Attribute>
float texcoordScale(const Attribute& a){return a.type==GX_F32?1.f/1024.f:a.scale;}
template<class Attribute>
bool readTexcoord(const Attribute& a,const u8* s,std::int16_t* o){
    switch(a.type){
    case GX_S16:copyNative<4,2>(o,s);return true;
    case GX_S8:o[0]=std::int8_t(s[0]);o[1]=std::int8_t(s[1]);return true;
    case GX_U8:o[0]=s[0];o[1]=s[1];return true;
    case GX_U16:for(unsigned c=0;c<2;++c){std::uint16_t v;std::memcpy(&v,s+2*c,2);if(v>32767)return false;o[c]=std::int16_t(v);}return true;
    default:for(unsigned c=0;c<2;++c){float v;std::memcpy(&v,s+4*c,4);v*=1024.f;if(!std::isfinite(v)||std::abs(v)>32767.f)return false;o[c]=std::int16_t(std::lround(v));}return true;
    }
}
template<class Attribute>
unsigned elementBytes(unsigned slot,const Attribute& a){return slot==2?colorBytes(a.type):a.scalarSize*a.components;}
// Direct (immediate) attributes are big-endian packed bytes; convert one
// element to the little-endian layout the readers above expect.
template<class Attribute>
const u8* littleElement(unsigned slot,const Attribute& a,const u8* source,u8* scratch){
    unsigned scalar,count;
    if(slot==2){
        if(a.type==GX_RGB565||a.type==GX_RGBA4){scalar=2;count=1;}
        else if(a.type==GX_RGBA6){scalar=3;count=1;}
        else return source; // byte-ordered RGB8/RGBX8/RGBA8
    }else{scalar=a.scalarSize;count=a.components;}
    if(scalar==1)return source;
    for(unsigned i=0;i<count;++i)for(unsigned b=0;b<scalar;++b)scratch[i*scalar+b]=source[i*scalar+scalar-1-b];
    return scratch;
}
}
bool decodeRigidBatch(const VertexDecodePlan& plan,std::span<const u8> packed,const VertexArrays& arrays,
    std::span<GpuVertex> output,const RasterColorPlan& color,const std::array<float,2>& scale,std::span<const std::int8_t> matrixOffsets){
    if(!plan.stride||output.empty()||output.size()>packed.size()/plan.stride||plan.count<3||plan.count>12)return false;
#if defined(__3DS__)
    if(gxVertexLayoutTracing)for(unsigned n=0;n<plan.count;++n){
        const auto& a=plan.attributes[n];
        if(a.attribute==GX_VA_POS||a.attribute==GX_VA_NRM||a.attribute==GX_VA_TEX0)
            OSReport("Native packed field attr=%u type=%u descriptor=%u scale=%g\n",unsigned(a.attribute),unsigned(a.type),unsigned(a.descriptor),double(a.scale));
    }
#endif
    const VertexDecodePlan::Attribute* fields[4]{};
    const VertexDecodePlan::Attribute* matrix=nullptr;
    for(unsigned i=0;i<plan.count;++i){
        const auto& a=plan.attributes[i];unsigned slot;
        if(a.attribute<=GX_VA_TEX7MTXIDX){
            if(a.attribute==GX_VA_TEX0MTXIDX||a.descriptor!=GX_DIRECT)return false;
            if(a.attribute==GX_VA_PNMTXIDX){if(matrixOffsets.size()!=10)return false;matrix=&a;}
            continue;
        }
        if(a.attribute==GX_VA_POS)slot=0;else if(a.attribute==GX_VA_NRM)slot=1;
        else if(a.attribute==GX_VA_CLR0)slot=2;else if(a.attribute==GX_VA_TEX0)slot=3;else return false;
        if(fields[slot]||(a.descriptor!=GX_DIRECT&&a.descriptor!=GX_INDEX8&&a.descriptor!=GX_INDEX16)||a.nbt3)return false;
        if(slot==2){if(a.type!=GX_RGBA8)return false;}
        else if((a.type!=GX_F32&&a.type!=GX_S16&&a.type!=GX_S8)||a.components!=(slot==3?2:3))return false;
        if(a.descriptor!=GX_DIRECT&&arrays[a.attribute].littleEndian!=(std::endian::native==std::endian::little))return false;
        fields[slot]=&a;
    }
    if(!fields[0]||!fields[1]||!fields[3]||((color.vertex|color.ambientVertex)&&!fields[2])||(color.ambientVertex&2))return false;
    // Validate the dynamic palette once, before gathering any vertex. Slot 0
    // occupies u0..5; additional matrices follow 1..8 compacted light records.
    if(matrix){
        const unsigned base=19+4*std::popcount(color.channels[0].mask);
        if(base<23||base>51)return false;
        for(int offset:matrixOffsets)
            if(offset < -1||(offset>0&&(offset<int(base)||offset+6>94||(offset-base)%6)))return false;
    }
    std::array<IndexedGather,4> gather{};
    for(unsigned slot=0;slot<4;++slot)if(const auto* a=fields[slot];a&&a->descriptor!=GX_DIRECT)
        if(!gather[slot].prepare(a->offset,a->descriptor==GX_INDEX16,arrays[a->attribute],slot==2?4:a->scalarSize*a->components))return false;
    const float alphaScale=color.lit&2?std::clamp(color.ambient[3],0.f,1.f):1;
    GpuVertex base;base.color=color.material;base.color[3]*=alphaScale;base.texcoord[2]={0,3};
    for(auto& v:output){
        const u8* source[4]{};
        for(unsigned slot=0;slot<4;++slot)if(const auto* a=fields[slot]){
            const auto* record=packed.data()+a->offset;
            if(a->descriptor==GX_DIRECT){source[slot]=record;continue;}
            source[slot]=gather[slot].read(packed.data());
            if(!source[slot])return false;
        }
        v=base;float normal[3];
        if(matrix){const unsigned index=packed[matrix->offset];
            if(index>27||index%3||matrixOffsets[index/3]<0)return false;
            const unsigned offset=matrixOffsets[index/3];
            v.texcoord[2]={float(offset),float(offset+3)};
        }
        auto unpack=[&](unsigned slot,float* dest){
            const auto& a=*fields[slot];
            const bool swap=a.descriptor==GX_DIRECT&&std::endian::native==std::endian::little;
            if(a.type==GX_F32){
                if(!swap)std::memcpy(dest,source[slot],a.components*4);
                else for(unsigned c=0;c<a.components;++c){u32 value;std::memcpy(&value,source[slot]+c*4,4);dest[c]=std::bit_cast<float>(__builtin_bswap32(value));}
                return;
            }
            if(a.type==GX_S16)for(unsigned c=0;c<a.components;++c){
                std::uint16_t value;std::memcpy(&value,source[slot]+c*2,2);if(swap)value=__builtin_bswap16(value);dest[c]=float(std::bit_cast<std::int16_t>(value))*a.scale;
            }else for(unsigned c=0;c<a.components;++c){
                std::int8_t value;std::memcpy(&value,source[slot]+c,1);dest[c]=float(value)*a.scale;
            }
        };
        unpack(0,v.position.data());unpack(1,normal);unpack(3,v.texcoord[0].data());
        for(unsigned c=0;c<3;++c)if(!std::isfinite(v.position[c])||std::abs(v.position[c])>1e7f||!std::isfinite(normal[c])||std::abs(normal[c])>1e4f)return false;
        for(float value:v.texcoord[0])if(!std::isfinite(value)||std::abs(value)>1e8f)return false;
        v.position[3]=normal[2];v.texcoord[1]={normal[0],normal[1]};
        v.texcoord[0]={v.texcoord[0][0]*scale[0],1-v.texcoord[0][1]*scale[1]};
        if((color.vertex|color.ambientVertex)&1)for(unsigned c=0;c<3;++c)v.color[c]=source[2][c]/255.f;
        if(color.vertex&2)v.color[3]=source[2][3]/255.f*alphaScale;
        packed=packed.subspan(plan.stride);
    }
    return true;
}
// Shared tail of the compact decoders and native shape replay: per-draw
// uniforms derived from the decoded record scales and the color plan.
bool finishCompactRigidUniforms(RigidVertexPlan& rigid,const std::array<float,2>& scale){
    auto material=rigid.color.material;
    if(rigid.color.lit&2)material[3]*=std::clamp(rigid.color.ambient[3],0.f,1.f);
    for(float value:material)if(!std::isfinite(value)||value<0||value>1)return false;
    if(rigid.multiTexture){
        for(unsigned unit=0;unit<3;++unit)if(rigid.uniforms[80][unit]==1){
            const unsigned base=81+4*unit;
            for(unsigned row:{0u,2u})for(unsigned c=0;c<2;++c)rigid.uniforms[base+row][c]*=rigid.decodedTex0Scale;
            for(unsigned row:{1u,3u})for(unsigned c=0;c<2;++c)rigid.uniforms[base+row][c]*=rigid.decodedTex1Scale;
        }
    }else{
        rigid.uniforms[13][2]=rigid.decodedTex0Scale*scale[0];rigid.uniforms[13][3]=rigid.decodedTex0Scale*scale[1];
    }
    rigid.uniforms[11][3]=rigid.decodedNormalScale;
    rigid.uniforms[12][3]=1;
    rigid.uniforms[14][3]=float(rigid.color.vertex&1);
    rigid.uniforms[15][3]=rigid.color.vertex&2?(rigid.color.lit&2?std::clamp(rigid.color.ambient[3],0.f,1.f):1.f):-1.f;
    rigid.uniforms[16][3]=1.f/255.f;
    rigid.uniforms[93]=material;rigid.compact=true;
    return true;
}
bool decodeCompactRigidBatch(const VertexDecodePlan& plan,std::span<const u8> packed,const VertexArrays& arrays,
    std::span<GpuVertex> output,const std::array<float,2>& scale,RigidVertexPlan& rigid,CompactVertexCache* cache){
    rigid.compact=false;rigid.uniforms[12][3]=0;rigid.staticVertices=nullptr;
    if(!plan.stride||output.empty()||output.size()>packed.size()/plan.stride||plan.count<3||plan.count>12)return false;
    for(float value:scale)if(!std::isfinite(value)||value<=0||value>1)return false;
    const VertexDecodePlan::Attribute* fields[4]{};
    const VertexDecodePlan::Attribute* matrix=nullptr;
    for(unsigned i=0;i<plan.count;++i){
        const auto& a=plan.attributes[i];unsigned slot;
        if(a.attribute<=GX_VA_TEX7MTXIDX){
            if(a.attribute==GX_VA_TEX0MTXIDX||a.descriptor!=GX_DIRECT)return false;
            if(a.attribute==GX_VA_PNMTXIDX)matrix=&a;
            continue;
        }
        if(a.attribute==GX_VA_POS)slot=0;else if(a.attribute==GX_VA_NRM)slot=1;
        else if(a.attribute==GX_VA_CLR0)slot=2;else if(a.attribute==GX_VA_TEX0)slot=3;
        else{++gxCompactDecodeRejects[40+(a.attribute-GX_VA_POS)%16];return false;}
        if(fields[slot]||(a.descriptor!=GX_INDEX8&&a.descriptor!=GX_INDEX16)||a.nbt3)return false;
        if(slot==2){if(!colorBytes(a.type))return false;}
        else if(slot==0?!acceptPosition(a):slot==1?!acceptNormal(a):!acceptTexcoord(a))return false;
        if(arrays[a.attribute].littleEndian!=(std::endian::native==std::endian::little))return false;
        fields[slot]=&a;
    }
    if(!fields[0]||!fields[1]||!fields[3]||((rigid.color.vertex|rigid.color.ambientVertex)&&!fields[2])||(rigid.color.ambientVertex&2)||bool(matrix)!=rigid.indexedMatrices)return false;
    auto material=rigid.color.material;
    if(rigid.color.lit&2)material[3]*=std::clamp(rigid.color.ambient[3],0.f,1.f);
    for(float value:material)if(!std::isfinite(value)||value<0||value>1)return false;
    const unsigned paletteBase=rigid.paletteBase();
    if(rigid.compactPalette){
        for(unsigned m=0;m<10;++m){
            const int offset=rigid.matrixOffsets[m],normal=rigid.normalOffsets[m];
            if(offset < -1)return false;
            if(offset<0)continue;
            if((offset>0&&(offset<int(paletteBase)||offset+3>93))||normal<0||normal+3>93||(normal!=offset&&normal!=offset+3))return false;
        }
        cache=nullptr; // offsets depend on live matrix values
    }else for(int offset:rigid.matrixOffsets)if(offset < -1||(offset>0&&
       (offset<int(paletteBase)||offset+6>93||(offset-paletteBase)%6)))return false;
    auto target=std::as_writable_bytes(output).first(output.size()*sizeof(CompactRigidVertex));
    const auto originalPacked=packed.first(output.size()*plan.stride);
    CompactVertexCache::Key cacheKey{};
    CompactVertexCache::Arrays cacheArrays{};
    CompactVertexCache::Ranges ranges{};
    cacheKey[0]=output.size();cacheKey[1]=plan.stride;cacheKey[2]=matrix?matrix->offset+1:0;
    if(cache)for(unsigned slot=0;slot<4;++slot)if(const auto* a=fields[slot]){
        const unsigned at=4+6*slot;
        cacheKey[at]=a->attribute;cacheKey[at+1]=a->offset;cacheKey[at+2]=a->descriptor;
        cacheKey[at+3]=a->type;cacheKey[at+4]=std::bit_cast<u32>(a->scale);cacheKey[at+5]=arrays[a->attribute].stride;
        cacheArrays[slot]=arrays[a->attribute].bytes;
        ranges[slot].first=cacheArrays[slot].size();
    }
    std::memcpy(cacheKey.data()+28,rigid.matrixOffsets.data(),rigid.matrixOffsets.size());
    std::array<IndexedGather,4> gather{};
    for(unsigned slot=0;slot<4;++slot)if(const auto* a=fields[slot])
        if(!gather[slot].prepare(a->offset,a->descriptor==GX_INDEX16,arrays[a->attribute],elementBytes(slot,*a)))return false;
    // Static J3D geometry: trusted by generation, no byte comparison.
    StaticCompactCache* staticCache=!rigid.compactPalette&&staticGeometryHint.generation&&staticGeometryHint.list?activeStaticCache:nullptr;
    StaticCompactCache::Key staticKey{};
    if(staticCache){
        staticKey.generation=staticGeometryHint.generation;staticKey.batch=staticGeometryHint.batch;
        staticKey.list=reinterpret_cast<std::uintptr_t>(staticGeometryHint.list);
        staticKey.count=output.size();staticKey.stride=plan.stride;
        std::uint32_t layout=2166136261u;
        for(unsigned slot=0;slot<4;++slot)if(const auto* a=fields[slot]){
            const auto& array=arrays[a->attribute];
            for(const std::uint32_t v:{std::uint32_t(a->attribute),std::uint32_t(a->offset),std::uint32_t(a->descriptor),
                std::uint32_t(a->type),std::bit_cast<std::uint32_t>(a->scale),std::uint32_t(array.stride),
                std::uint32_t(reinterpret_cast<std::uintptr_t>(array.bytes.data())),std::uint32_t(array.bytes.size())})
                layout=(layout^v)*16777619u;
        }
        staticKey.layout=layout^(matrix?matrix->offset+1:0);
        std::memcpy(staticKey.offsets.data(),rigid.matrixOffsets.data(),staticKey.offsets.size());
        cache=nullptr; // static draws never touch the validating cache
    }
    bool reused=false;
    if(staticCache)if(const auto* saved=staticCache->find(staticKey,target.size())){
        // Resident: the GPU binds the cached vertices; the arena copy is skipped.
        if(staticCache->resident())rigid.staticVertices=saved;
        else std::memcpy(target.data(),saved,target.size());
        reused=true;
    }
    if(!reused&&cache)reused=cache->find(cacheKey,originalPacked,cacheArrays,target);
    if(!reused)for(unsigned n=0;n<output.size();++n){
        const u8* source[4]{};
        for(unsigned slot=0;slot<4;++slot)if(const auto& g=gather[slot];g.data){
            source[slot]=g.read(packed.data());
            if(!source[slot])return false;
            if(cache){
                const auto offset=std::size_t(source[slot]-g.data);
                ranges[slot].first=std::min(ranges[slot].first,offset);
                ranges[slot].bytes=std::max(ranges[slot].bytes,offset+g.size);
            }
        }
        CompactRigidVertex v;
        if(fields[2])readColor(fields[2]->type,source[2],v.color.data());
        if(!readPosition(*fields[0],source[0],v.position.data())||!readNormal(*fields[1],source[1],v.normal.data())||
           !readTexcoord(*fields[3],source[3],v.texcoord.data()))return false;
        if(matrix){
            const unsigned index=packed[matrix->offset];
            if(index>27||index%3||rigid.matrixOffsets[index/3]<0)return false;
            v.matrix={rigid.matrixOffsets[index/3],rigid.normalOffsets[index/3]};
        }
        // The caller supplies GpuVertex storage (at least float-aligned), and
        // the compact stride is a multiple of that alignment.
        static_assert(sizeof(v)%alignof(GpuVertex)==0);
        std::memcpy(__builtin_assume_aligned(target.data()+n*sizeof(v),alignof(GpuVertex)),&v,sizeof(v));
        packed=packed.subspan(plan.stride);
    }
    if(staticCache&&!reused)staticCache->store(staticKey,target);
    if(cache&&!reused){
        for(unsigned slot=0;slot<4;++slot)if(fields[slot])ranges[slot].bytes-=ranges[slot].first;
        // Sparse index ranges can span much more data than this draw uses.
        // Re-decoding those shapes is cheaper than validating a broad snapshot.
        // Bound the total comparison work before admitting an output record.
        std::size_t comparisonBudget=target.size()*2;
        bool economical=true;
        for(const auto range:ranges){
            if(range.bytes>comparisonBudget){economical=false;break;}
            comparisonBudget-=range.bytes;
        }
        if(economical)cache->store(cacheKey,originalPacked,cacheArrays,ranges,target);
    }
    rigid.decodedNormalScale=normalScale(*fields[1]);rigid.decodedTex0Scale=texcoordScale(*fields[3]);rigid.decodedTex1Scale=1;
    (void)material;
    return finishCompactRigidUniforms(rigid,scale);
}

bool decodeCompactRigidBatchMT(const VertexDecodePlan& plan,std::span<const u8> packed,const VertexArrays& arrays,
    std::span<GpuVertex> output,RigidVertexPlan& rigid){
    rigid.staticVertices=nullptr;rigid.compact=false;
    auto reject=[](unsigned code){++gxCompactDecodeRejects[code&63];return false;};
    if(!rigid.multiTexture)return reject(56);
    if(!plan.stride||output.empty()||output.size()>packed.size()/plan.stride)return reject(57);
    if(plan.count<2||plan.count>plan.attributes.size())return reject(58);
    // Fields: 0 POS, 1 NRM, 2 CLR0, 3 TEX0, 4 TEX1.
    const VertexDecodePlan::Attribute* fields[5]{};
    const VertexDecodePlan::Attribute* matrix=nullptr;
    for(unsigned i=0;i<plan.count;++i){
        const auto& a=plan.attributes[i];unsigned slot;
        if(a.attribute<=GX_VA_TEX7MTXIDX){
            if(a.attribute!=GX_VA_PNMTXIDX||a.descriptor!=GX_DIRECT)return reject(1);
            matrix=&a;continue;
        }
        if(a.attribute==GX_VA_POS)slot=0;else if(a.attribute==GX_VA_NRM)slot=1;else if(a.attribute==GX_VA_CLR0)slot=2;
        else if(a.attribute==GX_VA_TEX0)slot=3;else if(a.attribute==GX_VA_TEX1)slot=4;else return reject(2);
        if(fields[slot]||a.nbt3)return reject(3);
        if(a.descriptor!=GX_INDEX8&&a.descriptor!=GX_INDEX16&&a.descriptor!=GX_DIRECT)return reject(4);
        if(slot==2){if(!colorBytes(a.type))return reject(8+slot*8+(a.type&7));}
        else if(slot==0?!acceptPosition(a):slot==1?!acceptNormal(a):!acceptTexcoord(a))return reject(8+slot*8+(a.type&7));
        if(a.descriptor!=GX_DIRECT&&arrays[a.attribute].littleEndian!=(std::endian::native==std::endian::little))return reject(5);
        fields[slot]=&a;
    }
    bool needsTex1=false,needsTex0=false;
    for(unsigned unit=0;unit<3;++unit)if(rigid.uniforms[80][unit]==1)(rigid.unitSource[unit]?needsTex1:needsTex0)=true;
    // Normals are optional for unlit plans (the dummy light has zero color).
    if(!fields[0]||(!fields[1]&&rigid.color.lit)||(needsTex0&&!fields[3])||(needsTex1&&!fields[4])||
       ((rigid.color.vertex|rigid.color.ambientVertex)&&!fields[2])||(rigid.color.ambientVertex&2)||bool(matrix)!=rigid.indexedMatrices)return reject(6);
    auto material=rigid.color.material;
    if(rigid.color.lit&2)material[3]*=std::clamp(rigid.color.ambient[3],0.f,1.f);
    for(float value:material)if(!std::isfinite(value)||value<0||value>1)return reject(7);
    const unsigned paletteBase=rigid.paletteBase();
    for(unsigned m=0;m<10;++m){
        const int offset=rigid.matrixOffsets[m],normal=rigid.normalOffsets[m];
        if(offset < -1)return reject(59);
        if(offset<0)continue;
        if(offset>0&&(offset<int(paletteBase)||offset+3>80))return reject(59);
        if(normal<0||normal+3>80||(normal!=offset&&normal!=offset+3))return reject(59);
        if(!rigid.compactPalette&&(normal!=offset+3||(offset>0&&(offset-paletteBase)%6)))return reject(59);
    }
    auto target=std::as_writable_bytes(output).first(output.size()*sizeof(CompactRigidVertexMT));
    std::array<IndexedGather,5> gather{};
    bool anyDirect=false;
    for(unsigned slot=0;slot<5;++slot)if(const auto* a=fields[slot]){
        if(a->descriptor==GX_DIRECT){anyDirect=true;continue;}
        if(!gather[slot].prepare(a->offset,a->descriptor==GX_INDEX16,arrays[a->attribute],elementBytes(slot,*a)))return reject(61);
    }
    StaticCompactCache* staticCache=!anyDirect&&!rigid.compactPalette&&staticGeometryHint.generation&&staticGeometryHint.list?staticCompactCache():nullptr;
    StaticCompactCache::Key staticKey{};
    if(staticCache){
        staticKey.generation=staticGeometryHint.generation;staticKey.batch=staticGeometryHint.batch;
        staticKey.list=reinterpret_cast<std::uintptr_t>(staticGeometryHint.list);
        staticKey.count=output.size();staticKey.stride=plan.stride;
        std::uint32_t layout=0x5bd1e995u; // distinct from the single-texture records
        for(unsigned slot=0;slot<5;++slot)if(const auto* a=fields[slot]){
            const auto& array=arrays[a->attribute];
            for(const std::uint32_t v:{std::uint32_t(a->attribute),std::uint32_t(a->offset),std::uint32_t(a->descriptor),
                std::uint32_t(a->type),std::bit_cast<std::uint32_t>(a->scale),std::uint32_t(array.stride),
                std::uint32_t(reinterpret_cast<std::uintptr_t>(array.bytes.data())),std::uint32_t(array.bytes.size())})
                layout=(layout^v)*16777619u;
        }
        staticKey.layout=layout^(matrix?matrix->offset+1:0);
        std::memcpy(staticKey.offsets.data(),rigid.matrixOffsets.data(),staticKey.offsets.size());
    }
    bool reused=false;
    if(staticCache)if(const auto* saved=staticCache->find(staticKey,target.size())){
        if(staticCache->resident())rigid.staticVertices=saved;
        else std::memcpy(target.data(),saved,target.size());
        reused=true;
    }
    if(!reused){
        for(unsigned n=0;n<output.size();++n){
            const u8* source[5]{};
            u8 scratch[5][16];
            for(unsigned slot=0;slot<5;++slot)if(const auto* a=fields[slot]){
                if(a->descriptor==GX_DIRECT){
                    if(elementBytes(slot,*a)>sizeof(scratch[slot])||a->offset+elementBytes(slot,*a)>plan.stride)return reject(62);
                    source[slot]=littleElement(slot,*a,packed.data()+a->offset,scratch[slot]);
                }else if(!(source[slot]=gather[slot].read(packed.data())))return reject(63);
            }
            CompactRigidVertexMT v;
            if(fields[2])readColor(fields[2]->type,source[2],v.color.data());
            if(!readPosition(*fields[0],source[0],v.position.data()))return reject(55);
            if(fields[1]&&!readNormal(*fields[1],source[1],v.normal.data()))return false;
            if(fields[3]&&!readTexcoord(*fields[3],source[3],v.texcoord0.data()))return false;
            if(fields[4]&&!readTexcoord(*fields[4],source[4],v.texcoord1.data()))return false;
            if(matrix){
                const unsigned index=packed[matrix->offset];
                if(index>27||index%3||rigid.matrixOffsets[index/3]<0)return reject(54);
                v.matrix={rigid.matrixOffsets[index/3],rigid.normalOffsets[index/3]};
            }
            static_assert(sizeof(v)%alignof(GpuVertex)==0);
            std::memcpy(__builtin_assume_aligned(target.data()+n*sizeof(v),alignof(GpuVertex)),&v,sizeof(v));
            packed=packed.subspan(plan.stride);
        }
        if(staticCache)staticCache->store(staticKey,target);
    }
    rigid.decodedNormalScale=fields[1]?normalScale(*fields[1]):0.f;
    rigid.decodedTex0Scale=fields[3]?texcoordScale(*fields[3]):1.f;rigid.decodedTex1Scale=fields[4]?texcoordScale(*fields[4]):1.f;
    (void)material;
    return finishCompactRigidUniforms(rigid,{1.f,1.f});
}

bool decodePositionTransformBatch(const VertexDecodePlan& plan,std::span<const u8> packed,const VertexArrays& arrays,
    std::span<GpuVertex> output,const CommandRegisters& r,const std::array<float,16>& projection){
    if(!plan.stride||output.empty()||output.size()>packed.size()/plan.stride||!plan.count||plan.count>plan.attributes.size())return false;
    const VertexDecodePlan::Attribute* position=nullptr;
    const VertexDecodePlan::Attribute* selector=nullptr;
    for(unsigned i=0;i<plan.count;++i){
        const auto& a=plan.attributes[i];
        if(a.attribute<=GX_VA_TEX7MTXIDX){
            if(a.descriptor!=GX_DIRECT)return false;
            if(a.attribute==GX_VA_PNMTXIDX)selector=&a;
            continue;
        }
        if((a.descriptor!=GX_INDEX8&&a.descriptor!=GX_INDEX16)||a.nbt3||
            arrays[a.attribute].littleEndian!=(std::endian::native==std::endian::little))return false;
        if(a.attribute==GX_VA_POS){
            if(a.components!=3||(a.type!=GX_F32&&a.type!=GX_S16&&a.type!=GX_S8))return false;
            position=&a;
        }
    }
    if(!position||(!selector&&!r.matrixIndexValid[0]))return false;
    for(float value:projection)if(!std::isfinite(value))return false;
    // Every indexed attribute keeps its bounds check. Only the position is
    // consumed here, so other attributes are not read or value-checked.
    IndexedGather positions;std::array<IndexedGather,GX_VA_TEX7+1> others{};unsigned otherCount=0;
    for(unsigned i=0;i<plan.count;++i){
        const auto& a=plan.attributes[i];if(a.attribute<=GX_VA_TEX7MTXIDX)continue;
        auto& gather=&a==position?positions:others[otherCount++];
        if(!gather.prepare(a.offset,a.descriptor==GX_INDEX16,arrays[a.attribute],a.components*a.scalarSize))return false;
    }
    // Same arithmetic order as transformVertex, so clip positions stay
    // bit-identical across paths. One final finiteness check suffices: any
    // nonfinite input, palette or view term reaches a clip component.
    std::array<std::array<float,12>,10> matrices;
    unsigned prepared=0;
    const float scale=position->scale;const auto type=position->type;
    for(auto& target:output){
        const u8* pos=positions.read(packed.data());
        if(!pos)return false;
        for(unsigned i=0;i<otherCount;++i)if(!others[i].read(packed.data()))return false;
        float input[4]{0,0,0,1};
        if(type==GX_F32)std::memcpy(input,pos,12);
        else if(type==GX_S16)for(unsigned c=0;c<3;++c){std::int16_t value;std::memcpy(&value,pos+2*c,2);input[c]=float(value)*scale;}
        else for(unsigned c=0;c<3;++c)input[c]=float(std::int8_t(pos[c]))*scale;
        // Valid position selectors are 0,3,...,27; test them without a divide.
        const unsigned selected=selector?packed[selector->offset]:r.matrixIndex[0]&63;
        if(selected>27||!((0x09249249u>>selected)&1))return false;
        const unsigned slot=selected/3;auto& matrix=matrices[slot];
        if(!(prepared&(1u<<slot))){
            for(unsigned c=0;c<12;++c){
                const unsigned at=4*selected+c;if(!r.xfValid[at])return false;
                matrix[c]=std::bit_cast<float>(r.xf[at]);
            }
            prepared|=1u<<slot;
        }
        // Unrolled in the generic loops' exact accumulation order.
        const float* m=matrix.data();float view[3];
        for(unsigned row=0;row<3;++row,m+=4){
            float total=0;total+=m[0]*input[0];total+=m[1]*input[1];total+=m[2]*input[2];total+=m[3]*input[3];
            view[row]=total;
        }
        const float* p=projection.data();float clip[4];
        for(unsigned row=0;row<4;++row,p+=4){
            float total=p[3];total+=p[0]*view[0];total+=p[1]*view[1];total+=p[2]*view[2];
            clip[row]=total;
        }
        if(!std::isfinite(clip[0])||!std::isfinite(clip[1])||!std::isfinite(clip[2])||!std::isfinite(clip[3]))return false;
        target.position={clip[0],clip[1],clip[2],clip[3]};
        target.color={};target.texcoord={};target.textureQ=1;
        packed=packed.subspan(plan.stride);
    }
    return true;
}

}
