#include "gx_command_draw.hpp"
#include "gx_geometry_pica.hpp"
#include "gx_texture_pica.hpp"
#include "gx_pica.hpp"
#include "runtime/gx_vertex_transform.hpp"
#include "runtime/gx_transform_failure.hpp"
#include "runtime/gx_raster_plan.hpp"
#include "runtime/gx_raster_color.hpp"
#include "runtime/gx_texture_commands.hpp"
#include "runtime/gx_fog.hpp"
#include "runtime/gx_fog_pass.hpp"
#include "runtime/gx_indirect.hpp"
#if defined(__3DS__) || defined(DUSK_INDIRECT_DIAGNOSTIC_TEST)
#include "runtime/gx_indirect_payload108.hpp"
#endif
#include "runtime/gx_early_alpha.hpp"
#include "runtime/gx_material_liveness.hpp"
#include "runtime/gx_profile.hpp"
#include "runtime/gx_rigid_vertex.hpp"
#include "runtime/gx_clip_geometry.hpp"
#include "runtime/gx_camera_clip.hpp"
#include "runtime/gx_i4_bake_cache68.hpp"
#include "runtime/gx_compact_cache.hpp"
#include "native/color_lookup.hpp"
#include "runtime/gx_stream.hpp"
#include <algorithm>
#include <cmath>
#include <bit>
#include <cstring>
#include "runtime/gx_state_serial.hpp"
#include <optional>

#if defined(__3DS__) || defined(DUSK_TRANSFORM_DIAGNOSTIC_TEST) || defined(DUSK_INDIRECT_DIAGNOSTIC_TEST)
extern "C" void OSReport(const char*,...);
#endif
namespace dusk::native {
namespace {
// Address-stable, bounded derived resources; command render owner only.
gx::bake68::Cache exactI4Cache;
// Degraded stand-in for an untranslatable TEV material: the first sampled
// texture modulated by raster color 0 (either one alone when the other is
// absent). Blend, depth and alpha test still come from the original raster plan.
bool simplifiedMaterial(const gx::MaterialState& s,gx::PicaMaterialPlan& p){
    using gx::MaterialSource;using gx::MaterialOperand;using gx::MaterialFunction;
    p={};
    int textured=-1;
    for(unsigned i=0;i<s.stageCount;++i){
        const auto& stage=s.stages[i];
        if(stage.textureEnabled&&stage.texture<8&&stage.coordinate<s.texgenCount){textured=int(i);break;}
    }
    const bool raster=s.channelCount>0;
    auto& stage=p.stages[0];
    auto channel=[&](gx::PicaMaterialChannel& c,MaterialOperand operand){
        if(textured>=0&&raster)c={MaterialFunction::modulate,{gx::MaterialInput{MaterialSource::texture0,operand},gx::MaterialInput{MaterialSource::primary,operand}}};
        else if(textured>=0)c={MaterialFunction::replace,{gx::MaterialInput{MaterialSource::texture0,operand}}};
        else if(raster)c={MaterialFunction::replace,{gx::MaterialInput{MaterialSource::primary,operand}}};
        else c={MaterialFunction::replace,{gx::MaterialInput{MaterialSource::constant,operand}}};
    };
    channel(stage.rgb,MaterialOperand::color);channel(stage.alpha,MaterialOperand::alpha);
    stage.constant={255,255,255,255};
    if(textured>=0){p.textures[0]={s.stages[textured].texture,s.stages[textured].coordinate};p.textureCount=1;}
    p.stageCount=1;
    return true;
}
// Only known representational limits may select the visual stand-in. Missing
// source state and unknown/new planner errors remain hard failures.
bool visualMaterialLimit(gx::MaterialPlanError error){
    switch(error){
    case gx::MaterialPlanError::stages:case gx::MaterialPlanError::operation:
    case gx::MaterialPlanError::constantRange:case gx::MaterialPlanError::konstFraction:
    case gx::MaterialPlanError::constantConflict:case gx::MaterialPlanError::swap:
    case gx::MaterialPlanError::textureUnits:case gx::MaterialPlanError::equation:return true;
    default:return false;
    }
}
// A planner may stop on a visual limit before reaching a later malformed input.
// Validate even inputs/textures discarded by the stand-in. This is deliberately
// conservative for dead TEV inputs; unsupported dependencies must not hide a
// missing register, stale retained display-list token or missing TLUT.
bool validFallbackInputs(const gx::MaterialState& state,const gx::CommandRegisters& registers,const char*& why){
    unsigned rgb=state.colorValid,alpha=state.colorValid;
    auto konst=[&](unsigned selector,bool isAlpha){
        if(selector<8)return true;
        if(!isAlpha&&selector>=12&&selector<=15)return bool(state.konstValid&(1u<<(selector-12)));
        return selector>=16&&selector<=31&&bool(state.konstValid&(1u<<((selector-16)%4)));
    };
    if(state.indirectCount){why="GX fallback indirect state unsupported";return false;}
    for(unsigned i=0;i<state.stageCount;++i){
        const auto& stage=state.stages[i];
        if(stage.indirect){why="GX fallback indirect state unsupported";return false;}
        bool sampledTexture=stage.textureEnabled;
        auto input=[&](unsigned id,bool isAlpha){
            if(isAlpha?id<4:id<8)return bool(((isAlpha||(id&1))?alpha:rgb)&(1u<<(isAlpha?id:id/2)));
            if(isAlpha?id==6:id==14)return konst(isAlpha?stage.konstAlpha:stage.konstColor,isAlpha);
            const bool texture=isAlpha?id==4:(id==8||id==9);
            const bool raster=isAlpha?id==5:(id==10||id==11);
            if(texture||raster){
                if(raster&&stage.raster==7)return true;
                const auto swap=texture?stage.textureSwap:stage.rasterSwap;
                if(!(state.swapValid&(1u<<swap)))return false;
                if(texture){sampledTexture=true;return stage.textureEnabled;}
                return stage.raster==0&&state.channelCount>0;
            }
            return true;
        };
        for(auto id:stage.color)if(!input(id,false)){why="GX fallback TEV source missing/unsupported";return false;}
        for(auto id:stage.alpha)if(!input(id,true)){why="GX fallback TEV source missing/unsupported";return false;}
        if(sampledTexture){
            if(stage.coordinate>=state.texgenCount){why="GX fallback texture coordinate invalid";return false;}
            gx::Texture texture;
            if(!gx::resolveCommandTexture(registers,stage.texture,texture,why))return false;
        }
        rgb|=1u<<stage.colorOp.output;alpha|=1u<<stage.alphaOp.output;
    }
    return true;
}
// Failure-only proof: all per-draw planning/resource/reservation checks have
// already succeeded. Keep its bounded palette cache off the healthy stack path.
// Once every original position and index proves zero side-plane coverage,
// later lighting/texgen arithmetic is intentionally unused, not declared valid.
[[gnu::noinline]] bool normalOverflowOutsideSides(const gx::CommandDraw& draw,
                                                 const gx::RasterPlan& raster,
                                                 std::span<const u16> indices){
    if(raster.clipDisabled||!gx::currentTargetExtent.valid())return false;
    switch(draw.primitive){
    case GX_TRIANGLES:case GX_QUADS:case GX_TRIANGLESTRIP:case GX_TRIANGLEFAN:break;
    default:return false; // Expanded lines/points have different coverage.
    }
    const unsigned count=draw.vertexCount,stride=draw.plan.packedStride();
    if(count<3||count>65535||!stride||count>draw.vertices.size()/stride||
       indices.empty()||indices.size()%3||indices.size()>196605)return false;
    // The adapter built these from the validated original topology before any
    // transform. Check the complete stream, including unreferenced vertices below.
    for(auto index:indices)if(index>=count)return false;
    gx::VertexTransformPlan position;
    if(!gx::planVertexTransform(draw.registers,0,position))return false;
    unsigned common=15;
    for(unsigned i=0;i<count;++i){
        gx::DecodedVertex decoded;gx::TransformedVertex vertex;
        // Full original decode preserves every encoded attribute/array bound
        // and nonfinite-source check. Only shading arithmetic is unevaluated.
        if(!draw.vertex(i,decoded)||!gx::transformVertex(position,decoded,vertex))return false;
        common&=gx::picaClipSideMask(vertex.clip);
    }
    // This uses enlarged homogeneous half-spaces and float24 range limits;
    // no divide by w, near-plane assumption, speculative normal, or epsilon clamp.
    return common!=0;
}
#if defined(__3DS__) || defined(DUSK_TRANSFORM_DIAGNOSTIC_TEST)
// Fatal-only observer: the recoverable side-plane proof has already refused.
// Independent of the early TNF incident; never changes draw admission.
[[gnu::noinline]] void captureFatalNormalTransform(const gx::CommandDraw& draw,
    const gx::VertexTransformPlan& transform,unsigned vertexIndex,bool normalRequired,
    gx::VertexTransformError originalError){
    static bool captured=false;
    if(captured)return;
    captured=true;
    auto sink=+[](const char* tag,unsigned a,unsigned b,unsigned c,unsigned d){
        OSReport("TNFLATE %s %08x %08x %08x %08x\n",tag,a,b,c,d);
    };
    sink("incident",vertexIndex,unsigned(originalError),unsigned(draw.primitive),draw.vertexCount);
    gx::DecodedVertex decoded;
    if(draw.vertex(vertexIndex,decoded)){
        gx::TransformTrace trace{sink};gx::VertexTransformError replayError;
        const bool replayOk=gx::diagnoseVertexTransform(transform,decoded,normalRequired,replayError,trace);
        sink("match",!replayOk&&replayError==originalError,unsigned(originalError),unsigned(replayError),0);
    }else sink("decode_failed",vertexIndex,0,0,0);
    gx::surveyTransformFailure(draw,sink);
    sink("fatal_end",vertexIndex,draw.vertexCount,draw.indexCount,0);
}
#endif

#if defined(__3DS__) || defined(DUSK_INDIRECT_DIAGNOSTIC_TEST)
// Fatal-only, first incident per process. Replay one original vertex after the
// coverage proof refused; never survey/replay the full draw or change admission.
[[gnu::noinline]] void captureFatalIndirectCoordinate(const gx::CommandDraw& draw,
    const gx::VertexTransformPlan& transform,const gx::VertexIndirectPlan& indirect,
    const gx::MaterialState& original,const gx::PicaMaterialPlan& material,
    const std::array<gx::Texture,3>& textures,
    const std::array<const gx::VertexIndirectEffect*,3>& units,unsigned index,bool normalRequired){
    static bool captured=false;
    if(captured)return;
    captured=true;
    auto words=[](const char* tag,unsigned a,unsigned b,unsigned c,unsigned d){
        OSReport("IC105 %s %08x %08x %08x %08x\n",tag,a,b,c,d);
    };
    auto bits=[](float value){return std::bit_cast<std::uint32_t>(value);};
    words("incident",index,unsigned(draw.primitive),draw.vertexCount,draw.indexCount);
    words("plan",transform.projectiveMask(),material.projectiveTexture0,material.textureCount,normalRequired);
    // Borrow only the already validated FINAL indirect sources, at this fatal
    // first-process latch. Separate prefix/schema; IC105 words stay unchanged.
    gx::payload108::emit(units,material.textureCount,indirect,draw.registers,index,
        +[](const char* row){OSReport("%s",row);});
    gx::DecodedVertex decoded;gx::TransformedVertex vertex;gx::VertexTransformError error{};
    const bool decodedOk=draw.vertex(index,decoded);
    const bool transformed=decodedOk&&gx::transformVertex(transform,decoded,vertex,&error,normalRequired);
    words("replay",decodedOk,transformed,unsigned(error),decoded.present);
    if(!transformed){words("end",index,0,0,0);return;}
    words("clip",bits(vertex.clip[0]),bits(vertex.clip[1]),bits(vertex.clip[2]),bits(vertex.clip[3]));
    // All active TEV effects, including effects not selected by the final PICA
    // texture binding. Stage numbers, raw order/scale/matrix and resolved values.
    for(unsigned stage=0;stage<indirect.stages.size();++stage){
        const auto& e=indirect.stages[stage];if(!e.enabled)continue;
        const auto& s=original.stages[stage];const unsigned ind=e.state&3,matrix=(e.state>>9)&15;
        words("effect",stage,e.state,e.coordinate,(unsigned(s.texture)<<16)|s.coordinate);
        words("order_scale",stage,e.bias,draw.registers.bp[0x27],draw.registers.bp[0x25+ind/2]);
        words("source",stage,bits(vertex.texcoord[e.coordinate][0]),bits(vertex.texcoord[e.coordinate][1]),bits(vertex.texcoord[e.coordinate][2]));
        words("base",stage,bits(vertex.texcoord[s.coordinate][0]),bits(vertex.texcoord[s.coordinate][1]),bits(vertex.texcoord[s.coordinate][2]));
        words("flags",stage,bool(transform.projectiveMask()&(1u<<e.coordinate)),bool(transform.projectiveMask()&(1u<<s.coordinate)),decoded.matrix[e.coordinate+1]);
        words("texgen",stage,draw.registers.xf[0x1040+e.coordinate],draw.registers.xf[0x1050+e.coordinate],decoded.matrix[s.coordinate+1]);
        words("base_texgen",stage,draw.registers.xf[0x1040+s.coordinate],draw.registers.xf[0x1050+s.coordinate],0);
        words("scales",bits(e.sourceScale[0]),bits(e.sourceScale[1]),bits(e.baseScale[0]),bits(e.baseScale[1]));
        for(unsigned row=0;row<2;++row)words("matrix",row,bits(e.matrix[row][0]),bits(e.matrix[row][1]),bits(e.matrix[row][2]));
        words("matrix_bp",stage,draw.registers.bp[6+3*(matrix-1)],draw.registers.bp[7+3*(matrix-1)],draw.registers.bp[8+3*(matrix-1)]);
        words("scale_bp",draw.registers.bp[0x30+2*e.coordinate],draw.registers.bp[0x31+2*e.coordinate],draw.registers.bp[0x30+2*s.coordinate],draw.registers.bp[0x31+2*s.coordinate]);
        const auto& t=e.source;
        words("image",t.width,t.height,t.format,unsigned(t.image.size()));
        words("sampler",t.wrapS,t.wrapT,t.minFilter,t.magFilter);
        words("lod",t.mipCount,t.minLod,t.maxLod,bits(t.lodBias));
        const unsigned map=(draw.registers.bp[0x27]>>(6*ind))&7,offset=(map/4)*32+map%4;
        words("identity",stage,unsigned(t.handle>>32),unsigned(t.handle),draw.registers.bp[0x94+offset]);
    }
    for(unsigned unit=0;unit<material.textureCount;++unit){
        const auto* e=units[unit];if(!e)continue;
        const auto& source=vertex.texcoord[e->coordinate];const auto& base=vertex.texcoord[material.textures[unit].coordinate];
        const bool sp=transform.projectiveMask()&(1u<<e->coordinate),bp=unit==0&&material.projectiveTexture0;
        const float sq=sp?source[2]:1,bq=bp?base[2]:1;
        words("unit",unit,unsigned(e-indirect.stages.data()),material.textures[unit].map,material.textures[unit].coordinate);
        words("q",sp,bp,bits(sq),bits(bq));
        words("destination",textures[unit].width,textures[unit].height,textures[unit].format,0);
        // The unchanged evaluator is the outcome oracle. The following cold
        // replay only classifies its first failing arithmetic operation.
        std::array<float,2> output{};
        const bool evaluated=gx::evaluateVertexIndirect(*e,source,base,sp,bp,textures[unit],output);
        words("evaluate",evaluated,bits(output[0]),bits(output[1]),0);
        const char* classification=[&]()->const char*{
            for(float f:source)if(!std::isfinite(f))return "source_nonfinite";
            if(!std::isfinite(sq)||!std::isfinite(bq))return "q_nonfinite";
            if(sq<=0||bq<=0)return "q_nonpositive";
            for(unsigned a=0;a<2;++a)if(!std::isfinite(base[a]))return "base_nonfinite";
            std::array<float,2> sampleUV{};
            for(unsigned a=0;a<2;++a){
                const float divided=source[a]/sq;sampleUV[a]=divided*e->sourceScale[a];
                words("sample_uv",a,bits(divided),bits(e->sourceScale[a]),bits(sampleUV[a]));
                if(!std::isfinite(divided))return "source_divide_overflow";
                if(!std::isfinite(sampleUV[a]))return "sample_scale_nonfinite";
            }
            std::array<float,3> sample{};
            if(!gx::sampleIndirectImage(e->source,sampleUV[0],sampleUV[1],sample))return "sample_rejected";
            words("sample",bits(sample[0]),bits(sample[1]),bits(sample[2]),e->bias);
            for(unsigned c=0;c<3;++c){if(!std::isfinite(sample[c]))return "sample_nonfinite";if(e->bias&(1u<<c))sample[c]-=128;}
            for(unsigned a=0;a<2;++a){
                const float divided=base[a]/bq;float texel=divided*e->baseScale[a];
                words("base_texel",a,bits(divided),bits(e->baseScale[a]),bits(texel));
                if(!std::isfinite(divided))return "base_divide_overflow";
                if(!std::isfinite(texel))return "base_scale_nonfinite";
                for(unsigned c=0;c<3;++c){
                    const float product=e->matrix[a][c]*sample[c];texel+=product;
                    words("accumulate",a*3+c,bits(product),bits(texel),0);
                    if(!std::isfinite(product))return "matrix_product_nonfinite";
                    if(!std::isfinite(texel))return "matrix_sum_nonfinite";
                }
                const unsigned size=a?textures[unit].height:textures[unit].width;
                const float dividedOutput=texel/size,result=dividedOutput*bq;
                words("output",a,bits(dividedOutput),bits(result),size);
                if(!size)return "destination_zero";
                if(!std::isfinite(dividedOutput))return "output_divide_overflow";
                if(!std::isfinite(result))return "output_q_overflow";
            }
            return evaluated?"ok":"evaluator_replay_mismatch";
        }();
        OSReport("IC105 class unit=%u %s\n",unit,classification);
    }
    words("end",index,0,0,0);
}
#endif

bool fogRampTexture(gx::Texture& texture) {
    // A small mathematical utility ramp, not a replacement for a game asset.
    // Immutable static lifetime, normal bounded texture registry/GPU cache.
    static constexpr auto ramp=[] {
        std::array<std::uint8_t,32+256*8> b{};
        b[0]=1;b[2]=1;b[5]=8;b[20]=b[21]=1;b[24]=1;b[31]=32;
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<256;++x)
            b[32+((y/4)*32+x/8)*32+(y%4)*8+x%8]=x;
        return b;
    }();
    static gx::TextureHandle handle{};
    if(!handle&&!gx::registerTexture(ramp,handle,true))return false;
    std::uint32_t token{};
    return gx::textureCommandToken(ramp.data()+32,token)&&gx::textureFromCommandToken(token,texture);
}
bool rasterLookupTextures(std::array<gx::Texture,3>& textures,unsigned first){
    if(first>1)return false;
    static constexpr std::array ramps{dusk::pica::colorLookupBti(0),dusk::pica::colorLookupBti(1)};
    static std::array<gx::TextureHandle,2> handles{};
    for(unsigned n=0;n<2;++n){
        if(!handles[n]&&!gx::registerTexture(ramps[n],handles[n],true))return false;
        std::uint32_t token{};
        if(!gx::textureCommandToken(ramps[n].data()+32,token)||!gx::textureFromCommandToken(token,textures[first+n]))return false;
    }
    return true;
}
// Native PICA engine: precompiled static draws. The first complete
// translation of a static J3D display-list batch records its final raster
// state and PICA material; later frames with an identical state fingerprint
// skip raster/material/fog planning. Lighting, transform, fog parameters,
// textures, matrices and resident vertices are still refreshed every frame.
struct PreparedDraw {
    std::uint32_t keyA{},keyB{};
    gx::RasterPlan raster;
    gx::PicaMaterialPlan material,colorMaterial;
    std::uint8_t originalTextureCount{};
    bool used{},fogEnabled{},fogPrimaryAlpha{},fogPrimaryRed{},rigidProjected{};
};
constexpr unsigned preparedCapacity=512;
std::array<PreparedDraw,preparedCapacity> preparedDraws{};
std::uint64_t preparedHits{},preparedMisses{},preparedStores{},preparedFallbacks{};
struct PreparedKey {std::uint32_t a{},b{};};
PreparedKey preparedKey(const gx::CommandDraw& draw){
    std::uint32_t a=2166136261u,b=0x811c9dc5u^0x5bd1e995u;
    auto mix=[&](std::uint32_t v){a=(a^v)*16777619u;b=(b^(v+0x9e3779b9u+(b<<6)+(b>>2)))*0x01000193u;};
    const auto& r=draw.registers;
    for(unsigned i=0;i<256;++i)if(r.bpValid[i]){mix(i);mix(r.bp[i]);}
    mix(r.bpMask);
    for(unsigned i=0;i<16;++i)if(r.tevColorValid[i]){mix(0x100+i);mix(r.tevColor[i/8][i%8]);}
    for(const auto& t:r.tluts){mix(t.token);mix(t.offset);mix(t.entries);}
    mix(r.tlutOverflow);
    auto cp=[&](unsigned i){if(r.cpValid[i]){mix(0x200+i);mix(r.cp[i]);}};
    cp(0x30);cp(0x40);cp(0x50);cp(0x60);
    for(unsigned i=0;i<8;++i){cp(0x70+i);cp(0x80+i);cp(0x90+i);}
    for(unsigned i=0;i<16;++i)cp(0xb0+i);
    // XF control registers; material/ambient colors (0x100a-0x100d) feed the
    // per-frame lighting plan and are deliberately excluded.
    for(unsigned i=0x1000;i<0x1058;++i)if((i<0x100a||i>0x100d)&&r.xfValid[i]){mix(i);mix(r.xf[i]);}
    mix(r.matrixIndex[0]);mix(r.matrixIndex[1]);
    mix(gx::currentTargetExtent.logicalWidth);mix(gx::currentTargetExtent.logicalHeight);
    mix(unsigned(draw.primitive));mix(draw.vertexCount);mix(draw.indexCount);mix(draw.plan.packedStride());
    mix(gx::staticGeometryHint.generation);mix(std::uint32_t(reinterpret_cast<std::uintptr_t>(gx::staticGeometryHint.list)));
    mix(gx::staticGeometryHint.batch);
    return {a,b};
}
// 1: submitted (or culled); 0: use the complete path; -1: failed after reservation.
int drawPrepared(const PreparedDraw& rec,const gx::CommandDraw& draw,CommandDrawContext& context){
    if(rec.raster.scissor.empty||rec.raster.cull==3){++context.culled;return 1;}
    gx::RasterColorPlan colorPlan;
    if(!gx::planRasterColor(draw.registers,rec.colorMaterial,colorPlan,nullptr))return 0;
    std::uint8_t coordinates=0;
    for(unsigned unit=0;unit<rec.colorMaterial.textureCount;++unit)coordinates|=1u<<rec.colorMaterial.textures[unit].coordinate;
    gx::VertexTransformPlan transform;
    if(!gx::planVertexTransform(draw.registers,coordinates,transform,nullptr))return 0;
    std::array<gx::Texture,3> textures{};
    for(unsigned unit=0;unit<rec.originalTextureCount;++unit){
        const char* why;
        if(!gx::resolveCommandTexture(draw.registers,rec.material.textures[unit].map,textures[unit],why))return 0;
    }
    gx::VertexFogPlan fog;
    if(context.allowVertexFog){
        if(!gx::planVertexFog(draw.registers,fog)||fog.enabled!=rec.fogEnabled)return 0;
        if(fog.enabled&&!rec.fogPrimaryAlpha&&!rec.fogPrimaryRed&&!fogRampTexture(textures[rec.originalTextureCount]))return 0;
    }else if(rec.fogEnabled)return 0;
    const auto count=draw.vertexCount;
    const auto stride=draw.plan.packedStride();
    if(count<3||!stride||count>draw.vertices.size()/stride)return 0;
    std::size_t indexCount=draw.indexCount;
    if(indexCount){
        if(draw.primitive!=GX_TRIANGLES||indexCount%3||indexCount>196605||draw.indices.size()/2<indexCount)return 0;
        for(unsigned i=0;i<indexCount;++i){u16 index;std::memcpy(&index,draw.indices.data()+i*sizeof(index),sizeof(index));if(index>=count)return 0;}
    }else if(draw.primitive==GX_TRIANGLES&&count%3==0)indexCount=count;
    else if(draw.primitive==GX_QUADS&&count%4==0)indexCount=std::size_t(count/4)*6;
    else if(draw.primitive==GX_TRIANGLESTRIP||draw.primitive==GX_TRIANGLEFAN)indexCount=std::size_t(count-2)*3;
    else return 0;
    std::array<std::array<float,2>,3> scales{};
    GxTextureDrawScope textureScope({textures.data(),rec.material.textureCount});
    if(!textureScope)return 0;
    for(unsigned unit=0;unit<rec.material.textureCount;++unit)
        if(!bindGxTexture(textures[unit],scales[unit][0],scales[unit][1],unit,unit==0&&rec.material.projectiveTexture0))return 0;
    const auto& rigidScale=scales[rec.rigidProjected?1:0];
    gx::RigidVertexPlan rigid;gx::DecodedVertex first;
    if(!context.allowSingleRigidShader)return 0;
    if(!draw.vertex(0,first)||!gx::planRigidVertex(draw.registers,first,transform,colorPlan,fog,rec.fogPrimaryAlpha,
        gx::currentTargetExtent.logicalWidth,rigid,rec.rigidProjected))return 0;
    if(rec.rigidProjected)gx::setProjectedScales(rigid,scales[0],scales[1]);
    if(fog.enabled&&rec.fogPrimaryRed)rigid.uniforms[12][2]=2;
    if(rigid.indexedMatrices)for(unsigned i=0;i<count;++i)if(!gx::addRigidMatrix(draw.registers,draw.vertices[i*stride],rigid))return 0;
    gx::GpuGeometryBatch batch;
    if(!reserveGxGeometry(count,indexCount,batch))return 0;
    batch.qPolicy=gx::GeometryQPolicy::positiveOnly;
    auto abandon=[&]{return cancelGxGeometry(batch)?0:-1;};
    if(draw.indexCount)std::memcpy(batch.indices.data(),draw.indices.data(),indexCount*sizeof(u16));
    else{
        std::size_t at=0;
        if(!aurora::gx::dl::expand_triangles(draw.primitive,count,[&](u16 a,u16 b,u16 c){batch.indices[at++]=a;batch.indices[at++]=b;batch.indices[at++]=c;}))
            return abandon();
    }
    {GxProfileScope vertexProfile(GxProfilePart::vertices);
    GxProfileScope section(GxProfilePart::vertexRigid);
    if(!gx::decodeCompactRigidBatch(draw.plan,draw.vertices,draw.arrays,batch.vertices,rigidScale,rigid,context.compactCache))return abandon();
    }
    if(!gx::validateRigidProjection(rigid,batch.vertices))return abandon();
    if(rigid.staticVertices&&batch.vertices.size()>3&&!shrinkGxGeometry(batch,3,batch.indices.size(),true))return abandon();
    if(!submitGxRigidGeometry(rec.raster,rec.material,batch,rigid)){
        cancelGxGeometry(batch);
        if(!context.rejected){context.failure=gxPicaFailure();context.failureKind=DrawFailureKind::submission;if(context.reportFailure)context.reportFailure(draw,context.failure);}
        ++context.rejected;++context.rejectedByKind[unsigned(DrawFailureKind::submission)];return -1;
    }
    gxProfileTotals.rigidVertices+=count;if(rigid.compact)gxProfileTotals.compactVertices+=count;
    ++context.submitted;if(rec.material.quantizedConstants)++context.quantizedDraws;
    if(fog.enabled)++context.vertexFogDraws;
    if(rec.raster.earlyDepthAlpha)++context.constantEarlyDepthDraws;
    gxProfileTotals.vertices+=count;if(colorPlan.needsNormal)gxProfileTotals.litVertices+=count;
    if(fog.enabled)gxProfileTotals.fogVertices+=count;
    return 1;
}
}
// ---------------------------------------------------------------------------
// Native PICA engine: direct J3D shape draws. A static, single-batch rigid
// display list that completed the normal path once is recorded (final raster
// and PICA material, resident vertices, persistent indices, matrix palette
// order). Later calls with the same GX state fingerprint skip the display list
// entirely: lighting, transform, fog, textures and matrices are refreshed from
// the live registers and the shape is drawn with one native call.
namespace {
struct NativeShapeRecord {
    std::uint32_t generation{};const void* list{};std::uint32_t keyA{},keyB{};
    std::uint32_t cacheEpoch{},indexEpoch{};
    gx::RasterPlan raster;gx::PicaMaterialPlan material,colorMaterial;
    gx::DecodedVertex first;
    std::array<std::int8_t,10> matrixOffsets{};std::array<std::uint8_t,10> matrixOrder{};unsigned matrixCount{};
    float normalScale{},tex0Scale{1},tex1Scale{1};
    std::array<float,3> origin{}; // of the resident positions (RigidVertexPlan::origin)
    const std::byte* vertices{};const std::uint16_t* indices{};unsigned indexCount{},vertexCount{};
    std::uint8_t originalTextureCount{};
    bool valid{},multiTexture{},fogEnabled{},fogPrimaryAlpha{},fogPrimaryRed{};
    // Values baked into `material` as PICA constants (TEV colour/konst banks,
    // fog colour). They are not part of the lookup key: the game animates them
    // every frame for lit actors, and a new value only needs new constants.
    std::uint32_t dynamic{};
    // The recorded raster was resolved from a constant TEV alpha; such a record
    // cannot take new constants and is re-recorded instead.
    bool constantEarlyDepth{};
    // A display list can produce several batches (large or multi-format
    // shapes). Each batch has its own record; the list is replayable once
    // every batch of one call was recorded (batchCount = their number).
    std::uint8_t batch{},batchCount{};
    bool pending{}; // recorded during the list call that is still running
    // Base texture plus one texture projected from position (lit terrain).
    // The projector moves, so every replay re-proves a positive divisor.
    bool rigidProjected{};
    // Bounds of the resident positions of a projected record: one test of the
    // box replaces the per-vertex divisor proof while the whole box is in
    // front of the projector.
    bool boxValid{};std::array<float,3> boxLow{},boxHigh{};
    std::array<std::uint32_t,6> parts{}; // key components, for miss attribution
};
constexpr unsigned nativeMaxBatches=12;
constexpr unsigned nativeShapeCapacity=1024,nativeShapeProbe=8;
std::array<NativeShapeRecord,nativeShapeCapacity> nativeShapes{};
CommandDrawContext* nativeContext{};
std::uint64_t nativeHits{},nativeMisses{},nativeRecords{},nativeKeyMisses{};
// Diagnostics: constants refreshed in place, refresh refusals, and which key
// component differed from the closest record of the same list on a key miss
// (bp, texture, cp, xf, matrix index, target; [6] = no record for the list).
std::uint64_t native2dDraws{},native2dZeroAlpha{},native2dAlphaBlend{};
std::uint64_t nativeConstantRefreshes{},nativeConstantRefusals{};
std::array<std::uint64_t,7> nativeMissParts{};
// Large key components, valid while the register bank's state epoch stands.
struct NativeKeyCache {bool valid{};std::uint32_t epoch{},bp{},texture{},cp{},xf{};};
NativeKeyCache nativeKeyCache;
std::uint64_t nativeKeyCacheHits{};
// Same-state continuation: consecutive replayed shapes of one material differ
// only in geometry and matrices. While the GX state epoch and the PICA state
// serial stand, the plans, bound textures and uniform block of the previous
// replayed shape are reused; only its matrix palette is rebuilt.
struct NativeChain {
    bool valid{};
    std::uint32_t stateEpoch{},picaSerial{},matrixTextures0{},matrixTextures1{},dynamic{};
    gx::TargetExtent extent{};
    gx::RasterPlan raster;gx::PicaMaterialPlan material,colorMaterial;
    std::uint32_t firstPresent{};std::uint8_t firstNormalCount{},originalTextureCount{};
    bool multiTexture{},fogEnabled{},fogPrimaryAlpha{},fogPrimaryRed{},rigidProjected{};
    gx::RigidVertexPlan plan; // planned uniforms before the per-shape palette and scales
    std::array<std::array<float,2>,3> scales{};
};
NativeChain nativeChain;
bool nativeChainEnabled=true;
std::uint64_t nativeChained{};
// Why a static rigid batch was not recorded (diagnostics): 0 recorded,
// 1 batch limit, 2 not compact, 3 not resident, 4 projected, 5 carrier/I4/
// fallback, 6 indirect, 7 signed-q/camera/depth route, 8 clip disabled,
// 9 fog substituted, 10 expanded/index mismatch, 11 pool, 12 CPU vertex path.
std::array<std::uint64_t,13> nativeRecordOutcome{};
// Records made by the display-list call in progress.
const void* nativePendingList{};unsigned nativePendingCount{};
unsigned nativeHome(const void* list,unsigned batch){
    return unsigned(((reinterpret_cast<std::uintptr_t>(list)>>2)+batch*0x9e3779b1u)*2654435761u)%nativeShapeCapacity;
}
// Linear probing over (list, batch, state key): instances of one model share
// a list but differ in material/lighting state, so each keeps its own record.
NativeShapeRecord* nativeFind(const void* list,PreparedKey key,unsigned batch){
    const unsigned home=nativeHome(list,batch);
    for(unsigned n=0;n<nativeShapeProbe;++n){auto& rec=nativeShapes[(home+n)%nativeShapeCapacity];
        if(rec.valid&&rec.list==list&&rec.batch==batch&&rec.keyA==key.a&&rec.keyB==key.b)return &rec;}
    return nullptr;
}
NativeShapeRecord& nativeClaim(const void* list,PreparedKey key,unsigned batch){
    if(auto* rec=nativeFind(list,key,batch))return *rec;
    const unsigned home=nativeHome(list,batch);
    for(unsigned n=0;n<nativeShapeProbe;++n){auto& rec=nativeShapes[(home+n)%nativeShapeCapacity];if(!rec.valid)return rec;}
    // Never evict a record of the call in progress: its list would stay incomplete.
    static unsigned victim{};
    for(unsigned n=0;n<nativeShapeProbe;++n){auto& rec=nativeShapes[(home+(victim++%nativeShapeProbe))%nativeShapeCapacity];if(!rec.pending)return rec;}
    return nativeShapes[home];
}
// The key covers the state that decides the recorded plans (raster, material
// structure, vertex layout). Values that the replay refreshes from the live
// registers are left out: TEV colour/konst banks and fog colour (constants,
// see nativeDynamic), fog curve parameters, projection and viewport. Keeping
// them in the key made every lit actor miss whenever its environment colours
// moved, i.e. every frame.
PreparedKey nativeStateKey(const gx::CommandRegisters& r,std::uint32_t generation,const void* list,
                           std::array<std::uint32_t,6>* parts=nullptr){
    // Raw register words, no per-register validity branches: a stale invalid
    // word can only produce a miss. FNV-1a per component.
    // The four large components only change with the bank's state epoch
    // (shapes of one material share them); the per-shape rest is mixed below.
    auto& kc=nativeKeyCache;
    if(!kc.valid||kc.epoch!=r.stateEpoch){
        auto lane=[](std::uint32_t seed){return 2166136261u^seed;};
        std::uint32_t bp=lane(1),texture=lane(2),cp=lane(3),xf=lane(4);
        auto mix=[](std::uint32_t& h,std::uint32_t v){h=(h^v)*16777619u;};
        for(unsigned i=0;i<0x80;++i)mix(bp,r.bp[i]);
        for(unsigned i=0x80;i<0xc0;++i)mix(texture,r.bp[i]);
        for(unsigned i=0xc0;i<0xe0;++i)mix(bp,r.bp[i]);
        // 0xe0-0xe7 TEV colour banks, 0xe8-0xf0 and 0xf2 fog curve/range/colour:
        // refreshed per replay. 0xf1 keeps only fog type and projection kind.
        mix(bp,r.bp[0xf1]>>20);
        for(unsigned i=0xf3;i<0x100;++i)mix(bp,r.bp[i]);
        for(const auto& t:r.tluts){mix(texture,t.token);mix(texture,t.offset^(t.entries<<16));}
        mix(texture,r.tlutOverflow);
        // CP 0x30/0x40 mirror the matrix index registers mixed per shape.
        mix(cp,r.cp[0x50]);mix(cp,r.cp[0x60]);
        for(unsigned i=0;i<8;++i){mix(cp,r.cp[0x70+i]);mix(cp,r.cp[0x80+i]);mix(cp,r.cp[0x90+i]);}
        for(unsigned i=0;i<16;++i)mix(cp,r.cp[0xb0+i]);
        for(unsigned i=0x1000;i<0x100a;++i)mix(xf,r.xf[i]);
        for(unsigned i=0x100e;i<0x1018;++i)mix(xf,r.xf[i]);
        // 0x1018/0x1019 matrix indices: mixed per shape. 0x101a-0x1025 viewport
        // and projection parameters: planned per replay.
        for(unsigned i=0x1026;i<0x1058;++i)mix(xf,r.xf[i]);
        kc={true,r.stateEpoch,bp,texture,cp,xf};
    }else ++nativeKeyCacheHits;
    const std::uint32_t bp=kc.bp,texture=kc.texture,cp=kc.cp,xf=kc.xf;
    const std::uint32_t matrix=r.matrixIndex[0]*0x9e3779b1u^r.matrixIndex[1];
    const std::uint32_t target=(gx::currentTargetExtent.logicalWidth|(gx::currentTargetExtent.logicalHeight<<16))^(r.bpMask*0x85ebca6bu);
    if(parts)*parts={bp,texture,cp,xf,matrix,target};
    std::uint32_t a=2166136261u^generation,b=0x9e3779b9u^std::uint32_t(reinterpret_cast<std::uintptr_t>(list));
    for(const auto v:{bp,texture,cp,xf,matrix,target}){a=(a^v)*16777619u;b=((b<<5)|(b>>27))^v;}
    return {a,b};
}
// Signature of the register values that become PICA constants of a recorded
// material: both TEV colour banks and the fog colour.
std::uint32_t nativeDynamic(const gx::CommandRegisters& r){
    std::uint32_t h=2166136261u;
    for(unsigned bank=0;bank<2;++bank)for(unsigned i=0;i<8;++i)h=(h^r.tevColor[bank][i])*16777619u;
    h=(h^std::uint32_t(r.tevColorValid.to_ulong()))*16777619u;
    return (h^r.bp[0xf2])*16777619u;
}
// ---------------------------------------------------------------------------
// Native PICA engine: immediate draw records. Immediate-mode draws (J2D panes,
// HUD, particles, effects) repeat the same GX state every frame with new
// vertices and a new position matrix; each one used to be planned from the
// registers again (raster, TEV material, colour, transform, rigid uniforms)
// for four vertices. The first complete translation of an unlit, fog-free
// multi-texture GPU draw records its final raster plan, PICA material and
// uniform block; a later draw with the same state fingerprint only resolves
// and binds its textures, reloads the position matrix, decodes its vertices
// and submits. Everything the recorded plans were derived from is part of
// the fingerprint; anything else takes the complete path, which records again.
struct ImmediateRecord {
    bool used{};
    std::uint32_t keyA{},keyB{},extra{};
    gx::TargetExtent extent{};
    gx::RasterPlan raster;gx::PicaMaterialPlan material;
    gx::RigidVertexPlan plan; // planned uniforms before decode (texture fractions not folded in)
    std::array<std::array<float,2>,3> scales{};
    std::uint8_t textureCount{};
};
constexpr unsigned immediateCapacity=256,immediateProbe=4;
std::array<ImmediateRecord,immediateCapacity> immediateRecords{};
bool immediateEnabled=true;
std::uint64_t immediateHits{},immediateStores{},immediateFallbacks{},immediateMisses{};
// State outside nativeStateKey that the recorded plans depend on: TEV colour
// banks and fog (constants), channel material/ambient colours, viewport and
// projection, and the texture matrices read by the active texgens.
std::uint32_t immediateExtra(const gx::CommandDraw& draw){
    const auto& r=draw.registers;
    std::uint32_t h=nativeDynamic(r);
    auto mix=[&](std::uint32_t v){h=(h^v)*16777619u;};
    for(unsigned i=0x100a;i<=0x100d;++i)mix(r.xf[i]);
    for(unsigned i=0x101a;i<=0x1025;++i)mix(r.xf[i]);
    for(unsigned i=0xe8;i<=0xf0;++i)mix(r.bp[i]);
    const unsigned gens=std::min<unsigned>(r.xf[0x103f]&15,8);
    for(unsigned c=0;c<gens;++c){
        const unsigned m=c<4?(r.matrixIndex[0]>>(6+6*c))&63:(r.matrixIndex[1]>>(6*(c-4)))&63;
        if(m<60)for(unsigned k=0;k<8;++k)mix(r.xf[4*m+k]);
        const unsigned postRow=r.xf[0x1050+c]&63;
        if(postRow<61)for(unsigned k=0;k<8;++k)mix(r.xf[0x500+4*postRow+k]);
    }
    mix(unsigned(draw.primitive));mix(draw.plan.packedStride());
    return h;
}
ImmediateRecord* immediateFind(PreparedKey key,std::uint32_t extra){
    for(unsigned n=0;n<immediateProbe;++n){auto& rec=immediateRecords[(key.a+n)%immediateCapacity];
        if(rec.used&&rec.keyA==key.a&&rec.keyB==key.b&&rec.extra==extra)return &rec;}
    return nullptr;
}
ImmediateRecord& immediateClaim(PreparedKey key){
    for(unsigned n=0;n<immediateProbe;++n){auto& rec=immediateRecords[(key.a+n)%immediateCapacity];if(!rec.used)return rec;}
    static unsigned victim{};
    auto& rec=immediateRecords[(key.a+(victim++%immediateProbe))%immediateCapacity];rec.used=false;return rec;
}
// 1: submitted; 0: use the complete path; -1: failed after reservation.
int drawImmediate(const ImmediateRecord& rec,const gx::CommandDraw& draw,CommandDrawContext& context){
    const auto& r=draw.registers;
    if(std::memcmp(&rec.extent,&gx::currentTargetExtent,sizeof(rec.extent)))return 0;
    const auto count=draw.vertexCount;
    const auto stride=draw.plan.packedStride();
    if(count<3||!stride||count>draw.vertices.size()/stride)return 0;
    std::size_t indexCount;
    switch(draw.primitive){
    case GX_TRIANGLES:if(count%3)return 0;indexCount=count;break;
    case GX_QUADS:if(count%4)return 0;indexCount=std::size_t(count/4)*6;break;
    case GX_TRIANGLESTRIP:case GX_TRIANGLEFAN:indexCount=std::size_t(count-2)*3;break;
    default:return 0;
    }
    std::array<gx::Texture,3> textures{};
    const unsigned gens=std::min<unsigned>(r.xf[0x103f]&15,8);
    for(unsigned unit=0;unit<rec.textureCount;++unit){
        const char* why;
        // The fingerprint covers the texture matrices of the active texgens only.
        if(rec.material.textures[unit].coordinate>=gens)return 0;
        if(!gx::resolveCommandTexture(r,rec.material.textures[unit].map,textures[unit],why))return 0;
    }
    std::array<std::array<float,2>,3> scales{};
    GxTextureDrawScope textureScope({textures.data(),rec.textureCount});
    if(!textureScope)return 0;
    for(unsigned unit=0;unit<rec.textureCount;++unit)
        if(!bindGxTexture(textures[unit],scales[unit][0],scales[unit][1],unit,false))return 0;
    // The recorded texture rows hold these scales (padded texture extents).
    if(scales!=rec.scales)return 0;
    gx::RigidVertexPlan rigid=rec.plan;
    rigid.matrixOffsets.fill(-1);rigid.normalOffsets.fill(-1);rigid.matrices=0;
    if(!r.matrixIndexValid[0]||!gx::addRigidMatrix(r,r.matrixIndex[0]&63,rigid))return 0;
    gx::GpuGeometryBatch batch;
    if(!reserveGxGeometry(count,indexCount,batch))return 0;
    batch.qPolicy=gx::GeometryQPolicy::positiveOnly;
    auto abandon=[&]{return cancelGxGeometry(batch)?0:-1;};
    std::size_t at=0;
    if(!aurora::gx::dl::expand_triangles(draw.primitive,count,[&](u16 a,u16 b,u16 c){batch.indices[at++]=a;batch.indices[at++]=b;batch.indices[at++]=c;}))
        return abandon();
    {GxProfileScope vertexProfile(GxProfilePart::vertices);
    GxProfileScope section(GxProfilePart::vertexRigid);
    if(!gx::decodeCompactRigidBatchMT(draw.plan,draw.vertices,draw.arrays,batch.vertices,rigid)||rigid.staticVertices)return abandon();
    }
    if(!submitGxRigidGeometry(rec.raster,rec.material,batch,rigid)){
        cancelGxGeometry(batch);
        if(!context.rejected){context.failure=gxPicaFailure();context.failureKind=DrawFailureKind::submission;if(context.reportFailure)context.reportFailure(draw,context.failure);}
        ++context.rejected;++context.rejectedByKind[unsigned(DrawFailureKind::submission)];return -1;
    }
    gxProfileTotals.rigidDecodedVertices+=count;gxProfileTotals.rigidVertices+=count;gxProfileTotals.compactVertices+=count;
    gxProfileTotals.vertices+=count;
    ++context.submitted;if(rec.material.quantizedConstants)++context.quantizedDraws;
    return 1;
}
// Two plans with the same combiner structure differ only in constants.
bool sameMaterialStructure(gx::PicaMaterialPlan a,gx::PicaMaterialPlan b){
    for(auto* plan:{&a,&b}){
        for(auto& stage:plan->stages)stage.constant={};
        plan->bufferConstant={};plan->quantizedConstants=false;
    }
    return !std::memcmp(&a,&b,sizeof(a));
}
// Rebuild the constants of a recorded material from the live TEV colours and
// fog colour, repeating the recorded draw's own material steps (decode, plan,
// projective routing, vertex fog). Any structural difference refuses; the
// caller then takes the complete path, which records again.
bool refreshNativeConstants(NativeShapeRecord& rec,const gx::CommandRegisters& r,const CommandDrawContext& context,
                            const gx::VertexFogPlan& fog,std::uint8_t projectiveMask){
    if(rec.constantEarlyDepth||rec.raster.earlyDepthAlpha)return false;
    gx::MaterialState state;
    if(!gx::decodeMaterial(r,state)||state.indirectCount)return false;
    // Recorded shapes never carry indirect stages (the complete path zeroes
    // them only after planning a vertex-indirect effect).
    for(unsigned i=0;i<state.stageCount;++i)if(state.stages[i].indirect)return false;
    gx::PicaMaterialPlan material;
    const auto policy=context.allowQuantizedConstants&&!rec.raster.alphaInterval.enabled?
        gx::MaterialPolicy::quantizedConstants:gx::MaterialPolicy::exactConstants;
    if(!gx::planPicaMaterial(state,material,nullptr,policy))return false;
    if(rec.raster.alphaBand&&!gx::appendAlphaBand(material,rec.raster.alphaBandUpper))return false;
    const gx::PicaMaterialPlan colorMaterial=material;
    if(!sameMaterialStructure(colorMaterial,rec.colorMaterial))return false;
    if(!gx::routeProjectiveTexture(material,projectiveMask))return false;
    if(!gx::appendVertexFog(material,fog,rec.fogPrimaryAlpha,rec.fogPrimaryRed))return false;
    if(!sameMaterialStructure(material,rec.material))return false;
    rec.material=material;rec.colorMaterial=colorMaterial;return true;
}
// Positive projective divisor for every resident vertex of a projected
// record. The divisor is affine in the position, so its minimum over the
// bounding box bounds every vertex from below (and the box maximum of the
// magnitude bounds the cancellation headroom from above); the extra margin
// covers the rounding of the per-vertex sums. A box that does not pass takes
// the exact per-vertex proof.
bool nativeProjectionPositive(const gx::RigidVertexPlan& rigid,const NativeShapeRecord& rec){
    if(rec.boxValid&&(rigid.projectedTexture||rigid.multiProjected)){
        const auto& m=rigid.multiProjected?rigid.uniforms[gx::multiUnitRow(0)+2]:rigid.uniforms[rigid.projectionBase()+2];
        float q=m[3],magnitude=std::abs(m[3]);
        for(unsigned c=0;c<3;++c){
            const float a=m[c]*rec.boxLow[c],b=m[c]*rec.boxHigh[c];
            q+=std::min(a,b);magnitude+=std::max(std::abs(a),std::abs(b));
        }
        if(std::isfinite(q)&&std::isfinite(magnitude)&&q>std::max(1e-5f,magnitude*1.01e-4f)+magnitude*1e-6f)return true;
    }
    return gx::validateRigidProjection(rigid,std::span<const gx::GpuVertex>(reinterpret_cast<const gx::GpuVertex*>(rec.vertices),rec.vertexCount));
}
void nativeListEnded(const void* list,std::uint32_t batches){
    // The call recorded every batch: the list becomes replayable. Otherwise
    // drop this call's records; a partly recorded list always takes the
    // normal path.
    if(nativePendingList==list&&nativePendingCount){
        const bool complete=nativePendingCount==batches&&batches<=nativeMaxBatches;
        for(unsigned batch=1;batch<=batches&&batch<=nativeMaxBatches;++batch){
            const unsigned home=nativeHome(list,batch);
            for(unsigned n=0;n<nativeShapeProbe;++n){auto& rec=nativeShapes[(home+n)%nativeShapeCapacity];
                if(!rec.pending||rec.list!=list||rec.batch!=batch)continue;
                rec.pending=false;
                if(complete)rec.batchCount=std::uint8_t(batches);else rec.valid=false;
            }
        }
    }
    nativePendingList=nullptr;nativePendingCount=0;
}
struct NativeListHook {NativeListHook(){gx::staticListEnded=nativeListEnded;}} nativeListHook;
}
void nativeShapeStatistics(std::uint64_t& hits,std::uint64_t& misses,std::uint64_t& records,std::uint64_t& keyMisses){
    hits=nativeHits;misses=nativeMisses;records=nativeRecords;keyMisses=nativeKeyMisses;
}
void nativeShapeDiagnostics(std::uint64_t& refreshes,std::uint64_t& refusals,std::array<std::uint64_t,7>& missParts){
    refreshes=nativeConstantRefreshes;refusals=nativeConstantRefusals;missParts=nativeMissParts;
}
void nativeShapeChainStatistics(std::uint64_t& chained,std::uint64_t& keyCacheHits){chained=nativeChained;keyCacheHits=nativeKeyCacheHits;}
void nativeShapeRecordOutcomes(std::array<std::uint64_t,13>& outcomes){outcomes=nativeRecordOutcome;}
void native2dStatistics(std::uint64_t& draws,std::uint64_t& zeroAlpha,std::uint64_t& alphaBlend){draws=native2dDraws;zeroAlpha=native2dZeroAlpha;alphaBlend=native2dAlphaBlend;}
void setNativeShapeChain(bool enabled){nativeChainEnabled=enabled;nativeChain.valid=false;}
void immediateDrawStatistics(std::uint64_t& hits,std::uint64_t& misses,std::uint64_t& stores,std::uint64_t& fallbacks){
    hits=immediateHits;misses=immediateMisses;stores=immediateStores;fallbacks=immediateFallbacks;
}
void setImmediateDrawRecords(bool enabled){immediateEnabled=enabled;for(auto& rec:immediateRecords)rec.used=false;}
void preparedDrawStatistics(std::uint64_t& hits,std::uint64_t& misses,std::uint64_t& stores,std::uint64_t& fallbacks){
    hits=preparedHits;misses=preparedMisses;stores=preparedStores;fallbacks=preparedFallbacks;
}
std::uint64_t clipDisabledGpuDraws;
// Triangle-fog draws kept on the GPU rigid path, and the one-shot switch a
// draw uses to restart itself on the CPU form.
std::uint64_t rigidTriangleFogDraws;
bool rigidTriangleFogSuppressed;
// One-shot switch: translate the draw again without its vertex warp (see
// DrawVisualFallback::indirectDivisor).
bool vertexIndirectSuppressed;
// Fog-overlay meshes on the GPU: the terrain materials that need it carry
// three textures with a projected one, which the multi-texture shader plans
// since 2026-11-14 (title: 2035 vertices a frame left the CPU vertex route).
// SD switch rigid-triangle-fog.txt (0 = the CPU form).
bool rigidTriangleFogEnabled=true;
void setRigidTriangleFog(bool enabled){rigidTriangleFogEnabled=enabled;}
std::array<std::uint64_t,8> rigidTriangleFogGate;
std::uint64_t rigidTriangleFogStatistics(std::array<std::uint64_t,8>* gates){if(gates)*gates=rigidTriangleFogGate;return rigidTriangleFogDraws;}
bool drawGxCommand(const gx::CommandDraw& draw,void* ptr){
    GxProfileScope profile(GxProfilePart::command);
    if(!ptr)return false;
    auto& context=*static_cast<CommandDrawContext*>(ptr);
    // Sparse-frame draw classification (see gxDrawClassTicks).
    struct DrawClassScope {
        unsigned cls=4;std::uint64_t start=gxSparseVertexProfiling?gxProfileTick():0;
        ~DrawClassScope(){if(start){gxDrawClassTicks[cls]+=gxProfileTick()-start;++gxDrawClassCount[cls];}}
    } drawClass;
    // Optional class filter: silence detailed timers for other draw classes.
    struct ClassFilterScope {bool saved=gxSparseVertexProfiling;~ClassFilterScope(){gxSparseVertexProfiling=saved;}} classFilter;
    if(gxProfileClassFilter>=0){
        const bool ortho=draw.registers.xfValid[0x1026]&&draw.registers.xf[0x1026]==1;
        if((gxProfileClassFilter==2)!=ortho)gxSparseVertexProfiling=false;
    }
    // Native PICA engine: precompiled static draw fast path. Measured no gain
    // for the ~60 static rigid batches per frame (candidate 99); kept off.
    constexpr bool preparedDrawsEnabled=false;
    bool preparedArmed=false;PreparedKey preparedSlot{};
    if(preparedDrawsEnabled&&context.allowRigidGpu&&gx::staticGeometryHint.generation&&gx::staticGeometryHint.list&&!gxVertexLayoutTracing){
        static const auto preparedIndex=registerGxNamedProfile("draw.prepared");GxNamedProfileSpan preparedSpan(preparedIndex);
        preparedSlot=preparedKey(draw);
        const auto& rec=preparedDraws[preparedSlot.a%preparedCapacity];
        if(rec.used&&rec.keyA==preparedSlot.a&&rec.keyB==preparedSlot.b){
            const int result=drawPrepared(rec,draw,context);
            if(result>0){++preparedHits;return true;}
            if(result<0)return false;
            ++preparedFallbacks;
        }else ++preparedMisses;
        preparedArmed=true;
    }
    // Native PICA engine: immediate draw records (see ImmediateRecord).
    ImmediateRecord* immediateSlot=nullptr;PreparedKey immediateKey{};std::uint32_t immediateExtraKey{};bool immediateCaptured=false;
    if(immediateEnabled&&context.allowRigidGpu&&!gx::staticGeometryHint.list&&!draw.indexCount&&!gxVertexLayoutTracing&&
       (draw.primitive==GX_QUADS||draw.primitive==GX_TRIANGLES||draw.primitive==GX_TRIANGLESTRIP||draw.primitive==GX_TRIANGLEFAN)){
        static const auto immediateIndex=registerGxNamedProfile("draw.immediate");GxNamedProfileSpan immediateSpan(immediateIndex);
        immediateKey=nativeStateKey(draw.registers,0x494d4d00u,nullptr);
        immediateExtraKey=immediateExtra(draw);
        if(auto* rec=immediateFind(immediateKey,immediateExtraKey)){
            const int result=drawImmediate(*rec,draw,context);
            if(result>0){++immediateHits;drawClass.cls=1;return true;}
            if(result<0)return false;
            rec->used=false;++immediateFallbacks;
        }else ++immediateMisses;
        immediateSlot=&immediateClaim(immediateKey);
    }
    bool carrierAttempt=false;
    std::uint32_t carrierFailureVertex=UINT32_MAX;
    auto fail=[&](const char* why,DrawFailureKind kind=DrawFailureKind::state){
        const bool skippable=context.skipUnsupportedDraws&&
            (kind==DrawFailureKind::state||kind==DrawFailureKind::protocol||kind==DrawFailureKind::resource);
        // Report the first rejection, and under the skip policy the first of
        // each distinct reason (reasons are static strings).
        bool report=!context.rejected;
        {
            CommandDrawContext::RejectReason* reason=nullptr;
            for(unsigned n=0;n<context.rejectReasonCount&&!reason;++n)
                if(context.rejectReasons[n].why==why||!std::strcmp(context.rejectReasons[n].why,why))reason=&context.rejectReasons[n];
            if(!reason&&context.rejectReasonCount<context.rejectReasons.size()){
                reason=&context.rejectReasons[context.rejectReasonCount++];*reason={why,0,0,0,false};
            }
            if(reason){
                ++reason->draws;reason->vertices+=draw.vertexCount;
                if(skippable&&!reason->reported)report=true;
                reason->reported=true;
            }
        }
        if(report){context.failure=why;context.failureKind=kind;if(context.reportFailure)context.reportFailure(draw,why);}
        if(carrierAttempt){
            ++context.rasterCarrierRejects;
            if(!context.rasterCarrierFirstFailure){context.rasterCarrierFirstFailure=why;context.rasterCarrierFirstVertex=carrierFailureVertex;}
        }
        ++context.rejected;++context.rejectedByKind[unsigned(kind)];
        if(skippable){++context.skippedDraws;return true;}
        return false;
    };
    auto allowsFallback=[&](DrawVisualFallback reason){
        if(reason==DrawVisualFallback::projectiveDivisor&&!context.allowProjectiveDivisorFallback)return false;
        if(reason==DrawVisualFallback::alphaCompare&&!context.allowAlphaCompareFallback)return false;
        if(reason==DrawVisualFallback::indirectDivisor&&!context.allowIndirectDivisorFallback)return false;
        return context.allowDegradedDraws&&(context.allowVisualFallback?context.allowVisualFallback(draw,reason):
            draw.registers.xfValid[0x1026]&&draw.registers.xf[0x1026]==0);
    };
    std::array<bool,unsigned(DrawVisualFallback::count)> simplified{};
    const bool points=draw.primitive==GX_POINTS;
    const bool lines=draw.primitive==GX_LINES||draw.primitive==GX_LINESTRIP;
    const bool expanded=points||lines;
    gx::RasterPlan raster;gx::RasterError rasterError;
    { static const auto index=registerGxNamedProfile("adapter.raster");GxNamedProfileScope detail(index);
    if(!gx::planRaster(draw.registers,raster,&rasterError,context.allowVertexFog?gx::FogPolicy::vertexApproximation:gx::FogPolicy::requireSupported,
        context.allowRgba6Storage?gx::ColorStoragePolicy::rgba8ForRgba6:gx::ColorStoragePolicy::rgb8Only,gx::ClipPolicy::validateGeometry,context.allowConstantEarlyDepth,context.allowAlphaUnion,context.alphaIntervalPolicy)){
        // A dual alpha comparison without an exact route: degraded draws use one test.
        if(rasterError!=gx::RasterError::alpha||!allowsFallback(DrawVisualFallback::alphaCompare)||
           !gx::planRaster(draw.registers,raster,&rasterError,context.allowVertexFog?gx::FogPolicy::vertexApproximation:gx::FogPolicy::requireSupported,
        context.allowRgba6Storage?gx::ColorStoragePolicy::rgba8ForRgba6:gx::ColorStoragePolicy::rgb8Only,gx::ClipPolicy::validateGeometry,context.allowConstantEarlyDepth,context.allowAlphaUnion,context.alphaIntervalPolicy,true))
            return fail(gx::rasterErrorText(rasterError));
        if(raster.alphaRelaxed)simplified[unsigned(DrawVisualFallback::alphaCompare)]=true;
    }
    }
    if(raster.scissor.empty||(!expanded&&raster.cull==3)){++context.culled;return true;}
    gx::MaterialState state;gx::MaterialError materialError;
    { static const auto index=registerGxNamedProfile("adapter.materialDecode");GxNamedProfileScope detail(index);
    if(!gx::decodeMaterial(draw.registers,state,&materialError))return fail("GX command material state missing");
    }
    const auto originalMaterial=state;
    gx::VertexIndirectPlan indirect;
    if(context.allowVertexIndirect){
        const char* why;
        if(!gx::planVertexIndirect(draw.registers,state,indirect,why))return fail(why);
        state.indirectCount=0;for(auto& stage:state.stages)stage.indirect=0;
        if(vertexIndirectSuppressed&&indirect.enabled){indirect={};simplified[unsigned(DrawVisualFallback::indirectDivisor)]=true;}
    }
    gx::PicaMaterialPlan material;gx::MaterialPlanError planError;
    bool carrier=false,lookupCarrier=false,exactI4Pending=false;
    const char* exactI4OriginalFailure="GX exact I4 material unsupported";
    { static const auto index=registerGxNamedProfile("adapter.materialPlan");GxNamedProfileScope detail(index);
    const auto policy=context.allowQuantizedConstants&&!raster.alphaInterval.enabled?gx::MaterialPolicy::quantizedConstants:gx::MaterialPolicy::exactConstants;
    if(!gx::planPicaMaterial(state,material,&planError,policy)){
        exactI4OriginalFailure=gx::materialPlanErrorText(planError);
        // Provisional recognition only: freshly resolved original pixels MUST
        // pass the complete legacy or finite-domain sampler proof before submission.
        if(context.allowExactI4Bake&&!expanded&&!raster.clipDisabled&&
           draw.registers.xfValid[0x1026]&&draw.registers.xf[0x1026]==1&&
           draw.registers.bpValid[0xf1]&&!(draw.registers.bp[0xf1]>>20)&&
           !raster.earlyDepthAlpha&&!raster.alphaUnion&&!raster.alphaInterval.enabled&&
           !originalMaterial.indirectCount&&!indirect.enabled&&
           (gx::bake67::plan(originalMaterial,material,gx::bake67::Policy::strictNonzeroI4)||
            gx::bake84::plan(originalMaterial,material,gx::bake84::Policy::exactNearestI4))){
            exactI4Pending=true;
        // The explicit diagnostic retains its original conservative input
        // contract. Automatic lookup must not preempt ordinary alpha liveness.
        }else if(context.diagnosticRasterCarrier||!gx::discardUnobservedTevAlpha(state,raster)||
                 !gx::planPicaMaterial(state,material,&planError,policy)){
        bool raster1Read=false;
        for(unsigned i=0;i<state.stageCount;++i)if(state.stages[i].raster==1){
            for(auto id:state.stages[i].color)raster1Read|=id==10||id==11;
            for(auto id:state.stages[i].alpha)raster1Read|=id==5;
        }
        if(raster1Read&&!context.diagnosticRasterCarrier){
            carrierAttempt=true;++context.rasterCarrierAttempts;
            // Liveness may remove alpha, not the lookup's original indirect veto.
            if(originalMaterial.indirectCount)return fail(gx::materialPlanErrorText(gx::MaterialPlanError::indirect));
            if(!gx::planRasterLookupMaterial(state,material,&planError,policy))return fail(gx::materialPlanErrorText(planError));
            carrier=lookupCarrier=true;
        }else if(context.diagnosticRasterCarrier&&raster1Read){
            carrierAttempt=true;++context.rasterCarrierAttempts;
            // No scene/material IDs. Restrict interactions until independently
            // proved. Active fog additionally needs the existing vertex-fog policy.
            if(expanded||draw.primitive!=GX_TRIANGLES||raster.clipDisabled||
               !draw.registers.xfValid[0x1026]||draw.registers.xf[0x1026]!=0||
               !draw.registers.bpValid[0xf1]||
               ((draw.registers.bp[0xf1]>>20)&&!context.allowVertexFog)||
               (draw.registers.bp[0x43]&64)||raster.earlyDepthAlpha||raster.alphaUnion||raster.alphaInterval.enabled||
               originalMaterial.indirectCount||indirect.enabled)
                return fail("GX diagnostic raster carrier interaction unsupported");
            if(!gx::planRasterCarrierMaterial(originalMaterial,material,&planError))
                return fail(gx::materialPlanErrorText(planError));
            carrier=true;
        }else{
            // Automatic recovery was already tried above; only the legacy
            // diagnostic's non-carrier branch still needs that attempt here.
            if(!context.diagnosticRasterCarrier||!gx::discardUnobservedTevAlpha(state,raster)||!gx::planPicaMaterial(state,material,&planError,policy)){
                if(raster.alphaInterval.enabled||!visualMaterialLimit(planError)||!allowsFallback(DrawVisualFallback::tevMaterial))
                    return fail(gx::materialPlanErrorText(planError));
                const char* why;
                if(!validFallbackInputs(state,draw.registers,why))return fail(why,DrawFailureKind::resource);
                simplifiedMaterial(state,material);simplified[unsigned(DrawVisualFallback::tevMaterial)]=true;
            }
        }
    }
    }
    } // Original planner fallback; exact I4 proof is separate.
    // Band alpha test: the hardware keeps the lower bound, the combiner alpha
    // takes the upper one. No room in the combiner leaves the draw unsupported.
    if(raster.alphaBand&&(carrier||exactI4Pending||!gx::appendAlphaBand(material,raster.alphaBandUpper)))
        return fail(gx::rasterErrorText(gx::RasterError::alpha));
    const bool constantEarlyDepth=raster.earlyDepthAlpha;
    if(!gx::resolveConstantEarlyAlpha(material,raster))return fail("GX early depth alpha is not constant");
    gx::RasterColorPlan colorPlan;gx::RasterColorError colorError;
    std::optional<gx::RasterColorPlan> secondColorPlan; // Disengaged: no light-array initialization on ordinary draws.
    const gx::PicaMaterialPlan colorMaterial=material; // prepared-draw lighting input
    bool fogSubstituted=false;
    { static const auto index=registerGxNamedProfile("adapter.lightingPlan");GxNamedProfileScope detail(index);
    if(carrier){
        secondColorPlan.emplace();
        if(!gx::planRasterColorChannel(draw.registers,0,lookupCarrier?material.rasterReadMask[0]:1,colorPlan,&colorError)||
           !gx::planRasterColorChannel(draw.registers,1,lookupCarrier?material.rasterReadMask[1]:1,*secondColorPlan,&colorError))return fail(gx::rasterColorErrorText(colorError));
    }else if(!gx::planRasterColor(draw.registers,material,colorPlan,&colorError))return fail(gx::rasterColorErrorText(colorError));
    }
    const unsigned originalTextureCount=material.sourceTextures();
    std::uint8_t coordinates=indirect.coordinates;
    std::array<gx::Texture,3> textures{};
    for(unsigned unit=0;unit<originalTextureCount;++unit)coordinates|=1u<<material.textures[unit].coordinate;
    gx::VertexTransformPlan transform;gx::VertexTransformError transformError;
    { static const auto index=registerGxNamedProfile("adapter.transformPlan");GxNamedProfileScope detail(index);
    if(!gx::planVertexTransform(draw.registers,coordinates,transform,&transformError))return fail(gx::vertexTransformErrorText(transformError));
    }
    if(exactI4Pending&&transform.projectiveMask())return fail(exactI4OriginalFailure);
    if(carrier&&!lookupCarrier&&transform.projectiveMask())return fail("GX diagnostic raster carrier projective unsupported");
    if(!gx::routeProjectiveTexture(material,transform.projectiveMask()))return fail("GX multiple projected textures pending");
    std::array<const gx::VertexIndirectEffect*,3> indirectUnits{};
    { static const auto index=registerGxNamedProfile("adapter.textureResolve");GxNamedProfileScope detail(index);
    for(unsigned unit=0;unit<originalTextureCount;++unit){
        const auto binding=material.textures[unit];
        const char* textureFailure;
        if(!gx::resolveCommandTexture(draw.registers,binding.map,textures[unit],textureFailure))return fail(textureFailure,DrawFailureKind::resource);
        int first=-1;bool plain=false;
        for(unsigned stage=0;stage<originalMaterial.stageCount;++stage){
            const auto& s=originalMaterial.stages[stage];
            if(!s.textureEnabled||s.texture!=binding.map||s.coordinate!=binding.coordinate)continue;
            if(first>=0&&indirect.stages[first].state!=indirect.stages[stage].state){
                // One PICA unit has one coordinate set: degraded draws keep
                // the texture and leave its warp out.
                if(!allowsFallback(DrawVisualFallback::sharedIndirect))
                    return fail("GX shared texture has different indirect coordinates");
                simplified[unsigned(DrawVisualFallback::sharedIndirect)]=plain=true;
            }
            first=stage;
        }
        if(first>=0&&indirect.stages[first].enabled&&!plain)indirectUnits[unit]=&indirect.stages[first];
    }
    }
    static const auto midIndex=registerGxNamedProfile("draw.mid");GxNamedProfileSpan midSpan(midIndex);
    if(exactI4Pending){
        gx::Texture derived;gx::bake68::Error error;gx::bake67::Error proof;
        // Preserve the original command token/format and every original pixel.
        // Only this local draw binding receives the permanent derived image.
        if(!exactI4Cache.fetch(originalMaterial,textures[0],derived,material,error,proof,gx::bake68::Mode::exactNearestI4)){
            if(error==gx::bake68::Error::proof)return fail(exactI4OriginalFailure);
            return fail("GX exact I4 derived resource unavailable",DrawFailureKind::resource);
        }
        textures[0]=derived;
    }
#if defined(__3DS__)
    if(gxVertexLayoutTracing){
        gx::DecodedVertex first; if(!draw.vertex(0,first))return fail("GX traced first vertex invalid");
        OSReport("Native vertex layout n=%u present=%lx cp50=%lx cp60=%lx mat=%lx/%lx lights=%x/%x diff=%u/%u att=%u/%u req=%u vertex=%u ambient=%u indirect=%u clip=%u tex=%u/%u/%u/%u gen=%lx/%lx/%lx post=%lx/%lx/%lx fog=%lx range=%lx\n",
            unsigned(draw.vertexCount),static_cast<unsigned long>(first.present),static_cast<unsigned long>(draw.registers.cp[0x50]),static_cast<unsigned long>(draw.registers.cp[0x60]),
            static_cast<unsigned long>(draw.registers.matrixIndex[0]),static_cast<unsigned long>(draw.registers.matrixIndex[1]),
            unsigned(colorPlan.channels[0].mask),unsigned(colorPlan.channels[1].mask),unsigned(colorPlan.channels[0].diffuse),unsigned(colorPlan.channels[1].diffuse),
            unsigned(colorPlan.channels[0].attenuation),unsigned(colorPlan.channels[1].attenuation),unsigned(colorPlan.required),unsigned(colorPlan.vertex),unsigned(colorPlan.ambientVertex),
            unsigned(indirect.enabled),unsigned(raster.clipDisabled),unsigned(material.textureCount),unsigned(material.textures[0].coordinate),unsigned(material.textures[1].coordinate),unsigned(material.textures[2].coordinate),
            static_cast<unsigned long>(draw.registers.xf[0x1040]),static_cast<unsigned long>(draw.registers.xf[0x1041]),static_cast<unsigned long>(draw.registers.xf[0x1042]),
            static_cast<unsigned long>(draw.registers.xf[0x1050]),static_cast<unsigned long>(draw.registers.xf[0x1051]),static_cast<unsigned long>(draw.registers.xf[0x1052]),
            static_cast<unsigned long>(draw.registers.bp[0xf1]),static_cast<unsigned long>(draw.registers.bp[0xe8]));
    }
#endif
    if(lookupCarrier&&!rasterLookupTextures(textures,originalTextureCount))return fail("GX raster color lookup unavailable",DrawFailureKind::resource);
    bool fogPrimaryAlpha=(!carrier||lookupCarrier)&&!(colorPlan.required&2);
    bool fogPrimaryRed=(!carrier||lookupCarrier)&&!fogPrimaryAlpha&&!(colorPlan.required&1);
    // Only the CPU vertex route can carry the fog factor in the red channel.
    // With a free texture unit the fog ramp gives the same factor and the
    // draw stays on the GPU vertex route (Lakebed water: 7500 vertices a frame).
    if(fogPrimaryRed&&!carrier&&context.preferFogRamp&&material.textureCount<3&&!indirect.enabled)fogPrimaryRed=false;
    gx::VertexFogPlan fog;
    bool triangleFog=false;gx::RasterPlan fogRaster;gx::PicaMaterialPlan fogMaterial;
    unsigned fogTextureUnit=0;
    if(context.allowVertexFog) {
        if(carrier&&!gx::currentTargetExtent.valid())return fail("GX diagnostic raster carrier target invalid");
        if(!gx::planVertexFog(draw.registers,fog))return fail("GX vertex fog parameters invalid");
        if(!carrier&&fog.enabled&&!fogPrimaryAlpha&&!fogPrimaryRed&&material.textureCount==3&&!(colorPlan.lit&2)){
            // Only a constant alpha across the complete original draw can be
            // moved to TEV constants. Varying or lit alpha retains its input.
            float alpha=colorPlan.material[3];bool constant=true;
            if(colorPlan.vertex&2){
                gx::DecodedVertex vertex;
                for(unsigned i=0;i<draw.vertexCount;++i){
                    if(!draw.vertex(i,vertex)||!(vertex.present&(1u<<GX_VA_CLR0))){constant=false;break;}
                    if(!i)alpha=vertex.color[0][3];
                    if(vertex.color[0][3]!=alpha){constant=false;break;}
                }
            }
            if(constant&&std::isfinite(alpha)&&alpha>=0&&alpha<=1){
                const auto byte=std::uint8_t(std::lround(alpha*255));
                if(alpha==float(byte)/255&&gx::substituteConstantPrimaryAlpha(material,byte)){
                    fogSubstituted=true;
                    if(!gx::planRasterColor(draw.registers,material,colorPlan,&colorError))return fail(gx::rasterColorErrorText(colorError));
                    fogPrimaryAlpha=!(colorPlan.required&2);
                }
            }
        }
        if(!carrier&&fog.enabled&&!fogPrimaryAlpha&&!fogPrimaryRed&&material.textureCount==3){
            std::array<std::uint8_t,3> rgb;
            if(gx::proveConstantPrimaryRgb(draw,colorPlan,rgb)){
                auto candidate=material;gx::RasterColorPlan candidateColor;
                if(gx::substituteConstantPrimaryRgb(candidate,rgb)&&
                   gx::planRasterColor(draw.registers,candidate,candidateColor,&colorError)&&!(candidateColor.required&1)){
                    material=candidate;colorPlan=candidateColor;fogPrimaryRed=true;fogSubstituted=true;
#if defined(__3DS__)
                    static unsigned rgbProofReports{};
                    if(rgbProofReports<4){
                        ++rgbProofReports;
                        OSReport("Native fog RGB proof: vertices=%u indices=%u rgb=%u,%u,%u required=%u materialStages=%u textures=%u route=primary-red\n",
                            unsigned(draw.vertexCount),unsigned(draw.indexCount),unsigned(rgb[0]),unsigned(rgb[1]),unsigned(rgb[2]),
                            unsigned(colorPlan.required),unsigned(material.stageCount),unsigned(material.textureCount));
                    }
#endif
                }
            }
        }
        fogTextureUnit=material.textureCount;
        if(!gx::appendVertexFog(material,fog,fogPrimaryAlpha,fogPrimaryRed)){
            if((carrier&&!lookupCarrier)||!context.allowTriangleFog||expanded||!gx::planTriangleFogPass(raster,fogRaster,fogMaterial))
                return fail("GX vertex fog TEV/texture capacity");
            triangleFog=true;
        }
        if(fog.enabled&&!triangleFog&&!fogPrimaryAlpha&&!fogPrimaryRed&&!fogRampTexture(textures[fogTextureUnit]))return fail("GX vertex fog ramp unavailable",carrier?DrawFailureKind::resource:DrawFailureKind::state);
    }
    const auto count=draw.vertexCount;
    const auto stride=draw.plan.packedStride();
    if(!count||(!expanded&&count<3)||!stride||count>draw.vertices.size()/stride)return fail("GX command vertex extent invalid",DrawFailureKind::protocol);
    std::size_t indexCount=draw.indexCount,outputCount=count,quads=0;
    float primitiveWidth=0,textureOffset=0;
    if(indexCount){
        if(draw.primitive!=GX_TRIANGLES||indexCount%3||indexCount>196605||draw.indices.size()/2<indexCount)
            return fail("GX command index extent invalid",DrawFailureKind::protocol);
        // The extent above bounds this native-endian stream. Keep the load
        // unaligned-safe without an out-of-line accessor for every index.
        if(!gx::staticGeometryHint.trustedIndices)for(unsigned i=0;i<indexCount;++i){
            u16 index;std::memcpy(&index,draw.indices.data()+i*sizeof(index),sizeof(index));
            if(index>=count)return fail("GX command index out of range",DrawFailureKind::protocol);
        }
    }else{
        switch(draw.primitive){
        case GX_TRIANGLES:
            if(count%3)return fail("GX triangle count invalid",DrawFailureKind::protocol);
            indexCount=count;break;
        case GX_QUADS:
            if(count%4)return fail("GX quad count invalid",DrawFailureKind::protocol);
            indexCount=std::size_t(count/4)*6;break;
        case GX_TRIANGLESTRIP:case GX_TRIANGLEFAN:indexCount=std::size_t(count-2)*3;break;
        case GX_LINES:case GX_LINESTRIP:case GX_POINTS: {
            if(lines&&(count<2||(draw.primitive==GX_LINES&&count%2)))return fail("GX line count invalid",DrawFailureKind::protocol);
            if(!draw.registers.bpValid[0x22])return fail("GX line/point size missing",DrawFailureKind::protocol);
            const auto sizes=draw.registers.bp[0x22];
            primitiveWidth=float((sizes>>(points?8:0))&255)/6;
            const auto offset=(sizes>>(points?19:16))&7;
            if(offset>5)return fail("GX line/point texture offset invalid",DrawFailureKind::protocol);
            textureOffset=offset?float(1u<<(offset-1))/16:0;
            quads=points?count:draw.primitive==GX_LINES?count/2:count-1;
            if(quads>16383)return fail("GX expanded primitive index capacity",DrawFailureKind::protocol);
            outputCount=quads*4;indexCount=quads*6;
            // GX polygon culling does not cull line/point primitives.
            raster.cull=0;
            break;
        }
        default:return fail("GX command primitive unsupported",DrawFailureKind::protocol);
        }
    }
    std::array<std::array<float,2>,3> scales{};
    // Optional: a draw that translates itself again ends this scope first.
    std::optional<GxTextureDrawScope> textureScope;textureScope.emplace(std::span<const gx::Texture>{textures.data(),material.textureCount});
    if(!*textureScope)return fail(gxTextureFailure(),DrawFailureKind::resource);
    for(unsigned unit=0;unit<material.textureCount;++unit)
        if(!bindGxTexture(textures[unit],scales[unit][0],scales[unit][1],unit,unit==0&&material.projectiveTexture0))return fail(gxTextureFailure(),DrawFailureKind::resource);
    const bool signedQDiagnostic=context.allowDirectSignedQDiagnostic&&
        draw.registers.xfValid[0x1026]&&draw.registers.xf[0x1026]==0&&
        material.projectiveTexture0&&originalTextureCount>0&&!indirect.enabled&&
        !originalMaterial.indirectCount&&!expanded&&!triangleFog&&!raster.alphaUnion&&!raster.alphaInterval.enabled&&
        !simplified[unsigned(DrawVisualFallback::tevMaterial)];
    const bool cameraAlignedEligible=(context.allowCameraAlignedIndirectQ||context.allowCameraAlignedCpuClip)&&
        draw.registers.xfValid[0x1026]&&draw.registers.xf[0x1026]==0&&
        material.projectiveTexture0&&originalTextureCount>0&&indirect.enabled&&indirectUnits[0]&&
        !carrier&&!expanded&&!triangleFog&&!raster.clipDisabled&&!raster.alphaUnion&&!raster.alphaInterval.enabled&&
        !simplified[unsigned(DrawVisualFallback::tevMaterial)];
    bool cameraAlignedProved=false,cameraCpuClip=false;
    auto recordCameraAligned=[&](CameraAlignedIndirectCounts::Total& total){++total.draws;total.vertices+=count;};
    gx::RigidVertexPlan rigid;
    bool rigidGpu=false,gateCounted=false,rigidTriangleFog=false;
    bool signedQPositiveRigidAttempt=false;
    auto recordSignedQPositiveRigid=[&](SignedQPositiveRigidCounts::Total& total){++total.draws;total.vertices+=count;};
    const bool rigidProjected=originalTextureCount==2&&material.projectiveTexture0&&
        material.textures[0].coordinate==1&&material.textures[1].coordinate==0;
    const auto& rigidScale=scales[rigidProjected?1:0];
    const unsigned cpuReason=(raster.alphaUnion||raster.alphaInterval.enabled)?0:triangleFog?1:expanded?3:raster.clipDisabled?4:
        indirect.enabled?5:!originalTextureCount?6:(originalTextureCount>1&&!rigidProjected)?7:
        (!rigidProjected&&material.textures[0].coordinate!=0)?8:9;
    midSpan.stop();
    // Orthographic (2D) draws are unlit; never attempt the lit rigid shaders.
    const bool orthographicDraw=draw.registers.xfValid[0x1026]&&draw.registers.xf[0x1026]==1;
    { static const auto index=registerGxNamedProfile("adapter.rigidPlan");GxNamedProfileScope detail(index);
    // Triangle fog on the GPU: the draw is planned without fog; the fog
    // overlay pass reuses its geometry and uniform block (see the submit).
    const bool rigidFogCandidate=triangleFog&&rigidTriangleFogEnabled&&!rigidTriangleFogSuppressed&&picaRigidTriangleFogAvailable()&&
        fog.function==2&&!fog.orthographic&&fog.a>=0&&fog.b>1&&!orthographicDraw&&!signedQDiagnostic;
    static const gx::VertexFogPlan noFog{};
    // The shaders write the fog factor to primary red when u[12].z is 2 (the
    // material does not read the vertex colour: its red channel is free).
    const bool fogRedCarrier=fog.enabled&&fogPrimaryRed&&!triangleFog;
    if(context.allowSingleRigidShader&&!orthographicDraw&&!exactI4Pending&&!carrier&&(!signedQDiagnostic||context.allowSignedQPositiveRigidGpu)&&context.allowRigidGpu&&!raster.alphaUnion&&!raster.alphaInterval.enabled&&(!triangleFog||rigidFogCandidate)&&!expanded&&!raster.clipDisabled&&!indirect.enabled&&
       ((originalTextureCount==1&&material.textures[0].coordinate==0)||rigidProjected)){
        signedQPositiveRigidAttempt=signedQDiagnostic;
        if(signedQPositiveRigidAttempt)recordSignedQPositiveRigid(context.signedQPositiveRigid.attempts);
        gx::DecodedVertex first;
        if(draw.vertex(0,first))rigidGpu=gx::planRigidVertex(draw.registers,first,transform,colorPlan,triangleFog?noFog:fog,fogPrimaryAlpha,gx::currentTargetExtent.logicalWidth,rigid,rigidProjected);
        if(rigidGpu&&rigidProjected)gx::setProjectedScales(rigid,scales[0],scales[1]);
        if(rigidGpu&&fogRedCarrier)rigid.uniforms[12][2]=2;
#if defined(__3DS__)
        if(!rigidGpu&&rigidProjected){
            // What kept a projected draw on the CPU vertex route (first three).
            static unsigned reports;
            if(reports<3){++reports;const auto& x=draw.registers;
                OSReport("Native projected rigid plan refused: gen0=%08lx post0=%08lx gen1=%08lx post1=%08lx index=%08lx present=%08lx normals=%u fog=%u,%u lit=%u required=%u mask=%02x vertices=%u\n",
                    static_cast<unsigned long>(x.xf[0x1040]),static_cast<unsigned long>(x.xf[0x1050]),static_cast<unsigned long>(x.xf[0x1041]),static_cast<unsigned long>(x.xf[0x1051]),
                    static_cast<unsigned long>(x.matrixIndex[0]),static_cast<unsigned long>(first.present),unsigned(first.normalCount),unsigned(fog.enabled),fog.function,
                    unsigned(colorPlan.lit),unsigned(colorPlan.required),unsigned(colorPlan.channels[0].mask),unsigned(count));}
        }
#endif
        rigidTriangleFog=rigidGpu&&triangleFog;
    }
    // Native multi-texture GPU path: 1-3 texgen coordinates on the PICA.
    // Includes orthographic 2D draws when unlit (vertex/material color only).
    if(!rigidGpu&&!exactI4Pending&&!carrier&&!signedQDiagnostic&&context.allowRigidGpu&&!raster.alphaUnion&&!raster.alphaInterval.enabled&&
       (!triangleFog||rigidFogCandidate)&&!expanded&&(!raster.clipDisabled||!orthographicDraw)&&!indirect.enabled&&
       originalTextureCount<=3&&(!material.projectiveTexture0||(context.allowMultiProjected&&!orthographicDraw))&&(!orthographicDraw||!colorPlan.lit)){
        gx::DecodedVertex first;
        const int fogUnit=!triangleFog&&fog.enabled&&!fogPrimaryAlpha&&!fogPrimaryRed?int(originalTextureCount):-1;
        if(draw.vertex(0,first))rigidGpu=gx::planRigidMultiVertex(draw.registers,first,transform,colorPlan,triangleFog?noFog:fog,fogPrimaryAlpha,
            gx::currentTargetExtent.logicalWidth,material,originalTextureCount,scales,fogUnit,rigid,context.allowMultiProjected);
        if(rigidGpu&&fogRedCarrier)rigid.uniforms[12][2]=2;
        rigidTriangleFog=rigidGpu&&triangleFog;
        if(triangleFog&&!rigidGpu)++rigidTriangleFogGate[7];
    }else if(triangleFog&&!rigidGpu){
        // Diagnostic: first gate that kept a triangle-fog draw on the CPU.
        ++rigidTriangleFogGate[(rigidTriangleFogSuppressed||!rigidTriangleFogEnabled)?0:!picaRigidTriangleFogAvailable()?1:fog.function!=2?2:
            (fog.orthographic||fog.a<0||fog.b<=1||orthographicDraw)?3:material.projectiveTexture0?4:
            originalTextureCount>3?5:6];
        // Native clip-disabled draws: the shader clamps depth instead of the
        // CPU transforming and re-clipping every vertex.
        if(rigidGpu&&raster.clipDisabled){gx::setMultiDepthClamp(rigid);++clipDisabledGpuDraws;}
    }
    if(!rigidGpu&&!orthographicDraw){gateCounted=true;
        // Diagnostic: first gate that kept a perspective draw off the GPU paths.
        const unsigned why=exactI4Pending?1:carrier?2:signedQDiagnostic?3:!context.allowRigidGpu?4:(raster.alphaUnion||raster.alphaInterval.enabled)?5:
            triangleFog?6:expanded?8:indirect.enabled?9:originalTextureCount>3?10:rigidProjected?11:
            material.projectiveTexture0?12:raster.clipDisabled?7:13;
        ++gxPerspCpuGate[why];gxPerspCpuGateVertices[why]+=count;
    }
    if(rigidGpu&&rigid.indexedMatrices){
        // PNMTXIDX is the first direct byte in a validated GX layout. Gather
        // only matrices actually used by this draw; larger palettes fall back.
        bool placed=true;
        for(unsigned i=0;i<count;++i)if(!gx::addRigidMatrix(draw.registers,draw.vertices[i*stride],rigid)){placed=false;break;}
        if(!placed&&!rigid.projectedTexture){
            gx::restartCompactPalette(rigid);placed=true;
            for(unsigned i=0;i<count;++i)if(!gx::addRigidMatrix(draw.registers,draw.vertices[i*stride],rigid)){placed=false;break;}
        }
        if(!placed)for(unsigned i=0;i<1;++i){
            rigidGpu=false;
            // Diagnostic: palette overflow by light count (40+) and distinct matrices (50+).
            unsigned used=0;for(unsigned k=0;k<count;++k){const unsigned m=draw.vertices[k*stride];if(m<=27&&m%3==0)used|=1u<<(m/3);}
            ++gx::gxRigidPlanRejects[40+std::min(rigid.lights,9u)];++gx::gxRigidPlanRejects[50+std::min(unsigned(__builtin_popcount(used)),10u)];
            break;}
    }
    if(signedQPositiveRigidAttempt&&!rigidGpu)recordSignedQPositiveRigid(context.signedQPositiveRigid.planningFailures);
    }
#if defined(__3DS__)
    if(gxVertexLayoutTracing)OSReport("Native rigid path n=%u eligible=%u matrices=%u lights=%u\n",unsigned(count),unsigned(rigidGpu),rigid.matrices,rigid.lights);
#endif
    static const auto reserveIndex=registerGxNamedProfile("draw.reserve");GxNamedProfileSpan reserveSpan(reserveIndex);
    gx::GpuGeometryBatch reservation;
    const auto pairedCount=(outputCount+3)&~std::size_t(3);
    const bool cpuTriangleFog=triangleFog&&!rigidTriangleFog;
    if(!reserveGxGeometry(cpuTriangleFog?pairedCount*2:outputCount,indexCount,reservation))return fail(gxGeometryFailure(),DrawFailureKind::geometry);
    reservation.qPolicy=signedQDiagnostic?gx::GeometryQPolicy::directTexture0Diagnostic:gx::GeometryQPolicy::positiveOnly;
    auto batch=reservation;if(cpuTriangleFog)batch.vertices=batch.vertices.first(outputCount);
    auto cancel=[&](const char* why,DrawFailureKind kind=DrawFailureKind::protocol){
        if(!cancelGxGeometry(reservation)){
            // A backend can fail after committing/emitting part of the draw.
            // Cancellation cannot undo that work; preserve the fatal submit
            // reason and never turn it into a successful visual fallback.
            if(kind==DrawFailureKind::submission)return fail(why,kind);
            return fail("GX geometry cancellation failed",DrawFailureKind::geometry);
        }
        return fail(why,kind);
    };
    auto overlaps=[](std::span<const std::byte> a,std::span<const std::byte> b){
        if(a.empty()||b.empty())return false;
        const auto x=reinterpret_cast<std::uintptr_t>(a.data()),y=reinterpret_cast<std::uintptr_t>(b.data());
        return x<=y?y-x<a.size():x-y<b.size();
    };
    auto aliases=[&](auto bytes){return overlaps(std::as_bytes(reservation.vertices),bytes)||overlaps(std::as_bytes(reservation.indices),bytes);};
    if(aliases(std::as_bytes(draw.vertices))||aliases(std::as_bytes(draw.indices))||
       aliases(std::as_bytes(std::span(&draw.registers,1)))||aliases(std::as_bytes(std::span(&draw.plan,1)))||
       aliases(std::as_bytes(std::span(&draw.arrays,1))))return cancel("GX command input aliases GPU reservation");
    for(const auto& array:draw.arrays)if(aliases(std::as_bytes(array.bytes)))return cancel("GX command input aliases GPU reservation");
    // Build the validated triangle topology before exceptional projective
    // clipping, so the proof uses exactly the primitives submitted to PICA.
    if(!expanded){
        if(draw.indexCount)std::memcpy(batch.indices.data(),draw.indices.data(),indexCount*sizeof(u16));
        else{
            std::size_t at=0;
            if(!aurora::gx::dl::expand_triangles(draw.primitive,count,[&](u16 a,u16 b,u16 c){batch.indices[at++]=a;batch.indices[at++]=b;batch.indices[at++]=c;}))
                return cancel("GX command triangulation failed");
        }
    }
    // One decode/transform per original vertex, directly into reserved GPU
    // storage. No full transformed/colored CPU mesh or duplicate decode pass.
    auto transformOne=[&](unsigned i,gx::GpuVertex& target,bool requirePositiveQ=true,bool* invalidDivisor=nullptr)->const char*{
        if(carrier)carrierFailureVertex=i;
        gx::DecodedVertex decoded;gx::TransformedVertex vertex;std::array<float,4> color,secondColor{};
        const bool sample=gxSparseVertexProfiling&&(i&63)==0;
        {GxProfileScope section(GxProfilePart::vertexDecode,sample);
        if(!draw.vertex(i,decoded))return "GX command vertex decode failed";}
        {GxProfileScope section(GxProfilePart::vertexTransform,sample);
        if(!gx::transformVertex(transform,decoded,vertex,&transformError,colorPlan.needsNormal||(carrier&&secondColorPlan->needsNormal))){
#if defined(__3DS__) || defined(DUSK_TRANSFORM_DIAGNOSTIC_TEST)
            // First incident only. No replay, logging, or counter on healthy vertices.
            static unsigned incidents=0;
            if(incidents==0){
                incidents=1;
                auto sink=+[](const char* tag,unsigned a,unsigned b,unsigned c,unsigned d){
                    OSReport("TNF %s %08x %08x %08x %08x\n",tag,a,b,c,d);
                };
                sink("incident",i,unsigned(transformError),unsigned(draw.primitive),count);
                gx::TransformTrace trace{sink};gx::VertexTransformError replayError;
                const bool replayOk=gx::diagnoseVertexTransform(transform,decoded,colorPlan.needsNormal||(carrier&&secondColorPlan->needsNormal),replayError,trace);
                sink("match",!replayOk&&replayError==transformError,unsigned(transformError),unsigned(replayError),0);
                gx::surveyTransformFailure(draw,sink);
            }
#endif
            return gx::vertexTransformErrorText(transformError);
        }}
        {GxProfileScope section(GxProfilePart::vertexLighting,sample);
        if(!gx::evaluateRasterColor(colorPlan,decoded,color,&colorError,&vertex))return gx::rasterColorErrorText(colorError);
        if(carrier){
            auto& second=secondColor;
            if(!gx::evaluateRasterColor(*secondColorPlan,decoded,second,&colorError,&vertex)){
                carrierFailureVertex=i;return gx::rasterColorErrorText(colorError);
            }
            for(unsigned c=0;c<3;++c)if(!std::isfinite(color[c])||color[c]<0||color[c]>1||
                !std::isfinite(second[c])||second[c]<0||second[c]>1){carrierFailureVertex=i;return "GX diagnostic raster carrier nonfinite/range";}
            if(!lookupCarrier){
                if(color[0]!=color[1]||color[1]!=color[2]){carrierFailureVertex=i;return "GX diagnostic raster0 not grayscale";}
                // Independent fields; the product stays in the material combiner.
                color={second[0],second[1],second[2],color[0]};
            }
        }} // Both independent evaluations belong to the same lighting phase.
        target={};target.position=vertex.clip;target.color=color;
        bool invalidQ=false,invalidIndirectQ=false;
        for(unsigned unit=0;unit<originalTextureCount;++unit){
            auto uv=vertex.texcoord[material.textures[unit].coordinate];
            const float q=unit==0&&material.projectiveTexture0?uv[2]:1;
            if(!std::isfinite(q))return "GX projected texture divisor nonfinite";
            if(q==0||(q<0&&!signedQDiagnostic&&!(cameraAlignedProved&&unit==0)))invalidQ=true;
            // Preserve out-of-range coordinates across interpolation. PICA
            // applies the original clamp/repeat/mirror sampler per fragment.
            // Converted padding repeats the edge texel; the texture binder
            // rejects padded repeat/mirror images that cannot use this mapping.
            if(!std::isfinite(uv[0])||!std::isfinite(uv[1]))return "GX texture coordinate is not finite";
            if(const auto* effect=indirectUnits[unit]){
                const auto& source=vertex.texcoord[effect->coordinate];
                for(float value:source)if(!std::isfinite(value))return "GX vertex indirect coordinate invalid";
                const float sourceQ=transform.projectiveMask()&(1u<<effect->coordinate)?source[2]:1;
                if(sourceQ<=0||q==0||(q<0&&!(cameraAlignedProved&&unit==0))){
                    // Defer only an undefined division with finite inputs.
                    // The exceptional path must prove every incident triangle
                    // invisible before these unevaluated attributes can leave
                    // the reservation. Keep the actual base q unchanged.
                    invalidIndirectQ=true;
                }else{
                    std::array<float,2> adjusted;
                    if(!gx::evaluateVertexIndirect(*effect,source,uv,
                        transform.projectiveMask()&(1u<<effect->coordinate),unit==0&&material.projectiveTexture0,textures[unit],adjusted,cameraAlignedProved&&unit==0))
                        return "GX vertex indirect coordinate invalid";
                    uv[0]=adjusted[0];uv[1]=adjusted[1];
                }
            }
            if(unit==0)target.textureQ=q;
            target.texcoord[unit]={uv[0]*scales[unit][0],q-uv[1]*scales[unit][1]};
            if(unit==0&&material.projectiveTexture0&&!context.allowProjectionMode&&q>0&&q!=1){
                // No projection mode: the divide happens here, per vertex.
                target.texcoord[0]={uv[0]*scales[0][0]/q,1-uv[1]*scales[0][1]/q};target.textureQ=1;
            }
        }
        if(lookupCarrier){
            target.texcoord[originalTextureCount]=dusk::pica::colorLookupUv(secondColor[0],secondColor[1]);
            target.texcoord[originalTextureCount+1]=dusk::pica::colorLookupUv(secondColor[2],secondColor[3]);
        }
        if(fog.enabled&&!triangleFog) {
            GxProfileScope section(GxProfilePart::vertexFog,sample);
            const float factor=gx::evaluateVertexFog(fog,vertex.clip,gx::currentTargetExtent.logicalWidth);
            if(carrier&&(!std::isfinite(factor)||factor<0||factor>1))return "GX diagnostic raster carrier fog invalid";
            if(fogPrimaryAlpha)target.color[3]=factor;
            else if(fogPrimaryRed)target.color[0]=factor;
            else target.texcoord[fogTextureUnit]={(factor*255+.5f)/256,.5f};
        }
        if(invalidDivisor)*invalidDivisor=invalidQ||invalidIndirectQ;
        if(!requirePositiveQ)return nullptr;
        return invalidIndirectQ?"GX vertex indirect coordinate invalid":invalidQ?"GX projected texture divisor invalid":nullptr;
    };
    auto proveCameraAlignedIndirect=[&](){
        // Separate caches/error state: failure cannot alter the original strict
        // loop, invisible proof, diagnostic observer or its failure attribution.
        // No topology shortcut: unused and positive-q vertices are required too.
        auto proofTransform=transform;
        using Reason=CameraAlignedIndirectProofReason;
        CameraAlignedIndirectProofFailure incident;
        auto rejectProof=[&](Reason reason){
            incident.reason=reason;
            if(context.cameraAlignedIndirectFirstFailure.reason==Reason::none)
                context.cameraAlignedIndirectFirstFailure=incident;
            return false;
        };
        for(unsigned i=0;i<count;++i){
            incident={};incident.index=i;
            gx::DecodedVertex decoded;gx::TransformedVertex vertex;gx::VertexTransformError error;
            if(!draw.vertex(i,decoded))return rejectProof(Reason::decode);
            if(!gx::transformVertex(proofTransform,decoded,vertex,&error,colorPlan.needsNormal)){
                incident.transformError=std::uint32_t(error);return rejectProof(Reason::transform);
            }
            const float baseQ=vertex.texcoord[material.textures[0].coordinate][2];
            incident.unit=0;incident.validBits=3;
            incident.baseQBits=std::bit_cast<std::uint32_t>(baseQ);
            incident.clipWBits=std::bit_cast<std::uint32_t>(vertex.clip[3]);
            const auto* source0=indirectUnits[0]; // Required by eligibility.
            const float sourceQ0=proofTransform.projectiveMask()&(1u<<source0->coordinate)?vertex.texcoord[source0->coordinate][2]:1;
            incident.sourceQBits=std::bit_cast<std::uint32_t>(sourceQ0);
            if(!std::isfinite(baseQ))return rejectProof(Reason::baseNonfinite);
            if(baseQ==0)return rejectProof(Reason::baseZero);
            if(incident.baseQBits!=incident.clipWBits)return rejectProof(Reason::cameraMismatch);
            for(unsigned unit=0;unit<originalTextureCount;++unit){
                incident.unit=unit;incident.validBits=1;incident.sourceQBits=0;
                auto uv=vertex.texcoord[material.textures[unit].coordinate];
                const float q=unit==0?baseQ:1;
                // Per-unit base bits are effective, including nonprojected q=1.
                incident.baseQBits=std::bit_cast<std::uint32_t>(q);
                for(float value:uv)if(!std::isfinite(value))return rejectProof(Reason::baseCoordinateNonfinite);
                if(const auto* effect=indirectUnits[unit]){
                    const auto& source=vertex.texcoord[effect->coordinate];
                    const bool projected=bool(proofTransform.projectiveMask()&(1u<<effect->coordinate));
                    const float sourceQ=projected?source[2]:1;
                    incident.sourceQBits=std::bit_cast<std::uint32_t>(sourceQ);incident.validBits|=2;
                    for(float value:source)if(!std::isfinite(value))return rejectProof(Reason::sourceCoordinateNonfinite);
                    if(!std::isfinite(sourceQ)||sourceQ<=0)return rejectProof(Reason::sourceDivisor);
                    std::array<float,2> adjusted;
                    if(!gx::evaluateVertexIndirect(*effect,source,uv,projected,unit==0,textures[unit],adjusted,unit==0))return rejectProof(Reason::indirectEvaluation);
                    uv[0]=adjusted[0];uv[1]=adjusted[1];
                }
                if(!std::isfinite(uv[0]*scales[unit][0])||!std::isfinite(q-uv[1]*scales[unit][1]))return rejectProof(Reason::outputNonfinite);
            }
        }
        // Exact linear clipping preserves q==w; visible homogeneous w>0 then
        // implies q>0. Hardware precision and Tier D interpolation are unproved.
        return true;
    };
    auto captureIndirect=[&](unsigned i,const char* error){
#if defined(__3DS__) || defined(DUSK_INDIRECT_DIAGNOSTIC_TEST)
        if(!std::strcmp(error,"GX vertex indirect coordinate invalid"))
            captureFatalIndirectCoordinate(draw,transform,indirect,originalMaterial,material,textures,indirectUnits,i,
                colorPlan.needsNormal||(carrier&&secondColorPlan->needsNormal));
#else
        (void)i;(void)error;
#endif
    };
    enum class ProjectiveClip { unproved, empty, trimmed };
    unsigned projectiveClipTriangles=0;
    auto recordProjectiveClip=[&](bool culled){
        if(!projectiveClipTriangles)return;
        ++context.projectiveClipDraws;context.projectiveClipTriangles+=projectiveClipTriangles;
#if defined(__3DS__)
        if(context.projectiveClipDraws<=4)OSReport("Native projected clip: homogeneous-side culled=%u removed=%u total=%u\n",
            unsigned(culled),projectiveClipTriangles,unsigned(indexCount/3));
#else
        (void)culled;
#endif
    };
    auto clipInvisibleProjection=[&]()->ProjectiveClip{
        // On this exceptional path validate every original vertex, including
        // consumed UVs/color, before proving that no fragment can sample q.
        // Only divisions by finite nonpositive base/source q are deferred;
        // valid indirect evaluations and all nonfinite errors remain strict.
        for(unsigned i=0;i<count;++i){
            if(transformOne(i,batch.vertices[i],false))return ProjectiveClip::unproved;
            for(const auto& uv:batch.vertices[i].texcoord)
                for(float value:uv)if(!std::isfinite(value))return ProjectiveClip::unproved;
        }
        if(gx::proveNoScreenCoverage(batch.vertices))return cancelGxGeometry(reservation)?ProjectiveClip::empty:ProjectiveClip::unproved;
        if((!raster.clipDisabled||context.allowBehindCameraCull)&&gx::proveBehindCamera(batch.vertices)){
            if(!cancelGxGeometry(reservation))return ProjectiveClip::unproved;
            if(raster.clipDisabled)++context.cameraCulled;
            return ProjectiveClip::empty;
        }
        if(raster.clipDisabled)return ProjectiveClip::unproved;
        auto invalidVertexDivisor=[&](unsigned index,bool& invalid){
            if(!indirect.enabled){invalid=batch.vertices[index].textureQ==0||(!signedQDiagnostic&&batch.vertices[index].textureQ<0);return true;}
            // Source-only indirect q is not the GPU's base textureQ. Reuse
            // the validated transform rather than inventing a GPU divisor or
            // allocating a second mesh/validity array on this rare path.
            gx::GpuVertex unused;
            return !transformOne(index,unused,false,&invalid);
        };
        unsigned survivor=unsigned(count);
        // Degraded draws: a visible triangle with a nonpositive divisor keeps
        // its geometry; its affected vertices get a tiny positive divisor below.
        const bool degradeDivisor=!indirect.enabled&&!signedQDiagnostic&&material.projectiveTexture0&&
            allowsFallback(DrawVisualFallback::projectiveDivisor);
        bool degraded=false;
        for(unsigned i=0;i<batch.indices.size();i+=3){
            const auto& a=batch.vertices[batch.indices[i]];
            const auto& b=batch.vertices[batch.indices[i+1]];
            const auto& c=batch.vertices[batch.indices[i+2]];
            bool invalid=false;
            for(unsigned n=0;n<3;++n){bool vertexInvalid;
                if(!invalidVertexDivisor(batch.indices[i+n],vertexInvalid))return ProjectiveClip::unproved;
                invalid|=vertexInvalid;
            }
            if(!invalid){survivor=batch.indices[i];continue;}
            const unsigned maskA=gx::picaClipSideMask(a.position),maskB=gx::picaClipSideMask(b.position),maskC=gx::picaClipSideMask(c.position);
            if(!(maskA&maskB&maskC)&&!gx::picaTriangleOutsideSides(a.position,b.position,c.position)){
                if(degradeDivisor){degraded=true;if(survivor==count)survivor=batch.indices[i];continue;}
#if defined(__3DS__)
                OSReport("Native projected clip refused: triangle=%u indices=%u,%u,%u q=%.9g,%.9g,%.9g side-masks=%x,%x,%x\n",
                    i/3,unsigned(batch.indices[i]),unsigned(batch.indices[i+1]),unsigned(batch.indices[i+2]),
                    double(a.textureQ),double(b.textureQ),double(c.textureQ),maskA,maskB,maskC);
                const gx::GpuVertex* vertices[]{&a,&b,&c};
                for(unsigned n=0;n<3;++n){const auto& p=vertices[n]->position;
                    OSReport("Native projected clip vertex %u: %.9g %.9g %.9g %.9g\n",unsigned(batch.indices[i+n]),
                        double(p[0]),double(p[1]),double(p[2]),double(p[3]));}
#endif
                return ProjectiveClip::unproved;
            }
            ++projectiveClipTriangles;
            // count is outside the validated u16 index range (vertexCount
            // is u16). This reservation-local marker is never submitted.
            batch.indices[i]=batch.indices[i+1]=batch.indices[i+2]=u16(count);
        }
        if(survivor==count)return cancelGxGeometry(reservation)?ProjectiveClip::empty:ProjectiveClip::unproved;
        // Keep the arena reservation and primitive order. Replace only proved
        // invisible triangles with zero-area primitives at a surviving vertex.
        // Invalid-divisor vertices are now unreferenced; copy that valid vertex
        // into their slots so backend validation never consumes an invalid q.
        // Every surviving triangle retains its original positions/UVs/colors.
        for(unsigned i=0;i<batch.indices.size();i+=3)
            if(batch.indices[i]==count)
                batch.indices[i]=batch.indices[i+1]=batch.indices[i+2]=u16(survivor);
        if(degraded){
            // Vertices still referenced by a kept triangle: replace only the
            // divisor. texcoord0 is {s, q - t}; keep s and t, move q to epsilon.
            constexpr float epsilon=1.0f/4096;
            for(unsigned i=0;i<batch.indices.size();++i){
                auto& vertex=batch.vertices[batch.indices[i]];
                if(vertex.textureQ>0)continue;
                vertex.texcoord[0][1]+=epsilon-vertex.textureQ;vertex.textureQ=epsilon;
            }
            simplified[unsigned(DrawVisualFallback::projectiveDivisor)]=true;
        }
        for(unsigned i=0;i<count;++i){bool invalid;
            if(!invalidVertexDivisor(i,invalid))return ProjectiveClip::unproved;
            if(invalid)batch.vertices[i]=batch.vertices[survivor];
        }
        return ProjectiveClip::trimmed;
    };
    reserveSpan.stop();
    {GxProfileScope vertexProfile(GxProfilePart::vertices);
    // CPU vertex route with a projected texture: per-pixel divide (see
    // setGxTextureProjection). The rigid routes keep the 2D mode of the bind.
    auto cpuProjection=[&]{if(material.projectiveTexture0&&originalTextureCount&&context.allowProjectionMode)setGxTextureProjection(true);};
    if(expanded) {
        cpuProjection();
        const auto& extent=gx::currentTargetExtent;
        for(unsigned q=0;q<quads;++q) {
            gx::GpuVertex a,b;
            const unsigned first=points?q:draw.primitive==GX_LINES?q*2:q;
            if(const char* error=transformOne(first,a)){captureIndirect(first,error);return cancel(error);}
            if(!points) {if(const char* error=transformOne(first+1,b)){captureIndirect(first+1,error);return cancel(error);}}
            else b=a;
            if(a.position[3]<=0||b.position[3]<=0)return cancel("GX line/point crosses camera plane");
            float dx=0,dy=0;
            if(lines) {
                dx=-(b.position[1]/b.position[3]-a.position[1]/a.position[3])*extent.logicalWidth*.5f;
                dy=-(b.position[0]/b.position[3]-a.position[0]/a.position[3])*extent.logicalHeight*.5f;
                const float length=std::hypot(dx,dy);
                if(length>1e-5f){dx/=length;dy/=length;}else{dx=1;dy=0;}
            }
            for(unsigned corner=0;corner<4;++corner) {
                auto& vertex=batch.vertices[q*4+corner];vertex=corner>=2?b:a;
                const float side=corner&1?1.f:-1.f;
                const float px=points?side*primitiveWidth*.5f:-dy*side*primitiveWidth*.5f;
                const float py=points?(corner>=2?1.f:-1.f)*primitiveWidth*.5f:dx*side*primitiveWidth*.5f;
                vertex.position[0]-=2*py/extent.logicalHeight*vertex.position[3];
                vertex.position[1]-=2*px/extent.logicalWidth*vertex.position[3];
                for(unsigned unit=0;unit<originalTextureCount;++unit) {
                    const auto coordinate=material.textures[unit].coordinate;
                    const auto reg=0x30+2*coordinate;
                    if(draw.registers.bpValid[reg]&&(draw.registers.bp[reg]&(1u<<(points?19:18)))) {
                        if(points&&(corner&1))vertex.texcoord[unit][0]+=textureOffset*scales[unit][0];
                        if(points?corner>=2:corner&1)vertex.texcoord[unit][1]-=textureOffset*scales[unit][1];
                    }
                }
            }
            const unsigned base=q*4;const unsigned pattern[]{0,1,2,1,3,2};
            for(unsigned i=0;i<6;++i)batch.indices[q*6+i]=base+pattern[i];
        }
    }else {
        const bool rigidPackingAttempt=rigidGpu;
        bool nativeRigid=false;
        // Immediate draw record: the planned uniform block, before the decode
        // folds this draw's fixed-point texture fractions into it.
        if(immediateSlot&&rigidGpu&&rigid.multiTexture&&!rigid.indexedMatrices&&!rigid.compactPalette&&rigid.matrices==1){
            immediateSlot->plan=rigid;immediateCaptured=true;
        }
        if(rigidGpu&&rigid.multiTexture){GxProfileScope section(GxProfilePart::vertexRigid);
            nativeRigid=gx::decodeCompactRigidBatchMT(draw.plan,draw.vertices,draw.arrays,batch.vertices,rigid);
            if(!nativeRigid){rigidGpu=false;++gx::gxRigidPlanRejects[63];} // no float fallback layout for the multi-texture shader
        }else if(rigidGpu){GxProfileScope section(GxProfilePart::vertexRigid);
            nativeRigid=gx::decodeCompactRigidBatch(draw.plan,draw.vertices,draw.arrays,batch.vertices,rigidScale,rigid,context.compactCache);
            if(!nativeRigid&&!rigid.compactPalette)nativeRigid=gx::decodeRigidBatch(draw.plan,draw.vertices,draw.arrays,batch.vertices,rigid.color,rigidScale,rigid.matrixOffsets);}
        if(nativeRigid)gxProfileTotals.rigidDecodedVertices+=count;
        if(rigidGpu&&!nativeRigid)++gx::gxCompactDecodeRejects[60];
        if(rigidGpu&&!nativeRigid)for(unsigned i=0;i<count;++i){
            gx::DecodedVertex decoded;
            if(!draw.vertex(i,decoded)||!gx::packRigidVertex(rigid,decoded,rigidScale,batch.vertices[i])){rigidGpu=false;++gx::gxRigidPlanRejects[60];break;}
        }
        if(signedQPositiveRigidAttempt&&rigidPackingAttempt&&!rigidGpu)recordSignedQPositiveRigid(context.signedQPositiveRigid.packingFailures);
        if(rigidGpu){
            const bool positive=gx::validateRigidProjection(rigid,batch.vertices);
            if(signedQPositiveRigidAttempt){
                recordSignedQPositiveRigid(positive?context.signedQPositiveRigid.proofPasses:context.signedQPositiveRigid.proofFailures);
                // Both handles must carry the strict GPU contract, including
                // compact storage. The signed diagnostic token is CPU-only.
                if(positive)reservation.qPolicy=batch.qPolicy=gx::GeometryQPolicy::positiveOnly;
            }
            // A projector crossing these vertices: degraded draws stay on the
            // GPU, where the shader clamps the divisor to a small positive value
            // (border sampling), instead of the per-vertex CPU route.
            const bool clampDivisor=!positive&&!signedQPositiveRigidAttempt&&(rigid.projectedTexture||rigid.multiProjected)&&
                allowsFallback(DrawVisualFallback::projectiveDivisor);
            if(clampDivisor)simplified[unsigned(DrawVisualFallback::projectiveDivisor)]=true;
            rigidGpu=positive||clampDivisor;if(!rigidGpu)++gx::gxRigidPlanRejects[61];
        }
        if(signedQPositiveRigidAttempt&&!rigidGpu){
            reservation.qPolicy=batch.qPolicy=gx::GeometryQPolicy::directTexture0Diagnostic;
            // The complete CPU loop below resets every GpuVertex, overwriting
            // all speculative raw/compact bytes before scanning/submission.
        }
        if(rigidTriangleFog&&!rigidGpu){
            // The reservation has no fog half: translate again without the GPU form.
            if(!cancelGxGeometry(reservation))return fail("GX geometry cancellation failed",DrawFailureKind::geometry);
            textureScope.reset();
            rigidTriangleFogSuppressed=true;const bool restarted=drawGxCommand(draw,ptr);rigidTriangleFogSuppressed=false;
            return restarted;
        }
        if(!rigidGpu){GxProfileScope section(GxProfilePart::vertexCpu);
            cpuProjection();
            bool positionOnly=false;
            if(!colorPlan.required&&!material.textureCount&&!fog.enabled&&!indirect.enabled&&!transform.coordinateMask()){
                GxProfileScope positionProfile(GxProfilePart::vertexPosition);
                positionOnly=gx::decodePositionTransformBatch(draw.plan,draw.vertices,draw.arrays,batch.vertices,draw.registers,transform.clipProjection());
                if(positionOnly)gxProfileTotals.positionOnlyVertices+=count;
            }
            if(!positionOnly)
            for(unsigned i=0;i<count;++i)if(const char* error=transformOne(i,batch.vertices[i])){
                if(!carrier&&transformError==gx::VertexTransformError::normalLengthOverflow&&
                   normalOverflowOutsideSides(draw,raster,batch.indices)){
                    if(!cancelGxGeometry(reservation))return fail("GX geometry cancellation failed",DrawFailureKind::geometry);
                    ++context.normalOverflowCulled;++context.culled;
#if defined(__3DS__)
                    if(context.normalOverflowCulled<=4)OSReport("Native normal-overflow side cull: vertices=%u indices=%u count=%llu\n",
                        unsigned(count),unsigned(indexCount),static_cast<unsigned long long>(context.normalOverflowCulled));
#endif
                    return true;
                }
#if defined(__3DS__) || defined(DUSK_TRANSFORM_DIAGNOSTIC_TEST)
                if(!std::strcmp(error,"GX transform nonfinite"))
                    captureFatalNormalTransform(draw,transform,i,
                        colorPlan.needsNormal||(carrier&&secondColorPlan->needsNormal),transformError);
#endif
                if(!std::strcmp(error,"GX projected texture divisor invalid")||!std::strcmp(error,"GX vertex indirect coordinate invalid")){
                    if(cameraAlignedEligible&&!cameraAlignedProved){
                        recordCameraAligned(context.cameraAlignedIndirect.attempts);
                        const bool proved=proveCameraAlignedIndirect();
                        recordCameraAligned(proved?context.cameraAlignedIndirect.proofPasses:context.cameraAlignedIndirect.proofFailures);
                        if(proved&&(context.allowCameraAlignedCpuClip||!context.cameraAlignedIndirectProofOnly)){
                            cameraAlignedProved=true;
                            cameraCpuClip=context.allowCameraAlignedCpuClip;
                            if(cameraCpuClip)++context.cameraClipAttempts;
                            reservation.qPolicy=batch.qPolicy=cameraCpuClip?gx::GeometryQPolicy::positiveOnly:gx::GeometryQPolicy::directTexture0Diagnostic;
                            // Full normal/light/material/attribute validation still
                            // runs for EVERY original vertex, including the prefix.
                            i=unsigned(-1);continue;
                        }
                    }
                    const auto clipped=clipInvisibleProjection();
                    if(clipped==ProjectiveClip::empty){recordProjectiveClip(true);++context.culled;return true;}
                    if(clipped==ProjectiveClip::trimmed)break;
                }
                if(!std::strcmp(error,"GX vertex indirect coordinate invalid")&&indirect.enabled&&!vertexIndirectSuppressed&&
                   !material.projectiveTexture0&&allowsFallback(DrawVisualFallback::indirectDivisor)){
                    // The warp is undefined at a visible vertex: draw the mesh
                    // without it. Not when the warped texture is itself
                    // projected (water that refracts the frame copy): without
                    // the camera-plane clip its divisor is invalid too and the
                    // border fallback smears the frame across the surface.
                    if(!cancelGxGeometry(reservation))return fail("GX geometry cancellation failed",DrawFailureKind::geometry);
                    textureScope.reset();
                    vertexIndirectSuppressed=true;const bool restarted=drawGxCommand(draw,ptr);vertexIndirectSuppressed=false;
                    return restarted;
                }
                captureIndirect(i,error);
                return cancel(error);
            }}
    }
    }
    static const auto postIndex=registerGxNamedProfile("draw.post");GxNamedProfileSpan postSpan(postIndex);
    if(cameraCpuClip){
        // Clip the triangles against the near plane (z=-w) here: every vertex
        // that reaches PICA is then in front of the camera with q=w>0, and
        // PICA clips the other planes as for any draw. A crossing is computed
        // from the outside vertex towards the inside one, so the two triangles
        // sharing an edge get the same new vertex. (The interval-arithmetic
        // kernel in gx_camera_clip.hpp cost 31 ms for one 77-vertex patch.)
        constexpr std::size_t maxClipVertices=1024,maxClipIndices=4096;
        static std::array<gx::GpuVertex,maxClipVertices> clipVertices;
        static std::array<u16,maxClipIndices> clipIndices;
        static std::array<u16,maxClipVertices> clipRemap;
        if(count>maxClipVertices)return cancel("GX camera clipping capacity",DrawFailureKind::geometry);
        std::fill_n(clipRemap.begin(),count,u16(0xffff));
        std::size_t nv=0,ni=0;bool overflow=false,invalid=false;
        const auto distance=[](const gx::GpuVertex& v){return v.position[3]+v.position[2];};
        const auto original=[&](u16 index)->u16{
            if(clipRemap[index]==0xffff){
                if(nv>=maxClipVertices){overflow=true;return 0;}
                if(!(batch.vertices[index].position[3]>0))invalid=true;
                clipVertices[nv]=batch.vertices[index];clipRemap[index]=u16(nv++);
            }
            return clipRemap[index];
        };
        const auto crossing=[&](u16 outsideIndex,u16 insideIndex)->u16{
            if(nv>=maxClipVertices){overflow=true;return 0;}
            const auto& o=batch.vertices[outsideIndex];const auto& n=batch.vertices[insideIndex];
            const float dOut=distance(o),dIn=distance(n),t=dOut/(dOut-dIn);
            auto& v=clipVertices[nv];
            for(unsigned k=0;k<4;++k){
                v.position[k]=o.position[k]+t*(n.position[k]-o.position[k]);
                v.color[k]=std::clamp(o.color[k]+t*(n.color[k]-o.color[k]),0.f,1.f);
            }
            for(unsigned unit=0;unit<3;++unit)for(unsigned k=0;k<2;++k)
                v.texcoord[unit][k]=o.texcoord[unit][k]+t*(n.texcoord[unit][k]-o.texcoord[unit][k]);
            v.textureQ=v.position[3]; // q equals w on every original vertex (proved above)
            if(!(v.position[3]>0))invalid=true;
            return u16(nv++);
        };
        for(std::size_t at=0;at+2<indexCount&&!overflow;at+=3){
            const u16 corner[3]{batch.indices[at],batch.indices[at+1],batch.indices[at+2]};
            if(corner[0]>=count||corner[1]>=count||corner[2]>=count){invalid=true;break;}
            bool inside[3];unsigned insideCount=0;
            for(unsigned k=0;k<3;++k){inside[k]=distance(batch.vertices[corner[k]])>=0;insideCount+=inside[k];}
            if(!insideCount)continue;
            u16 polygon[4];unsigned corners=0;
            for(unsigned k=0;k<3;++k){
                const unsigned next=(k+1)%3;
                if(inside[k])polygon[corners++]=original(corner[k]);
                if(inside[k]!=inside[next])polygon[corners++]=inside[k]?crossing(corner[next],corner[k]):crossing(corner[k],corner[next]);
            }
            if(ni+3*(corners-2)>maxClipIndices){overflow=true;break;}
            for(unsigned k=1;k+1<corners;++k){clipIndices[ni++]=polygon[0];clipIndices[ni++]=polygon[k];clipIndices[ni++]=polygon[k+1];}
        }
        if(overflow)return cancel("GX camera clipping capacity",DrawFailureKind::geometry);
        if(invalid){++context.cameraClipKernelRejected;return cancel("GX camera clipping plan rejected");}
        // reserve() can reclaim the old arena: the clipped mesh waits in the
        // scratch above while the reservation is replaced by one of its size.
        if(!cancelGxGeometry(reservation))return fail("GX camera clipping cancellation failed",DrawFailureKind::geometry);
        if(!ni){++context.cameraClipCulled;++context.culled;return true;}
        reservation={};batch={};
        if(!reserveGxGeometry(nv,ni,reservation))return fail(gxGeometryFailure(),DrawFailureKind::geometry);
        reservation.qPolicy=gx::GeometryQPolicy::positiveOnly;
        if(aliases(std::as_bytes(draw.vertices))||aliases(std::as_bytes(draw.indices))||
           aliases(std::as_bytes(std::span(&draw.registers,1)))||aliases(std::as_bytes(std::span(&draw.plan,1)))||
           aliases(std::as_bytes(std::span(&draw.arrays,1))))return cancel("GX command input aliases GPU reservation");
        for(const auto& array:draw.arrays)if(aliases(std::as_bytes(array.bytes)))return cancel("GX command input aliases GPU reservation");
        std::copy_n(clipVertices.begin(),nv,reservation.vertices.begin());
        std::copy_n(clipIndices.begin(),ni,reservation.indices.begin());
        batch=reservation;
        if(!gx::validateGpuGeometry(batch))return cancel("GX camera clipping strict output invalid");
        context.cameraClipOriginalVertices+=count;
        context.cameraClipOutputVertices+=nv;context.cameraClipOutputIndices+=ni;
    }
    carrierFailureVertex=UINT32_MAX; // Per-vertex validation finished; submission has not started.
    if(carrier&&!gx::validateGpuGeometry(batch))return cancel("GX diagnostic raster carrier geometry invalid");
    if(!rigidGpu)context.cpuVertexReasons[cpuReason]+=count;
    if(!rigidGpu&&!orthographicDraw){++gxPerspCpuGate[gateCounted?15:14];gxPerspCpuGateVertices[gateCounted?15:14]+=count;}
    drawClass.cls=rigidGpu?(gx::staticGeometryHint.generation?0:1):(draw.registers.xfValid[0x1026]&&draw.registers.xf[0x1026]==1?2:3);
    if(gxSparseVertexProfiling&&orthographicDraw){
        // Diagnostics: how many 2D draws carry only zero vertex alpha (hidden
        // panes drawn anyway) and how many use the standard alpha blend.
        ++native2dDraws;
        bool zero=count>0;gx::DecodedVertex vertex;
        for(unsigned i=0;i<count&&zero;++i)zero=draw.vertex(i,vertex)&&(vertex.present&(1u<<GX_VA_CLR0))&&vertex.color[0][3]==0;
        if(zero)++native2dZeroAlpha;
        if(raster.blend.mode==1&&raster.blend.src==4&&raster.blend.dst==5)++native2dAlphaBlend;
    }
    bool positiveQDepthFallback=false;
    bool depthlessOrthoNormalized=false;
    // GX disabled depth clipping must not lose an orthographic silhouette to
    // PICA's mandatory Z planes when Z is otherwise unobservable. Restrict this
    // exact transport rewrite to simple, fully validated CPU color geometry:
    // no depth test/write, texture, indirect, fog or multi-pass alpha consumer.
    // A constant interior Z preserves XY coverage, winding, interpolation and
    // draw order because EVERY original W is exactly one. Validate originals
    // (including unreferenced records) before replacing any depth component.
    if(raster.clipDisabled&&!rigidGpu&&!expanded&&!carrier&&!raster.depth.enabled&&!raster.depth.write&&
       !fog.enabled&&!triangleFog&&!indirect.enabled&&!originalMaterial.indirectCount&&
       !originalTextureCount&&!material.textureCount&&!simplified[unsigned(DrawVisualFallback::tevMaterial)]&&
       !raster.alphaUnion&&!raster.alphaInterval.enabled&&
       draw.registers.xfValid[0x1026]&&draw.registers.xf[0x1026]==1){
        bool unitW=true,outsideDepth=false;
        for(const auto& vertex:batch.vertices){
            unitW&=vertex.position[3]==1.f;
            outsideDepth|=vertex.position[2]<-1.f||vertex.position[2]>0.f;
        }
        if(unitW&&outsideDepth){
            auto positive=batch;positive.qPolicy=gx::GeometryQPolicy::positiveOnly;
            if(!gx::validateGpuGeometry(positive))return cancel("GX depthless orthographic original geometry invalid");
            for(auto& vertex:batch.vertices)vertex.position[2]=-.5f;
            reservation.qPolicy=batch.qPolicy=gx::GeometryQPolicy::positiveOnly;
            depthlessOrthoNormalized=true;
        }
    }
    if(raster.clipDisabled&&!rigidGpu)for(const auto& vertex:batch.vertices) {
        const auto& p=vertex.position;
        // Positive-w triangles wholly inside the depth interval may extend
        // laterally beyond the screen: clipping those edges preserves all
        // covered framebuffer samples and perspective interpolation. Depth
        // or camera-plane crossings have different clip-disable semantics.
        if(p[3]<=0||p[2]<-p[3]||p[2]>0){
            if(gx::proveNoScreenCoverage(batch.vertices)||
               (context.allowBehindCameraCull&&gx::proveBehindCamera(batch.vertices))){
                if(!gx::proveNoScreenCoverage(batch.vertices))++context.cameraCulled;
                if(!cancelGxGeometry(reservation))return fail("GX invisible geometry cancellation failed",DrawFailureKind::geometry);
                ++context.culled;return true;
            }
            if(!allowsFallback(DrawVisualFallback::clippedDepth))return cancel("GX disabled clipping outside depth/camera volume",DrawFailureKind::state);
            if(signedQDiagnostic){
                // The diagnostic flag must not veto the ordinary world policy
                // when EVERY prepared record satisfies its strict positive Q
                // contract. Include unreferenced originals and every attribute.
                auto positive=batch;positive.qPolicy=gx::GeometryQPolicy::positiveOnly;
                if(!gx::validateGpuGeometry(positive))return cancel("GX disabled clipping outside depth/camera volume",DrawFailureKind::state);
                reservation.qPolicy=batch.qPolicy=gx::GeometryQPolicy::positiveOnly;
                positiveQDepthFallback=true;
            }
            // PICA clips what GX would have drawn beyond the depth range.
            simplified[unsigned(DrawVisualFallback::clippedDepth)]=true;
            break;
        }
    }
    bool signedQNegative=false,signedQMixed=false;
    if(signedQDiagnostic&&!rigidGpu){
        if(!gx::validateGpuGeometry(batch))return cancel("GX signed-q diagnostic attributes invalid");
        unsigned negative=0,mixed=0,firstNegative=unsigned(batch.indices.size()),firstMixed=firstNegative;
        for(const auto& vertex:batch.vertices)negative+=vertex.textureQ<0;
        for(unsigned i=0;i<batch.indices.size();i+=3){
            unsigned n=0;for(unsigned k=0;k<3;++k)n+=batch.vertices[batch.indices[i+k]].textureQ<0;
            if(n&&firstNegative==batch.indices.size())firstNegative=i;
            if(n&&n!=3){++mixed;if(firstMixed==batch.indices.size())firstMixed=i;}
        }
        signedQNegative=negative!=0;signedQMixed=negative!=0&&negative<batch.vertices.size();
        if(signedQNegative&&context.reportSignedQ&&context.signedQReports<4){
            SignedQDrawSample sample;
            sample.vertices=unsigned(batch.vertices.size());sample.indices=unsigned(batch.indices.size());
            sample.negativeVertices=negative;sample.mixedTriangles=mixed;
            sample.coordinate=material.textures[0].coordinate;sample.map=material.textures[0].map;
            const auto& extent=gx::currentTargetExtent;
            sample.target={extent.logicalWidth,extent.logicalHeight,extent.pixelWidth,extent.pixelHeight};
            sample.texgen={draw.registers.xf[0x1040+sample.coordinate],draw.registers.xf[0x1050+sample.coordinate]};
            const unsigned first=mixed?firstMixed:firstNegative;
            // An indexed draw can retain unused negative-q vertices. Include
            // one such source even when no referenced triangle is negative.
            unsigned unused=0;
            if(first==batch.indices.size())while(unused<batch.vertices.size()&&batch.vertices[unused].textureQ>0)++unused;
            const unsigned end=first==batch.indices.size()?first+1:unsigned(batch.indices.size());
            for(unsigned i=first;i<end&&sample.samples<sample.vertex.size();++i){
                auto& out=sample.vertex[sample.samples++];out.index=first==batch.indices.size()?unused:batch.indices[i];
                const auto& v=batch.vertices[out.index];gx::DecodedVertex decoded;gx::TransformedVertex original;
                if(!draw.vertex(out.index,decoded)||!gx::transformVertex(transform,decoded,original,&transformError,colorPlan.needsNormal))
                    return cancel("GX signed-q telemetry source invalid");
                const unsigned attr=GX_VA_TEX0MTXIDX+sample.coordinate;
                const unsigned slot=sample.coordinate<4?0:1,shift=sample.coordinate<4?6+6*sample.coordinate:6*(sample.coordinate-4);
                out.textureMatrix=decoded.present&(1u<<attr)?decoded.matrix[sample.coordinate+1]:
                    (draw.registers.matrixIndex[slot]>>shift)&63;
                for(unsigned c=0;c<4;++c)out.clipBits[c]=std::bit_cast<std::uint32_t>(v.position[c]);
                for(unsigned c=0;c<3;++c)out.transformedBits[c]=std::bit_cast<std::uint32_t>(original.texcoord[sample.coordinate][c]);
                out.gpuBits={std::bit_cast<std::uint32_t>(v.texcoord[0][0]),std::bit_cast<std::uint32_t>(v.texcoord[0][1]),std::bit_cast<std::uint32_t>(v.textureQ)};
            }
            ++context.signedQReports;context.reportSignedQ(draw,material,sample);
        }
    }
    postSpan.stop();
    if(rigidTriangleFog){
        if(rigid.staticVertices&&batch.vertices.size()>3){
            if(!shrinkGxGeometry(batch,3,batch.indices.size(),true))return cancel(gxGeometryFailure(),DrawFailureKind::geometry);
            reservation=batch;
        }
        // Same uniform block with the fog rows of a primary-alpha fog plan.
        gx::RigidVertexPlan fogRigid=rigid;
        {auto& u=fogRigid.uniforms;
         u[11][0]=float(fog.a);u[11][1]=float(fog.b);u[11][2]=float(fog.c);
         u[12][0]=1;u[12][1]=float(fog.rangeEnabled);u[12][2]=1;
         if(fog.rangeEnabled){
            const unsigned logicalWidth=gx::currentTargetExtent.logicalWidth;
            u[13][0]=float(-double(logicalWidth)/fog.viewportWidth);u[13][1]=float((logicalWidth-2*fog.rangeCenter)/fog.viewportWidth);
            for(unsigned i=0;i<5;++i){u[14+i][0]=float(fog.rangeK[2*i]);u[14+i][1]=float(fog.rangeK[2*i+1]);u[14+i][2]=float(fog.rangeK[std::min(2*i+2,9u)]);}
         }
         for(unsigned row=11;row<19;++row)for(float value:u[row])if(!std::isfinite(value)||std::abs(value)>1e8f)return cancel("GX rigid fog uniforms invalid");}
        gx::PicaMaterialPlan fogConstant=fogMaterial;
        fogConstant.stages[0].constant=fog.color;
        fogConstant.stages[0].rgb.inputs[0]={gx::MaterialSource::constant,gx::MaterialOperand::color,false};
        if(!submitGxRigidFogGeometry(raster,material,fogRaster,fogConstant,batch,rigid,fogRigid))return cancel(gxPicaFailure(),DrawFailureKind::submission);
        gxProfileTotals.rigidVertices+=count;if(rigid.compact)gxProfileTotals.compactVertices+=count;
        ++context.triangleFogDraws;++rigidTriangleFogDraws;
    }
    else if(triangleFog){
        for(unsigned i=outputCount;i<pairedCount;++i)reservation.vertices[i]=batch.vertices[0];
        for(unsigned i=0;i<pairedCount;++i){auto& vertex=reservation.vertices[pairedCount+i];vertex={};
            vertex.position=reservation.vertices[i].position;
            for(unsigned c=0;c<3;++c)vertex.color[c]=float(fog.color[c])/255;
            vertex.color[3]=gx::evaluateVertexFog(fog,vertex.position,gx::currentTargetExtent.logicalWidth);
        }
        if(!submitGxTriangleFogGeometry(raster,material,fogRaster,fogMaterial,reservation))return cancel(gxPicaFailure(),DrawFailureKind::submission);
        ++context.triangleFogDraws;
    }
    else if(rigidGpu){
        // Resident static vertices: keep only the indices in the frame arena.
        if(rigid.staticVertices&&batch.vertices.size()>3){
            if(!shrinkGxGeometry(batch,3,batch.indices.size(),true))return cancel(gxGeometryFailure(),DrawFailureKind::geometry);
            reservation=batch;
        }
        if(!submitGxRigidGeometry(raster,material,batch,rigid))return cancel(gxPicaFailure(),DrawFailureKind::submission);
        gxProfileTotals.rigidVertices+=count;if(rigid.compact)gxProfileTotals.compactVertices+=count;
        bool anyFallback=false;for(bool v:simplified)anyFallback|=v;
        if(immediateCaptured&&rigid.multiTexture&&rigid.compact&&!rigid.staticVertices&&!colorPlan.lit&&!fog.enabled&&!anyFallback&&
           !carrier&&!exactI4Pending&&!indirect.enabled&&!originalMaterial.indirectCount&&!signedQDiagnostic&&!cameraCpuClip&&
           !cameraAlignedProved&&!depthlessOrthoNormalized&&!positiveQDepthFallback&&!fogSubstituted&&!constantEarlyDepth&&
           material.textureCount==originalTextureCount&&!material.projectiveTexture0){
            auto& rec=*immediateSlot;
            rec.keyA=immediateKey.a;rec.keyB=immediateKey.b;rec.extra=immediateExtraKey;rec.extent=gx::currentTargetExtent;
            rec.raster=raster;rec.material=material;rec.scales=scales;rec.textureCount=std::uint8_t(originalTextureCount);
            rec.used=true;++immediateStores;
        }
        // Native shape record (direct J3D draw on later frames).
        const bool staticList=gx::staticGeometryHint.generation&&gx::staticGeometryHint.list;
        const unsigned listBatch=gx::staticGeometryHint.batch;
        // Topology must be the complete, view-independent expansion of the
        // list (pre-indexed by the batcher or expanded here from strips, fans
        // and quads); points and lines are widened per frame.
        const unsigned refusal=!staticList?0:listBatch<1||listBatch>nativeMaxBatches?1:!rigid.compact?2:!rigid.staticVertices?3:
            (carrier||exactI4Pending||anyFallback)?5:(indirect.enabled||originalMaterial.indirectCount)?6:
            (signedQDiagnostic||cameraCpuClip||cameraAlignedProved||depthlessOrthoNormalized||positiveQDepthFallback)?7:
            raster.clipDisabled?8:fogSubstituted?9:(expanded||batch.indices.size()!=indexCount)?10:0;
        if(staticList&&refusal)++nativeRecordOutcome[refusal];
        if(staticList&&!refusal){
            auto* cache=gx::staticCompactCache();
            std::array<std::uint32_t,6> keyParts;
            const auto key=nativeStateKey(draw.registers,gx::staticGeometryHint.generation,gx::staticGeometryHint.list,&keyParts);
            auto& rec=nativeClaim(gx::staticGeometryHint.list,key,listBatch);
            bool recorded=false;
            if(cache&&cache->resident()){
                // Reuse persistent indices recorded for this batch (any instance).
                const std::uint16_t* reuse=nullptr;
                {const unsigned home=nativeHome(gx::staticGeometryHint.list,listBatch);
                 for(unsigned n=0;n<nativeShapeProbe&&!reuse;++n){const auto& other=nativeShapes[(home+n)%nativeShapeCapacity];
                    if(other.list==gx::staticGeometryHint.list&&other.batch==listBatch&&other.indices&&other.indexCount==batch.indices.size()&&other.indexEpoch==residentIndexEpoch()&&
                       !std::memcmp(other.indices,batch.indices.data(),batch.indices.size()*sizeof(u16)))reuse=other.indices;}}
                const auto* indices=reuse?reuse:allocateResidentIndices(batch.indices.data(),unsigned(batch.indices.size()));
                gx::DecodedVertex first;
                if(indices&&draw.vertex(0,first)){
                    rec={};rec.generation=gx::staticGeometryHint.generation;rec.list=gx::staticGeometryHint.list;
                    rec.keyA=key.a;rec.keyB=key.b;
                    rec.cacheEpoch=cache->epoch;rec.indexEpoch=residentIndexEpoch();
                    rec.raster=raster;rec.material=material;rec.colorMaterial=colorMaterial;rec.first=first;
                    rec.matrixOffsets=rigid.matrixOffsets;
                    for(unsigned m=0;m<10;++m)if(rigid.matrixOffsets[m]>=0)rec.matrixOrder[rec.matrixCount++]=std::uint8_t(m);
                    std::sort(rec.matrixOrder.begin(),rec.matrixOrder.begin()+rec.matrixCount,
                        [&](std::uint8_t x,std::uint8_t y){return rigid.matrixOffsets[x]<rigid.matrixOffsets[y];});
                    rec.normalScale=rigid.decodedNormalScale;rec.tex0Scale=rigid.decodedTex0Scale;rec.tex1Scale=rigid.decodedTex1Scale;
                    rec.vertices=rigid.staticVertices;rec.indices=indices;rec.indexCount=unsigned(batch.indices.size());rec.vertexCount=count;
                    rec.originalTextureCount=std::uint8_t(originalTextureCount);rec.multiTexture=rigid.multiTexture;
                    rec.fogEnabled=fog.enabled;rec.fogPrimaryAlpha=fogPrimaryAlpha;rec.fogPrimaryRed=fogPrimaryRed;
                    rec.dynamic=nativeDynamic(draw.registers);rec.constantEarlyDepth=constantEarlyDepth;rec.parts=keyParts;
                    rec.batch=std::uint8_t(listBatch);rec.batchCount=0;rec.pending=true;rec.rigidProjected=rigid.projectedTexture||rigid.multiProjected;
                    rec.origin=rigid.origin;
                    if(rec.rigidProjected&&count){
                        bool finite=true;
                        for(unsigned i=0;i<count;++i){
                            gx::CompactRigidVertex v;std::memcpy(&v,rigid.staticVertices+i*sizeof(v),sizeof(v));
                            for(unsigned c=0;c<3;++c){
                                finite&=std::isfinite(v.position[c]);
                                if(!i||v.position[c]<rec.boxLow[c])rec.boxLow[c]=v.position[c];
                                if(!i||v.position[c]>rec.boxHigh[c])rec.boxHigh[c]=v.position[c];
                            }
                        }
                        rec.boxValid=finite;
                    }
                    rec.valid=true;nativeContext=&context;++nativeRecords;recorded=true;
                    if(nativePendingList!=gx::staticGeometryHint.list){nativePendingList=gx::staticGeometryHint.list;nativePendingCount=0;}
                    ++nativePendingCount;
                }
            }
            ++nativeRecordOutcome[recorded?0:11];
        }
        if(preparedArmed&&rigid.compact&&!carrier&&!exactI4Pending&&!anyFallback&&!indirect.enabled&&!originalMaterial.indirectCount&&
           !signedQDiagnostic&&!cameraCpuClip&&!cameraAlignedProved&&!depthlessOrthoNormalized&&!positiveQDepthFallback&&
           !raster.clipDisabled&&!fogSubstituted&&!expanded&&originalTextureCount<=3&&(!fog.enabled||fogPrimaryAlpha||fogPrimaryRed||originalTextureCount<3)){
            auto& rec=preparedDraws[preparedSlot.a%preparedCapacity];
            rec.keyA=preparedSlot.a;rec.keyB=preparedSlot.b;rec.raster=raster;rec.material=material;rec.colorMaterial=colorMaterial;
            rec.originalTextureCount=std::uint8_t(originalTextureCount);rec.fogEnabled=fog.enabled;
            rec.fogPrimaryAlpha=fogPrimaryAlpha;rec.fogPrimaryRed=fogPrimaryRed;rec.rigidProjected=rigidProjected;rec.used=true;
            ++preparedStores;
        }
    }
    else{
        if(gx::staticGeometryHint.generation&&gx::staticGeometryHint.list)++nativeRecordOutcome[12];
        if(!submitGxCommandGeometry(raster,material,batch))return cancel(gxPicaFailure(),DrawFailureKind::submission);
    }
    if(positiveQDepthFallback){++context.positiveQDepthFallbackDraws;context.positiveQDepthFallbackVertices+=batch.vertices.size();}
    if(exactI4Pending){++context.exactI4Draws;context.exactI4Vertices+=batch.vertices.size();context.exactI4CpuImages=exactI4Cache.count();}
    if(depthlessOrthoNormalized){++context.depthlessOrthoDraws;context.depthlessOrthoVertices+=batch.vertices.size();}
    if(cameraCpuClip)++context.cameraClipDraws;
    else if(cameraAlignedProved)recordCameraAligned(context.cameraAlignedIndirect.submitted);
    if(signedQPositiveRigidAttempt&&rigidGpu)recordSignedQPositiveRigid(context.signedQPositiveRigid.submitted);
    if(signedQNegative)++context.signedQNegativeDraws;
    if(signedQMixed)++context.signedQMixedDraws;
    recordProjectiveClip(false);
    bool anySimplified=false;
    for(unsigned i=0;i<simplified.size();++i)if(simplified[i]){
        ++context.simplifiedByReason[i];anySimplified=true;
        static constexpr const char* names[unsigned(DrawVisualFallback::count)]{
            "GX visual fallback: simplified TEV material",
            "GX visual fallback: disabled depth clipping (PICA clipped)",
            "GX visual fallback: projected texture behind its projector (border sampled)",
            "GX visual fallback: dual alpha comparison drawn with one test",
            "GX visual fallback: texture shared by warped and plain stages drawn without the warp",
            "GX visual fallback: warp coordinate behind its projector (drawn without the warp)"};
        if(context.reportFailure&&context.simplifiedByReason[i]==1)context.reportFailure(draw,names[i]);
    }
    if(anySimplified)++context.simplifiedDraws;
    if(carrier)++context.rasterCarrierDraws;
    ++context.submitted;if(material.quantizedConstants)++context.quantizedDraws;
    if(fog.enabled)++context.vertexFogDraws;
    if(indirect.enabled)++context.vertexIndirectDraws;
    if(constantEarlyDepth)++context.constantEarlyDepthDraws;
    if(raster.alphaUnion)++context.alphaUnionDraws;
    if(raster.alphaInterval.enabled)++context.alphaIntervalDraws;
    if(raster.alphaBand)++context.alphaBandDraws;
    gxProfileTotals.vertices+=count;if(carrier?bool(colorPlan.lit||secondColorPlan->lit):colorPlan.needsNormal)gxProfileTotals.litVertices+=count;
    if(fog.enabled)gxProfileTotals.fogVertices+=count;
    return true;
}
}

namespace dusk::native {
namespace {
// Replay one recorded batch from the live registers. False: nothing was
// submitted for this batch.
bool nativeReplay(NativeShapeRecord& rec,const gx::CommandRegisters& r,CommandDrawContext& context){
    if(rec.raster.scissor.empty||rec.raster.cull==3){++context.culled;++nativeHits;return true;}
    auto& chain=nativeChain;
    if(chain.valid&&chain.stateEpoch==r.stateEpoch&&chain.picaSerial==gxPicaStateSerial&&
       chain.matrixTextures0==(r.matrixIndex[0]>>6)&&chain.matrixTextures1==r.matrixIndex[1]&&
       rec.dynamic==chain.dynamic&&rec.multiTexture==chain.multiTexture&&rec.fogEnabled==chain.fogEnabled&&
       rec.fogPrimaryAlpha==chain.fogPrimaryAlpha&&rec.fogPrimaryRed==chain.fogPrimaryRed&&
       rec.rigidProjected==chain.rigidProjected&&
       rec.originalTextureCount==chain.originalTextureCount&&rec.first.present==chain.firstPresent&&
       rec.first.normalCount==chain.firstNormalCount&&
       !std::memcmp(&chain.extent,&gx::currentTargetExtent,sizeof(chain.extent))&&
       !std::memcmp(&rec.raster,&chain.raster,sizeof(rec.raster))&&
       !std::memcmp(&rec.material,&chain.material,sizeof(rec.material))&&
       !std::memcmp(&rec.colorMaterial,&chain.colorMaterial,sizeof(rec.colorMaterial))){
        static const auto chainIndex=registerGxNamedProfile("native.chained");GxNamedProfileScope chainScope(chainIndex);
        gx::RigidVertexPlan rigid=chain.plan;
        rigid.matrixOffsets.fill(-1);rigid.normalOffsets.fill(-1);rigid.matrices=0;rigid.compactPalette=false;rigid.paletteEnd=0;
        const unsigned position=rec.first.present&1?rec.first.matrix[0]:r.matrixIndex[0]&63;
        bool okay=position<=27&&position%3==0&&gx::addRigidMatrix(r,position,rigid);
        for(unsigned n=0;okay&&n<rec.matrixCount;++n)okay=gx::addRigidMatrix(r,rec.matrixOrder[n]*3u,rigid);
        if(okay&&rigid.matrixOffsets==rec.matrixOffsets){
            rigid.decodedNormalScale=rec.normalScale;rigid.decodedTex0Scale=rec.tex0Scale;rigid.decodedTex1Scale=rec.tex1Scale;
            rigid.origin=rec.origin;
            if(gx::finishCompactRigidUniforms(rigid,chain.scales[rec.rigidProjected?1:0])&&
               (rigid.staticVertices=rec.vertices,!rec.rigidProjected||nativeProjectionPositive(rigid,rec))){
                static const auto chainSubmit=registerGxNamedProfile("native.chained.submit");GxNamedProfileScope chainSubmitScope(chainSubmit);
                if(!submitNativeRigid(rec.raster,rec.material,rigid,rec.vertices,rec.indices,rec.indexCount,true)){
                    rec.valid=false;chain.valid=false;return false;
                }
                ++context.submitted;++nativeHits;++nativeChained;
                gxProfileTotals.rigidVertices+=rec.vertexCount;gxProfileTotals.compactVertices+=rec.vertexCount;gxProfileTotals.vertices+=rec.vertexCount;
                return true;
            }
        }
        // Not expressible as a continuation: take the complete replay below.
    }
    chain.valid=false;
    static const auto planIndex=registerGxNamedProfile("native.plans");GxNamedProfileSpan planSpan(planIndex);
    gx::RasterColorPlan colorPlan;
    if(!gx::planRasterColor(r,rec.colorMaterial,colorPlan,nullptr))return false;
    std::uint8_t coordinates=0;
    for(unsigned unit=0;unit<rec.colorMaterial.textureCount;++unit)coordinates|=1u<<rec.colorMaterial.textures[unit].coordinate;
    gx::VertexTransformPlan transform;
    if(!gx::planVertexTransform(r,coordinates,transform,nullptr))return false;
    planSpan.stop();
    static const auto texIndex=registerGxNamedProfile("native.textures");GxNamedProfileSpan texSpan(texIndex);
    std::array<gx::Texture,3> textures{};
    {static const auto i=registerGxNamedProfile("native.tex.resolve");GxNamedProfileScope sc(i);
    for(unsigned unit=0;unit<rec.originalTextureCount;++unit){
        const char* why;if(!gx::resolveCommandTexture(r,rec.material.textures[unit].map,textures[unit],why))return false;
    }}
    gx::VertexFogPlan fog;
    if(context.allowVertexFog){
        if(!gx::planVertexFog(r,fog)||fog.enabled!=rec.fogEnabled)return false;
        if(fog.enabled&&!rec.fogPrimaryAlpha&&!rec.fogPrimaryRed&&!fogRampTexture(textures[rec.originalTextureCount]))return false;
    }else if(rec.fogEnabled)return false;
    // Animated TEV colours or fog colour: same combiner, new constants.
    if(const auto dynamic=nativeDynamic(r);dynamic!=rec.dynamic){
        if(!refreshNativeConstants(rec,r,context,fog,transform.projectiveMask())){++nativeConstantRefusals;return false;}
        rec.dynamic=dynamic;++nativeConstantRefreshes;
    }
    std::array<std::array<float,2>,3> scales{};
    static const auto scopeIndex=registerGxNamedProfile("native.tex.scope");GxNamedProfileSpan scopeSpan(scopeIndex);
    GxTextureDrawScope textureScope({textures.data(),rec.material.textureCount});
    if(!textureScope)return false;
    scopeSpan.stop();
    {static const auto i=registerGxNamedProfile("native.tex.bind");GxNamedProfileScope sc(i);
    for(unsigned unit=0;unit<rec.material.textureCount;++unit)
        if(!bindGxTexture(textures[unit],scales[unit][0],scales[unit][1],unit,unit==0&&rec.material.projectiveTexture0))return false;}
    texSpan.stop();
    static const auto rigidIndex=registerGxNamedProfile("native.rigid");GxNamedProfileSpan rigidSpan(rigidIndex);
    gx::RigidVertexPlan rigid;
    const int fogUnit=fog.enabled&&!rec.fogPrimaryAlpha&&!rec.fogPrimaryRed?int(rec.originalTextureCount):-1;
    const bool planned=rec.multiTexture?
        gx::planRigidMultiVertex(r,rec.first,transform,colorPlan,fog,rec.fogPrimaryAlpha,gx::currentTargetExtent.logicalWidth,
                                 rec.material,rec.originalTextureCount,scales,fogUnit,rigid,context.allowMultiProjected):
        (context.allowSingleRigidShader&&gx::planRigidVertex(r,rec.first,transform,colorPlan,fog,rec.fogPrimaryAlpha,gx::currentTargetExtent.logicalWidth,rigid,rec.rigidProjected));
    if(!planned)return false;
    if(rec.rigidProjected&&!rec.multiTexture)gx::setProjectedScales(rigid,scales[0],scales[1]);
    if(fog.enabled&&rec.fogPrimaryRed)rigid.uniforms[12][2]=2;
    if(nativeChainEnabled)chain.plan=rigid; // before this shape's palette and decoded scales
    for(unsigned n=0;n<rec.matrixCount;++n)if(!gx::addRigidMatrix(r,rec.matrixOrder[n]*3u,rigid))return false;
    if(rigid.matrixOffsets!=rec.matrixOffsets)return false;
    rigid.decodedNormalScale=rec.normalScale;rigid.decodedTex0Scale=rec.tex0Scale;rigid.decodedTex1Scale=rec.tex1Scale;
    rigid.origin=rec.origin;
    if(!gx::finishCompactRigidUniforms(rigid,scales[rec.rigidProjected?1:0]))return false;
    rigid.staticVertices=rec.vertices;
    // The projector may now cross these vertices: the complete path then
    // takes its CPU route for this frame.
    if(rec.rigidProjected&&!nativeProjectionPositive(rigid,rec))return false;
    rigidSpan.stop();
    static const auto submitIndex=registerGxNamedProfile("native.submit");GxNamedProfileScope submitScope(submitIndex);
    if(!submitNativeRigid(rec.raster,rec.material,rigid,rec.vertices,rec.indices,rec.indexCount)){
        rec.valid=false;return false;
    }
    if(nativeChainEnabled){
        // The epoch does not cover position-matrix memory: a texgen that reads
        // its matrix from those slots cannot be continued.
        bool continuable=true;
        for(unsigned unit=0;unit<rec.originalTextureCount&&continuable;++unit){
            const unsigned c=rec.material.textures[unit].coordinate;
            const unsigned m=c<4?(r.matrixIndex[0]>>(6+6*c))&63:c<8?(r.matrixIndex[1]>>(6*(c-4)))&63:0;
            continuable=c<8&&m>=30;
        }
        if(continuable){
            chain.stateEpoch=r.stateEpoch;chain.picaSerial=gxPicaStateSerial;
            chain.matrixTextures0=r.matrixIndex[0]>>6;chain.matrixTextures1=r.matrixIndex[1];chain.dynamic=rec.dynamic;
            chain.extent=gx::currentTargetExtent;chain.raster=rec.raster;chain.material=rec.material;chain.colorMaterial=rec.colorMaterial;
            chain.firstPresent=rec.first.present;chain.firstNormalCount=rec.first.normalCount;chain.originalTextureCount=rec.originalTextureCount;
            chain.multiTexture=rec.multiTexture;chain.fogEnabled=rec.fogEnabled;chain.fogPrimaryAlpha=rec.fogPrimaryAlpha;chain.fogPrimaryRed=rec.fogPrimaryRed;
            chain.rigidProjected=rec.rigidProjected;
            chain.scales=scales;chain.valid=true;
        }
    }
    ++context.submitted;++nativeHits;
    gxProfileTotals.rigidVertices+=rec.vertexCount;gxProfileTotals.compactVertices+=rec.vertexCount;gxProfileTotals.vertices+=rec.vertexCount;
    return true;
}
}
}
// Direct J3D shape draw; false means "use the normal display-list path".
extern "C" bool duskNativeDrawRecorded(std::uint32_t generation,const void* list){
    using namespace dusk::native;
    auto* cache=gx::staticCompactCache();
    auto* stream=gx::commandStream();
    if(!nativeContext||!cache||!stream||!stream->idle()){++nativeMisses;return false;}
    static const auto index=registerGxNamedProfile("native.shape");GxNamedProfileScope profile(index);
    const auto& r=stream->registers();
    static const auto keyIndex=registerGxNamedProfile("native.key");GxNamedProfileSpan keySpan(keyIndex);
    std::array<std::uint32_t,6> keyParts;
    const auto key=nativeStateKey(r,generation,list,gxSparseVertexProfiling?&keyParts:nullptr);
    keySpan.stop();
    // Every batch of the list must be recorded for this state before anything
    // is submitted: a list is replayed completely or not at all.
    std::array<NativeShapeRecord*,nativeMaxBatches> records{};
    auto* found=nativeFind(list,key,1);
    unsigned batches=found?found->batchCount:0;
    for(unsigned batch=2;batch<=batches;++batch){
        records[batch-1]=nativeFind(list,key,batch);
        if(!records[batch-1]||records[batch-1]->batchCount!=batches){batches=0;break;}
    }
    if(!batches||batches>nativeMaxBatches){
        ++nativeKeyMisses;
        if(gxSparseVertexProfiling){
            // Which component differs from the closest record of this list?
            const unsigned home=nativeHome(list,1);int best=-1;unsigned bestEqual=0;
            for(unsigned n=0;n<nativeShapeProbe;++n){const auto& other=nativeShapes[(home+n)%nativeShapeCapacity];
                if(!other.valid||other.list!=list||other.batch!=1)continue;
                unsigned equal=0;for(unsigned c=0;c<6;++c)equal+=other.parts[c]==keyParts[c];
                if(best<0||equal>bestEqual){best=int((home+n)%nativeShapeCapacity);bestEqual=equal;}}
            if(best<0)++nativeMissParts[6];
            else for(unsigned c=0;c<6;++c)if(nativeShapes[best].parts[c]!=keyParts[c])++nativeMissParts[c];
        }
        return false;
    }
    records[0]=found;
    for(unsigned n=0;n<batches;++n){
        const auto& rec=*records[n];
        if(rec.generation!=generation||cache->epoch!=rec.cacheEpoch||rec.indexEpoch!=residentIndexEpoch()){
            for(unsigned m=0;m<batches;++m)records[m]->valid=false;
            ++nativeMisses;return false;
        }
    }
    auto& context=*nativeContext;
    for(unsigned n=0;n<batches;++n){
        if(nativeReplay(*records[n],r,context))continue;
        // Nothing submitted yet: the normal path draws the whole list. After a
        // partial replay the list must not be drawn twice; drop its records
        // and let the next frame take the normal path.
        for(unsigned m=0;m<batches;++m)records[m]->valid=false;
        ++nativeMisses;
        return n!=0;
    }
    return true;
}
