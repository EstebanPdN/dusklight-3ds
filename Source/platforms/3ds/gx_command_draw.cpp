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
#include "runtime/gx_stream.hpp"
#include <algorithm>
#include <cmath>
#include <bit>
#include <cstring>
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
    if(!draw.vertex(0,first)||!gx::planRigidVertex(draw.registers,first,transform,colorPlan,fog,rec.fogPrimaryAlpha,
        gx::currentTargetExtent.logicalWidth,rigid,rec.rigidProjected))return 0;
    if(rec.rigidProjected)rigid.uniforms[rigid.projectionBase()+3]={scales[0][0],scales[0][1],0,0};
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
    const std::byte* vertices{};const std::uint16_t* indices{};unsigned indexCount{},vertexCount{};
    std::uint8_t originalTextureCount{};
    bool valid{},multiTexture{},fogEnabled{},fogPrimaryAlpha{},fogPrimaryRed{};
};
constexpr unsigned nativeShapeCapacity=1024,nativeShapeProbe=8;
std::array<NativeShapeRecord,nativeShapeCapacity> nativeShapes{};
CommandDrawContext* nativeContext{};
std::uint64_t nativeHits{},nativeMisses{},nativeRecords{},nativeKeyMisses{};
unsigned nativeHome(const void* list){return unsigned((reinterpret_cast<std::uintptr_t>(list)>>2)*2654435761u)%nativeShapeCapacity;}
// Linear probing over (list, state key): instances of one model share a list
// but differ in material/lighting state, so each keeps its own record.
NativeShapeRecord* nativeFind(const void* list,PreparedKey key){
    const unsigned home=nativeHome(list);
    for(unsigned n=0;n<nativeShapeProbe;++n){auto& rec=nativeShapes[(home+n)%nativeShapeCapacity];
        if(rec.valid&&rec.list==list&&rec.keyA==key.a&&rec.keyB==key.b)return &rec;}
    return nullptr;
}
NativeShapeRecord& nativeClaim(const void* list,PreparedKey key){
    if(auto* rec=nativeFind(list,key))return *rec;
    const unsigned home=nativeHome(list);
    for(unsigned n=0;n<nativeShapeProbe;++n){auto& rec=nativeShapes[(home+n)%nativeShapeCapacity];if(!rec.valid)return rec;}
    static unsigned victim{};return nativeShapes[(home+(victim++%nativeShapeProbe))%nativeShapeCapacity];
}
PreparedKey nativeStateKey(const gx::CommandRegisters& r,std::uint32_t generation,const void* list){
    // Raw register words, no per-register validity branches: a stale invalid
    // word can only produce a miss. FNV-1a plus a rotate-xor second lane.
    std::uint32_t a=2166136261u^generation,b=0x9e3779b9u^std::uint32_t(reinterpret_cast<std::uintptr_t>(list));
    auto mix=[&](std::uint32_t v){a=(a^v)*16777619u;b=((b<<5)|(b>>27))^v;};
    for(unsigned i=0;i<256;++i)mix(r.bp[i]);
    mix(r.bpMask);
    for(unsigned bank=0;bank<2;++bank)for(unsigned i=0;i<8;++i)mix(r.tevColor[bank][i]);
    for(const auto& t:r.tluts){mix(t.token);mix(t.offset^(t.entries<<16));}
    mix(r.tlutOverflow);
    mix(r.cp[0x30]);mix(r.cp[0x40]);mix(r.cp[0x50]);mix(r.cp[0x60]);
    for(unsigned i=0;i<8;++i){mix(r.cp[0x70+i]);mix(r.cp[0x80+i]);mix(r.cp[0x90+i]);}
    for(unsigned i=0;i<16;++i)mix(r.cp[0xb0+i]);
    for(unsigned i=0x1000;i<0x100a;++i)mix(r.xf[i]);
    for(unsigned i=0x100e;i<0x1058;++i)mix(r.xf[i]);
    mix(r.matrixIndex[0]);mix(r.matrixIndex[1]);
    mix(gx::currentTargetExtent.logicalWidth|(gx::currentTargetExtent.logicalHeight<<16));
    return {a,b};
}
void nativeListEnded(const void* list,std::uint32_t batches){
    if(batches==1)return;
    const unsigned home=nativeHome(list); // multi-batch lists stay on the normal path
    for(unsigned n=0;n<nativeShapeProbe;++n){auto& rec=nativeShapes[(home+n)%nativeShapeCapacity];if(rec.list==list)rec.valid=false;}
}
struct NativeListHook {NativeListHook(){gx::staticListEnded=nativeListEnded;}} nativeListHook;
}
void nativeShapeStatistics(std::uint64_t& hits,std::uint64_t& misses,std::uint64_t& records,std::uint64_t& keyMisses){
    hits=nativeHits;misses=nativeMisses;records=nativeRecords;keyMisses=nativeKeyMisses;
}
void preparedDrawStatistics(std::uint64_t& hits,std::uint64_t& misses,std::uint64_t& stores,std::uint64_t& fallbacks){
    hits=preparedHits;misses=preparedMisses;stores=preparedStores;fallbacks=preparedFallbacks;
}
std::uint64_t clipDisabledGpuDraws;
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
    bool carrierAttempt=false;
    std::uint32_t carrierFailureVertex=UINT32_MAX;
    auto fail=[&](const char* why,DrawFailureKind kind=DrawFailureKind::state){
        if(!context.rejected){context.failure=why;context.failureKind=kind;if(context.reportFailure)context.reportFailure(draw,why);}
        if(carrierAttempt){
            ++context.rasterCarrierRejects;
            if(!context.rasterCarrierFirstFailure){context.rasterCarrierFirstFailure=why;context.rasterCarrierFirstVertex=carrierFailureVertex;}
        }
        ++context.rejected;++context.rejectedByKind[unsigned(kind)];return false;
    };
    auto allowsFallback=[&](DrawVisualFallback reason){
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
        context.allowRgba6Storage?gx::ColorStoragePolicy::rgba8ForRgba6:gx::ColorStoragePolicy::rgb8Only,gx::ClipPolicy::validateGeometry,context.allowConstantEarlyDepth,context.allowAlphaUnion,context.alphaIntervalPolicy))
        return fail(gx::rasterErrorText(rasterError));
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
    }
    gx::PicaMaterialPlan material;gx::MaterialPlanError planError;
    bool carrier=false,exactI4Pending=false;
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
        }else{
        bool raster1Read=false;
        for(unsigned i=0;i<state.stageCount;++i)if(state.stages[i].raster==1){
            for(auto id:state.stages[i].color)raster1Read|=id==10||id==11;
            for(auto id:state.stages[i].alpha)raster1Read|=id==5;
        }
        if(context.diagnosticRasterCarrier&&raster1Read){
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
            if(!gx::discardUnobservedTevAlpha(state,raster)||!gx::planPicaMaterial(state,material,&planError,policy)){
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
    const bool constantEarlyDepth=raster.earlyDepthAlpha;
    if(!gx::resolveConstantEarlyAlpha(material,raster))return fail("GX early depth alpha is not constant");
    gx::RasterColorPlan colorPlan;gx::RasterColorError colorError;
    std::optional<gx::RasterColorPlan> secondColorPlan; // Disengaged: no light-array initialization on ordinary draws.
    const gx::PicaMaterialPlan colorMaterial=material; // prepared-draw lighting input
    bool fogSubstituted=false;
    { static const auto index=registerGxNamedProfile("adapter.lightingPlan");GxNamedProfileScope detail(index);
    if(carrier){
        secondColorPlan.emplace();
        if(!gx::planRasterColorChannel(draw.registers,0,1,colorPlan,&colorError)||
           !gx::planRasterColorChannel(draw.registers,1,1,*secondColorPlan,&colorError))return fail(gx::rasterColorErrorText(colorError));
    }else if(!gx::planRasterColor(draw.registers,material,colorPlan,&colorError))return fail(gx::rasterColorErrorText(colorError));
    }
    std::uint8_t coordinates=indirect.coordinates;
    std::array<gx::Texture,3> textures{};
    for(unsigned unit=0;unit<material.textureCount;++unit)coordinates|=1u<<material.textures[unit].coordinate;
    gx::VertexTransformPlan transform;gx::VertexTransformError transformError;
    { static const auto index=registerGxNamedProfile("adapter.transformPlan");GxNamedProfileScope detail(index);
    if(!gx::planVertexTransform(draw.registers,coordinates,transform,&transformError))return fail(gx::vertexTransformErrorText(transformError));
    }
    if(exactI4Pending&&transform.projectiveMask())return fail(exactI4OriginalFailure);
    if(carrier&&transform.projectiveMask())return fail("GX diagnostic raster carrier projective unsupported");
    if(!gx::routeProjectiveTexture(material,transform.projectiveMask()))return fail("GX multiple projected textures pending");
    std::array<const gx::VertexIndirectEffect*,3> indirectUnits{};
    { static const auto index=registerGxNamedProfile("adapter.textureResolve");GxNamedProfileScope detail(index);
    for(unsigned unit=0;unit<material.textureCount;++unit){
        const auto binding=material.textures[unit];
        const char* textureFailure;
        if(!gx::resolveCommandTexture(draw.registers,binding.map,textures[unit],textureFailure))return fail(textureFailure,DrawFailureKind::resource);
        int first=-1;
        for(unsigned stage=0;stage<originalMaterial.stageCount;++stage){
            const auto& s=originalMaterial.stages[stage];
            if(!s.textureEnabled||s.texture!=binding.map||s.coordinate!=binding.coordinate)continue;
            if(first>=0&&indirect.stages[first].state!=indirect.stages[stage].state)
                return fail("GX shared texture has different indirect coordinates");
            first=stage;
        }
        if(first>=0&&indirect.stages[first].enabled)indirectUnits[unit]=&indirect.stages[first];
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
    const unsigned originalTextureCount=material.textureCount;
    bool fogPrimaryAlpha=!carrier&&!(colorPlan.required&2);
    bool fogPrimaryRed=!carrier&&!fogPrimaryAlpha&&!(colorPlan.required&1);
    gx::VertexFogPlan fog;
    bool triangleFog=false;gx::RasterPlan fogRaster;gx::PicaMaterialPlan fogMaterial;
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
        if(!gx::appendVertexFog(material,fog,fogPrimaryAlpha,fogPrimaryRed)){
            if(carrier||!context.allowTriangleFog||expanded||!gx::planTriangleFogPass(raster,fogRaster,fogMaterial))
                return fail("GX vertex fog TEV/texture capacity");
            triangleFog=true;
        }
        if(fog.enabled&&!triangleFog&&!fogPrimaryAlpha&&!fogPrimaryRed&&!fogRampTexture(textures[originalTextureCount]))return fail("GX vertex fog ramp unavailable",carrier?DrawFailureKind::resource:DrawFailureKind::state);
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
    GxTextureDrawScope textureScope({textures.data(),material.textureCount});
    if(!textureScope)return fail(gxTextureFailure(),DrawFailureKind::resource);
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
    bool rigidGpu=false,gateCounted=false;
    bool signedQPositiveRigidAttempt=false;
    auto recordSignedQPositiveRigid=[&](SignedQPositiveRigidCounts::Total& total){++total.draws;total.vertices+=count;};
    const bool rigidProjected=originalTextureCount==2&&material.projectiveTexture0&&
        material.textures[0].coordinate==1&&material.textures[1].coordinate==0;
    const auto& rigidScale=scales[rigidProjected?1:0];
    const unsigned cpuReason=(raster.alphaUnion||raster.alphaInterval.enabled)?0:triangleFog?1:(fog.enabled&&fogPrimaryRed)?2:expanded?3:raster.clipDisabled?4:
        indirect.enabled?5:!originalTextureCount?6:(originalTextureCount>1&&!rigidProjected)?7:
        (!rigidProjected&&material.textures[0].coordinate!=0)?8:9;
    midSpan.stop();
    // Orthographic (2D) draws are unlit; never attempt the lit rigid shaders.
    const bool orthographicDraw=draw.registers.xfValid[0x1026]&&draw.registers.xf[0x1026]==1;
    { static const auto index=registerGxNamedProfile("adapter.rigidPlan");GxNamedProfileScope detail(index);
    if(!orthographicDraw&&!exactI4Pending&&!carrier&&(!signedQDiagnostic||context.allowSignedQPositiveRigidGpu)&&context.allowRigidGpu&&!raster.alphaUnion&&!raster.alphaInterval.enabled&&!triangleFog&&!(fog.enabled&&fogPrimaryRed)&&!expanded&&!raster.clipDisabled&&!indirect.enabled&&
       ((originalTextureCount==1&&material.textures[0].coordinate==0)||rigidProjected)){
        signedQPositiveRigidAttempt=signedQDiagnostic;
        if(signedQPositiveRigidAttempt)recordSignedQPositiveRigid(context.signedQPositiveRigid.attempts);
        gx::DecodedVertex first;
        if(draw.vertex(0,first))rigidGpu=gx::planRigidVertex(draw.registers,first,transform,colorPlan,fog,fogPrimaryAlpha,gx::currentTargetExtent.logicalWidth,rigid,rigidProjected);
        if(rigidGpu&&rigidProjected)rigid.uniforms[rigid.projectionBase()+3]={scales[0][0],scales[0][1],0,0};
    }
    // Native multi-texture GPU path: 1-3 texgen coordinates on the PICA.
    // Includes orthographic 2D draws when unlit (vertex/material color only).
    if(!rigidGpu&&!exactI4Pending&&!carrier&&!signedQDiagnostic&&context.allowRigidGpu&&!raster.alphaUnion&&!raster.alphaInterval.enabled&&
       !triangleFog&&!(fog.enabled&&fogPrimaryRed)&&!expanded&&(!raster.clipDisabled||!orthographicDraw)&&!indirect.enabled&&
       originalTextureCount<=3&&!rigidProjected&&!material.projectiveTexture0&&(!orthographicDraw||!colorPlan.lit)){
        gx::DecodedVertex first;
        const int fogUnit=fog.enabled&&!fogPrimaryAlpha&&!fogPrimaryRed?int(originalTextureCount):-1;
        if(draw.vertex(0,first))rigidGpu=gx::planRigidMultiVertex(draw.registers,first,transform,colorPlan,fog,fogPrimaryAlpha,
            gx::currentTargetExtent.logicalWidth,material,originalTextureCount,scales,fogUnit,rigid);
        // Native clip-disabled draws: the shader clamps depth instead of the
        // CPU transforming and re-clipping every vertex.
        if(rigidGpu&&raster.clipDisabled){rigid.uniforms[80][3]=1;++clipDisabledGpuDraws;}
    }
    if(!rigidGpu&&!orthographicDraw){gateCounted=true;
        // Diagnostic: first gate that kept a perspective draw off the GPU paths.
        const unsigned why=exactI4Pending?1:carrier?2:signedQDiagnostic?3:!context.allowRigidGpu?4:(raster.alphaUnion||raster.alphaInterval.enabled)?5:
            triangleFog?6:(fog.enabled&&fogPrimaryRed)?7:expanded?8:indirect.enabled?9:originalTextureCount>3?10:rigidProjected?11:
            material.projectiveTexture0?12:13;
        ++gxPerspCpuGate[why];gxPerspCpuGateVertices[why]+=count;
    }
    if(rigidGpu&&rigid.indexedMatrices){
        // PNMTXIDX is the first direct byte in a validated GX layout. Gather
        // only matrices actually used by this draw; larger palettes fall back.
        bool placed=true;
        for(unsigned i=0;i<count;++i)if(!gx::addRigidMatrix(draw.registers,draw.vertices[i*stride],rigid)){placed=false;break;}
        if(!placed&&!rigidProjected){
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
    if(!reserveGxGeometry(triangleFog?pairedCount*2:outputCount,indexCount,reservation))return fail(gxGeometryFailure(),DrawFailureKind::geometry);
    reservation.qPolicy=signedQDiagnostic?gx::GeometryQPolicy::directTexture0Diagnostic:gx::GeometryQPolicy::positiveOnly;
    auto batch=reservation;if(triangleFog)batch.vertices=batch.vertices.first(outputCount);
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
        gx::DecodedVertex decoded;gx::TransformedVertex vertex;std::array<float,4> color;
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
            std::array<float,4> second;
            if(!gx::evaluateRasterColor(*secondColorPlan,decoded,second,&colorError,&vertex)){
                carrierFailureVertex=i;return gx::rasterColorErrorText(colorError);
            }
            for(unsigned c=0;c<3;++c)if(!std::isfinite(color[c])||color[c]<0||color[c]>1||
                !std::isfinite(second[c])||second[c]<0||second[c]>1){carrierFailureVertex=i;return "GX diagnostic raster carrier nonfinite/range";}
            if(color[0]!=color[1]||color[1]!=color[2]){carrierFailureVertex=i;return "GX diagnostic raster0 not grayscale";}
            // Independent fields; the product stays in the material combiner.
            color={second[0],second[1],second[2],color[0]};
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
        }
        if(fog.enabled&&!triangleFog) {
            GxProfileScope section(GxProfilePart::vertexFog,sample);
            const float factor=gx::evaluateVertexFog(fog,vertex.clip,gx::currentTargetExtent.logicalWidth);
            if(carrier&&(!std::isfinite(factor)||factor<0||factor>1))return "GX diagnostic raster carrier fog invalid";
            if(fogPrimaryAlpha)target.color[3]=factor;
            else if(fogPrimaryRed)target.color[0]=factor;
            else target.texcoord[originalTextureCount]={(factor*255+.5f)/256,.5f};
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
        for(unsigned i=0;i<count;++i){bool invalid;
            if(!invalidVertexDivisor(i,invalid))return ProjectiveClip::unproved;
            if(invalid)batch.vertices[i]=batch.vertices[survivor];
        }
        return ProjectiveClip::trimmed;
    };
    reserveSpan.stop();
    {GxProfileScope vertexProfile(GxProfilePart::vertices);
    if(expanded) {
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
            rigidGpu=positive;if(!positive)++gx::gxRigidPlanRejects[61];
        }
        if(signedQPositiveRigidAttempt&&!rigidGpu){
            reservation.qPolicy=batch.qPolicy=gx::GeometryQPolicy::directTexture0Diagnostic;
            // The complete CPU loop below resets every GpuVertex, overwriting
            // all speculative raw/compact bytes before scanning/submission.
        }
        if(!rigidGpu){GxProfileScope section(GxProfilePart::vertexCpu);
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
                captureIndirect(i,error);
                return cancel(error);
            }}
    }
    }
    static const auto postIndex=registerGxNamedProfile("draw.post");GxNamedProfileSpan postSpan(postIndex);
    if(cameraCpuClip){
        namespace cc=gx::camera_clip109;
        // Single render owner, cold exception only. Fixed scratch is independent
        // of mesh size; no original or negative-Q vertex may leave CPU scratch.
        static cc::Scratch scratch;
        static bool busy=false;
        if(busy)return cancel("GX camera clipping reentrant",DrawFailureKind::geometry);
        struct BusyGuard {bool& value;explicit BusyGuard(bool& v):value(v){value=true;}~BusyGuard(){value=false;}} guard(busy);
        auto clipFailure=[&](const cc::Plan& p){
            ++context.cameraClipKernelRejected;
            if(!context.cameraClipFirstError){
                context.cameraClipFirstError=unsigned(p.error);
                if(p.failedVertex!=cc::noDetail)context.cameraClipFirstVertex=unsigned(p.failedVertex);
                if(p.failedTriangle!=cc::noDetail)context.cameraClipFirstTriangle=unsigned(p.failedTriangle);
            }
        };
        // Scoped gradual-underflow environment must wrap ONLY plan/emit here;
        // original transforms/lighting/indirect remain in their normal mode.
        const auto planned=cc::plan({batch.vertices,batch.indices},scratch);
        if(planned.error!=cc::Error::none){clipFailure(planned);return cancel("GX camera clipping plan rejected");}
        if(!planned.counts.vertices){
            if(!cancelGxGeometry(reservation))return fail("GX camera clipping cancellation failed",DrawFailureKind::geometry);
            ++context.cameraClipCulled;++context.culled;return true;
        }
        const auto nv=planned.counts.vertices,ni=planned.counts.indices;
        if(nv>cc::maxVertices-count||ni>cc::maxIndices-indexCount||
           (((nv+count)*sizeof(gx::GpuVertex)+15)&~std::size_t(15))+(ni+indexCount)*sizeof(u16)>512*1024)
            return cancel("GX camera clipping combined capacity",DrawFailureKind::geometry);
        // reserve() can complete/reclaim the entire old arena. Never retain or
        // read original prepared spans after cancel: rebuild into a disjoint tail.
        if(!cancelGxGeometry(reservation))return fail("GX camera clipping cancellation failed",DrawFailureKind::geometry);
        reservation={};batch={};
        if(!reserveGxGeometry(nv+count,ni+indexCount,reservation))return fail(gxGeometryFailure(),DrawFailureKind::geometry);
        reservation.qPolicy=gx::GeometryQPolicy::positiveOnly;
        if(aliases(std::as_bytes(draw.vertices))||aliases(std::as_bytes(draw.indices))||
           aliases(std::as_bytes(std::span(&draw.registers,1)))||aliases(std::as_bytes(std::span(&draw.plan,1)))||
           aliases(std::as_bytes(std::span(&draw.arrays,1))))return cancel("GX command input aliases GPU reservation");
        for(const auto& array:draw.arrays)if(aliases(std::as_bytes(array.bytes)))return cancel("GX command input aliases GPU reservation");
        auto originals=reservation.vertices.subspan(nv,count);
        auto originalIndices=reservation.indices.subspan(ni,indexCount);
        if(draw.indexCount)std::memcpy(originalIndices.data(),draw.indices.data(),indexCount*sizeof(u16));
        else{
            std::size_t at=0;
            if(!aurora::gx::dl::expand_triangles(draw.primitive,count,[&](u16 a,u16 b,u16 c){originalIndices[at++]=a;originalIndices[at++]=b;originalIndices[at++]=c;}))
                return cancel("GX camera clipping triangulation failed");
        }
        for(unsigned i=0;i<count;++i)if(const char* error=transformOne(i,originals[i]))return cancel(error);
        const cc::Input prepared{originals,originalIndices};
        const auto rebuilt=cc::plan(prepared,scratch);
        if(rebuilt.error!=cc::Error::none||rebuilt.counts!=planned.counts||rebuilt.digest!=planned.digest){
            auto failed=rebuilt;if(failed.error==cc::Error::none)failed.error=cc::Error::changed;
            clipFailure(failed);return cancel("GX camera clipping rebuilt input differs");
        }
        const auto emitted=cc::emit(prepared,rebuilt,scratch,reservation.vertices.first(nv),reservation.indices.first(ni));
        if(emitted.error!=cc::Error::none){auto failed=rebuilt;failed.error=emitted.error;clipFailure(failed);return cancel("GX camera clipping emission rejected");}
        // End all scratch-tail views before index relocation overwrites them.
        originals={};originalIndices={};
        if(!shrinkGxGeometry(reservation,nv,ni))return cancel(gxGeometryFailure(),DrawFailureKind::geometry);
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
    if(triangleFog){
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
        if(!submitGxRigidGeometry(raster,material,batch,rigid))return cancel(gxPicaFailure(),DrawFailureKind::submission);gxProfileTotals.rigidVertices+=count;if(rigid.compact)gxProfileTotals.compactVertices+=count;
        bool anyFallback=false;for(bool v:simplified)anyFallback|=v;
        // Native shape record (direct J3D draw on later frames).
        if(gx::staticGeometryHint.generation&&gx::staticGeometryHint.list&&gx::staticGeometryHint.batch==1&&
           rigid.compact&&rigid.staticVertices&&!rigidProjected&&!carrier&&!exactI4Pending&&!anyFallback&&!indirect.enabled&&
           !originalMaterial.indirectCount&&!signedQDiagnostic&&!cameraCpuClip&&!cameraAlignedProved&&!depthlessOrthoNormalized&&
           !positiveQDepthFallback&&!raster.clipDisabled&&!fogSubstituted&&!expanded&&draw.indexCount==batch.indices.size()){
            auto* cache=gx::staticCompactCache();
            const auto key=nativeStateKey(draw.registers,gx::staticGeometryHint.generation,gx::staticGeometryHint.list);
            auto& rec=nativeClaim(gx::staticGeometryHint.list,key);
            if(cache&&cache->resident()){
                // Reuse persistent indices recorded for this list (any instance).
                const std::uint16_t* reuse=nullptr;
                {const unsigned home=nativeHome(gx::staticGeometryHint.list);
                 for(unsigned n=0;n<nativeShapeProbe&&!reuse;++n){const auto& other=nativeShapes[(home+n)%nativeShapeCapacity];
                    if(other.list==gx::staticGeometryHint.list&&other.indices&&other.indexCount==batch.indices.size()&&other.indexEpoch==residentIndexEpoch()&&
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
                    rec.valid=true;nativeContext=&context;++nativeRecords;
                }
            }
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
    else if(!submitGxCommandGeometry(raster,material,batch))return cancel(gxPicaFailure(),DrawFailureKind::submission);
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
        if(context.reportFailure&&context.simplifiedByReason[i]==1)context.reportFailure(draw,i==unsigned(DrawVisualFallback::tevMaterial)?
            "GX visual fallback: simplified TEV material":"GX visual fallback: disabled depth clipping (PICA clipped)");
    }
    if(anySimplified)++context.simplifiedDraws;
    if(carrier)++context.rasterCarrierDraws;
    ++context.submitted;if(material.quantizedConstants)++context.quantizedDraws;
    if(fog.enabled)++context.vertexFogDraws;
    if(indirect.enabled)++context.vertexIndirectDraws;
    if(constantEarlyDepth)++context.constantEarlyDepthDraws;
    if(raster.alphaUnion)++context.alphaUnionDraws;
    if(raster.alphaInterval.enabled)++context.alphaIntervalDraws;
    gxProfileTotals.vertices+=count;if(carrier?bool(colorPlan.lit||secondColorPlan->lit):colorPlan.needsNormal)gxProfileTotals.litVertices+=count;
    if(fog.enabled)gxProfileTotals.fogVertices+=count;
    return true;
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
    const auto key=nativeStateKey(r,generation,list);
    keySpan.stop();
    auto* found=nativeFind(list,key);
    if(!found){++nativeKeyMisses;return false;}
    auto& rec=*found;
    if(rec.generation!=generation||cache->epoch!=rec.cacheEpoch||rec.indexEpoch!=residentIndexEpoch()){rec.valid=false;++nativeMisses;return false;}
    auto& context=*nativeContext;
    if(rec.raster.scissor.empty||rec.raster.cull==3){++context.culled;++nativeHits;return true;}
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
                                 rec.material,rec.originalTextureCount,scales,fogUnit,rigid):
        gx::planRigidVertex(r,rec.first,transform,colorPlan,fog,rec.fogPrimaryAlpha,gx::currentTargetExtent.logicalWidth,rigid,false);
    if(!planned)return false;
    for(unsigned n=0;n<rec.matrixCount;++n)if(!gx::addRigidMatrix(r,rec.matrixOrder[n]*3u,rigid))return false;
    if(rigid.matrixOffsets!=rec.matrixOffsets)return false;
    rigid.decodedNormalScale=rec.normalScale;rigid.decodedTex0Scale=rec.tex0Scale;rigid.decodedTex1Scale=rec.tex1Scale;
    if(!gx::finishCompactRigidUniforms(rigid,scales[0]))return false;
    rigid.staticVertices=rec.vertices;
    rigidSpan.stop();
    static const auto submitIndex=registerGxNamedProfile("native.submit");GxNamedProfileScope submitScope(submitIndex);
    if(!submitNativeRigid(rec.raster,rec.material,rigid,rec.vertices,rec.indices,rec.indexCount)){
        rec.valid=false;return false;
    }
    ++context.submitted;++nativeHits;
    gxProfileTotals.rigidVertices+=rec.vertexCount;gxProfileTotals.compactVertices+=rec.vertexCount;gxProfileTotals.vertices+=rec.vertexCount;
    return true;
}
