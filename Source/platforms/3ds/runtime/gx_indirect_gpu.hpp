#pragma once
// Native PICA engine: indirect warps on the multi-texture GPU route (Alpha 7,
// plan item A19). The PICA vertex shader cannot read the indirect image, so
// every warped draw used to take the CPU vertex route (gx_indirect.hpp, about
// 10 us a vertex): 14-52 ms a frame in the Lakebed and water zones of the
// Alpha 6 console run. A warp whose image keeps it within a few texels of its
// mean becomes that mean, a constant texture offset folded into the unit's
// texgen rows, and the draw stays on the GPU.
//
// How close this is: the original offsets every pixel by w(p) with
// |w(p) - mean| <= D, D the bound computed below. The per-vertex route
// interpolates w at the vertices, whose error can reach max - min (up to 2D)
// and grows where the warp map tiles several times across a triangle; the
// constant is never further than D from the original. What is lost is the
// slow per-vertex wobble when the warp map scrolls, so this is opt-in.
//
// SD switch indirect-constant.txt (indirectConstantLimit):
//  N > 0 warps with D <= N texels and projected units divided per pixel.
//    Default since Alpha 8: indirectConstantEveryWarp, every warp. The
//    Alpha 7 console run drew 5,000-9,500 warped water vertices a picture on
//    the CPU route in the Lakebed and Lake Hylia (107-189 ms of a picture);
//    164,902 of its 190,985 refused plans were refused for D alone. The
//    owner accepts water without the moving distortion (2026-11-17).
//  0 exact plans only (the Alpha 7 default): every warp has D = 0 (its map
//    is uniform over the levels the vertex sampler reads), or the CPU route
//    already leaves it out (a texture shared by two warps), and no unit is
//    projected. The GPU route then computes the CPU route's coordinates
//    (float24 vertex arithmetic, as for any draw it takes); the per-pixel
//    projection branch of gx_rigid_mt.v.pica is never used in this mode.
//    Other warps keep the CPU route and their animation.
//  negative: off, every warped draw on the CPU route (Alpha 7 wave 1).
//
// Alpha 9: the constant is no longer the whole warp. planVertexWarp adds what
// the map gives each vertex beyond its mean, sampled by the decoder and
// carried in the record's second texture coordinate (RigidVertexPlan::
// vertexWarp), so a warped unit gets the coordinates the CPU vertex route
// gave it: mean + (sample - mean), interpolated between the vertices. A draw
// the per-vertex part cannot serve keeps the constant alone. SD switch
// vertex-warp-off.txt.
#include "gx_indirect.hpp"
#include "gx_pica_material.hpp"
#include "gx_rigid_vertex.hpp"
#include <cstdio>
#include <limits>

namespace dusk::native::gx {
// Per-channel abg statistics, in byte units, of the texels the vertex
// sampler reads (sampleIndirectImage). The range also covers level 0, the
// widest level the original's per-pixel LOD can select.
struct IndirectImageStats {
    std::array<float,3> mean{},low{},high{};
};
inline bool computeIndirectImageStats(const Texture& t,IndirectImageStats& out){
    if(!validIndirectImage(t))return false;
    const float lod=indirectVertexLod(t);
    const bool blend=t.minFilter>=4;
    const unsigned low=blend?unsigned(lod):unsigned(std::floor(lod+.5f));
    const bool second=blend&&lod>low;
    out={};
    out.low.fill(std::numeric_limits<float>::infinity());out.high.fill(-std::numeric_limits<float>::infinity());
    auto scan=[&](unsigned selected,std::array<double,3>* sum){
        const auto image=indirectLevel(t,selected);
        for(unsigned y=0;y<image.height;++y)for(unsigned x=0;x<image.width;++x){
            const auto abg=indirectTexel(image,x,y);
            for(unsigned c=0;c<3;++c){
                out.low[c]=std::min(out.low[c],abg[c]);out.high[c]=std::max(out.high[c],abg[c]);
                if(sum)(*sum)[c]+=abg[c];
            }
        }
        return double(image.width)*image.height;
    };
    std::array<double,3> sum{};
    const double texels=scan(low,&sum);
    for(unsigned c=0;c<3;++c)out.mean[c]=float(sum[c]/texels);
    if(second){
        std::array<double,3> next{};const double count=scan(low+1,&next);
        const float fraction=lod-low;
        for(unsigned c=0;c<3;++c)out.mean[c]+=(float(next[c]/count)-out.mean[c])*fraction;
    }
    if(low!=0)scan(0,nullptr);
    return true;
}
// Exact mode needs only D = 0: every texel of the levels the statistics
// cover equals the first one. Stops at the first other texel, so a varied
// map costs a few texel reads per draw instead of a scan.
inline bool indirectImageUniform(const Texture& t){
    if(!validIndirectImage(t))return false;
    const float lod=indirectVertexLod(t);
    const bool blend=t.minFilter>=4;
    const unsigned low=blend?unsigned(lod):unsigned(std::floor(lod+.5f));
    const auto first=indirectTexel(indirectLevel(t,0),0,0);
    auto same=[&](unsigned selected){
        const auto image=indirectLevel(t,selected);
        for(unsigned y=0;y<image.height;++y)for(unsigned x=0;x<image.width;++x)if(indirectTexel(image,x,y)!=first)return false;
        return true;
    };
    return same(0)&&(low==0||same(low))&&(!(blend&&lod>low)||same(low+1));
}
// Small cache: a warp map is shared by every draw of a material. Retained
// private images never change; other images are scanned again each frame
// (textureFrameCounter), so a rewritten map is picked up the next frame.
struct IndirectStatsCache {
    struct Entry {
        const std::uint8_t* data{};std::size_t size{};TextureHandle handle{};
        std::uint32_t frame{};std::uint16_t width{},height{};
        std::uint8_t format{},mipCount{},minFilter{},minLod{},maxLod{};
        float lodBias{};bool immutable{},used{};
        IndirectImageStats stats;
    };
    std::array<Entry,16> entries{};
    unsigned victim{};
    std::uint64_t scans{},hits{};
};
inline IndirectStatsCache indirectStatsCache{};
// requireUniform (exact mode): a map that is not cached is probed first and,
// unless uniform, refused without a scan (*varied set); varied maps are not
// cached, uniform ones are, as in the other modes.
inline bool indirectImageStats(const Texture& t,IndirectImageStats& out,bool requireUniform=false,bool* varied=nullptr){
    auto& cache=indirectStatsCache;
    auto probe=[&]{const bool uniform=indirectImageUniform(t);if(!uniform&&varied)*varied=true;return uniform;};
    auto same=[&](const IndirectStatsCache::Entry& e){
        return e.used&&e.data==t.image.data()&&e.size==t.image.size()&&e.handle==t.handle&&e.width==t.width&&
            e.height==t.height&&e.format==t.format&&e.mipCount==t.mipCount&&e.minFilter==t.minFilter&&
            e.minLod==t.minLod&&e.maxLod==t.maxLod&&e.lodBias==t.lodBias&&e.immutable==t.immutablePixels;
    };
    for(auto& e:cache.entries)if(same(e)){
        if(e.immutable||e.frame==textureFrameCounter){++cache.hits;out=e.stats;return true;}
        if(requireUniform&&!probe())return false;
        if(!computeIndirectImageStats(t,e.stats)){e.used=false;return false;}
        ++cache.scans;e.frame=textureFrameCounter;out=e.stats;return true;
    }
    if(requireUniform&&!probe())return false;
    IndirectImageStats stats;
    if(!computeIndirectImageStats(t,stats))return false;
    ++cache.scans;
    auto& e=cache.entries[cache.victim++%cache.entries.size()];
    e={t.image.data(),t.image.size(),t.handle,textureFrameCounter,t.width,t.height,t.format,t.mipCount,
       t.minFilter,t.minLod,t.maxLod,t.lodBias,t.immutablePixels,true,stats};
    out=stats;return true;
}
// Warp of one stage, in units of the base coordinate scale (destination
// texels when that scale is the texture size): mean and a bound on the
// distance of any texel's warp from it. Channels a format repeats are one
// value (I4/I8: a=b=g; IA4/IA8: b=g), so their matrix columns add first.
struct IndirectWarpStats {
    std::array<float,2> mean{},deviation{};
};
inline IndirectWarpStats indirectWarpStats(const VertexIndirectEffect& e,const IndirectImageStats& s){
    IndirectWarpStats w;
    std::array<float,3> spread;
    for(unsigned c=0;c<3;++c)spread[c]=std::max(s.high[c]-s.mean[c],s.mean[c]-s.low[c]);
    const unsigned format=e.source.format;
    for(unsigned axis=0;axis<2;++axis){
        const auto& m=e.matrix[axis];
        float mean=0;
        for(unsigned c=0;c<3;++c)mean+=m[c]*(s.mean[c]-((e.bias&(1u<<c))?128.f:0.f));
        w.mean[axis]=mean;
        w.deviation[axis]=format<=1?std::abs(m[0]+m[1]+m[2])*spread[0]:
                          format<=3?std::abs(m[0])*spread[0]+std::abs(m[1]+m[2])*spread[1]:
                          std::abs(m[0])*spread[0]+std::abs(m[1])*spread[1]+std::abs(m[2])*spread[2];
    }
    return w;
}
// Largest accepted bound D, destination texels (see the modes above).
inline constexpr float indirectConstantEveryWarp=4096;
inline float indirectConstantLimit=indirectConstantEveryWarp;
inline void setIndirectConstantLimit(float texels){indirectConstantLimit=std::isfinite(texels)?std::max(texels,-1.f):0;}
enum class IndirectConstantRefusal : std::uint8_t {
    deviation, // a warp moves further than the limit from its mean
    scale,     // base coordinate scale differs from the destination size
    shared,    // one texture/coordinate pair under two different warps, no fallback
    texture,   // destination size or warp map statistics unavailable
    units,     // more than three texture units
    projected, // a projected unit without per-pixel division (exact mode, or no projection mode)
    count
};
// One texture unit, keyed by its binding (routeProjectiveTexture may still
// swap units after this plan): its offset in normalized coordinates.
struct IndirectConstantBinding {
    std::array<float,2> offset{};
    float deviation{};
    std::uint8_t map{},coordinate{};
    bool warped{};
};
// What the per-vertex part of a warped unit is made of (planVertexWarp).
// planIndirectConstant fills the entry of every binding it marks warped; the
// others are never read, so the array needs no initial value.
struct IndirectWarpSource {
    Texture map;
    std::array<std::array<float,3>,2> matrix;
    // spread: the warp's largest distance from its mean, destination texels.
    std::array<float,2> sourceScale,spread;
    std::array<float,3> mean;
    std::uint16_t width,height; // destination size
    std::uint8_t coordinate;
};
using IndirectWarpSources=std::array<IndirectWarpSource,3>;
struct IndirectConstantPlan {
    std::array<IndirectConstantBinding,3> bindings{};
    std::uint8_t count{};
    // A unit sampled under two different warps keeps its texture without
    // either (DrawVisualFallback::sharedIndirect), as on the CPU route.
    bool sharedPlain{};
    // A projected unit may be divided per pixel (applyIndirectConstant).
    bool perPixel{};
    bool enabled{};
};
struct IndirectConstantCounters {
    // cameraDivisor: draws whose projected unit follows the camera, kept on
    // the per-pixel division whatever side of the camera plane their vertices
    // lie on (gx_rigid_vertex.hpp, projectedUnitFollowsCamera).
    std::uint64_t planned{},gpu{},vertices{},perPixel{},restarts{},cameraDivisor{};
    std::array<std::uint64_t,unsigned(IndirectConstantRefusal::count)> refused{};
    float largestAccepted{},largestRefused{};
    IndirectConstantBinding last{};
    std::uint16_t lastWidth{},lastHeight{};
};
inline IndirectConstantCounters indirectConstantCounters{};
// Plan the draw's warps as constant offsets. original: the decoded material
// with its indirect bits; material: the PICA plan, whose units are matched to
// TEV stages as the CPU route matches them (gx_command_draw.cpp).
// allowPerPixel: the GPU route can take a projected unit and the PICA
// projection mode is in use (the CPU route divides per pixel too). True: drop
// the per-vertex warp and use applyIndirectConstant on the GPU route.
inline bool planIndirectConstant(const CommandRegisters& r,const MaterialState& original,const PicaMaterialPlan& material,
                                 unsigned textureUnits,const VertexIndirectPlan& indirect,bool allowSharedPlain,
                                 bool allowPerPixel,IndirectConstantPlan& out,IndirectWarpSources* sources=nullptr){
    out={};
    if(!indirect.enabled||indirectConstantLimit<0)return false;
    auto& counters=indirectConstantCounters;
    auto refuse=[&](IndirectConstantRefusal why){++counters.refused[unsigned(why)];out={};return false;};
    if(textureUnits>3)return refuse(IndirectConstantRefusal::units);
    // The GPU route divides a projected unit per vertex unless the plan asks
    // for the PICA projection mode, which only the approximate mode does.
    const bool perPixel=allowPerPixel&&indirectConstantLimit>0;
    // Projective texgen as planVertexTransform reads it (an unreadable one
    // counts as projected: the draw then keeps the unchanged CPU route).
    auto projective=[&](unsigned coordinate){
        if(coordinate>7||!r.xfValid[0x1040+coordinate])return true;
        const auto config=r.xf[0x1040+coordinate];const unsigned type=(config>>4)&7;
        return type!=2&&type!=3&&((config>>1)&1);
    };
    float largest=0;
    for(unsigned unit=0;unit<textureUnits;++unit){
        const auto binding=material.textures[unit];
        if(!perPixel&&projective(binding.coordinate))return refuse(IndirectConstantRefusal::projected);
        auto& entry=out.bindings[unit];entry.map=std::uint8_t(binding.map);entry.coordinate=std::uint8_t(binding.coordinate);
        int first=-1;bool plain=false;
        for(unsigned stage=0;stage<original.stageCount;++stage){
            const auto& s=original.stages[stage];
            if(!s.textureEnabled||s.texture!=binding.map||s.coordinate!=binding.coordinate)continue;
            if(first>=0&&indirect.stages[first].state!=indirect.stages[stage].state){
                if(!allowSharedPlain)return refuse(IndirectConstantRefusal::shared);
                plain=out.sharedPlain=true;
            }
            first=stage;
        }
        if(first<0||!indirect.stages[first].enabled||plain)continue;
        const auto& effect=indirect.stages[first];
        // Destination size from the map's image register: the texture the
        // unit binds has these dimensions (resolveCommandTexture checks it).
        const unsigned offset=(binding.map/4)*32+binding.map%4;
        if(binding.map>7||!r.bpValid[0x88+offset])return refuse(IndirectConstantRefusal::texture);
        const unsigned width=(r.bp[0x88+offset]&1023)+1,height=((r.bp[0x88+offset]>>10)&1023)+1;
        // The CPU route adds the warp to base * scale and divides by the size:
        // a plain coordinate offset only when the scale is the size.
        if(effect.baseScale[0]!=float(width)||effect.baseScale[1]!=float(height))return refuse(IndirectConstantRefusal::scale);
        IndirectImageStats stats;bool varied=false;
        if(!indirectImageStats(effect.source,stats,!(indirectConstantLimit>0),&varied))
            return refuse(varied?IndirectConstantRefusal::deviation:IndirectConstantRefusal::texture);
        const auto warp=indirectWarpStats(effect,stats);
        const float deviation=std::max(warp.deviation[0],warp.deviation[1]);
        if(!std::isfinite(deviation)||!std::isfinite(warp.mean[0])||!std::isfinite(warp.mean[1])||deviation>indirectConstantLimit){
            if(std::isfinite(deviation))counters.largestRefused=std::max(counters.largestRefused,deviation);
            return refuse(IndirectConstantRefusal::deviation);
        }
        entry.offset={warp.mean[0]/float(width),warp.mean[1]/float(height)};
        entry.deviation=deviation;entry.warped=true;
        if(sources){
            auto& s=(*sources)[unit];
            s.map=effect.source;s.matrix=effect.matrix;s.sourceScale=effect.sourceScale;s.spread=warp.deviation;s.mean=stats.mean;
            s.width=std::uint16_t(width);s.height=std::uint16_t(height);s.coordinate=std::uint8_t(effect.coordinate);
        }
        largest=std::max(largest,deviation);
        counters.last=entry;counters.lastWidth=std::uint16_t(width);counters.lastHeight=std::uint16_t(height);
    }
    out.count=std::uint8_t(textureUnits);out.perPixel=perPixel;out.enabled=true;
    ++counters.planned;counters.largestAccepted=std::max(counters.largestAccepted,largest);
    return true;
}
// Fold the offsets into a planned multi-texture uniform block
// (gx_rigid_mt.v.pica). Rows 1/-1: s' = S.in + c, t' = T.in + c' with the
// texture scale and vertical flip folded in, so the offset enters the
// constants. Role 2 (unit 0 projected from the position): s/q + o needs
// S + o.Q, and the unit is divided per pixel (u[92].z = 1, the PICA
// projection mode the CPU route uses) instead of per vertex. False: the
// plan does not fit (the caller translates the draw again with its warp).
inline bool applyIndirectConstant(RigidVertexPlan& p,const IndirectConstantPlan& plan,const PicaMaterialPlan& material,
                                  unsigned textureUnits,const std::array<std::array<float,2>,3>& scales){
    if(!plan.enabled||!p.multiTexture||textureUnits>3||textureUnits!=plan.count||(p.multiProjected&&!plan.perPixel))return false;
    auto& u=p.uniforms;
    // Only the unit rows change (81-92): the plan is written back when every
    // one of them holds, and a warped draw does not copy the whole block
    // (Morpheel's room has 90 of them a picture).
    constexpr unsigned first=multiUnitRow(2),rows=12;
    std::array<std::array<float,4>,rows> next;
    std::copy_n(u.begin()+first,rows,next.begin());
    auto row=[&](unsigned index)->std::array<float,4>&{return next[index-first];};
    for(unsigned unit=0;unit<textureUnits;++unit){
        const IndirectConstantBinding* entry=nullptr;
        for(unsigned n=0;n<plan.count;++n){const auto& b=plan.bindings[n];
            if(b.map==material.textures[unit].map&&b.coordinate==material.textures[unit].coordinate){entry=&b;break;}}
        if(!entry)return false;
        if(!entry->warped)continue;
        const float role=multiRole(p,unit);const unsigned base=multiUnitRow(unit);
        const auto& o=entry->offset;
        if(role==1||role==-1){
            row(base)[3]+=scales[unit][0]*o[0];
            row(base+2)[3]-=scales[unit][1]*o[1];
        }else if(role==2&&unit==0&&p.multiProjected){
            for(unsigned c=0;c<4;++c){row(base)[c]+=o[0]*u[base+2][c];row(base+1)[c]+=o[1]*u[base+2][c];}
        }else return false;
    }
    if(p.multiProjected)row(multiUnitRow(0)+3)[2]=1;
    for(unsigned unit=0;unit<textureUnits;++unit)for(unsigned n=0;n<4;++n)
        for(float value:row(multiUnitRow(unit)+n))if(!std::isfinite(value)||std::abs(value)>1e8f)return false;
    std::copy_n(next.begin(),rows,u.begin()+first);return true;
}
// The plan asks the PICA to divide unit 0 per pixel (applyIndirectConstant).
inline bool indirectConstantPerPixel(const RigidVertexPlan& p){
    return p.multiTexture&&p.multiProjected&&p.uniforms[multiUnitRow(0)+3][2]>0;
}
// The per-vertex part of the warps (Alpha 9). SD switch vertex-warp-off.txt:
// off, a warp is its constant offset alone, as in Alpha 8.
inline bool vertexWarpEnabled{true};
enum class VertexWarpRefusal : std::uint8_t {
    record,    // a unit reads the record's second coordinate: no place for the warp
    role,      // no warped unit the shader can add an offset to
    texgen,    // the map's coordinate is no texgen this pass makes (2x4 from a texture coordinate, 2x4 or 3x4 from the position)
    attribute, // its source attribute is absent, or its matrix is chosen per vertex
    uniform,   // the map does not vary: the constant is the whole warp
    range,     // a coefficient is not finite
    count
};
struct VertexWarpCounters {
    // planned: plans made; draws and vertices: submitted with one; units:
    // warped units given their per-vertex part, constantUnits: left with the
    // constant (another map or matrix than the unit the vertex stores).
    std::uint64_t planned{},draws{},vertices{},units{},constantUnits{};
    std::array<std::uint64_t,unsigned(VertexWarpRefusal::count)> refused{};
};
inline VertexWarpCounters vertexWarpCounters{};
// Plan the per-vertex part for a draw whose constant offsets are folded
// (call after applyIndirectConstant succeeded). One warped unit is the
// reference: a vertex stores that unit's two offsets, as fixed point, where
// its record keeps the second texture coordinate (shader input v5); none of
// the warped materials measured reads one (their meshes carry TEX0 only),
// and a material that does keeps the constant. The projected unit has one free
// uniform component (u[92].w), enough for the scale of its own offsets, so it
// is the reference when it is warped; otherwise the first warped unit made
// from a texture coordinate or the normal (roles 1 and -1). Every other
// warped unit reads the same map through its own matrix: its offsets are a
// 2x2 combination K of the reference's (K F = A below) when the two matrices
// allow one, and it keeps the constant alone when they do not.
// True: the decoder fills the warps (decodeCompactRigidBatchMT).
inline bool planVertexWarp(const CommandRegisters& r,const DecodedVertex& first,RigidVertexPlan& p,const IndirectConstantPlan& plan,
                           const IndirectWarpSources& sources,const PicaMaterialPlan& material,unsigned textureUnits,
                           const std::array<std::array<float,2>,3>& scales){
    if(!vertexWarpEnabled||!plan.enabled||!p.multiTexture||textureUnits>3||textureUnits!=plan.count)return false;
    auto& counters=vertexWarpCounters;
    auto refuse=[&](VertexWarpRefusal why){++counters.refused[unsigned(why)];return false;};
    // The second coordinate must be free: no wide record (three coordinate
    // sets) and no unit generated from the second set.
    if(p.wideRecord)return refuse(VertexWarpRefusal::record);
    for(unsigned unit=0;unit<textureUnits;++unit)if(multiRole(p,unit)==1&&p.unitSource[unit]!=0)return refuse(VertexWarpRefusal::record);
    // Bindings are keyed by map and coordinate: the units may have been
    // swapped since the constant plan (routeProjectiveTexture).
    std::array<const IndirectWarpSource*,3> warps{};
    for(unsigned unit=0;unit<textureUnits;++unit)for(unsigned n=0;n<plan.count;++n){
        const auto& b=plan.bindings[n];
        if(b.map!=material.textures[unit].map||b.coordinate!=material.textures[unit].coordinate)continue;
        if(b.warped)warps[unit]=&sources[n];
        break;
    }
    int reference=-1;
    if(warps[0]&&p.multiProjected&&multiRole(p,0)==2)reference=0;
    auto offsetRole=[&](unsigned unit){const float role=multiRole(p,unit);return role==1||role==-1;};
    for(unsigned unit=0;unit<textureUnits&&reference<0;++unit)if(warps[unit]&&offsetRole(unit))reference=int(unit);
    if(reference<0)return refuse(VertexWarpRefusal::role);
    const auto& ref=*warps[reference];
    // The map's coordinate: a regular texgen from a texture coordinate (2x4)
    // or from the position (2x4, or 3x4 with its divisor), one matrix for
    // the draw, an optional post matrix without normalisation, composed as
    // the unit rows are (buildRigidMultiVertex).
    const unsigned c=ref.coordinate;
    if(c>7||!r.xfValid[0x1040+c]||!r.xfValid[0x1050+c]||!r.matrixIndexValid[0]||(c>=4&&!r.matrixIndexValid[1]))
        return refuse(VertexWarpRefusal::texgen);
    const auto gen=r.xf[0x1040+c],post=r.xf[0x1050+c];
    const unsigned source=(gen>>7)&31,postRow=post&63;
    if(((gen>>4)&7)||(post&0x100)||(postRow!=61&&(postRow>57||postRow%3)))return refuse(VertexWarpRefusal::texgen);
    const bool fromPosition=source==0,projective=gen&2;
    if(!fromPosition&&(source<5||source>12||(gen&4)||projective))return refuse(VertexWarpRefusal::texgen);
    const unsigned attribute=fromPosition?unsigned(GX_VA_POS):unsigned(GX_VA_TEX0)+(source-5);
    if(!(first.present&(1u<<attribute))||(first.present&(1u<<(GX_VA_TEX0MTXIDX+c))))return refuse(VertexWarpRefusal::attribute);
    const unsigned m=c<4?(r.matrixIndex[0]>>(6+6*c))&63:(r.matrixIndex[1]>>(6*(c-4)))&63;
    auto word=[&](unsigned at,double& value){
        if(!r.xfValid[at])return false;
        const float f=std::bit_cast<float>(r.xf[at]);
        if(!std::isfinite(f)||std::abs(f)>1e6f)return false;
        value=f;return true;
    };
    const unsigned rowCount=projective?3:2;
    std::array<std::array<double,4>,3> rows{{{1,0,0,0},{0,1,0,0},{0,0,1,0}}};
    if(m!=60){
        if(m>=60||m%3)return refuse(VertexWarpRefusal::texgen);
        for(unsigned row=0;row<rowCount;++row)for(unsigned k=0;k<4;++k)if(!word(4*m+4*row+k,rows[row][k]))return refuse(VertexWarpRefusal::texgen);
    }
    // Input form AB11 feeds (a,b,1,1): the third coefficient is a constant.
    if(!(gen&4))for(auto& row:rows){row[3]+=row[2];row[2]=0;}
    if(postRow!=61){
        // The post matrix acts on (s',t',q',1), q' being 1 after a 2x4
        // texgen: linear in the first-stage rows either way.
        std::array<std::array<double,4>,3> composed{{{0,0,0,0},{0,0,0,0},{0,0,1,0}}};
        for(unsigned row=0;row<rowCount;++row){
            double pr[4];
            for(unsigned k=0;k<4;++k)if(!word(0x500+4*(postRow+row)+k,pr[k]))return refuse(VertexWarpRefusal::texgen);
            for(unsigned k=0;k<4;++k)composed[row][k]=pr[0]*rows[0][k]+pr[1]*rows[1][k]+(projective?pr[2]*rows[2][k]:0.0);
            composed[row][3]+=projective?pr[3]:pr[2]+pr[3];
        }
        rows=composed;
    }
    // The map as the vertex sampler reads it (sampleIndirectImage).
    const Texture& t=ref.map;
    const float lod=indirectVertexLod(t);
    const bool blend=t.minFilter>=4;
    const unsigned low=blend?unsigned(lod):unsigned(std::floor(lod+.5f));
    VertexWarpPlan warp;
    warp.format=t.format;warp.wrapS=t.wrapS;warp.wrapT=t.wrapT;warp.projected=projective;warp.constantMap=t.immutablePixels;
    auto level=[&](unsigned selected,VertexWarpPlan::Level& out){
        const auto image=indirectLevel(t,selected);
        out={image.image.data(),std::uint32_t(image.image.size()),image.width,image.height,bool(lod<=0?t.magFilter:(t.minFilter&1))};
    };
    level(low,warp.levels[0]);
    if(blend&&lod>low){level(low+1,warp.levels[1]);warp.blend=lod-low;}
    // A unit's offsets per unit of the sample, in normalized coordinates,
    // with the channels a format repeats (I: a = b = g; IA: b = g) as one.
    auto offsets=[&](const IndirectWarpSource& s){
        std::array<std::array<double,3>,2> a{};
        for(unsigned axis=0;axis<2;++axis){
            const double size=axis?s.height:s.width;
            const auto& row=s.matrix[axis];
            if(t.format<=1)a[axis]={(double(row[0])+row[1]+row[2])/size,0,0};
            else if(t.format<=3)a[axis]={row[0]/size,(double(row[1])+row[2])/size,0};
            else a[axis]={row[0]/size,row[1]/size,row[2]/size};
        }
        return a;
    };
    auto sameMap=[](const IndirectWarpSource& a,const IndirectWarpSource& b){
        const auto& x=a.map;const auto& y=b.map;
        return a.coordinate==b.coordinate&&a.sourceScale==b.sourceScale&&x.image.data()==y.image.data()&&x.image.size()==y.image.size()&&
            x.width==y.width&&x.height==y.height&&x.format==y.format&&x.wrapS==y.wrapS&&x.wrapT==y.wrapT&&x.minFilter==y.minFilter&&
            x.magFilter==y.magFilter&&x.mipCount==y.mipCount&&x.minLod==y.minLod&&x.maxLod==y.maxLod&&x.lodBias==y.lodBias;
    };
    const auto F=offsets(ref);
    // The stored numbers are the reference's offsets times a power of two
    // that keeps the largest one (the bound of the constant plan) in 16 bits.
    double largest=0;
    for(unsigned axis=0;axis<2;++axis)largest=std::max(largest,double(ref.spread[axis])/(axis?ref.height:ref.width));
    if(!std::isfinite(largest))return refuse(VertexWarpRefusal::range);
    if(!(largest>0))return refuse(VertexWarpRefusal::uniform);
    // The largest power of two not above 32000 / largest, from the bits of
    // the ratio (no library call on this per-draw path).
    const float ratio=float(32000.0/largest);
    if(!(ratio>0)||!std::isfinite(ratio))return refuse(VertexWarpRefusal::range);
    const int exponent=std::clamp(int((std::bit_cast<std::uint32_t>(ratio)>>23)&255)-127,-24,30);
    const double scale=double(std::bit_cast<float>(std::uint32_t(exponent+127)<<23));
    // The coefficients to place, written only when the whole plan holds.
    struct Placed {std::uint8_t row;std::array<float,2> value;};
    std::array<Placed,6> placed{};unsigned placedCount=0;
    float projectedScale=0;bool projectedCarried=false;
    unsigned carried=0,constant=0;
    for(unsigned unit=0;unit<textureUnits;++unit)if(const auto* s=warps[unit]){
        const float role=multiRole(p,unit);
        double K[2][2]{{1,0},{0,1}};
        bool held=int(unit)==reference;
        if(!held&&offsetRole(unit)&&sameMap(*s,ref)){
            // K F = A: K = A F^T (F F^T)^-1, or A F^T / trace when F has rank one.
            const auto A=offsets(*s);
            double G[2][2]{},AF[2][2]{};
            for(unsigned i=0;i<2;++i)for(unsigned j=0;j<2;++j)for(unsigned k=0;k<3;++k){G[i][j]+=F[i][k]*F[j][k];AF[i][j]+=A[i][k]*F[j][k];}
            const double trace=G[0][0]+G[1][1],det=G[0][0]*G[1][1]-G[0][1]*G[1][0];
            if(trace>0){
                if(det>1e-9*trace*trace){
                    const double inverse[2][2]{{G[1][1]/det,-G[0][1]/det},{-G[1][0]/det,G[0][0]/det}};
                    for(unsigned i=0;i<2;++i)for(unsigned j=0;j<2;++j)K[i][j]=AF[i][0]*inverse[0][j]+AF[i][1]*inverse[1][j];
                }else for(unsigned i=0;i<2;++i)for(unsigned j=0;j<2;++j)K[i][j]=AF[i][j]/trace;
                double error=0,size=0;
                for(unsigned i=0;i<2;++i)for(unsigned k=0;k<3;++k){
                    error=std::max(error,std::abs(K[i][0]*F[0][k]+K[i][1]*F[1][k]-A[i][k]));size=std::max(size,std::abs(A[i][k]));
                }
                held=error<=1e-4*size;
            }
        }
        if(!held){++constant;continue;}
        ++carried;
        const unsigned base=multiUnitRow(unit);
        if(role==2){projectedScale=float(1/scale);projectedCarried=true;}
        else{
            // Rows S1 and T1 act on (second coordinate, third coordinate):
            // x and y take the stored numbers, with the texture scale and
            // the vertical flip of the unit's other rows.
            const double su=scales[unit][0]/scale,sv=scales[unit][1]/scale;
            placed[placedCount++]={std::uint8_t(base+1),{float(su*K[0][0]),float(su*K[0][1])}};
            placed[placedCount++]={std::uint8_t(base+3),{float(-sv*K[1][0]),float(-sv*K[1][1])}};
        }
    }
    if(!std::isfinite(projectedScale)||projectedScale>1e8f)return refuse(VertexWarpRefusal::range);
    for(unsigned n=0;n<placedCount;++n)for(float value:placed[n].value)
        if(!std::isfinite(value)||std::abs(value)>1e8f)return refuse(VertexWarpRefusal::range);
    warp.sourceAttribute=std::uint8_t(attribute);
    for(unsigned row=0;row<3;++row)for(unsigned k=0;k<4;++k){
        const double value=rows[row][k]*(row<2?double(ref.sourceScale[row]):1.0);
        if(!std::isfinite(value)||std::abs(value)>1e8)return refuse(VertexWarpRefusal::range);
        warp.rows[row][k]=float(value);
    }
    for(unsigned i=0;i<2;++i)for(unsigned k=0;k<3;++k)warp.functional[i][k]=float(F[i][k]);
    warp.mean=ref.mean;warp.recordScale=float(scale);warp.enabled=true;
    for(unsigned n=0;n<placedCount;++n){p.uniforms[placed[n].row][0]=placed[n].value[0];p.uniforms[placed[n].row][1]=placed[n].value[1];}
    if(projectedCarried)p.uniforms[multiUnitRow(0)+3][3]=projectedScale;
    p.vertexWarp=warp;
    // No attribute is the record's second coordinate: the decoder leaves the
    // place to the warp.
    p.texcoordAttribute[1]=0xff;
    ++counters.planned;counters.units+=carried;counters.constantUnits+=constant;
    return true;
}
inline void countIndirectConstant(const RigidVertexPlan& p,unsigned vertices){
    auto& c=indirectConstantCounters;++c.gpu;c.vertices+=vertices;if(indirectConstantPerPixel(p))++c.perPixel;
    if(p.vertexWarp.enabled){++vertexWarpCounters.draws;vertexWarpCounters.vertices+=vertices;}
}
// Profiled-frame report (gx_command_draw.cpp, with the degraded-draw lines):
// prints only when a count moved. OSReport keeps 255 characters of a line.
inline void reportIndirectConstant(void(*sink)(const char*)){
    static std::uint64_t reported=~std::uint64_t(0),formats=~std::uint64_t(0);
    const auto& c=indirectConstantCounters;const auto& f=indirectFormatAdmissions;
    std::uint64_t sum=c.planned+c.gpu+c.restarts+c.cameraDivisor+vertexWarpCounters.planned+vertexWarpCounters.draws;
    for(auto v:c.refused)sum+=v;
    for(auto v:vertexWarpCounters.refused)sum+=v;
    char line[256];
    auto shown=[](float value){return double(std::clamp(value,-99999.f,99999.f));};
    if(sum!=reported){
        reported=sum;
        std::snprintf(line,sizeof(line),"Native indirect constant offsets (%s, limit %.2f texels): planned=%llu gpu=%llu vertices=%llu per-pixel=%llu "
            "camera-divisor=%llu restarts=%llu largest=%.2f\n",indirectConstantLimit<0?"off":indirectConstantLimit>0?"approximate":"exact",
            shown(indirectConstantLimit),static_cast<unsigned long long>(c.planned),static_cast<unsigned long long>(c.gpu),
            static_cast<unsigned long long>(c.vertices),static_cast<unsigned long long>(c.perPixel),
            static_cast<unsigned long long>(c.cameraDivisor),static_cast<unsigned long long>(c.restarts),shown(c.largestAccepted));
        sink(line);
        const auto& r=c.refused;using R=IndirectConstantRefusal;
        auto n=[&](R why){return static_cast<unsigned long long>(r[unsigned(why)]);};
        std::snprintf(line,sizeof(line),"Native indirect constant refusals: deviation=%llu (largest %.2f texels) scale=%llu shared=%llu texture=%llu "
            "units=%llu projected=%llu; map scans=%llu cached=%llu\n",n(R::deviation),shown(c.largestRefused),n(R::scale),n(R::shared),
            n(R::texture),n(R::units),n(R::projected),static_cast<unsigned long long>(indirectStatsCache.scans),
            static_cast<unsigned long long>(indirectStatsCache.hits));
        sink(line);
        {const auto& w=vertexWarpCounters;using W=VertexWarpRefusal;
         auto refused=[&](W why){return static_cast<unsigned long long>(w.refused[unsigned(why)]);};
         std::snprintf(line,sizeof(line),"Native vertex warps (%s): planned=%llu draws=%llu vertices=%llu without floats=%llu copied=%llu units=%llu "
            "units left with the constant=%llu\n",vertexWarpEnabled?"on":"off",static_cast<unsigned long long>(w.planned),
            static_cast<unsigned long long>(w.draws),static_cast<unsigned long long>(w.vertices),static_cast<unsigned long long>(vertexWarpIntegerVertices),
            static_cast<unsigned long long>(vertexWarpRepeatVertices),
            static_cast<unsigned long long>(w.units),static_cast<unsigned long long>(w.constantUnits));
         sink(line);
         std::snprintf(line,sizeof(line),"Native vertex warp refusals: record=%llu role=%llu texgen=%llu attribute=%llu uniform=%llu range=%llu\n",
            refused(W::record),refused(W::role),refused(W::texgen),refused(W::attribute),refused(W::uniform),refused(W::range));
         sink(line);}
        if(c.last.warped){
            std::snprintf(line,sizeof(line),"Native indirect constant last: map=%u coord=%u offset=%.5f,%.5f (%.2f,%.2f texels) deviation=%.2f dest=%ux%u\n",
                unsigned(c.last.map),unsigned(c.last.coordinate),shown(c.last.offset[0]),shown(c.last.offset[1]),
                shown(c.last.offset[0]*c.lastWidth),shown(c.last.offset[1]*c.lastHeight),shown(c.last.deviation),
                unsigned(c.lastWidth),unsigned(c.lastHeight));
            sink(line);
        }
    }
    const std::uint64_t admitted=f.rgb565+f.rgba8+f.cmpr;
    if(admitted!=formats&&admitted){
        formats=admitted;
        std::snprintf(line,sizeof(line),"Native indirect draws with warp maps admitted in Alpha 7: rgb565=%llu rgba8=%llu cmpr=%llu last=%ux%u format=%u\n",
            static_cast<unsigned long long>(f.rgb565),static_cast<unsigned long long>(f.rgba8),static_cast<unsigned long long>(f.cmpr),
            unsigned(f.lastWidth),unsigned(f.lastHeight),unsigned(f.lastFormat));
        sink(line);
    }
}
}
