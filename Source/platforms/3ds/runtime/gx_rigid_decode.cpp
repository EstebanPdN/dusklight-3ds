#include "gx_vertex_decode.hpp"
#include "gx_raster_color.hpp"
#include "gx_gpu_geometry.hpp"
#include "gx_profile.hpp"
#include "gx_rigid_vertex.hpp"
#include "gx_skin_palette.hpp"
#include "gx_vertex_warp.hpp"
#include "gx_compact_cache.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#if defined(__3DS__)
extern "C" void OSReport(const char*,...);
#endif
namespace dusk::native::gx {
thread_local StaticGeometryHint staticGeometryHint{};
namespace {
StaticCompactCache* activeStaticCache=nullptr;
#if defined(__3DS__)
// Diagnostics: one line per pool clear, printed by the first static list
// drawn after it (at most 160 lines), so stage clears and full-pool resets
// can be told apart in a run log.
std::uint32_t reportedStaticEpoch{};
unsigned staticClearLines{};
void reportStaticClear(const StaticCompactCache& cache){
    reportedStaticEpoch=cache.epoch;
    if(staticClearLines>=160)return;
    ++staticClearLines;
    static constexpr const char* reasons[]{"start","full","stage"};
    OSReport("Native static geometry cleared reason=%s bytes-before=%u stage-clears=%llu full-resets=%llu palette hits=%llu stores=%llu arena=%llu replays=%llu refusals=%llu\n",
        reasons[unsigned(cache.lastClear)%3],unsigned(cache.clearedBytes),static_cast<unsigned long long>(cache.stageClears),
        static_cast<unsigned long long>(cache.resets),static_cast<unsigned long long>(cache.paletteHits),
        static_cast<unsigned long long>(cache.paletteStores),static_cast<unsigned long long>(cache.paletteArena),
        static_cast<unsigned long long>(cache.paletteReplays),static_cast<unsigned long long>(cache.paletteRefusals));
}
#endif
}
StaticCompactCache* staticCompactCache(){return activeStaticCache;}
void setStaticCompactCache(StaticCompactCache* cache){activeStaticCache=cache;}
}
// Called by J3DShape around each display list of a static model (generation
// nonzero) and after it (zero). Batches inside one list are numbered by the
// command processor in submission order.
extern "C" void duskNativeStaticGeometry(std::uint32_t generation,const void* list){
    using namespace dusk::native::gx;
    auto& hint=staticGeometryHint;
    if(!generation&&hint.list&&staticListEnded)staticListEnded(hint.list,hint.batch);
    hint={generation,generation?list:nullptr,0};
#if defined(__3DS__)
    if(generation&&activeStaticCache&&activeStaticCache->epoch!=reportedStaticEpoch)reportStaticClear(*activeStaticCache);
#endif
}
namespace dusk::native::gx {
namespace {
// Keep arbitrary GX array alignment legal, while exposing naturally aligned
// native arrays to the compiler. ARM11 otherwise assembles these tiny copies
// one byte at a time even for the original aligned position/normal streams.
template<unsigned Bytes,unsigned Alignment>
[[gnu::always_inline]] inline void copyNative(void* destination,const void* source){
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
    [[gnu::always_inline]] const u8* read(const u8* packed)const{
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
// Two components (GX_POS_XY: flat effect meshes, some water planes) read as
// z = 0, as GX does. Until Alpha 8 they were refused (decode reject 12 for
// floats) and took the CPU vertex route.
template<class Attribute>
bool acceptPosition(const Attribute& a){
    return (a.components==3||a.components==2)&&(a.type==GX_F32||a.type==GX_S16||a.type==GX_U16||a.type==GX_S8||a.type==GX_U8);
}
template<class Attribute>
bool readPosition(const Attribute& a,const u8* s,float* o){
    o[2]=0;
    for(unsigned c=0;c<a.components;++c){
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
// F32 as fixed point with `bits` fraction bits: 10 (1/1024, |uv| < 32) unless
// the caller asks for fewer (decodeCompactRigidBatchMT); a value that does
// not fit is refused.
constexpr unsigned floatTexcoordBits=10;
template<class Attribute>
bool acceptTexcoord(const Attribute& a){
    return a.components==2&&(a.type==GX_S16||a.type==GX_S8||a.type==GX_U8||a.type==GX_U16||a.type==GX_F32);
}
template<class Attribute>
float texcoordScale(const Attribute& a,unsigned bits=floatTexcoordBits,bool halved=false){
    return a.type==GX_F32?1.f/float(1u<<bits):a.type==GX_U16&&halved?2*a.scale:a.scale;
}
// `halved`: a U16 coordinate is stored without its lowest bit, so that the
// values above 32767 fit (texcoordScale doubles the scale).
template<class Attribute>
bool readTexcoord(const Attribute& a,const u8* s,std::int16_t* o,unsigned bits=floatTexcoordBits,bool halved=false){
    switch(a.type){
    case GX_S16:copyNative<4,2>(o,s);return true;
    case GX_S8:o[0]=std::int8_t(s[0]);o[1]=std::int8_t(s[1]);return true;
    case GX_U8:o[0]=s[0];o[1]=s[1];return true;
    case GX_U16:for(unsigned c=0;c<2;++c){std::uint16_t v;std::memcpy(&v,s+2*c,2);if(halved)v>>=1;if(v>32767)return false;o[c]=std::int16_t(v);}return true;
    default:{const float unit=float(1u<<bits);
        for(unsigned c=0;c<2;++c){float v;std::memcpy(&v,s+4*c,4);v*=unit;if(!std::isfinite(v)||std::abs(v)>32767.f)return false;o[c]=std::int16_t(std::lround(v));}
        return true;}
    }
}
template<class Attribute>
unsigned elementBytes(unsigned slot,const Attribute& a){return slot==2?colorBytes(a.type):a.scalarSize*a.components;}
// Origin of the positions of one compact record set (RigidVertexPlan::origin):
// the first vertex on a 256-unit grid, zero near the world origin so models,
// 2D and effects keep their coordinates. The same source data always gives
// the same origin, so cached and GPU-resident records stay valid. The
// subtraction is exact or rounds far below the PICA's own 24-bit precision.
inline std::array<float,3> compactOrigin(const float* first){
    std::array<float,3> origin{};
    for(unsigned c=0;c<3;++c)if(std::abs(first[c])>=512.f)origin[c]=256.f*std::round(first[c]/256.f);
    return origin;
}
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
// The readers of the multi-texture decoder's vertex loop. They take the
// element where it lies, in its own byte order (`swapped`: direct attributes,
// which GX packs in the console's order, and arrays stored that way), and
// handle the formats game code and models use most without the intermediate
// copy of littleElement or a library call per value; every other format goes
// through the readers above. The results are theirs bit for bit. Until
// Alpha 8 a vertex decoded each picture (flowers, grass, ropes: 4,000 to
// 7,000 a picture outdoors) cost about 2.8 microseconds here.
// They are always inlined, as are the decoder's field passes (`each` there):
// every one is read by several loops, and a reader the compiler leaves as a
// function costs a call and a format test for every vertex (Alpha 9, when
// the per-vertex warp became a second reader of positions: 0.39 to 0.53
// microseconds a vertex in every zone). Tools/3ds/check_hot_inlining.py
// fails the build when the image holds one of them as a function.
// lround's value for a coordinate already scaled, without the call: false
// when it is not finite or does not fit 16 bits (as readTexcoord).
[[gnu::always_inline]] inline bool roundCoordinate(float value,std::int16_t& out){
    if((std::bit_cast<std::uint32_t>(value)&0x7fffffffu)>std::bit_cast<std::uint32_t>(32767.f))return false;
    const int whole=int(value);
    const float rest=value-float(whole); // exact: |value| is far below 2^23
    out=std::int16_t(whole+(rest>=.5f)-(rest<=-.5f));
    return true;
}
template<class Attribute>
[[gnu::always_inline]] inline bool readPositionAt(const Attribute& a,const u8* s,bool swapped,float* o){
    if(a.type==GX_F32){
        std::uint32_t w[3]{};
        if(a.components==3)std::memcpy(w,s,12);else std::memcpy(w,s,8);
        if(swapped){w[0]=__builtin_bswap32(w[0]);w[1]=__builtin_bswap32(w[1]);w[2]=__builtin_bswap32(w[2]);}
        constexpr std::uint32_t limit=std::bit_cast<std::uint32_t>(1e7f);
        if((w[0]&0x7fffffffu)>limit||(w[1]&0x7fffffffu)>limit||(w[2]&0x7fffffffu)>limit)return false;
        std::memcpy(o,w,12);
        return true;
    }
    u8 scratch[16];
    return readPosition(a,swapped?littleElement(0,a,s,scratch):s,o);
}
template<class Attribute>
[[gnu::always_inline]] inline bool readNormalAt(const Attribute& a,const u8* s,bool swapped,std::int16_t* o){
    if(a.type==GX_S8){o[0]=std::int8_t(s[0]);o[1]=std::int8_t(s[1]);o[2]=std::int8_t(s[2]);return true;}
    if(a.type==GX_S16&&swapped){
        for(unsigned c=0;c<3;++c)o[c]=std::int16_t(std::uint16_t((unsigned(s[2*c])<<8)|s[2*c+1]));
        return true;
    }
    if(a.type==GX_F32){
        // Grass and flowers index a table of float normals for every vertex.
        std::uint32_t w[3];std::memcpy(w,s,12);
        if(swapped){w[0]=__builtin_bswap32(w[0]);w[1]=__builtin_bswap32(w[1]);w[2]=__builtin_bswap32(w[2]);}
        return roundCoordinate(std::bit_cast<float>(w[0])*16384.f,o[0])&&roundCoordinate(std::bit_cast<float>(w[1])*16384.f,o[1])&&
               roundCoordinate(std::bit_cast<float>(w[2])*16384.f,o[2]);
    }
    u8 scratch[16];
    return readNormal(a,swapped?littleElement(1,a,s,scratch):s,o);
}
template<class Attribute>
[[gnu::always_inline]] inline bool readTexcoordAt(const Attribute& a,const u8* s,bool swapped,std::int16_t* o,unsigned bits,bool halved){
    switch(a.type){
    case GX_S8:o[0]=std::int8_t(s[0]);o[1]=std::int8_t(s[1]);return true;
    case GX_U8:o[0]=s[0];o[1]=s[1];return true;
    case GX_S16:
        if(!swapped){copyNative<4,2>(o,s);return true;}
        o[0]=std::int16_t(std::uint16_t((unsigned(s[0])<<8)|s[1]));o[1]=std::int16_t(std::uint16_t((unsigned(s[2])<<8)|s[3]));
        return true;
    case GX_F32:{
        std::uint32_t w[2];std::memcpy(w,s,8);
        if(swapped){w[0]=__builtin_bswap32(w[0]);w[1]=__builtin_bswap32(w[1]);}
        const float unit=float(1u<<bits);
        return roundCoordinate(std::bit_cast<float>(w[0])*unit,o[0])&&roundCoordinate(std::bit_cast<float>(w[1])*unit,o[1]);
    }
    default:{u8 scratch[16];return readTexcoord(a,swapped?littleElement(3,a,s,scratch):s,o,bits,halved);}
    }
}
// Consecutive XYZ/RGBA vertices used by the sky, and by vegetation with
// a single normal and byte-indexed UVs. Fill a compact record in one pass:
// several field passes over thousands of records exceed the ARM11 data cache.
template<bool indexedUV>
bool packPositionColor(std::span<const u8> packed,std::span<std::byte> target,
                       const std::array<float,3>& origin,const IndexedGather& uv,
                       const std::int16_t (&normal)[3]){
    constexpr unsigned stride=indexedUV?18:16;
    CompactRigidVertexMT record;
    std::copy_n(normal,3,record.normal.begin());
    std::array<std::uint32_t,8> known{};
    std::array<std::array<std::int16_t,2>,256> coordinates;
    const u8* src=packed.data();
    auto* dst=static_cast<std::byte*>(__builtin_assume_aligned(target.data(),alignof(CompactRigidVertexMT)));
    for(std::size_t n=0;n<target.size()/sizeof(record);++n,src+=stride,dst+=sizeof(record)){
        for(unsigned c=0;c<3;++c){
            std::uint32_t w;
            std::memcpy(&w,src+4*c,4);
            const float value=std::bit_cast<float>(__builtin_bswap32(w));
            if(!boundedPosition(value))return false;
            record.position[c]=value-origin[c];
        }
        if constexpr(indexedUV){
            const unsigned idx=src[17];
            if(src[12]!=0||idx>uv.limit)return false;
            const auto bit=std::uint32_t(1)<<(idx%32);
            if(!(known[idx/32]&bit)){
                float raw[2];std::memcpy(raw,uv.data+idx*uv.stride,sizeof(raw));
                if(!roundCoordinate(raw[0]*1024.f,coordinates[idx][0])||
                   !roundCoordinate(raw[1]*1024.f,coordinates[idx][1]))return false;
                known[idx/32]|=bit;
            }
            record.texcoord0=coordinates[idx];
        }
        std::memcpy(record.color.data(),src+(indexedUV?13:12),4);
        std::memcpy(dst,&record,sizeof(record));
    }
    return true;
}
// A texture coordinate as GX reads it (the stored number times the
// attribute's scale), for the per-vertex warp. False when it is not finite.
template<class Attribute>
inline bool readTexcoordValueAt(const Attribute& a,const u8* s,bool swapped,float* o){
    switch(a.type){
    case GX_S8:o[0]=float(std::int8_t(s[0]))*a.scale;o[1]=float(std::int8_t(s[1]))*a.scale;return true;
    case GX_U8:o[0]=float(s[0])*a.scale;o[1]=float(s[1])*a.scale;return true;
    case GX_S16:case GX_U16:
        for(unsigned c=0;c<2;++c){
            std::uint16_t v;std::memcpy(&v,s+2*c,2);
            if(swapped)v=__builtin_bswap16(v);
            o[c]=(a.type==GX_S16?float(std::int16_t(v)):float(v))*a.scale;
        }
        return true;
    default:{
        std::uint32_t w[2];std::memcpy(w,s,8);
        if(swapped){w[0]=__builtin_bswap32(w[0]);w[1]=__builtin_bswap32(w[1]);}
        constexpr std::uint32_t limit=std::bit_cast<std::uint32_t>(1e8f);
        if((w[0]&0x7fffffffu)>limit||(w[1]&0x7fffffffu)>limit)return false;
        std::memcpy(o,w,8);return true;
    }
    }
}
[[gnu::always_inline]] inline void readColorAt(unsigned type,const u8* s,bool swapped,std::uint8_t* o){
    if(type==GX_RGBA8){std::memcpy(o,s,4);return;}
    // Only the packed 16 and 24-bit colours have a byte order.
    u8 scratch[4];
    if(swapped&&(type==GX_RGB565||type==GX_RGBA4)){scratch[0]=s[1];scratch[1]=s[0];s=scratch;}
    else if(swapped&&type==GX_RGBA6){scratch[0]=s[2];scratch[1]=s[1];scratch[2]=s[0];s=scratch;}
    readColor(type,s,o);
}
// A compact palette's layout follows live matrix values (shared normal rows),
// so its native record can be refused on a later frame. Only the first batch
// of a list hands out resident vertices and so becomes a record: a refused
// first batch sends the whole list down the normal path, while a refused
// later batch would leave the rest of the list undrawn for that frame.
// Later batches still reuse the decoded entry, copied into the frame arena
// (as every compact batch does while the replay cannot place them).
inline bool exposesResident(const RigidVertexPlan& rigid){
    return !rigid.compactPalette||(compactPaletteRecords&&staticGeometryHint.batch==1);
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
    // No CLR0: a consumed vertex colour reads missingVertexColor (white).
    if(!fields[0]||!fields[1]||!fields[3]||(color.ambientVertex&2))return false;
    if((color.vertex|color.ambientVertex)&&!fields[2])++gxMissingColorDecodes;
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
        const u8* vertexColor=fields[2]?source[2]:missingVertexColorBytes.data();
        if((color.vertex|color.ambientVertex)&1)for(unsigned c=0;c<3;++c)v.color[c]=vertexColor[c]/255.f;
        if(color.vertex&2)v.color[3]=vertexColor[3]/255.f*alphaScale;
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
        for(unsigned unit=0;unit<3;++unit)if(multiRole(rigid,unit)==1){
            const unsigned base=multiUnitRow(unit);
            for(unsigned row:{0u,2u})for(unsigned c=0;c<2;++c)rigid.uniforms[base+row][c]*=rigid.decodedTex0Scale;
            for(unsigned row:{1u,3u})for(unsigned c=0;c<2;++c)rigid.uniforms[base+row][c]*=rigid.decodedTex1Scale;
            // The third coordinate of a wide record: z and w of the same rows.
            if(rigid.wideRecord)for(unsigned row:{1u,3u})for(unsigned c=2;c<4;++c)rigid.uniforms[base+row][c]*=rigid.decodedTex2Scale;
        }
    }else{
        rigid.uniforms[13][2]=rigid.decodedTex0Scale*scale[0];rigid.uniforms[13][3]=rigid.decodedTex0Scale*scale[1];
    }
    // The records hold positions relative to rigid.origin: every row applied
    // to (position,1) takes the origin into its constant, M.(p-o,1) + M.o.
    // A draw reaches this once, with rows freshly placed from the registers;
    // foldedOrigin keeps a second pass over the same plan from adding it again.
    const double o[3]{double(rigid.origin[0])-rigid.foldedOrigin[0],double(rigid.origin[1])-rigid.foldedOrigin[1],double(rigid.origin[2])-rigid.foldedOrigin[2]};
    if(o[0]!=0||o[1]!=0||o[2]!=0){
        // First pass proves every result finite; nothing changes on failure.
        for(const bool apply:{false,true}){
            bool finite=true;
            auto fold=[&](unsigned row){
                auto& m=rigid.uniforms[row];
                const float value=float(double(m[3])+double(m[0])*o[0]+double(m[1])*o[1]+double(m[2])*o[2]);
                if(apply)m[3]=value;else finite&=std::isfinite(value)&&std::abs(value)<=1e8f;
            };
            for(unsigned m=0;m<rigid.matrixOffsets.size();++m){
                // Identical matrices share rows (addRigidMatrix): once each.
                const int offset=rigid.matrixOffsets[m];bool earlier=false;
                for(unsigned k=0;k<m;++k)earlier|=rigid.matrixOffsets[k]==offset;
                if(offset>=0&&!earlier)for(unsigned row=0;row<3;++row)fold(unsigned(offset)+row);
            }
            for(unsigned k=0;k<rigid.skinEntries;++k)for(unsigned row=0;row<3;++row)fold(skin::row(k)+row);
            if(rigid.multiTexture){if(rigid.multiProjected)for(unsigned row=0;row<3;++row)fold(multiUnitRow(0)+row);}
            else if(rigid.projectedTexture)for(unsigned row=0;row<3;++row)fold(rigid.projectionBase()+row);
            if(!finite)return false;
        }
        rigid.foldedOrigin=rigid.origin;
    }
    rigid.uniforms[11][3]=rigid.decodedNormalScale;
    // u[12].w: the single-texture shader's switch for compact records; in
    // the multi-texture shader, whose records are always compact, the switch
    // of the paired normal texgens (gx_rigid_vertex.hpp).
    rigid.uniforms[12][3]=rigid.multiTexture?(rigid.pairedUnits?1.f:0.f):1.f;
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
    // No CLR0: the records carry missingVertexColor (white), read only when
    // the plan consumes the vertex colour (u[14].w, u[15].w, u[17].w).
    if(!fields[0]||!fields[1]||!fields[3]||(rigid.color.ambientVertex&2)||bool(matrix)!=rigid.indexedMatrices)return false;
    if((rigid.color.vertex|rigid.color.ambientVertex)&&!fields[2])++gxMissingColorDecodes;
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
    }else for(int offset:rigid.matrixOffsets)if(offset < -1||(offset>0&&
       (offset<int(paletteBase)||offset+6>93||(offset-paletteBase)%6)))return false;
    // The records' v4 follows the palette layout, which a compact palette
    // takes from live matrix values: both caches key on the offsets and the
    // shared normal rows, so another layout is another entry.
    std::uint16_t sharedNormals=0;
    const bool layoutKeyed=sharedNormalMask(rigid.matrixOffsets,rigid.normalOffsets,sharedNormals);
    if(!layoutKeyed)cache=nullptr;
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
    cacheKey[31]=sharedNormals; // words 28..30 end with two free bytes
    std::array<IndexedGather,4> gather{};
    for(unsigned slot=0;slot<4;++slot)if(const auto* a=fields[slot])
        if(!gather[slot].prepare(a->offset,a->descriptor==GX_INDEX16,arrays[a->attribute],elementBytes(slot,*a)))return false;
    std::array<float,3> origin{};
    {float first[3];const u8* at=gather[0].read(packed.data());
     if(!at||!readPosition(*fields[0],at,first))return false;
     origin=compactOrigin(first);}
    // Static J3D geometry: trusted by generation, no byte comparison.
    StaticCompactCache* staticCache=layoutKeyed&&staticGeometryHint.generation&&staticGeometryHint.list?activeStaticCache:nullptr;
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
        staticKey.sharedNormals=sharedNormals;
        cache=nullptr; // static draws never touch the validating cache
    }
    bool reused=false;
    if(staticCache)if(const auto* saved=staticCache->find(staticKey,target.size())){
        // Resident: the GPU binds the cached vertices; the arena copy is skipped.
        if(staticCache->resident()&&exposesResident(rigid))rigid.staticVertices=saved;
        else std::memcpy(target.data(),saved,target.size());
        reused=true;
        if(rigid.compactPalette){++staticCache->paletteHits;if(staticCache->resident()&&!rigid.staticVertices)++staticCache->paletteArena;}
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
        if(fields[2])readColor(fields[2]->type,source[2],v.color.data());else v.color=missingVertexColorBytes;
        if(!readPosition(*fields[0],source[0],v.position.data())||!readNormal(*fields[1],source[1],v.normal.data())||
           !readTexcoord(*fields[3],source[3],v.texcoord.data()))return false;
        for(unsigned c=0;c<3;++c)v.position[c]-=origin[c];
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
    if(staticCache&&!reused&&staticCache->store(staticKey,target)&&rigid.compactPalette)++staticCache->paletteStores;
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
    rigid.origin=origin;
    (void)material;
    // A failed finish (origin fold out of range) sends the batch to the slow
    // path, which must not take resident vertices handed out above.
    if(!finishCompactRigidUniforms(rigid,scale)){rigid.staticVertices=nullptr;return false;}
    return true;
}

namespace {
// The per-vertex warp of a draw (RigidVertexPlan::vertexWarp): for every
// vertex, the warp map read at the vertex's map coordinate, written as two
// shorts at `out` in its record (gx_vertex_warp.hpp). `a` is the attribute
// the map's coordinate is generated from, read where the decoder's field
// passes read theirs. 0 done, 1 an index outside its array, 2 an element
// that is not finite.
// Its own function: inside the decoder its loops changed how the compiler
// laid out the field passes (the grass decode went from 1.44 to 1.86 ms).
template<class Attribute>
[[gnu::noinline]] int writeVertexWarps(const VertexWarpPlan& warp,const Attribute& a,bool direct,bool swapped,
    const IndexedGather& indexed,const u8* packed,std::size_t packedStride,std::size_t vertices,std::byte* out,std::size_t recordBytes){
    const VertexWarpSampler sampler(warp);
    const bool position=a.attribute==GX_VA_POS;
    // The water's form: an indexed integer coordinate, no float in the pass.
    if(!direct&&!swapped&&!position){
        const int result=sampler.runTurns(a.type,a.scale,indexed.data,indexed.stride,indexed.limit,indexed.wide,packed+indexed.offset,packedStride,
                                          vertices,out,recordBytes);
        if(result>=0){vertexWarpIntegerVertices+=vertices;return result;}
    }
    auto one=[&](const u8* source,std::byte* at){
        float in[3];
        if(position){if(!readPositionAt(a,source,swapped,in))return false;}
        else{if(!readTexcoordValueAt(a,source,swapped,in))return false;in[2]=1;}
        sampler.store(in,at);return true;
    };
    if(direct){
        const u8* source=packed+a.offset;
        for(std::size_t n=0;n<vertices;++n,source+=packedStride,out+=recordBytes)if(!one(source,out))return 2;
        return 0;
    }
    const u8* vertex=packed;
    for(std::size_t n=0;n<vertices;++n,vertex+=packedStride,out+=recordBytes){
        const u8* const source=indexed.read(vertex);
        if(!source)return 1;
        if(!one(source,out))return 2;
    }
    return 0;
}
// Warps that were sampled before. Most warps of a room do not move: the
// map's coordinate has no animated matrix and the warp matrix stands still,
// so a mesh gets the same numbers picture after picture (Lakebed Temple: 95
// to 99 % of the warped vertices of three rooms, 5,000 to 9,000 a picture).
// The numbers of a static batch are kept here, in memory of their own, with
// the warp they were sampled under; a later draw of the batch under that
// same warp copies them (5 instructions a vertex instead of 120), and one
// under another warp samples again into the same place, so a warp that does
// move takes no more room than one that does not. A warp whose map may be
// rewritten is kept for the picture it was sampled in only. The store is
// emptied when it is full and when the static cache starts again.
struct WarpStore {
    struct Entry {
        std::uint64_t mesh{},warp{};   // which vertices under which map; the whole plan
        std::uint32_t offset{},count{},frame{};
        bool used{},anyPicture{};
    };
    static constexpr std::size_t slots=128,probes=4,capacity=16384; // entries, vertices
    std::array<Entry,slots> entries{};
    std::array<std::int16_t,2*capacity> values{};
    std::uint32_t usedVertices{},epoch{};
    void clear(std::uint32_t newEpoch){for(auto& e:entries)e.used=false;usedVertices=0;epoch=newEpoch;}
    Entry* entry(std::uint64_t mesh){
        for(std::size_t probe=0,at=std::size_t(mesh^(mesh>>32))&(slots-1);probe<probes;++probe,at=(at+1)&(slots-1))
            if(entries[at].used&&entries[at].mesh==mesh)return &entries[at];
        return nullptr;
    }
    // The kept numbers of this mesh if they were sampled under this warp.
    const std::int16_t* find(std::uint64_t mesh,std::uint64_t warp,std::size_t count){
        const auto* e=entry(mesh);
        return e&&e->warp==warp&&e->count==count&&(e->anyPicture||e->frame==textureFrameCounter)?values.data()+2*e->offset:nullptr;
    }
    // Where to keep the numbers of this mesh under this warp: its own place
    // again, or a new one; null when there is none (the draw is not kept).
    std::int16_t* keep(std::uint64_t mesh,std::uint64_t warp,std::size_t count,bool anyPicture){
        auto* e=entry(mesh);
        if(e&&e->count!=count){e->used=false;e=nullptr;} // its room is given up until the store starts again
        if(!e){
            if(count>capacity-usedVertices){clear(epoch);if(count>capacity)return nullptr;}
            for(std::size_t probe=0,at=std::size_t(mesh^(mesh>>32))&(slots-1);probe<probes&&!e;++probe,at=(at+1)&(slots-1))
                if(!entries[at].used)e=&entries[at];
            if(!e)return nullptr;
            *e={mesh,warp,usedVertices,std::uint32_t(count),textureFrameCounter,true,anyPicture};
            usedVertices+=std::uint32_t(count);
        }else{e->warp=warp;e->frame=textureFrameCounter;e->anyPicture=anyPicture;}
        return values.data()+2*e->offset;
    }
};
WarpStore warpStore{};
// FNV-1a over fields (not the padding between them).
struct WarpHash {
    std::uint64_t value=14695981039346656037ull;
    void mix(std::uint64_t v){value=(value^v)*1099511628211ull;}
    void real(float v){mix(std::bit_cast<std::uint32_t>(v));}
};
// What stays the same from picture to picture when a warp moves: the batch
// and the map it reads, not the rows or the matrix.
std::uint64_t warpMeshKey(const StaticCompactCache::Key& batch,const VertexWarpPlan& w){
    WarpHash h;
    h.mix(batch.generation);h.mix(batch.batch);h.mix(batch.count);h.mix(batch.stride);h.mix(batch.layout);h.mix(batch.list);
    for(const auto offset:batch.offsets)h.mix(std::uint8_t(offset));
    h.mix(batch.sharedNormals);
    h.mix(reinterpret_cast<std::uintptr_t>(w.levels[0].texels));h.mix(w.levels[0].bytes);h.mix(w.sourceAttribute);h.mix(w.format);
    return h.value;
}
// Everything of a plan that decides the stored numbers.
std::uint64_t warpPlanKey(const VertexWarpPlan& w){
    WarpHash h;
    h.mix(w.sourceAttribute);h.mix(w.projected);h.mix(w.format);h.mix(w.wrapS);h.mix(w.wrapT);
    for(const auto& row:w.rows)for(float value:row)h.real(value);
    for(const auto& level:w.levels){
        h.mix(reinterpret_cast<std::uintptr_t>(level.texels));h.mix(level.bytes);h.mix(level.width);h.mix(level.height);h.mix(level.linear);
    }
    h.real(w.blend);
    for(const auto& row:w.functional)for(float value:row)h.real(value);
    for(float value:w.mean)h.real(value);
    h.real(w.recordScale);h.mix(w.constantMap);
    return h.value;
}
}
namespace {
// The warps of a draw into its records (`warps`: the second coordinate of
// the first record): copied from the store when this static batch was
// sampled under this warp before, sampled and kept otherwise. Results as
// writeVertexWarps. Its own function, like writeVertexWarps: the decoder
// below must stay as small as its field passes need (see `each`).
template<class Attribute>
[[gnu::noinline]] int placeVertexWarps(const VertexWarpPlan& warp,const Attribute& a,bool direct,bool swapped,const IndexedGather& indexed,
    const u8* packed,std::size_t packedStride,std::size_t count,std::byte* warps,std::size_t recordBytes,
    const StaticCompactCache* staticCache,const StaticCompactCache::Key& staticKey){
    const bool keepable=vertexWarpRepeats&&staticCache&&count<=WarpStore::capacity;
    const std::int16_t* kept=nullptr;
    std::uint64_t mesh=0,key=0;
    if(keepable){
        if(warpStore.epoch!=staticCache->epoch)warpStore.clear(staticCache->epoch);
        mesh=warpMeshKey(staticKey,warp);key=warpPlanKey(warp);
        kept=warpStore.find(mesh,key,count);
    }
    if(kept){
        std::byte* to=warps;
        for(std::size_t n=0;n<count;++n,kept+=2,to+=recordBytes)std::memcpy(to,kept,4);
        ++vertexWarpRepeatDraws;vertexWarpRepeatVertices+=count;
        return 0;
    }
    const int result=writeVertexWarps(warp,a,direct,swapped,indexed,packed,packedStride,count,warps,recordBytes);
    if(result)return result;
    if(keepable)if(std::int16_t* to=warpStore.keep(mesh,key,count,warp.constantMap)){
        const std::byte* from=warps;
        for(std::size_t n=0;n<count;++n,from+=recordBytes,to+=2)std::memcpy(to,from,4);
    }
    return 0;
}
}
bool decodeCompactRigidBatchMT(const VertexDecodePlan& plan,std::span<const u8> packed,const VertexArrays& arrays,
    std::span<GpuVertex> output,RigidVertexPlan& rigid){
    rigid.staticVertices=nullptr;rigid.compact=false;
    auto reject=[](unsigned code){++gxCompactDecodeRejects[code&63];return false;};
    // Profiled pictures: this decode, by vertex format (gxDecodeCensus).
#if defined(__3DS__)
    struct Census {
        const VertexDecodePlan& plan;std::size_t vertices;bool active,done{},reused{};std::uint64_t start;
        ~Census(){
            if(!active||!done)return;
            const std::uint64_t ticks=gxProfileTick()-start;
            char text[76];plan.describe(text,sizeof(text));
            std::uint32_t kind=2166136261u;
            for(const char* c=text;*c;++c)kind=(kind^std::uint8_t(*c))*16777619u;
            kind|=1;
            for(auto& e:gxDecodeCensus)if(!e.kind||e.kind==kind){
                if(!e.kind){e.kind=kind;std::memcpy(e.text,text,sizeof(text));}
                ++e.draws;e.vertices+=std::uint32_t(vertices);e.reused+=reused;e.ticks+=ticks;return;
            }
        }
    } census{plan,output.size(),gxSparseVertexProfiling,false,false,gxSparseVertexProfiling?gxProfileTick():0};
#endif
    if(!rigid.multiTexture)return reject(56);
    if(!plan.stride||output.empty()||output.size()>packed.size()/plan.stride)return reject(57);
    // One attribute (position only: unlit untextured or position-projected
    // draws) is a complete layout: absent fields stay constant in the records
    // (zero normal and coordinates, white colour); the field checks below
    // refuse a plan that needs a normal or a coordinate. Until 2026-11-16
    // these formats were reject 58, then the CPU vertex route (switch
    // gxCompactSingleAttribute).
    if(plan.count<(gxCompactSingleAttribute?1:2)||plan.count>plan.attributes.size())return reject(58);
    // Fields: 0 POS, 1 NRM, 2 CLR0, 3 and 4 the record's two coordinates
    // (TEX0 and TEX1 unless the plan says otherwise), 5 the third coordinate
    // of a wide record: the fields a record stores. 6 is the coordinate a
    // per-vertex warp reads its map with (RigidVertexPlan::vertexWarp) when
    // none of the others is that attribute: read, never stored (the warp
    // itself takes the place of the second coordinate).
    constexpr unsigned recordFields=6,fieldCount=7;
    const VertexDecodePlan::Attribute* fields[fieldCount]{};
    const VertexDecodePlan::Attribute* matrix=nullptr;
    // Texture matrix selectors the plan took as following the position
    // selector (RigidVertexPlan::pairedSelectors): checked in every vertex.
    const VertexDecodePlan::Attribute* paired[8]{};unsigned pairedCount=0;
    // Indexed arrays in the other byte order (tables built into the game in
    // the console's order: the volume of the simple shadow, one or two draws
    // in every picture) are converted element by element, as direct
    // attributes are. Until Alpha 8 they were reject 5, and since the
    // multi-texture shader has no other record, the CPU vertex route.
    std::array<bool,fieldCount> swappedArray{};
    for(unsigned i=0;i<plan.count;++i){
        const auto& a=plan.attributes[i];unsigned slot;
        if(a.attribute<=GX_VA_TEX7MTXIDX){
            if(a.descriptor!=GX_DIRECT)return reject(1);
            // Texture matrix selectors: the plan refused the draw if a
            // coordinate it reads has one that is not paired; the others
            // are skipped.
            if(a.attribute==GX_VA_PNMTXIDX)matrix=&a;
            else if(rigid.pairedSelectors&(1u<<(a.attribute-GX_VA_TEX0MTXIDX)))paired[pairedCount++]=&a;
            continue;
        }
        if(a.attribute==GX_VA_POS)slot=0;else if(a.attribute==GX_VA_NRM)slot=1;else if(a.attribute==GX_VA_CLR0)slot=2;
        else if(a.attribute==rigid.texcoordAttribute[0])slot=3;else if(a.attribute==rigid.texcoordAttribute[1])slot=4;
        else if(rigid.wideRecord&&a.attribute==rigid.texcoordAttribute[2])slot=5;
        else if(rigid.vertexWarp.enabled&&a.attribute==rigid.vertexWarp.sourceAttribute)slot=6;
        // Attributes the plan does not read are left where they are: its
        // texgens take the two coordinates above or the normal, and a
        // material that reads the second colour channel never gets here
        // (lookup carrier). Until Alpha 8 a format that carried a third
        // coordinate or CLR1 was refused, then drawn on the CPU route.
        else if((a.attribute>=GX_VA_TEX0&&a.attribute<=GX_VA_TEX7)||(a.attribute==GX_VA_CLR1&&rigid.color.rasterChannel==0)){
            ++gxUnreadAttributeDecodes;continue;
        }
        else return reject(2);
        if(fields[slot]||a.nbt3)return reject(3);
        if(a.descriptor!=GX_INDEX8&&a.descriptor!=GX_INDEX16&&a.descriptor!=GX_DIRECT)return reject(4);
        // Codes 8 + field*8 + type; the third coordinate reports as the second.
        const unsigned formatCode=8+std::min(slot,4u)*8+(a.type&7);
        if(slot==2){if(!colorBytes(a.type))return reject(formatCode);}
        else if(slot==0?!acceptPosition(a):slot==1?!acceptNormal(a):!acceptTexcoord(a))return reject(formatCode);
        if(a.descriptor!=GX_DIRECT&&arrays[a.attribute].littleEndian!=(std::endian::native==std::endian::little)){
            if(!gxCompactSwappedArrays||elementBytes(slot,a)>16)return reject(5);
            swappedArray[slot]=true;
        }
        fields[slot]=&a;
    }
    bool needsTex1=false,needsTex0=false,needsTex2=false,needsNormal=false;
    for(unsigned unit=0;unit<3;++unit){
        const float role=multiRole(rigid,unit);
        if(role==1)(rigid.unitSource[unit]==3?needsTex2:rigid.unitSource[unit]?needsTex1:needsTex0)=true;
        else if(role<0)needsNormal=true; // texgen from the normal (role -1)
    }
    // Normals are optional for plans that never read them (unlit, or lit by
    // ambient only: the dummy light has zero color) unless a unit's texgen does.
    // No CLR0: the records carry missingVertexColor (white), read only when
    // the plan consumes the vertex colour (u[14].w, u[15].w, u[17].w). Until
    // 2026-11-16 a consumed missing colour was reject 6 (then the CPU route,
    // which refused the draw: Darkhammer's room D_MN11B r49).
    if(!fields[0]||(!fields[1]&&(!rigid.normalsUnused||needsNormal))||(needsTex0&&!fields[3])||(needsTex1&&!fields[4])||
       (needsTex2&&!fields[5])||(rigid.color.ambientVertex&2)||bool(matrix)!=rigid.indexedMatrices)return reject(6);
    if(pairedCount!=unsigned(__builtin_popcount(rigid.pairedSelectors))||(pairedCount&&!matrix))return reject(48);
    if((rigid.color.vertex|rigid.color.ambientVertex)&&!fields[2])++gxMissingColorDecodes;
    auto material=rigid.color.material;
    if(rigid.color.lit&2)material[3]*=std::clamp(rigid.color.ambient[3],0.f,1.f);
    for(float value:material)if(!std::isfinite(value)||value<0||value>1)return reject(7);
    const unsigned paletteBase=rigid.paletteBase();
    for(unsigned m=0;m<10;++m){
        const int offset=rigid.matrixOffsets[m],normal=rigid.normalOffsets[m];
        if(offset < -1)return reject(59);
        if(offset<0)continue;
        if(offset>0&&(offset<int(paletteBase)||offset+3>int(rigid.paletteLimit())))return reject(59);
        if(normal<0||normal+3>int(rigid.paletteLimit())||(normal!=offset&&normal!=offset+3))return reject(59);
        if(!rigid.compactPalette&&(normal!=offset+3||(offset>0&&(offset-paletteBase)%6)))return reject(59);
    }
    // Wide records (a third coordinate) hold the narrow record's fields in
    // the same order with the coordinate between the second one and the
    // matrix rows.
    static_assert(offsetof(CompactRigidVertexMT,matrix)==26&&offsetof(CompactRigidVertexMT,color)==28&&sizeof(CompactRigidVertexMT)==32&&
                  offsetof(CompactRigidVertexMTWide,texcoord2)==26&&offsetof(CompactRigidVertexMTWide,matrix)==30&&
                  offsetof(CompactRigidVertexMTWide,color)==32&&sizeof(CompactRigidVertexMTWide)<=sizeof(GpuVertex));
    const bool wide=rigid.wideRecord;
    const std::size_t recordBytes=rigid.recordBytes();
    auto target=std::as_writable_bytes(output).first(output.size()*recordBytes);
    std::array<IndexedGather,fieldCount> gather{};
    bool anyDirect=false;
    for(unsigned slot=0;slot<fieldCount;++slot)if(const auto* a=fields[slot]){
        if(a->descriptor==GX_DIRECT){anyDirect=true;continue;}
        if(!gather[slot].prepare(a->offset,a->descriptor==GX_INDEX16,arrays[a->attribute],elementBytes(slot,*a)))return reject(61);
    }
    std::array<float,3> origin{};
    {float first[3];u8 firstScratch[16];const auto& a=*fields[0];const u8* at=nullptr;
     if(a.descriptor!=GX_DIRECT){at=gather[0].read(packed.data());if(at&&swappedArray[0])at=littleElement(0,a,at,firstScratch);}
     else if(elementBytes(0,a)<=sizeof(firstScratch)&&a.offset+elementBytes(0,a)<=plan.stride)at=littleElement(0,a,packed.data()+a.offset,firstScratch);
     if(!at||!readPosition(a,at,first))return reject(55);
     origin=compactOrigin(first);}
    // Static J3D geometry. A compact palette keys its layout (see
    // sharedNormalMask; the checks above leave only the two placements).
    std::uint16_t sharedNormals=0;
    const bool layoutKeyed=sharedNormalMask(rigid.matrixOffsets,rigid.normalOffsets,sharedNormals);
    StaticCompactCache* staticCache=!anyDirect&&layoutKeyed&&staticGeometryHint.generation&&staticGeometryHint.list?staticCompactCache():nullptr;
    StaticCompactCache::Key staticKey{};
    if(staticCache){
        staticKey.generation=staticGeometryHint.generation;staticKey.batch=staticGeometryHint.batch;
        staticKey.list=reinterpret_cast<std::uintptr_t>(staticGeometryHint.list);
        staticKey.count=output.size();staticKey.stride=plan.stride;
        std::uint32_t layout=wide?0x6a09e667u:0x5bd1e995u; // distinct from the single-texture records, and wide from narrow
        for(unsigned slot=0;slot<fieldCount;++slot)if(const auto* a=fields[slot]){
            const auto& array=arrays[a->attribute];
            for(const std::uint32_t v:{std::uint32_t(a->attribute),std::uint32_t(a->offset),std::uint32_t(a->descriptor),
                std::uint32_t(a->type),std::bit_cast<std::uint32_t>(a->scale),std::uint32_t(array.stride),
                std::uint32_t(reinterpret_cast<std::uintptr_t>(array.bytes.data())),std::uint32_t(array.bytes.size())})
                layout=(layout^v)*16777619u;
        }
        staticKey.layout=layout^(matrix?matrix->offset+1:0)^(rigid.pairedSelectors*0x01000193u);
        std::memcpy(staticKey.offsets.data(),rigid.matrixOffsets.data(),staticKey.offsets.size());
        staticKey.sharedNormals=sharedNormals;
    }
    // Texture coordinates are stored as 16-bit fixed point. Float ones take
    // ten fraction bits, which hold 32 repeats either way; U16 ones are kept
    // as they are when none exceeds 32767. A mesh outside that (one that
    // tiles a small texture many times: the water of Ordon Spring, 90 strips
    // a picture on the CPU vertex route until Alpha 8; particles with U16
    // coordinates up to 65535) is stored one level coarser at a time: float
    // coordinates with 8, 6 or 4 bits, the most that hold them, U16 ones
    // without their lowest bit. Both coordinates of a draw share the level;
    // static records are kept under it (level 0 keeps the old key).
    bool floatTexcoords=false,wordTexcoords=false;
    for(unsigned slot=3;slot<recordFields;++slot)if(fields[slot]){
        floatTexcoords|=fields[slot]->type==GX_F32;wordTexcoords|=fields[slot]->type==GX_U16;
    }
    const unsigned coarsest=floatTexcoords?3:wordTexcoords?1:0;
    auto fractionBits=[](unsigned level){return floatTexcoordBits-2*level;};
    auto keyedLayout=[base=staticKey.layout,&fractionBits](unsigned level){return level?base^(0x9e3779b9u*fractionBits(level)):base;};
    unsigned level=0;
    bool reused=false;
    if(staticCache)for(unsigned tried=0;tried<=coarsest;++tried){
        staticKey.layout=keyedLayout(tried);
        if(const auto* saved=staticCache->find(staticKey,target.size())){
            // Wide records are copied: everything that draws resident
            // vertices assumes the narrow record.
            // So are the records of a per-vertex warp, whose second
            // coordinate is written again every picture.
            if(staticCache->resident()&&exposesResident(rigid)&&!wide&&!rigid.vertexWarp.enabled)rigid.staticVertices=saved;
            else std::memcpy(target.data(),saved,target.size());
            reused=true;level=tried;
            if(rigid.compactPalette){++staticCache->paletteHits;if(staticCache->resident()&&!rigid.staticVertices)++staticCache->paletteArena;}
            break;
        }
    }
    // A refused value (including a UV needing coarser quantization) goes
    // through the regular field passes and their existing rejection rules.
    bool packedDirect=false;
    if(!reused&&!wide&&!matrix&&fields[0]->descriptor==GX_DIRECT&&fields[0]->type==GX_F32&&fields[0]->components==3&&
       fields[0]->offset==0&&fields[2]&&fields[2]->descriptor==GX_DIRECT&&fields[2]->type==GX_RGBA8){
        std::int16_t normal[3]{};
        if(plan.count==2&&plan.stride==16&&fields[2]->offset==12)
            packedDirect=packPositionColor<false>(packed,target,origin,gather[3],normal);
        else if(plan.count==4&&plan.stride==18&&fields[2]->offset==13&&fields[1]&&fields[1]->descriptor==GX_INDEX8&&
                fields[1]->offset==12&&fields[1]->type==GX_F32&&!swappedArray[1]&&gather[1].limit==0&&
                fields[3]&&fields[3]->descriptor==GX_INDEX8&&fields[3]->offset==17&&fields[3]->type==GX_F32&&!swappedArray[3]&&
                readNormalAt(*fields[1],gather[1].data,false,normal))
            packedDirect=packPositionColor<true>(packed,target,origin,gather[3],normal);
    }
    if(!reused&&!packedDirect){
        // What does not depend on the vertex: where each field is read and in
        // which byte order. A direct field lies inside the packed vertex (its
        // extent is checked here, once); an indexed one in its array.
        std::array<bool,fieldCount> directField{},swappedField{};
        for(unsigned slot=0;slot<fieldCount;++slot)if(const auto* a=fields[slot]){
            if(a->descriptor==GX_DIRECT){
                if(elementBytes(slot,*a)>16||a->offset+elementBytes(slot,*a)>plan.stride)return reject(62);
                directField[slot]=true;swappedField[slot]=true;
            }else swappedField[slot]=swappedArray[slot];
        }
        const u8* const packedStart=packed.data();
        const std::size_t packedStride=plan.stride,vertices=output.size();
        std::byte* const records=target.data();
        // One pass over the vertices per field, so that a field's format is
        // chosen once and not for every vertex (a vertex took about 250
        // instructions when all its fields were read together; the grass of
        // Ordon is 5,500 vertices a picture). each() hands `body` the element
        // of every vertex and where its `bytes` converted bytes go, `at` in
        // the vertex's record: 0 done, 1 an index outside its array, 2 `body`
        // refused an element.
        // A field indexed with one byte has a table of at most 256 elements
        // (grass and flowers index a few float normals and coordinates):
        // each element is converted the first time a vertex names it and
        // copied from then on. An element no vertex names is never read, as
        // before.
        // Always inlined, the bodies and the readers they call too: left to
        // its own judgement the compiler stopped inlining one pass when this
        // function grew by a few hundred instructions (Alpha 9), and a call
        // per vertex cost 30 to 40 instructions on every dynamic vertex of
        // every zone (the grass of Ordon village: 3.02 to 3.71 ms).
        auto each=[&](unsigned slot,std::size_t at,std::size_t bytes,auto&& body) __attribute__((always_inline)) ->int{
            const auto& a=*fields[slot];
            std::byte* out=records+at;
            if(directField[slot]){
                const u8* source=packedStart+a.offset;
                for(std::size_t n=0;n<vertices;++n,source+=packedStride,out+=recordBytes)if(!body(source,out))return 2;
            }else if(const auto& indexed=gather[slot];!indexed.wide&&bytes<=8){
                std::byte converted[256][8];
                std::uint8_t known[256]{};
                const u8* index=packedStart+indexed.offset;
                for(std::size_t n=0;n<vertices;++n,index+=packedStride,out+=recordBytes){
                    const unsigned element=*index;
                    if(!known[element]){
                        if(element>indexed.limit)return 1;
                        if(!body(indexed.data+std::size_t(element)*indexed.stride,converted[element]))return 2;
                        known[element]=1;
                    }
                    std::memcpy(out,converted[element],bytes);
                }
            }else{
                const u8* vertex=packedStart;
                for(std::size_t n=0;n<vertices;++n,vertex+=packedStride,out+=recordBytes){
                    const u8* const source=indexed.read(vertex);
                    if(!source)return 1;
                    if(!body(source,out))return 2;
                }
            }
            return 0;
        };
        // Where the fields lie in a record (CompactRigidVertexMT, and
        // CompactRigidVertexMTWide with the third coordinate before the
        // matrix rows).
        constexpr std::size_t normalAt=12;constexpr std::size_t texcoordAt[3]{18,22,26};
        const std::size_t matrixAt=wide?30:26,colorAt=wide?32:28;
        {const auto& a=*fields[0];const bool swapped=swappedField[0];
         const float ox=origin[0],oy=origin[1],oz=origin[2];
         const int result=each(0,0,12,[&](const u8* source,std::byte* out) __attribute__((always_inline)) {
             float position[3];
             if(!readPositionAt(a,source,swapped,position))return false;
             position[0]-=ox;position[1]-=oy;position[2]-=oz;
             std::memcpy(out,position,12);return true;});
         if(result)return reject(result==1?63:55);}
        if(fields[1]){
            const auto& a=*fields[1];const bool swapped=swappedField[1];
            const int result=each(1,normalAt,6,[&](const u8* source,std::byte* out) __attribute__((always_inline)) {
                std::int16_t normal[3];
                if(!readNormalAt(a,source,swapped,normal))return false;
                std::memcpy(out,normal,6);return true;});
            // Counted since Alpha 8 (they returned without a code).
            if(result)return reject(result==1?63:50);
        }else for(std::size_t n=0;n<vertices;++n)std::memset(records+n*recordBytes+normalAt,0,6);
        if(fields[2]){
            const unsigned type=fields[2]->type;const bool swapped=swappedField[2];
            if(each(2,colorAt,4,[&](const u8* source,std::byte* out) __attribute__((always_inline)) {
                readColorAt(type,source,swapped,reinterpret_cast<std::uint8_t*>(out));return true;}))return reject(63);
        }else for(std::size_t n=0;n<vertices;++n)std::memcpy(records+n*recordBytes+colorAt,missingVertexColorBytes.data(),4);
        if(matrix&&rigid.skinEntries){
            // A merged skin draw: the list byte names a palette entry, the
            // record names its rows (position and normal alike).
            const u8* vertex=packedStart;std::byte* record=records;
            for(std::size_t n=0;n<vertices;++n,vertex+=packedStride,record+=recordBytes){
                const int row=skinRecordRow(rigid,vertex[matrix->offset]);
                if(row<0)return reject(54);
                record[matrixAt]=std::byte(row);record[matrixAt+1]=std::byte(row);
            }
        }else if(matrix){
            const u8* vertex=packedStart;std::byte* record=records;
            for(std::size_t n=0;n<vertices;++n,vertex+=packedStride,record+=recordBytes){
                const unsigned index=vertex[matrix->offset];
                if(index>27||index%3||rigid.matrixOffsets[index/3]<0)return reject(54);
                for(unsigned k=0;k<pairedCount;++k)if(vertex[paired[k]->offset]!=index+30)return reject(49);
                record[matrixAt]=std::byte(rigid.matrixOffsets[index/3]);record[matrixAt+1]=std::byte(rigid.normalOffsets[index/3]);
            }
        }else for(std::size_t n=0;n<vertices;++n){
            records[n*recordBytes+matrixAt]=std::byte(0);records[n*recordBytes+matrixAt+1]=std::byte(3);
        }
        for(unsigned slot=3;slot<(wide?recordFields:5u);++slot)if(!fields[slot])
            for(std::size_t n=0;n<vertices;++n)std::memset(records+n*recordBytes+texcoordAt[slot-3],0,4);
        for(level=0;;){
            // A coordinate out of range: every coordinate once more, at the
            // level it needs (a larger one further on asks again).
            bool coarser=false;
            const unsigned bits=fractionBits(level);const bool halved=level!=0;
            for(unsigned slot=3;slot<recordFields&&!coarser;++slot)if(fields[slot]){
                const auto& a=*fields[slot];const bool swapped=swappedField[slot];
                const std::size_t at=texcoordAt[slot-3];
                const u8* refused=nullptr;
                const int result=each(slot,at,4,[&](const u8* source,std::byte* out) __attribute__((always_inline)) {
                    std::int16_t coordinate[2];
                    if(!readTexcoordAt(a,source,swapped,coordinate,bits,halved)){refused=source;return false;}
                    std::memcpy(out,coordinate,4);return true;});
                if(result==1)return reject(63);
                if(result==2){
                    const auto type=a.type;
                    if(level>=(type==GX_F32?3u:type==GX_U16?1u:0u))return reject(slot==3?51:52);
                    unsigned wanted=level+1;
                    if(type==GX_F32){
                        float size=0;
                        for(unsigned c=0;c<2;++c){
                            std::uint32_t word;std::memcpy(&word,refused+4*c,4);
                            if(swapped)word=__builtin_bswap32(word);
                            size=std::max(size,std::abs(std::bit_cast<float>(word)));
                        }
                        while(wanted<3&&!(size*float(1u<<fractionBits(wanted))<=32767.f))++wanted;
                    }
                    level=wanted;coarser=true;
                }
            }
            if(!coarser)break;
        }
        if(level)++gxCoarseTexcoordDecodes;
        staticKey.layout=keyedLayout(level);
        if(staticCache&&staticCache->store(staticKey,target)&&rigid.compactPalette)++staticCache->paletteStores;
    }
    // The warp of every vertex, in the place of the second coordinate. It
    // may change with the map's scroll, so it is written every picture
    // (placeVertexWarps), after the stored records, which keep zero there,
    // were copied or decoded.
    if(rigid.vertexWarp.enabled){
        const auto& warp=rigid.vertexWarp;
        if(wide||fields[4])return reject(46);
        const unsigned slot=warp.sourceAttribute==GX_VA_POS?0u:warp.sourceAttribute==rigid.texcoordAttribute[0]?3u:
            warp.sourceAttribute==rigid.texcoordAttribute[1]?4u:6u;
        const auto* a=fields[slot];
        if(!a)return reject(46);
        const bool direct=a->descriptor==GX_DIRECT;
        if(direct&&(elementBytes(slot,*a)>16||a->offset+elementBytes(slot,*a)>plan.stride))return reject(62);
        const int result=placeVertexWarps(warp,*a,direct,direct||swappedArray[slot],gather[slot],packed.data(),plan.stride,output.size(),
            target.data()+offsetof(CompactRigidVertexMT,texcoord1),recordBytes,staticCache,staticKey);
        if(result)return reject(result==1?63:46);
    }
    rigid.decodedNormalScale=fields[1]?normalScale(*fields[1]):0.f;
    rigid.decodedTex0Scale=fields[3]?texcoordScale(*fields[3],fractionBits(level),level!=0):1.f;
    rigid.decodedTex1Scale=fields[4]?texcoordScale(*fields[4],fractionBits(level),level!=0):1.f;
    rigid.decodedTex2Scale=fields[5]?texcoordScale(*fields[5],fractionBits(level),level!=0):1.f;
    rigid.origin=origin;
    (void)material;
    if(!finishCompactRigidUniforms(rigid,{1.f,1.f})){rigid.staticVertices=nullptr;return reject(53);}
    if(plan.count==1)++gxCompactSingleAttributeDecodes;
    for(const bool swapped:swappedArray)if(swapped){++gxSwappedArrayDecodes;break;}
#if defined(__3DS__)
    census.done=true;census.reused=reused;
#endif
    return true;
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
