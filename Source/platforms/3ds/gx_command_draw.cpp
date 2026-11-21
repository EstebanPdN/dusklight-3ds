#include "gx_command_draw.hpp"
#include "runtime/gx_worker.hpp"
#include "runtime/gx_worker_sync.hpp"
#include "runtime/hud_tape.hpp"
#include "runtime/screen_tape.hpp"
#include "runtime/gx_copy_images.hpp"
#include "runtime/shadow_casters.hpp"
#include "runtime/screen_effects.hpp"
#include "gx_geometry_pica.hpp"
#include "gx_texture_pica.hpp"
#include "gx_pica.hpp"
#include "runtime/gx_vertex_transform.hpp"
#include "runtime/gx_screen_projectors.hpp"
#include "runtime/gx_transform_failure.hpp"
#include "runtime/gx_raster_plan.hpp"
#include "runtime/gx_raster_color.hpp"
#include "runtime/gx_texture_commands.hpp"
#include "runtime/gx_world_mips.hpp"
#include "runtime/gx_fog.hpp"
#include "runtime/gx_fog_pass.hpp"
#include "runtime/gx_indirect.hpp"
#include "runtime/gx_indirect_gpu.hpp"
#if defined(__3DS__) || defined(DUSK_INDIRECT_DIAGNOSTIC_TEST)
#include "runtime/gx_indirect_payload108.hpp"
#endif
#include "runtime/gx_early_alpha.hpp"
#include "runtime/gx_material_liveness.hpp"
#include "runtime/gx_profile.hpp"
#include "runtime/gx_rigid_vertex.hpp"
#include "runtime/gx_skin_palette.hpp"
#include "runtime/gx_clip_geometry.hpp"
#include "runtime/gx_camera_clip.hpp"
#include "runtime/gx_i4_bake_cache68.hpp"
#include "runtime/gx_compact_cache.hpp"
#include "native/color_lookup.hpp"
#include "runtime/gx_stream.hpp"
#if defined(__3DS__)
#include "runtime/native_packets.hpp"
#endif
#include <algorithm>
#include <cmath>
#include <bit>
#include <cstring>
#include "runtime/gx_state_serial.hpp"
#include "runtime/pica_blocks.hpp"
#include "runtime/model_lod.hpp"
#include "runtime/model_lod_store.hpp"
#include "runtime/gx_framebuffer.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <optional>
#include <new>

// The game log (the host tests supply their own).
extern "C" void OSReport(const char*,...);
// gx_rigid_decode.cpp: the static display list the draws that follow belong to.
extern "C" void duskNativeStaticGeometry(std::uint32_t generation,const void* list);
namespace dusk::native {
namespace {
// Address-stable, bounded derived resources; command render owner only.
gx::bake68::Cache exactI4Cache;
// The colour the original output can never go below, when the last GX stage
// adds nonnegative terms to a D known before the draw (an initial register,
// ONE, HALF or a konst): ADD with no bias, or a compare. Zero means unknown.
std::array<std::uint8_t,3> standInFloor(const gx::MaterialState& s){
    if(!s.stageCount||s.stageCount>16)return {};
    const auto& last=s.stages[s.stageCount-1];const auto& op=last.colorOp;
    const bool adds=op.operation>=8||(op.operation==0&&op.bias==0&&op.scale<3);
    const unsigned id=last.color[3];
    if(!adds||id==15||(id>=8&&id<12))return {};
    std::array<int,3> d{};
    if(id<8){
        const unsigned reg=id/2;
        if(!(s.colorValid&(1u<<reg)))return {};
        for(unsigned i=0;i+1<s.stageCount;++i)
            if(((id&1)?s.stages[i].alphaOp.output:s.stages[i].colorOp.output)==reg)return {};
        for(unsigned c=0;c<3;++c)d[c]=s.colors[reg][(id&1)?3:c];
    }else if(id==12)d={255,255,255};
    else if(id==13)d={128,128,128};
    else{
        const unsigned k=last.konstColor;
        constexpr std::uint8_t fractions[]{255,223,191,159,128,96,64,32};
        if(k<8)d.fill(fractions[k]);
        else if(k>=12&&k<=15&&(s.konstValid&(1u<<(k-12))))for(unsigned c=0;c<3;++c)d[c]=s.konst[k-12][c];
        else if(k>=16&&(s.konstValid&(1u<<((k-16)%4))))d.fill(s.konst[(k-16)%4][(k-16)/4]);
        else return {};
    }
    const unsigned scale=op.operation>=8?1u:1u<<op.scale;
    std::array<std::uint8_t,3> floor{};
    for(unsigned c=0;c<3;++c)floor[c]=std::uint8_t(std::clamp(d[c]*int(scale),0,255));
    return floor;
}
// Degraded stand-in for an untranslatable TEV material: the first sampled
// texture modulated by raster color 0 (either one alone when the other is
// absent). Blend, depth and alpha test still come from the original raster plan.
// Under a multiply blend (DSTCLR*src or SRCCLR*dst) that product would darken
// the scene far below the original (water shine MA09 drew shore vertices
// black): the RGB is the material's floor colour instead (K1 for MA09),
// or white (no change) when the floor is unknown. Returns true then.
bool simplifiedMaterial(const gx::MaterialState& s,const gx::RasterPlan& pixel,gx::PicaMaterialPlan& p){
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
    const auto& b=pixel.blend;
    const bool multiply=(b.mode&1)&&!pixel.subtractBlend&&((b.src==2&&b.dst==0)||(b.src==0&&b.dst==2));
    if(!multiply)return false;
    const auto floor=standInFloor(s);
    if(floor[0]||floor[1]||floor[2])for(unsigned c=0;c<3;++c)stage.constant[c]=floor[c];
    stage.rgb={MaterialFunction::replace,{gx::MaterialInput{MaterialSource::constant,MaterialOperand::color}}};
    return true;
}
// How a draw's TEV stand-in was chosen. A shape record keeps it, so that a
// replay with new TEV colours repeats the same choice (replanStandIn).
struct RecordedStandIn {
    std::uint8_t kind{};  // 0 none, 1 after a planner refusal, 2 lowered plan without room
    std::uint8_t maps{};  // texture maps validFallbackInputs resolved
    std::uint8_t error{}; // tevStandInByError index
    bool multiply{};      // the multiply-blend constant form (simplifiedMaterial)
};
// Diagnostics, printed once per profiled frame when a count moved (the zone
// report has only the degraded total): TEV stand-ins by planner error
// (gx::MaterialPlanError), multiply-blend constant stand-ins, and translated
// plans per planner lowering (gx::MaterialLowering bits). Replays of shape
// records that keep a stand-in count too (countReplayedFallback), so the
// stand-in counts stay per drawn draw; record replays of lowered plans and
// clip-disabled draws are not counted.
std::array<std::uint64_t,12> tevStandInByError{};
std::uint64_t multiplyStandIns{};
std::array<std::uint64_t,gx::MaterialLowering::count> loweredDraws{};
std::uint64_t degradedReportTick{};
std::uint64_t degradedReportSum=~std::uint64_t(0);
void reportDegradedReasons(const CommandDrawContext& c,bool profiledFrame,std::uint64_t clipDisabledGpu){
    // The first translation of each profiled frame checks. Profiled frames
    // are 30 frames (at least half a second) apart and one frame's draws take
    // far less than a quarter second, so the tick gap stands in for a frame
    // counter; a frame served only from records still re-arms the next one.
    constexpr std::uint64_t gapTicks=268111856/4; // ARM11 system ticks
    if(!profiledFrame)return;
    const auto now=gxProfileTick();
    if(degradedReportTick&&now-degradedReportTick<gapTicks)return;
    degradedReportTick=now;
#if defined(__3DS__)
    gx::reportIndirectConstant(+[](const char* line){OSReport("%s",line);});
    gx::reportMaterialOperationRefusals(+[](const char* line){OSReport("%s",line);});
#endif
    std::uint64_t sum=clipDisabledGpu+multiplyStandIns;
    for(auto v:c.simplifiedByReason)sum+=v;
    for(auto v:tevStandInByError)sum+=v;
    for(auto v:loweredDraws)sum+=v;
    if(sum==degradedReportSum)return;
    degradedReportSum=sum;
#if defined(__3DS__)
    // Four lines: OSReport keeps 255 characters of a line.
    const auto& r=c.simplifiedByReason;const auto& e=tevStandInByError;const auto& l=loweredDraws;
    OSReport("Native degraded draws by reason: tev=%llu clipped-depth=%llu projected-divisor=%llu dual-alpha=%llu shared-indirect=%llu "
             "indirect-divisor=%llu multiply-blend-constant=%llu\n",r[0],r[1],r[2],r[3],r[4],r[5],multiplyStandIns);
    OSReport("Native TEV stand-ins by planner error: stages=%llu operation=%llu constant-range=%llu fraction=%llu constant-conflict=%llu "
             "swap=%llu texture-units=%llu equation=%llu\n",e[1],e[2],e[6],e[7],e[8],e[9],e[10],e[11]);
    OSReport("Native TEV lowerings translated (replays not counted): halve=%llu compare=%llu soft-compare=%llu final-clamp=%llu signed-d=%llu "
             "hoist=%llu pass-through=%llu clip-disabled-depth-clamp=%llu\n",l[0],l[1],l[5],l[2],l[3],l[4],l[6],clipDisabledGpu);
    // Sum rules (gx::MaterialLowering::sumRules): second planner pass.
    OSReport("Native TEV sum rules translated (replays not counted): running-sum=%llu bias-fold=%llu prev-capture=%llu subtract-terms=%llu\n",
             l[7],l[8],l[9],l[10]);
#endif
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
// `maps` (optional) receives the texture maps it resolved: a recorded
// stand-in resolves them again on every replay (see NativeShapeRecord).
bool validFallbackInputs(const gx::MaterialState& state,const gx::CommandRegisters& registers,const char*& why,std::uint8_t* maps=nullptr){
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
            if(maps)*maps|=std::uint8_t(1u<<(stage.texture&7));
        }
        rgb|=1u<<stage.colorOp.output;alpha|=1u<<stage.alphaOp.output;
    }
    return true;
}
// A TEV stage reads rasterized colour 1 (the lookup carrier's input).
bool readsRaster1(const gx::MaterialState& state){
    bool read=false;
    for(unsigned i=0;i<state.stageCount;++i)if(state.stages[i].raster==1){
        for(auto id:state.stages[i].color)read|=id==10||id==11;
        for(auto id:state.stages[i].alpha)read|=id==5;
    }
    return read;
}
// A plan that needs a lowering rule leaves no room for the band alpha test or
// the vertex fog stage, and the fog overlay pass cannot take the draw: the
// complete path then draws the one-stage stand-in (see drawGxCommand).
bool loweredPlanNoRoom(const gx::PicaMaterialPlan& material,const gx::RasterPlan& raster,const gx::CommandRegisters& r,
                       const CommandDrawContext& context,bool expanded){
    bool noRoom=raster.alphaBand&&material.stageCount>3;
    const unsigned stagesAfterBand=material.stageCount+(raster.alphaBand?3u:0u);
    if(!noRoom&&context.allowVertexFog&&(stagesAfterBand>=material.stages.size()||material.textureCount>=material.textures.size())){
        gx::VertexFogPlan probe;gx::RasterPlan probeRaster;gx::PicaMaterialPlan probeMaterial;
        noRoom=gx::planVertexFog(r,probe)&&probe.enabled&&
            (!context.allowTriangleFog||expanded||!gx::planTriangleFogPass(raster,probeRaster,probeMaterial));
    }
    return noRoom;
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

// The ramp a fog unit samples: texel x holds the fog curve of GX fog function
// `function` at x/255 (gx_fog.hpp), so the unit's coordinate is the clamped
// linear ramp coordinate and the curve is applied per pixel. Function 2
// (linear) is the identity ramp every fog draw used until Alpha 8; the
// exponential functions (4-7) then needed the curve at each vertex and with
// it the CPU vertex route (Lake Hylia water, Alpha 7 console: 132 ms of a
// picture).
bool fogRampTexture(gx::Texture& texture,unsigned function) {
    // A small mathematical utility ramp, not a replacement for a game asset.
    // Immutable static lifetime, normal bounded texture registry/GPU cache.
    struct Ramp {std::array<std::uint8_t,32+256*8> bytes{};gx::TextureHandle handle{};bool built{};};
    static std::array<Ramp,5> ramps{};
    const unsigned slot=function==2?0:function>=4&&function<=7?function-3:5;
    if(slot>=ramps.size())return false;
    auto& ramp=ramps[slot];
    if(!ramp.built){
        auto& b=ramp.bytes;
        b[0]=1;b[2]=1;b[5]=8;b[20]=b[21]=1;b[24]=1;b[31]=32;
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<256;++x)
            b[32+((y/4)*32+x/8)*32+(y%4)*8+x%8]=function==2?std::uint8_t(x):
                std::uint8_t(std::lround(std::clamp(gx::fogCurve(function,x/255.0),0.0,1.0)*255));
        ramp.built=true;
    }
    if(!ramp.handle&&!gx::registerTexture(ramp.bytes,ramp.handle,true))return false;
    std::uint32_t token{};
    return gx::textureCommandToken(ramp.bytes.data()+32,token)&&gx::textureFromCommandToken(token,texture);
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
        if(fog.enabled&&!rec.fogPrimaryAlpha&&!rec.fogPrimaryRed&&!fogRampTexture(textures[rec.originalTextureCount],fog.function))return 0;
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
    const bool worldDraw=gx::worldMipDraw(draw.registers,gx::staticGeometryHint.generation&&gx::staticGeometryHint.list);
    for(unsigned unit=0;unit<rec.material.textureCount;++unit)
        if(!bindGxTexture(textures[unit],scales[unit][0],scales[unit][1],unit,unit==0&&rec.material.projectiveTexture0,
            unit<rec.originalTextureCount?gx::worldMipUse(draw.registers,worldDraw,rec.material.textures[unit].coordinate):GxWorldMipUse{}))return 0;
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
    // Compact palette (gx_rigid_vertex.hpp restartCompactPalette): its rows
    // follow live matrix values (shared normal rows), so a replay rebuilds the
    // compact layout and compares both row maps. Only a list's first batch
    // can be such a record (gx_rigid_decode.cpp exposesResident).
    std::array<std::int8_t,10> normalOffsets{};bool compactPalette{};
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
    // A merged skin list (runtime/gx_skin_palette.hpp): its palette entries,
    // placed from the bank at every replay; `first` names slot 0.
    std::uint8_t skinEntries{};
    // A visual fallback the record keeps (since 2026-11-16): the TEV stand-in
    // and, in `raster`, a relaxed dual alpha test. Both follow from the key's
    // registers and the TEV colours; a replay resolves the texture maps the
    // stand-in decision resolved and, on new colours, repeats that decision
    // (replanStandIn). The projected-divisor clamp is never recorded.
    RecordedStandIn standIn{};
};
constexpr unsigned nativeMaxBatches=12;
constexpr unsigned nativeShapeCapacity=1024,nativeShapeProbe=8;
// Merged skin lists, drawing side (whichever thread has the GPU: the worker,
// or the game thread at a sync). `refused`: drawGxCommand refused a batch of
// the merged list being executed, `batches`: those it drew; drawQueuedShape
// reads both after the list and draws the groups instead (skinFallback).
struct SkinDrawState {
    bool refused{};unsigned batches{};
    std::array<std::uint64_t,unsigned(gx::SkinRefusal::count)> refusals{};
    std::uint64_t recorded{},replayed{},replayRefusals{},fallbacks{},fallbackGroups{},partial{},lost{};
};
SkinDrawState skinDrawState;
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
    // Lean replays (nativeLeanReplay): `plan` is not copied. The shared
    // replay plan keeps the predecessor's block, whose rows the finish step
    // only overwrites, except the unit rows 81-92 it scales and folds: those
    // are kept here as planned.
    bool rowsOnly{};
    std::array<std::array<float,4>,12> unitRows{};
    // The previous shape went out as a PICA block (rows mode): its plans
    // continue, but Citro3D's state is not its state, so the next submit
    // applies the state again instead of running as a continuation.
    bool afterBlock{};
};
NativeChain nativeChain;
bool nativeChainEnabled=true;
std::uint64_t nativeChained{};
// The plan every replayed shape builds (gx::prepareRigidMultiReuse): one
// storage instead of a 2.2 KB cleared plan per shape and a 2.2 KB copy per
// continuation. Only nativeReplay uses it.
gx::RigidVertexPlan nativeReplayPlan;
// Lean shape replay (multi-texture records). Per shape the complete replay
// builds a transform plan (2.7 KB, cleared) for its clip projection and a
// fog plan, clears the 2.2 KB uniform plan and copies it for continuations.
// The lean replay takes the projection and the fog plan from the caches
// below, checks the texgens with gx::replayTexgenMask and builds the plan in
// nativeReplayPlan; the rows it uploads are those of the complete replay
// (host test gx_rigid_texgen). SD switch native-lean-replay.txt ('0' off).
bool nativeLeanReplay=true;
// Clip projection as planVertexTransform builds it from XF 0x101a-0x1026
// (viewport, projection, its type) and the target extent, for as long as
// those words, their validity and the extent stay as they were. An equal
// state epoch of the same bank proves the words unchanged without reading
// them; otherwise they are compared.
struct NativeProjectionCache {
    bool filled{},ok{};
    const gx::CommandRegisters* bank{};std::uint32_t epoch{};
    std::array<std::uint32_t,13> words{};std::uint32_t valid{};
    gx::TargetExtent extent{};
    std::array<float,16> projection{};
};
NativeProjectionCache nativeProjectionCache;
// The vertex fog plan (planVertexFog reads BP 0xe8-0xf2 and XF 0x101a),
// cached the same way.
struct NativeFogCache {
    bool filled{},ok{};
    const gx::CommandRegisters* bank{};std::uint32_t epoch{};
    std::array<std::uint32_t,11> bp{};std::uint32_t valid{},viewport{};
    gx::VertexFogPlan plan{};
};
NativeFogCache nativeFogCache;
// Diagnostics (profiled report "Native shape replay"): lean plans, lean
// replays refused at the plan step (the complete path then draws the list),
// replays planned the complete way (switch off or single-texture records),
// continuations served from the kept unit rows, and both caches' lookups by
// outcome: same epoch, words compared equal, planned again.
std::uint64_t nativeLeanPlans{},nativeLeanRefusals{},nativeFullPlans{},nativeRowContinuations{};
std::uint64_t nativeProjectionEpochHits{},nativeProjectionEqualHits{},nativeProjectionPlanned{};
std::uint64_t nativeFogEpochHits{},nativeFogEqualHits{},nativeFogPlanned{};
// Not inlined: its 2.7 KB transform plan stays out of nativeReplay's frame.
[[gnu::noinline]] const NativeProjectionCache& nativeProjection(const gx::CommandRegisters& r){
    auto& c=nativeProjectionCache;
    constexpr unsigned first=0x101a,count=13;
    if(c.filled&&!std::memcmp(&c.extent,&gx::currentTargetExtent,sizeof(c.extent))){
        if(c.bank==&r&&c.epoch==r.stateEpoch){++nativeProjectionEpochHits;return c;}
        std::uint32_t valid=0;for(unsigned i=0;i<count;++i)valid|=std::uint32_t(r.xfValid[first+i])<<i;
        if(valid==c.valid&&!std::memcmp(c.words.data(),r.xf.data()+first,sizeof(c.words))){
            c.bank=&r;c.epoch=r.stateEpoch;++nativeProjectionEqualHits;return c;
        }
    }
    ++nativeProjectionPlanned;
    {gx::VertexTransformPlan plan;c.ok=gx::planVertexTransform(r,0,plan,nullptr);c.projection=plan.clipProjection();}
    c.valid=0;for(unsigned i=0;i<count;++i)c.valid|=std::uint32_t(r.xfValid[first+i])<<i;
    std::memcpy(c.words.data(),r.xf.data()+first,sizeof(c.words));
    c.extent=gx::currentTargetExtent;c.bank=&r;c.epoch=r.stateEpoch;c.filled=true;
    return c;
}
const NativeFogCache& nativeFog(const gx::CommandRegisters& r){
    auto& c=nativeFogCache;
    constexpr unsigned first=0xe8,count=11;
    if(c.filled){
        if(c.bank==&r&&c.epoch==r.stateEpoch){++nativeFogEpochHits;return c;}
        std::uint32_t valid=std::uint32_t(r.xfValid[0x101a])<<count;
        for(unsigned i=0;i<count;++i)valid|=std::uint32_t(r.bpValid[first+i])<<i;
        if(valid==c.valid&&r.xf[0x101a]==c.viewport&&!std::memcmp(c.bp.data(),r.bp.data()+first,sizeof(c.bp))){
            c.bank=&r;c.epoch=r.stateEpoch;++nativeFogEqualHits;return c;
        }
    }
    ++nativeFogPlanned;
    c.ok=gx::planVertexFog(r,c.plan);
    c.valid=std::uint32_t(r.xfValid[0x101a])<<count;
    for(unsigned i=0;i<count;++i)c.valid|=std::uint32_t(r.bpValid[first+i])<<i;
    c.viewport=r.xf[0x101a];std::memcpy(c.bp.data(),r.bp.data()+first,sizeof(c.bp));
    c.bank=&r;c.epoch=r.stateEpoch;c.filled=true;
    return c;
}
std::uint64_t nativeReplayReportTick{};
std::uint64_t nativeReplayReportSum=~std::uint64_t(0);
// Once per profiled frame when a count moved (as reportImmediateDetail).
void reportNativeReplay(){
    constexpr std::uint64_t gapTicks=268111856/4; // ARM11 system ticks
    const auto now=gxProfileTick();
    if(nativeReplayReportTick&&now-nativeReplayReportTick<gapTicks)return;
    nativeReplayReportTick=now;
    const std::uint64_t sum=nativeLeanPlans+nativeLeanRefusals+nativeFullPlans+nativeRowContinuations+nativeProjectionEpochHits+
        nativeProjectionEqualHits+nativeProjectionPlanned+nativeFogEpochHits+nativeFogEqualHits+nativeFogPlanned;
    if(sum==nativeReplayReportSum)return;
    nativeReplayReportSum=sum;
#if defined(__3DS__)
    OSReport("Native shape replay: lean plans=%llu refused=%llu full=%llu rows-continued=%llu; projection epoch=%llu equal=%llu planned=%llu; "
             "fog epoch=%llu equal=%llu planned=%llu\n",nativeLeanPlans,nativeLeanRefusals,nativeFullPlans,nativeRowContinuations,
             nativeProjectionEpochHits,nativeProjectionEqualHits,nativeProjectionPlanned,nativeFogEpochHits,nativeFogEqualHits,nativeFogPlanned);
#endif
}
// Records that keep a deterministic visual fallback (see NativeShapeRecord;
// SD switch fallback-records.txt, '0' refuses them as before): stored, then
// replays of a stand-in and of a relaxed alpha test, stand-in decisions
// repeated on new TEV colours, and those that chose differently (the complete
// path then draws and records again).
bool nativeFallbackRecords=true;
std::uint64_t nativeFallbackStored{},nativeStandInReplays{},nativeRelaxedAlphaReplays{},nativeStandInRefreshes{},nativeStandInRefusals{};
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
    // The command processor keeps the four components as it writes the bank
    // (exact Zobrist sums of these words, gx_commands.hpp): no rehash when
    // the epoch moves. state-hash.txt '0' restores the rehash below.
    if(gx::stateHashComponents(r,kc.bp,kc.texture,kc.cp,kc.xf))kc.valid=false;
    else if(!kc.valid||kc.epoch!=r.stateEpoch){
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
// A replay finishes the record's own uniform block in place instead of a
// 1.5 KiB copy per draw: the texgen rows go back to their planned values
// (unitRows) and the matrix rows are placed afresh, so the decoder's tail
// gives exactly the rows a copy of the planned block would get. A replay
// whose inputs all equal the record's previous one (an unchanged HUD pane)
// reuses that finished block and its decoded vertices (ImmediateRepeat).
constexpr unsigned immediateUnitRowBase=gx::multiUnitRow(2);
constexpr unsigned immediateUnitRowCount=gx::multiUnitRow(0)+4-immediateUnitRowBase;
constexpr unsigned immediateRepeatVertices=4;
struct ImmediateRepeat {
    bool valid{};
    std::uint8_t count{},matrix{};
    const void* format{}; // the decode plan (VAT slot) the payload was read with
    std::uint32_t matrixValid{};
    std::array<std::uint32_t,21> matrixWords{}; // what addRigidMatrix read
    std::array<std::uint8_t,128> payload{};
    std::array<std::byte,immediateRepeatVertices*sizeof(gx::CompactRigidVertexMT)> vertices{};
};
struct ImmediateRecord {
    bool used{};
    // The frame (gx::textureFrameCounter) of the last replay or store: a
    // claim replaces the record that has waited longest.
    std::uint32_t stamp{};
    std::uint32_t keyA{},keyB{},extra{};
    std::uint32_t extraColor{},extraOther{}; // the two parts of `extra` (miss attribution)
    gx::TargetExtent extent{};
    gx::RasterPlan raster;gx::PicaMaterialPlan material;
    gx::RigidVertexPlan plan; // planned uniforms; replays finish them in place
    // The texgen rows as planned, before a decode folded the fixed-point
    // texture fractions in (multiUnitRow(2) to multiUnitRow(0)+3).
    std::array<std::array<float,4>,immediateUnitRowCount> unitRows{};
    std::array<std::array<float,2>,3> scales{};
    std::uint8_t textureCount{};
    // Vertex fog (immediate-fog.txt): the fog rows are in `plan`, the fog
    // stage in `material`; a fog applied through a ramp unit binds the ramp
    // of `fogFunction` after the material's textures.
    bool fogged{},fogRamp{};std::uint8_t fogFunction{};
    ImmediateRepeat repeat;
};
constexpr unsigned immediateCapacity=256,immediateProbe=4;
std::array<ImmediateRecord,immediateCapacity> immediateRecords{};
bool immediateEnabled=true;
// Fogged immediate records (SD switch immediate-fog.txt: 1 on, v verify, 0
// or no file off). Their fog inputs are all in the fingerprint: the fog type
// and projection kind in the state key, the curve, range and fog C value in
// the extra part, the fog colour in the colour part, the viewport and the
// logical width in both. Verify: a fogged draw with a record is translated
// completely instead, and the record's plans are compared with the new ones.
enum class ImmediateFog : std::uint8_t {off,on,verify};
std::atomic<std::uint8_t> immediateFogMode{std::uint8_t(ImmediateFog::off)};
std::uint64_t immediateFogStores{},immediateFogHits{},immediateFogVerified{},immediateFogMismatches{};
ImmediateRecord* immediateFogVerifyRecord{};
// A/B switch for the two shortcuts below (unchanged-draw repeats and the
// lookup key memo); the in-place finish stays.
bool immediateShortcuts=true;
std::uint64_t immediateHits{},immediateStores{},immediateFallbacks{},immediateMisses{};
// Diagnostics (profiled report): repeats, key memo hits, records replaced
// while in use, misses with no record of the state key / a record whose
// non-colour extra differs / one that differs in colours only, and replays
// whose every vertex alpha is zero under a vertex-alpha channel.
std::uint64_t immediateRepeats{},immediateKeyMemoHits{},immediateEvictions{};
std::uint64_t immediateMissState{},immediateMissExtra{},immediateMissColor{},immediateZeroAlpha{};
// State outside nativeStateKey that the recorded plans depend on. Colour
// part: TEV colour/konst banks and fog colour (PICA constants), channel
// material/ambient colours. The rest: viewport and projection, fog range,
// the texture matrices read by the active texgens, primitive and stride.
struct ImmediateExtra {std::uint32_t color{},other{},combined{};};
ImmediateExtra immediateExtra(const gx::CommandDraw& draw){
    const auto& r=draw.registers;
    auto mix=[](std::uint32_t& h,std::uint32_t v){h=(h^v)*16777619u;};
    std::uint32_t color=nativeDynamic(r),other=2166136261u;
    for(unsigned i=0x100a;i<=0x100d;++i)mix(color,r.xf[i]);
    for(unsigned i=0x101a;i<=0x1025;++i)mix(other,r.xf[i]);
    for(unsigned i=0xe8;i<=0xf0;++i)mix(other,r.bp[i]);
    // Fog C (BP 0xf1 below bit 20; the fog type above it is a key word).
    if((r.bp[0xf1]>>21)&7)mix(other,r.bp[0xf1]);
    const unsigned gens=std::min<unsigned>(r.xf[0x103f]&15,8);
    for(unsigned c=0;c<gens;++c){
        const unsigned m=c<4?(r.matrixIndex[0]>>(6+6*c))&63:(r.matrixIndex[1]>>(6*(c-4)))&63;
        if(m<60)for(unsigned k=0;k<8;++k)mix(other,r.xf[4*m+k]);
        const unsigned postRow=r.xf[0x1050+c]&63;
        if(postRow<61)for(unsigned k=0;k<8;++k)mix(other,r.xf[0x500+4*postRow+k]);
    }
    mix(other,unsigned(draw.primitive));mix(other,draw.plan.packedStride());
    return {color,other,(color*0x9e3779b1u)^other};
}
// The lookup key of the last immediate draw. It stands while the register
// bank's state epoch does: the epoch covers every register both hashes read
// except the per-draw ones compared here (matrix index registers, target,
// BP mask, primitive, stride) and texture matrices in position-matrix memory
// (rows below 30), which keep a draw out of the memo. Only the command
// stream's own bank qualifies: a copied bank keeps its epoch while changed.
struct ImmediateKeyMemo {
    bool valid{};
    const gx::CommandRegisters* bank{};
    std::uint32_t epoch{},matrix0{},matrix1{},bpMask{},width{},height{},primitive{},stride{};
    PreparedKey key{};ImmediateExtra extra{};
};
ImmediateKeyMemo immediateKeyMemo;
bool immediateMemoizable(const gx::CommandRegisters& r){
    const auto* stream=gx::commandStream();
    if(!immediateShortcuts||!stream||&stream->registers()!=&r)return false;
    const unsigned gens=std::min<unsigned>(r.xf[0x103f]&15,8);
    for(unsigned c=0;c<gens;++c){
        const unsigned m=c<4?(r.matrixIndex[0]>>(6+6*c))&63:(r.matrixIndex[1]>>(6*(c-4)))&63;
        if(m<30)return false;
    }
    return true;
}
void immediateLookupKey(const gx::CommandDraw& draw,PreparedKey& key,ImmediateExtra& extra){
    const auto& r=draw.registers;auto& m=immediateKeyMemo;
    const auto& extent=gx::currentTargetExtent;
    const std::uint32_t primitive=unsigned(draw.primitive),stride=draw.plan.packedStride();
    if(m.valid&&m.bank==&r&&m.epoch==r.stateEpoch&&m.matrix0==r.matrixIndex[0]&&m.matrix1==r.matrixIndex[1]&&
       m.bpMask==r.bpMask&&m.width==extent.logicalWidth&&m.height==extent.logicalHeight&&
       m.primitive==primitive&&m.stride==stride){
        key=m.key;extra=m.extra;++immediateKeyMemoHits;return;
    }
    key=nativeStateKey(r,0x494d4d00u,nullptr);extra=immediateExtra(draw);
    m={immediateMemoizable(r),&r,r.stateEpoch,r.matrixIndex[0],r.matrixIndex[1],r.bpMask,
       extent.logicalWidth,extent.logicalHeight,primitive,stride,key,extra};
}
ImmediateRecord* immediateFind(PreparedKey key,std::uint32_t extra){
    for(unsigned n=0;n<immediateProbe;++n){auto& rec=immediateRecords[(key.a+n)%immediateCapacity];
        if(rec.used&&rec.keyA==key.a&&rec.keyB==key.b&&rec.extra==extra)return &rec;}
    return nullptr;
}
// Why a lookup missed (see the diagnostics above).
void immediateMiss(PreparedKey key,const ImmediateExtra& extra){
    ++immediateMisses;
    const ImmediateRecord* sameState=nullptr;
    for(unsigned n=0;n<immediateProbe;++n){const auto& rec=immediateRecords[(key.a+n)%immediateCapacity];
        if(!rec.used||rec.keyA!=key.a||rec.keyB!=key.b)continue;
        sameState=&rec;if(rec.extraOther==extra.other)break;}
    if(!sameState)++immediateMissState;
    else if(sameState->extraOther==extra.other)++immediateMissColor;
    else ++immediateMissExtra;
}
// The slot a new record of this draw would take. Nothing is changed here: a
// draw that ends without a record (CPU route, several matrices, a fallback)
// must not cost another draw its record. Until 2026-11-16 the claim emptied
// its victim at once, and every such draw evicted a live record (5 to 12 a
// picture in the Alpha 7 wave 2 run, each one a later miss).
// Order: the same draw with other colours that was not drawn this frame (an
// animated pane leaves one stale record per picture), a free slot, then the
// record unused for longest.
ImmediateRecord& immediateClaim(PreparedKey key,const ImmediateExtra& extra){
    const std::uint32_t now=gx::textureFrameCounter;
    ImmediateRecord* empty=nullptr;ImmediateRecord* oldest=nullptr;
    for(unsigned n=0;n<immediateProbe;++n){auto& rec=immediateRecords[(key.a+n)%immediateCapacity];
        if(!rec.used){if(!empty)empty=&rec;continue;}
        if(rec.keyA==key.a&&rec.keyB==key.b&&rec.extraOther==extra.other&&rec.stamp!=now)return rec;
        if(!oldest||now-rec.stamp>now-oldest->stamp)oldest=&rec;}
    return empty?*empty:*oldest;
}
// The uniform block of the draw being translated, as planned; copied into
// the claimed slot only when the draw ends as a record.
gx::RigidVertexPlan& immediateCapturedPlan(){static gx::RigidVertexPlan plan;return plan;}
// Sub-phases of one immediate lookup on profiled frames, timed back to back
// from one start (one tick read per boundary): imm.key (key, lookup, claim),
// imm.textures (resolve, protect, bind), imm.plan (uniform block),
// imm.geometry (reserve, indices, vertices), imm.submit. draw.immediate is
// the whole span, from the same reads.
struct ImmediateProfiles {unsigned total,key,textures,plan,geometry,submit;};
const ImmediateProfiles& immediateProfiles(){
    static const ImmediateProfiles names{registerGxNamedProfile("draw.immediate"),registerGxNamedProfile("imm.key"),
        registerGxNamedProfile("imm.textures"),registerGxNamedProfile("imm.plan"),registerGxNamedProfile("imm.geometry"),
        registerGxNamedProfile("imm.submit")};
    return names;
}
class ImmediateLaps {
public:
#if defined(__3DS__)
    explicit ImmediateLaps(bool enabled):active(enabled){if(active)start=last=gxProfileTick();}
    void lap(unsigned index){if(!active)return;const auto now=gxProfileTick();add(index,now-last);last=now;}
    // atLastLap: the span ends at the last boundary read (no further read).
    void stop(bool atLastLap=false){
        if(!active)return;
        add(immediateProfiles().total,(atLastLap?last:gxProfileTick())-start);active=false;
    }
    ~ImmediateLaps(){stop();}
private:
    static void add(unsigned index,std::uint64_t ticks){
        if(index>=gxNamedProfiles.size())return;
        auto& e=gxNamedProfiles[index];e.ticks+=ticks;++e.calls;
    }
    std::uint64_t start{},last{};bool active{};
#else
    explicit ImmediateLaps(bool){}
    void lap(unsigned){}
    void stop(bool=false){}
#endif
};
// Every attribute in the packed stream (GX_DIRECT) or absent: the decoded
// records then depend only on the packed bytes and the vertex format. Same
// fields as CommandProcessor::prepare reads (POS-CLR1 low, TEX0-7 high).
bool directVertexFormat(const gx::CommandRegisters& r){
    const std::uint32_t low=r.cp[0x50],high=r.cp[0x60];
    for(unsigned i=0;i<4;++i)if(((low>>(9+2*i))&3)>1)return false;
    for(unsigned i=0;i<8;++i)if(((high>>(2*i))&3)>1)return false;
    return true;
}
// The register words addRigidMatrix reads for one matrix (3 position rows,
// 3 normal rows) and their validity bits; invalid words read as zero.
void rigidMatrixWords(const gx::CommandRegisters& r,unsigned index,std::array<std::uint32_t,21>& words,std::uint32_t& valid){
    valid=0;
    for(unsigned k=0;k<12;++k){const unsigned at=4*index+k;const bool v=r.xfValid[at];words[k]=v?r.xf[at]:0;valid|=std::uint32_t(v)<<k;}
    for(unsigned k=0;k<9;++k){const unsigned at=0x400+3*index+k;const bool v=r.xfValid[at];words[12+k]=v?r.xf[at]:0;valid|=std::uint32_t(v)<<(12+k);}
}
// 1: submitted; 0: use the complete path; -1: failed after reservation.
// A 0 after the plan step leaves the record's block half finished: the
// caller drops the record (the complete path records it again).
int drawImmediate(ImmediateRecord& rec,const gx::CommandDraw& draw,CommandDrawContext& context,ImmediateLaps& laps){
    const auto& names=immediateProfiles();
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
    // The fog ramp after them, as the complete translation binds it.
    const unsigned units=rec.textureCount+(rec.fogRamp?1u:0u);
    if(rec.fogRamp&&(units>3||!fogRampTexture(textures[rec.textureCount],rec.fogFunction)))return 0;
    std::array<std::array<float,2>,3> scales{};
    GxTextureDrawScope textureScope({textures.data(),units});
    if(!textureScope)return 0;
    for(unsigned unit=0;unit<units;++unit)
        if(!bindGxTexture(textures[unit],scales[unit][0],scales[unit][1],unit,false))return 0;
    // The recorded texture rows hold these scales (padded texture extents).
    if(scales!=rec.scales)return 0;
    laps.lap(names.textures);
    const unsigned matrix=r.matrixIndex[0]&63;
    if(!r.matrixIndexValid[0]||matrix>27||matrix%3)return 0;
    // Unchanged draw: the block and the vertices of the previous replay are
    // functions of the record, the decode plan, the packed bytes (direct
    // attributes only) and the matrix words, all equal here.
    auto& repeat=rec.repeat;
    const std::size_t payloadBytes=std::size_t(count)*stride;
    const bool repeatable=immediateShortcuts&&count<=immediateRepeatVertices&&payloadBytes<=repeat.payload.size()&&directVertexFormat(r);
    std::array<std::uint32_t,21> words{};std::uint32_t valid=0;
    bool same=false;
    if(repeatable){
        rigidMatrixWords(r,matrix,words,valid);
        same=repeat.valid&&repeat.format==&draw.plan&&repeat.count==count&&repeat.matrix==matrix&&repeat.matrixValid==valid&&
             repeat.matrixWords==words&&!std::memcmp(repeat.payload.data(),draw.vertices.data(),payloadBytes);
    }
    auto& rigid=rec.plan;
    if(!same){
        repeat.valid=false;
        std::copy(rec.unitRows.begin(),rec.unitRows.end(),rigid.uniforms.begin()+immediateUnitRowBase);
        rigid.matrixOffsets.fill(-1);rigid.normalOffsets.fill(-1);rigid.matrices=0;rigid.foldedOrigin={};
        if(!gx::addRigidMatrix(r,matrix,rigid))return 0;
    }
    laps.lap(names.plan);
    gx::GpuGeometryBatch batch;
    if(!reserveGxGeometry(count,indexCount,batch))return 0;
    batch.qPolicy=gx::GeometryQPolicy::positiveOnly;
    auto abandon=[&]{return cancelGxGeometry(batch)?0:-1;};
    std::size_t at=0;
    if(!aurora::gx::dl::expand_triangles(draw.primitive,count,[&](u16 a,u16 b,u16 c){batch.indices[at++]=a;batch.indices[at++]=b;batch.indices[at++]=c;}))
        return abandon();
    const auto records=std::as_writable_bytes(batch.vertices).first(std::size_t(count)*sizeof(gx::CompactRigidVertexMT));
    if(same){std::memcpy(records.data(),repeat.vertices.data(),records.size());++immediateRepeats;gxProfileTotals.rigidDecodedVertices+=count;}
    else{
        {GxProfileScope vertexProfile(GxProfilePart::vertices);
        GxProfileScope section(GxProfilePart::vertexRigid);
        if(!gx::decodeCompactRigidBatchMT(draw.plan,draw.vertices,draw.arrays,batch.vertices,rigid)||rigid.staticVertices)return abandon();
        }
        gxProfileTotals.rigidDecodedVertices+=count;
        if(repeatable){
            repeat.format=&draw.plan;repeat.count=std::uint8_t(count);repeat.matrix=std::uint8_t(matrix);
            repeat.matrixValid=valid;repeat.matrixWords=words;
            std::memcpy(repeat.payload.data(),draw.vertices.data(),payloadBytes);
            std::memcpy(repeat.vertices.data(),records.data(),records.size());
            repeat.valid=true;
        }
    }
    // Hidden panes still drawn: every vertex alpha zero under a vertex-alpha channel.
    if((rigid.color.vertex&2)&&!(rigid.color.lit&2)){
        bool zero=true;
        for(unsigned i=0;i<count&&zero;++i)zero=std::to_integer<unsigned>(records[i*sizeof(gx::CompactRigidVertexMT)+offsetof(gx::CompactRigidVertexMT,color)+3])==0;
        if(zero)++immediateZeroAlpha;
    }
    laps.lap(names.geometry);
    if(!submitGxRigidGeometry(rec.raster,rec.material,batch,rigid)){
        cancelGxGeometry(batch);
        if(!context.rejected){context.failure=gxPicaFailure();context.failureKind=DrawFailureKind::submission;if(context.reportFailure)context.reportFailure(draw,context.failure);}
        ++context.rejected;++context.rejectedByKind[unsigned(DrawFailureKind::submission)];return -1;
    }
    laps.lap(names.submit);
    gxProfileTotals.rigidVertices+=count;gxProfileTotals.compactVertices+=count;
    gxProfileTotals.vertices+=count;
    ++context.submitted;if(rec.material.quantizedConstants)++context.quantizedDraws;
    if(rec.fogged)++immediateFogHits;
    return 1;
}
std::uint64_t immediateReportTick{};
std::uint64_t immediateReportSum=~std::uint64_t(0);
// Once per profiled frame when a count moved (same pacing as
// reportDegradedReasons); cumulative counts, like the record line.
void reportImmediateDetail(bool profiledFrame){
    constexpr std::uint64_t gapTicks=268111856/4; // ARM11 system ticks
    if(!profiledFrame)return;
    const auto now=gxProfileTick();
    if(immediateReportTick&&now-immediateReportTick<gapTicks)return;
    immediateReportTick=now;
    const std::uint64_t sum=immediateRepeats+immediateKeyMemoHits+immediateEvictions+immediateMissState+immediateMissExtra+
        immediateMissColor+immediateZeroAlpha+immediateFogStores+immediateFogHits+immediateFogVerified;
    if(sum==immediateReportSum)return;
    immediateReportSum=sum;
#if defined(__3DS__)
    OSReport("Native immediate draw detail: repeats=%llu key-memo=%llu evictions=%llu misses state=%llu extra=%llu colour-only=%llu zero-vertex-alpha=%llu\n",
        immediateRepeats,immediateKeyMemoHits,immediateEvictions,immediateMissState,immediateMissExtra,immediateMissColor,immediateZeroAlpha);
    if(immediateFogMode.load(std::memory_order_relaxed))
        OSReport("Native immediate fogged records: stored=%llu hits=%llu verified=%llu mismatches=%llu (of %llu stores, %llu hits)\n",
            immediateFogStores,immediateFogHits,immediateFogVerified,immediateFogMismatches,immediateStores,immediateHits);
#endif
}
// Equal material plans, member by member. Every member up to rasterCarrier
// is made of single bytes with nothing between them, so those bytes compare
// as one block; `lowering` (two bytes since wave 2) follows a padding byte
// whose value is whatever the stack held: a byte compare of whole plans
// refused constant refreshes and missed continuations at random.
constexpr std::size_t materialHeadBytes=offsetof(gx::PicaMaterialPlan,rasterCarrier)+sizeof(gx::RasterCarrier);
static_assert(sizeof(gx::MaterialInput)==sizeof(gx::MaterialSource)+sizeof(gx::MaterialOperand)+sizeof(bool)&&
              sizeof(gx::PicaMaterialChannel)==sizeof(gx::MaterialFunction)+3*sizeof(gx::MaterialInput)+1&&
              sizeof(gx::PicaMaterialStage)==2*sizeof(gx::PicaMaterialChannel)+4,"material stage members gained padding");
static_assert(materialHeadBytes==sizeof(gx::PicaMaterialPlan::stages)+sizeof(gx::PicaMaterialPlan::textures)+2+
              sizeof(gx::PicaMaterialPlan::bufferConstant)+2+sizeof(gx::PicaMaterialPlan::rasterReadMask)+2+sizeof(gx::RasterCarrier)&&
              offsetof(gx::PicaMaterialPlan,lowering)+sizeof(gx::PicaMaterialPlan::lowering)==sizeof(gx::PicaMaterialPlan),
              "PicaMaterialPlan members changed: update sameMaterial");
bool sameMaterial(const gx::PicaMaterialPlan& a,const gx::PicaMaterialPlan& b){
    return a.lowering==b.lowering&&!std::memcmp(&a,&b,materialHeadBytes);
}
// Two plans with the same combiner structure differ only in constants.
bool sameMaterialStructure(gx::PicaMaterialPlan a,gx::PicaMaterialPlan b){
    for(auto* plan:{&a,&b}){
        for(auto& stage:plan->stages)stage.constant={};
        plan->bufferConstant={};plan->quantizedConstants=false;plan->lowering=0;
    }
    return sameMaterial(a,b);
}
// Rebuild the constants of a recorded material from the live TEV colours and
// fog colour, repeating the recorded draw's own material steps (decode, plan,
// projective routing, vertex fog). Any structural difference refuses; the
// caller then takes the complete path, which records again.
// The complete path's stand-in decision (drawGxCommand) repeated for a
// record on new TEV colours: the planner must refuse (kind 1) or lower without
// room (kind 2) as it did, and the stand-in is built from the same state. The
// record's key fixes everything else the decision reads: the TEV stages, the
// raster (rec.raster, never changed after the decision for records that
// refresh), a perspective projection (no exact I4 bake, the default fallback
// policy) and the fog type; the replay has checked the fog plan.
bool replanStandIn(const NativeShapeRecord& rec,gx::MaterialState& state,const gx::CommandRegisters& r,const CommandDrawContext& context,
                   gx::MaterialPolicy policy,gx::PicaMaterialPlan& material,RecordedStandIn& chosen){
    if(!context.allowDegradedDraws||context.allowVisualFallback||context.diagnosticRasterCarrier||rec.raster.alphaInterval.enabled)return false;
    gx::MaterialPlanError error{};
    const bool planned=gx::planPicaMaterial(state,material,&error,policy);
    std::uint8_t errorIndex=std::uint8_t(gx::MaterialPlanError::stages);
    if(rec.standIn.kind==1){
        if(planned)return false;
        if(gx::discardUnobservedTevAlpha(state,rec.raster)&&gx::planPicaMaterial(state,material,&error,policy))return false;
        if(readsRaster1(state)||!visualMaterialLimit(error))return false;
        errorIndex=std::uint8_t(std::min<unsigned>(unsigned(error),tevStandInByError.size()-1));
    }else if(!planned||!material.lowering||!loweredPlanNoRoom(material,rec.raster,r,context,false))return false;
    const char* why;std::uint8_t maps=0;
    if(!validFallbackInputs(state,r,why,&maps)||maps!=rec.standIn.maps)return false;
    chosen={rec.standIn.kind,maps,errorIndex,simplifiedMaterial(state,rec.raster,material)};
    return true;
}
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
    RecordedStandIn standIn{};
    if(rec.standIn.kind){
        if(!replanStandIn(rec,state,r,context,policy,material,standIn)){++nativeStandInRefusals;return false;}
        ++nativeStandInRefreshes;
    }else if(!gx::planPicaMaterial(state,material,nullptr,policy))return false;
    if(rec.raster.alphaBand&&!gx::appendAlphaBand(material,rec.raster.alphaBandUpper))return false;
    const gx::PicaMaterialPlan colorMaterial=material;
    if(!sameMaterialStructure(colorMaterial,rec.colorMaterial))return false;
    if(!gx::routeProjectiveTexture(material,projectiveMask))return false;
    if(!gx::appendVertexFog(material,fog,rec.fogPrimaryAlpha,rec.fogPrimaryRed))return false;
    if(!sameMaterialStructure(material,rec.material))return false;
    rec.material=material;rec.colorMaterial=colorMaterial;rec.standIn=standIn;return true;
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
// Also announces that nativeReplay places compact palettes (the decoders
// then let a list's first compact batch become a record).
struct NativeListHook {NativeListHook(){gx::staticListEnded=nativeListEnded;gx::compactPaletteRecords=true;}} nativeListHook;
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
void setNativeLeanReplay(bool enabled){
    nativeLeanReplay=enabled;nativeChain.valid=false;nativeProjectionCache.filled=false;nativeFogCache.filled=false;
}
void nativeLeanReplayStatistics(NativeLeanReplayStatistics& out){
    out={nativeLeanPlans,nativeLeanRefusals,nativeFullPlans,nativeRowContinuations,nativeProjectionEpochHits,nativeProjectionEqualHits,
         nativeProjectionPlanned,nativeFogEpochHits,nativeFogEqualHits,nativeFogPlanned};
}
void immediateDrawStatistics(std::uint64_t& hits,std::uint64_t& misses,std::uint64_t& stores,std::uint64_t& fallbacks){
    hits=immediateHits;misses=immediateMisses;stores=immediateStores;fallbacks=immediateFallbacks;
}
void setImmediateDrawRecords(bool enabled){
    immediateEnabled=enabled;immediateKeyMemo.valid=false;
    for(auto& rec:immediateRecords){rec.used=false;rec.repeat.valid=false;}
}
void immediateDrawDetail(ImmediateDrawDetail& detail){
    detail={immediateRepeats,immediateKeyMemoHits,immediateEvictions,immediateMissState,immediateMissExtra,immediateMissColor,immediateZeroAlpha};
}
void setNativeFallbackRecords(bool enabled){
    nativeFallbackRecords=enabled;nativeChain.valid=false;
    if(!enabled)for(auto& rec:nativeShapes)if(rec.valid&&(rec.standIn.kind||rec.raster.alphaRelaxed))rec.valid=false;
}
void setImmediateDrawShortcuts(bool enabled){
    immediateShortcuts=enabled;immediateKeyMemo.valid=false;
    for(auto& rec:immediateRecords)rec.repeat.valid=false;
}
void preparedDrawStatistics(std::uint64_t& hits,std::uint64_t& misses,std::uint64_t& stores,std::uint64_t& fallbacks){
    hits=preparedHits;misses=preparedMisses;stores=preparedStores;fallbacks=preparedFallbacks;
}
// Palette split (rigid plan rejects 45/46/59/60 until 2026-11-16): a lit
// multi-texture draw whose matrices do not fit the palette beside its light
// records is drawn as consecutive runs of its triangles, each run with a
// compact palette of the matrices its triangles use. Triangle order, vertex
// records (one origin for the whole vertex set) and every
// uniform value are those of one draw; only the rows a matrix occupies differ
// between runs. Matrices a run does not use point at the kept first matrix
// (rows 0-5): only vertices of other runs carry them, and no triangle of the
// run references those. SD switch palette-split.txt ('0' = the CPU route).
bool paletteSplitEnabled=true;
constexpr unsigned paletteSplitMaxRuns=16;
std::uint64_t paletteSplitDraws{},paletteSplitRuns{},paletteSplitRefusals{};
namespace {
struct PaletteSplit {unsigned runs{};std::array<std::uint32_t,paletteSplitMaxRuns+1> start{};};
// The plan of one run (one storage; command render owner only).
gx::RigidVertexPlan& paletteSplitPlan(){static gx::RigidVertexPlan plan;return plan;}
// `plan` = `base` restarted as a compact palette (first matrix kept), then the
// matrices of triangles [from,to) placed in order until one does not fit.
// Returns the first triangle not placed.
unsigned placePaletteRun(const gx::CommandDraw& draw,const gx::RigidVertexPlan& base,std::span<const u16> indices,
                         unsigned from,unsigned to,gx::RigidVertexPlan& plan){
    plan=base;gx::restartCompactPalette(plan);
    const unsigned stride=draw.plan.packedStride();
    unsigned t=from;
    for(;t<to;++t){
        bool fits=true;
        for(unsigned k=0;k<3&&fits;++k)fits=gx::addRigidMatrix(draw.registers,draw.vertices[std::size_t(indices[3*t+k])*stride],plan);
        if(!fits)break;
    }
    return t;
}
// Every matrix slot the draw's vertices use that the run left unplaced reads
// the first matrix's rows, so the decoder accepts every vertex.
bool aliasUnplacedMatrices(gx::RigidVertexPlan& plan,unsigned used){
    int first=-1;
    for(unsigned m=0;m<10;++m)if(plan.matrixOffsets[m]==0)first=int(m);
    if(first<0)return false;
    for(unsigned m=0;m<10;++m)if((used&(1u<<m))&&plan.matrixOffsets[m]<0){
        plan.matrixOffsets[m]=plan.matrixOffsets[first];plan.normalOffsets[m]=plan.normalOffsets[first];
    }
    return true;
}
[[gnu::noinline]] bool planPaletteSplit(const gx::CommandDraw& draw,const gx::RigidVertexPlan& base,std::span<const u16> indices,PaletteSplit& split){
    const unsigned triangles=unsigned(indices.size()/3);
    auto& plan=paletteSplitPlan();
    split.runs=0;
    for(unsigned t=0;t<triangles;){
        if(split.runs==paletteSplitMaxRuns)return false;
        const unsigned end=placePaletteRun(draw,base,indices,t,triangles,plan);
        if(end==t)return false; // one triangle alone does not fit
        split.start[split.runs++]=t;t=end;
    }
    split.start[split.runs]=triangles;
    // One run: the triangles fit; only unreferenced vertices overflowed.
    return split.runs>0;
}
// 1: every run submitted; 0: nothing submitted, `reservation` holds the
// complete topology still (the CPU route takes the draw); -1: a later run
// failed after the first was drawn (`why` set).
[[gnu::noinline]] int drawPaletteSplit(const gx::CommandDraw& draw,const gx::RasterPlan& raster,const gx::PicaMaterialPlan& material,
                                       const gx::RigidVertexPlan& base,gx::GpuGeometryBatch& reservation,const char*& why){
    const unsigned count=draw.vertexCount,stride=draw.plan.packedStride();
    PaletteSplit split;
    if(!planPaletteSplit(draw,base,reservation.indices,split)){++paletteSplitRefusals;return 0;}
    unsigned used=0;
    for(unsigned k=0;k<count;++k){const unsigned m=draw.vertices[std::size_t(k)*stride];if(m<=27&&m%3==0)used|=1u<<(m/3);}
    auto& plan=paletteSplitPlan();
    // Runs decode without the static cache: its key holds the palette layout,
    // not the run, and a list keeps one resident set per batch.
    auto decode=[&](gx::GpuGeometryBatch& batch){
        const auto hint=gx::staticGeometryHint;gx::staticGeometryHint.list=nullptr;
        const bool okay=gx::decodeCompactRigidBatchMT(draw.plan,draw.vertices,draw.arrays,batch.vertices,plan);
        gx::staticGeometryHint=hint;
        return okay&&!plan.staticVertices;
    };
    // Each run changes only the two matrix row selectors. The indirect warp
    // samples original attributes and its map, independently of those rows.
    // Keep the first run's records across submissions, which can recycle the
    // GPU arena. A mutable warp map keeps its original full decode.
    const auto recordBytes=base.recordBytes();
    const auto matrixAt=base.wideRecord?offsetof(gx::CompactRigidVertexMTWide,matrix):
                                       offsetof(gx::CompactRigidVertexMT,matrix);
    alignas(4) static std::array<std::byte,24*1024> splitRecords;
    const bool reuse=split.runs>1&&(!base.vertexWarp.enabled||base.vertexWarp.constantMap)&&
                     count<=splitRecords.size()/recordBytes;
    // Run 0: the triangles are the reservation's first ones.
    if(placePaletteRun(draw,base,reservation.indices,0,split.start[1],plan)!=split.start[1]||!aliasUnplacedMatrices(plan,used)||
       !decode(reservation)){++paletteSplitRefusals;return 0;}
    const auto origin=plan.origin;
    const auto normalScale=plan.decodedNormalScale;
    const std::array texScales{plan.decodedTex0Scale,plan.decodedTex1Scale,plan.decodedTex2Scale};
    if(reuse)std::memcpy(splitRecords.data(),reservation.vertices.data(),count*recordBytes);
    auto repack=[&](gx::GpuGeometryBatch& batch){
        if(!reuse)return decode(batch);
        auto* record=reinterpret_cast<std::byte*>(batch.vertices.data());
        std::memcpy(record,splitRecords.data(),count*recordBytes);
        // Run 0 already checked each PNMTXIDX and paired texture selector.
        // The new palette has an offset for every one of those matrices.
        std::array<std::array<std::byte,2>,28> rows;
        for(unsigned m=0;m<10;++m)
            rows[m*3]={std::byte(plan.matrixOffsets[m]),std::byte(plan.normalOffsets[m])};
        const auto* selector=draw.vertices.data();
        for(unsigned i=0;i<count;++i,record+=recordBytes,selector+=stride)
            std::memcpy(record+matrixAt,rows[*selector].data(),2);
        plan.origin=origin;
        plan.decodedNormalScale=normalScale;
        plan.decodedTex0Scale=texScales[0];
        plan.decodedTex1Scale=texScales[1];
        plan.decodedTex2Scale=texScales[2];
        return gx::finishCompactRigidUniforms(plan,{1.f,1.f});
    };
    if(!shrinkGxGeometry(reservation,count,std::size_t(split.start[1])*3)){++paletteSplitRefusals;return 0;}
    // A failed submission leaves its reservation pending: cancelled, as the
    // single draw's cancel() does; the draw is refused either way.
    if(!submitGxRigidGeometry(raster,material,reservation,plan)){why=gxPicaFailure();cancelGxGeometry(reservation);return -1;}
    for(unsigned run=1;run<split.runs;++run){
        const unsigned from=split.start[run],to=split.start[run+1];
        gx::GpuGeometryBatch batch;
        if(!reserveGxGeometry(count,std::size_t(to-from)*3,batch)){why=gxGeometryFailure();return -1;}
        batch.qPolicy=gx::GeometryQPolicy::positiveOnly;
        // The run's triangles, regenerated from the draw as the reservation's were.
        unsigned triangle=0;std::size_t at=0;
        auto take=[&](u16 a,u16 b,u16 c){
            if(triangle>=from&&triangle<to&&at+3<=batch.indices.size()){batch.indices[at++]=a;batch.indices[at++]=b;batch.indices[at++]=c;}
            ++triangle;
        };
        bool built=true;
        if(draw.indexCount){
            for(unsigned i=0;i+2<draw.indexCount;i+=3){u16 t[3];std::memcpy(t,draw.indices.data()+std::size_t(i)*sizeof(u16),sizeof(t));take(t[0],t[1],t[2]);}
        }else built=aurora::gx::dl::expand_triangles(draw.primitive,u16(count),take);
        if(!built||at!=batch.indices.size()||placePaletteRun(draw,base,batch.indices,0,to-from,plan)!=to-from||
           !aliasUnplacedMatrices(plan,used)||!repack(batch)){
            cancelGxGeometry(batch);why="GX palette split run invalid";return -1;
        }
        if(!submitGxRigidGeometry(raster,material,batch,plan)){why=gxPicaFailure();cancelGxGeometry(batch);return -1;}
    }
    ++paletteSplitDraws;paletteSplitRuns+=split.runs;
    return 1;
}
}
void setPaletteSplit(bool enabled){paletteSplitEnabled=enabled;}
void setCompactSingleAttribute(bool enabled){gx::gxCompactSingleAttribute=enabled;}
void drawGapStatistics(DrawGapStatistics& out){
    out={nativeFallbackStored,nativeStandInReplays,nativeRelaxedAlphaReplays,nativeStandInRefreshes,nativeStandInRefusals,
         paletteSplitDraws,paletteSplitRuns,paletteSplitRefusals};
}
namespace {
std::uint64_t drawGapsReportTick{};
std::uint64_t drawGapsReportSum=~std::uint64_t(0);
// Once per profiled frame when a count moved (as reportDegradedReasons):
// the 2026-11-16 draw-gap paths, cumulative.
void reportDrawGaps(bool profiledFrame){
    constexpr std::uint64_t gapTicks=268111856/4; // ARM11 system ticks
    if(!profiledFrame)return;
    const auto now=gxProfileTick();
    if(drawGapsReportTick&&now-drawGapsReportTick<gapTicks)return;
    drawGapsReportTick=now;
    const std::uint64_t sum=nativeFallbackStored+nativeStandInReplays+nativeRelaxedAlphaReplays+nativeStandInRefreshes+nativeStandInRefusals+
        paletteSplitDraws+paletteSplitRuns+paletteSplitRefusals+gx::gxMissingColorVertices+gx::gxMissingColorDecodes+gx::gxCompactSingleAttributeDecodes+
        gx::gxCoarseTexcoordDecodes+gx::gxUnreadAttributeDecodes+gx::gxSwappedArrayDecodes+gx::gxWideRecordPlans;
    if(sum==drawGapsReportSum)return;
    drawGapsReportSum=sum;
#if defined(__3DS__)
    OSReport("Native fallback records: stored=%llu replays stand-in=%llu relaxed-alpha=%llu; stand-in refreshes=%llu refused=%llu\n",
             nativeFallbackStored,nativeStandInReplays,nativeRelaxedAlphaReplays,nativeStandInRefreshes,nativeStandInRefusals);
    OSReport("Native draw gaps: palette split draws=%llu runs=%llu refused=%llu; missing vertex colour gpu-decodes=%llu cpu-vertices=%llu; "
             "single-attribute decodes=%llu; coarser texture coordinates decodes=%llu; unread attributes skipped=%llu; "
             "arrays in the other byte order decodes=%llu; three coordinate sets plans=%llu\n",paletteSplitDraws,
             paletteSplitRuns,paletteSplitRefusals,gx::gxMissingColorDecodes,gx::gxMissingColorVertices,gx::gxCompactSingleAttributeDecodes,
             gx::gxCoarseTexcoordDecodes,gx::gxUnreadAttributeDecodes,gx::gxSwappedArrayDecodes,gx::gxWideRecordPlans);
#endif
}
}
std::uint64_t clipDisabledGpuDraws;
// Triangle-fog draws kept on the GPU rigid path, and the one-shot switch a
// draw uses to restart itself on the CPU form.
std::uint64_t rigidTriangleFogDraws;
bool rigidTriangleFogSuppressed;
// One-shot switch: translate the draw again without its vertex warp (see
// DrawVisualFallback::indirectDivisor).
bool vertexIndirectSuppressed;
// One-shot switch: translate the draw again with its per-vertex warp when its
// constant-offset plan (gx_indirect_gpu.hpp) did not end on the GPU route.
bool indirectConstantSuppressed;
// Fog-overlay meshes on the GPU: the terrain materials that need it carry
// three textures with a projected one, which the multi-texture shader plans
// since 2026-11-14 (title: 2035 vertices a frame left the CPU vertex route).
// SD switch rigid-triangle-fog.txt (0 = the CPU form).
bool rigidTriangleFogEnabled=true;
void setRigidTriangleFog(bool enabled){rigidTriangleFogEnabled=enabled;}
std::array<std::uint64_t,8> rigidTriangleFogGate;
std::uint64_t rigidTriangleFogStatistics(std::array<std::uint64_t,8>* gates){if(gates)*gates=rigidTriangleFogGate;return rigidTriangleFogDraws;}
#if defined(__3DS__)
// Each kind of warped draw the constant-offset plan takes, once (the first 24
// kinds of a session): the texgens, the warp map and the matrix a per-vertex
// warp on the GPU route has to carry. Matrix values are not part of the kind
// (a material may animate them).
static void reportWarpKind(const gx::CommandDraw& draw,const gx::MaterialState& original,const gx::PicaMaterialPlan& material,
                           unsigned units,const gx::VertexIndirectPlan& indirect){
    static std::array<std::uint32_t,24> kinds;static unsigned count;
    if(count>=kinds.size())return;
    const auto& r=draw.registers;
    std::uint32_t kind=2166136261u;
    auto mix=[&](std::uint32_t v){kind=(kind^v)*16777619u;};
    auto matrixIndex=[&](unsigned c){return c<4?(r.matrixIndex[0]>>(6+6*c))&63:c<8?(r.matrixIndex[1]>>(6*(c-4)))&63:63u;};
    std::array<int,3> stages{-1,-1,-1};
    for(unsigned unit=0;unit<units&&unit<3;++unit){
        const auto binding=material.textures[unit];
        mix(binding.map);mix(binding.coordinate);
        if(binding.coordinate<=7){mix(r.xf[0x1040+binding.coordinate]);mix(r.xf[0x1050+binding.coordinate]);mix(matrixIndex(binding.coordinate)==60);}
        for(unsigned stage=0;stage<original.stageCount;++stage){
            const auto& s=original.stages[stage];
            if(s.textureEnabled&&s.texture==binding.map&&s.coordinate==binding.coordinate&&indirect.stages[stage].enabled){stages[unit]=int(stage);break;}
        }
        if(stages[unit]<0)continue;
        const auto& e=indirect.stages[stages[unit]];
        mix(e.state);mix(e.coordinate);mix(e.source.format);mix(e.source.width);mix(e.source.height);mix(e.bias);
        if(e.coordinate<=7){mix(r.xf[0x1040+e.coordinate]);mix(r.xf[0x1050+e.coordinate]);mix(matrixIndex(e.coordinate)==60);}
    }
    kind|=1;
    for(unsigned n=0;n<count;++n)if(kinds[n]==kind)return;
    kinds[count++]=kind;
    char format[76];draw.plan.describe(format,sizeof(format));
    OSReport("Native warp kind %u: units=%u vertices=%u format=%s\n",count,units,unsigned(draw.vertexCount),format);
    for(unsigned unit=0;unit<units&&unit<3;++unit){
        const auto binding=material.textures[unit];const unsigned c=binding.coordinate;
        const unsigned offset=(binding.map/4)*32+binding.map%4;
        const std::uint32_t image=binding.map<=7&&r.bpValid[0x88+offset]?r.bp[0x88+offset]:0;
        // stage: the TEV stage whose warp the unit takes, -1 without one.
        OSReport("Native warp kind %u unit %u: map=%u coord=%u gen=%08lx post=%08lx matrix=%u dest=%ux%u stage=%d\n",count,unit,
            unsigned(binding.map),c,static_cast<unsigned long>(c<=7?r.xf[0x1040+c]:0),static_cast<unsigned long>(c<=7?r.xf[0x1050+c]:0),
            matrixIndex(c),unsigned(image&1023)+1,unsigned((image>>10)&1023)+1,stages[unit]);
        if(stages[unit]<0)continue;
        const auto& e=indirect.stages[stages[unit]];const auto& t=e.source;const unsigned sc=e.coordinate;
        OSReport("Native warp kind %u unit %u map: coord=%u gen=%08lx post=%08lx matrix=%u image=%ux%u format=%u wrap=%u,%u filter=%u,%u levels=%u lod=%.2f "
            "source-scale=%.6f,%.6f base-scale=%.0f,%.0f bias=%u state=%08lx\n",count,unit,sc,
            static_cast<unsigned long>(sc<=7?r.xf[0x1040+sc]:0),static_cast<unsigned long>(sc<=7?r.xf[0x1050+sc]:0),matrixIndex(sc),
            unsigned(t.width),unsigned(t.height),unsigned(t.format),unsigned(t.wrapS),unsigned(t.wrapT),unsigned(t.minFilter),unsigned(t.magFilter),
            unsigned(t.mipCount),double(gx::indirectVertexLod(t)),double(e.sourceScale[0]),double(e.sourceScale[1]),
            double(e.baseScale[0]),double(e.baseScale[1]),e.bias,static_cast<unsigned long>(e.state));
        OSReport("Native warp kind %u unit %u matrix: %.6f %.6f %.6f / %.6f %.6f %.6f\n",count,unit,
            double(e.matrix[0][0]),double(e.matrix[0][1]),double(e.matrix[0][2]),double(e.matrix[1][0]),double(e.matrix[1][1]),double(e.matrix[1][2]));
    }
}
// A draw whose material reads the second rasterized colour (lookup carrier:
// the CPU vertex route computes it per vertex): what that channel is made
// of, once per kind, for the first eight kinds. A channel that is a constant
// or a plain vertex colour needs no lighting and could stay on the GPU.
static void reportLookupCarrier(const gx::CommandDraw& draw){
    static std::array<std::uint32_t,8> kinds;static unsigned count;
    const auto& x=draw.registers;
    std::uint32_t kind=2166136261u;
    for(const unsigned at:{0x1009u,0x100eu,0x100fu,0x1010u,0x1011u})kind=(kind^(x.xfValid[at]?x.xf[at]:0xffffffffu))*16777619u;
    for(unsigned n=0;n<count;++n)if(kinds[n]==kind)return;
    if(count>=kinds.size())return;
    kinds[count++]=kind;
    gx::DecodedVertex first;const bool decoded=draw.vertex(0,first);
    OSReport("Native lookup carrier draw: vertices=%u channels=%08lx colour0=%08lx colour1=%08lx alpha0=%08lx alpha1=%08lx ambient1=%08lx "
        "material1=%08lx present=%08lx format%s\n",unsigned(draw.vertexCount),static_cast<unsigned long>(x.xf[0x1009]),
        static_cast<unsigned long>(x.xf[0x100e]),static_cast<unsigned long>(x.xf[0x100f]),static_cast<unsigned long>(x.xf[0x1010]),
        static_cast<unsigned long>(x.xf[0x1011]),static_cast<unsigned long>(x.xf[0x100b]),static_cast<unsigned long>(x.xf[0x100d]),
        static_cast<unsigned long>(decoded?first.present:0),[&]{static char text[120];draw.plan.describe(text,sizeof(text));return text;}());
}
// A draw the multi-texture plan accepted and its vertex decoder refused (plan
// reject 63, then the CPU vertex route): the decoder's code and the vertex
// format, for the first eight such draws.
static unsigned multiDecodeReports;
static void reportMultiDecodeRefusal(const gx::CommandDraw& draw,unsigned count,const std::array<std::uint32_t,64>& before){
    char text[240];
    // Codes and vertex format first: a kind of refusal is reported once.
    int used=std::snprintf(text,sizeof(text),"codes");
    for(unsigned code=0;code<64&&used>0&&std::size_t(used)+8<sizeof(text);++code)
        if(gx::gxCompactDecodeRejects[code]!=before[code])used+=std::snprintf(text+used,sizeof(text)-used," %u",code);
    if(used>0&&std::size_t(used)+14<sizeof(text))used+=std::snprintf(text+used,sizeof(text)-used," attributes");
    if(used>0&&std::size_t(used)<sizeof(text))used+=draw.plan.describe(text+used,sizeof(text)-used);
    std::uint32_t kind=2166136261u;
    for(int i=0;i<used;++i)kind=(kind^std::uint8_t(text[i]))*16777619u;
    static std::array<std::uint32_t,12> kinds;
    for(unsigned n=0;n<multiDecodeReports;++n)if(kinds[n]==kind)return;
    kinds[multiDecodeReports++]=kind;
    const auto& x=draw.registers;
    OSReport("Native multi-texture decode refused: vertices=%u stride=%u primitive=%02x %s; texgen0=%08lx,%08lx index=%08lx\n",count,
        unsigned(draw.plan.packedStride()),unsigned(draw.primitive),text,static_cast<unsigned long>(x.xf[0x1040]),
        static_cast<unsigned long>(x.xf[0x1050]),static_cast<unsigned long>(x.matrixIndex[0]));
}
// A draw kept off the multi-texture GPU route by a per-vertex texture matrix
// selector (plan reject 2): what its selectors hold, for the first eight such
// draws. A selector that is the same in every vertex, or that follows the
// position selector (J3D loads texture matrix k beside position matrix k:
// selector = position selector + 30), could stay on the GPU.
static void reportTextureMatrixSelectors(const gx::CommandDraw& draw,const gx::PicaMaterialPlan& material,unsigned units,unsigned count){
    static unsigned reports;
    if(reports>=8)return;
    ++reports;
    for(unsigned u=0;u<units&&u<3;++u){
        const unsigned c=material.textures[u].coordinate;
        if(c>7)continue;
        unsigned low=255,high=0,positionLow=255,positionHigh=0,paired=0,seen=0;u32 present=0;
        for(unsigned i=0;i<count;++i){
            gx::DecodedVertex v;if(!draw.vertex(i,v))break;
            present=v.present;
            if(!(v.present&(1u<<(GX_VA_TEX0MTXIDX+c))))break;
            const unsigned t=v.matrix[1+c],q=v.present&1?v.matrix[0]:0;
            low=std::min(low,t);high=std::max(high,t);positionLow=std::min(positionLow,q);positionHigh=std::max(positionHigh,q);
            paired+=t==q+30;++seen;
        }
        if(!seen)continue;
        OSReport("Native texture matrix selector refused: unit=%u coordinate=%u selectors=%u..%u position selectors=%u..%u paired=%u of %u vertices=%u "
            "gen=%08lx post=%08lx present=%08lx index=%08lx,%08lx\n",u,c,low,high,positionLow,positionHigh,paired,seen,count,
            static_cast<unsigned long>(draw.registers.xf[0x1040+c]),static_cast<unsigned long>(draw.registers.xf[0x1050+c]),static_cast<unsigned long>(present),
            static_cast<unsigned long>(draw.registers.matrixIndex[0]),static_cast<unsigned long>(draw.registers.matrixIndex[1]));
    }
}
#endif
bool drawGxCommand(const gx::CommandDraw& draw,void* ptr){
    // Before anything else: the second-core worker may still be drawing the
    // shapes issued before this draw (gx_worker.hpp). Everything below reads
    // tables the worker owns until then.
    gxworker::sync(gxworker::SyncCause::draw);
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
    ImmediateRecord* immediateSlot=nullptr;PreparedKey immediateKey{};ImmediateExtra immediateExtraKey{};bool immediateCaptured=false;
    if(immediateEnabled&&context.allowRigidGpu&&!gx::staticGeometryHint.list&&!draw.indexCount&&!gxVertexLayoutTracing&&
       (draw.primitive==GX_QUADS||draw.primitive==GX_TRIANGLES||draw.primitive==GX_TRIANGLESTRIP||draw.primitive==GX_TRIANGLEFAN)){
        reportImmediateDetail(classFilter.saved);
        ImmediateLaps laps(gxSparseVertexProfiling);
        immediateLookupKey(draw,immediateKey,immediateExtraKey);
        auto* rec=immediateFind(immediateKey,immediateExtraKey.combined);
        laps.lap(immediateProfiles().key);
        immediateFogVerifyRecord=nullptr;
        // Verify: a fogged record is checked against a complete translation.
        if(rec&&rec->fogged&&ImmediateFog(immediateFogMode.load(std::memory_order_relaxed))!=ImmediateFog::on){
            if(ImmediateFog(immediateFogMode.load(std::memory_order_relaxed))==ImmediateFog::verify)immediateFogVerifyRecord=rec;
            rec->used=ImmediateFog(immediateFogMode.load(std::memory_order_relaxed))==ImmediateFog::verify;
            rec=nullptr;
        }
        if(rec){
            const int result=drawImmediate(*rec,draw,context,laps);
            if(result>0){laps.stop(true);++immediateHits;rec->stamp=gx::textureFrameCounter;drawClass.cls=1;return true;}
            if(result<0)return false;
            rec->used=false;rec->repeat.valid=false;++immediateFallbacks;
        }else immediateMiss(immediateKey,immediateExtraKey);
        immediateSlot=&immediateClaim(immediateKey,immediateExtraKey);
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
    // A batch of a merged skin list that the skin route cannot draw: nothing
    // of it is drawn here (no CPU vertex route: its matrix bytes name palette
    // entries); the caller draws the list's groups.
    auto skinRefuse=[&](gx::SkinRefusal why){
        ++skinDrawState.refusals[unsigned(why)];skinDrawState.refused=true;
        if(auto* header=skin::headerOf(gx::staticGeometryHint.list,gx::staticGeometryHint.generation))
            header->refusals.fetch_add(1,std::memory_order_relaxed);
        return true;
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
    // Warp maps in the formats admitted in Alpha 7, counted at submission.
    const auto indirectFormat=gx::indirectFormatNote(indirect);
    gx::PicaMaterialPlan material;gx::MaterialPlanError planError;
    gx::RasterPlan fogRaster;
    gx::PicaMaterialPlan fogMaterial;
    std::optional<gx::PicaMaterialPlan> additiveTexture;
    bool carrier=false,lookupCarrier=false,exactI4Pending=false;
    RecordedStandIn standIn; // how the TEV stand-in below was chosen (records keep it)
    const char* exactI4OriginalFailure="GX exact I4 material unsupported";
    { static const auto index=registerGxNamedProfile("adapter.materialPlan");GxNamedProfileScope detail(index);
    const auto policy=context.allowQuantizedConstants&&!raster.alphaInterval.enabled?gx::MaterialPolicy::quantizedConstants:gx::MaterialPolicy::exactConstants;
    // The opt-in exact I4 bake keeps precedence over the compare lowering,
    // whose quarter konst (64/255) can pick the neighbouring palette shade.
    const bool planned=gx::planPicaMaterial(state,material,&planError,policy);
    const bool bakeFirst=planned&&context.allowExactI4Bake&&(material.lowering&gx::MaterialLowering::compare);
    gx::PicaMaterialPlan lowered;
    if(bakeFirst){lowered=material;planError=gx::MaterialPlanError::operation;}
    // The CPU passes do not retain a native model-shadow caster. Keep those
    // draws on their existing route.
    if(!planned&&planError==gx::MaterialPlanError::textureUnits&&!expanded&&context.allowTriangleFog&&
       !gxShadowCasterSlot()&&
       draw.registers.xfValid[0x1026]&&draw.registers.xf[0x1026]==0&&
       gx::planTriangleFogPass(raster,fogRaster,fogMaterial)){
        const auto coordinate=state.stages[state.stageCount-1].coordinate;
        // The extra pass carries a direct 2x4 texgen (its warp source can
        // still be projected). Other shapes keep their existing planner.
        const bool direct=coordinate<8&&draw.registers.xfValid[0x1040+coordinate]&&
            !(draw.registers.xf[0x1040+coordinate]&0x72);
        if(direct&&!gx::planAdditiveTextureSplit(state,material,additiveTexture.emplace(),policy))additiveTexture.reset();
    }
    if((!planned&&!additiveTexture)||bakeFirst){
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
        }else if(bakeFirst){
            material=lowered; // No exact bake shape: keep the compare plan.
        // The explicit diagnostic retains its original conservative input
        // contract. Automatic lookup must not preempt ordinary alpha liveness.
        }else if(context.diagnosticRasterCarrier||!gx::discardUnobservedTevAlpha(state,raster)||
                 !gx::planPicaMaterial(state,material,&planError,policy)){
        const bool raster1Read=readsRaster1(state);
        if(raster1Read&&!context.diagnosticRasterCarrier){
            carrierAttempt=true;++context.rasterCarrierAttempts;
            // Liveness may remove alpha, not the lookup's original indirect veto.
            if(originalMaterial.indirectCount)return fail(gx::materialPlanErrorText(gx::MaterialPlanError::indirect));
            if(!gx::planRasterLookupMaterial(state,material,&planError,policy))return fail(gx::materialPlanErrorText(planError));
            carrier=lookupCarrier=true;
#if defined(__3DS__)
            reportLookupCarrier(draw);
#endif
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
                if(!validFallbackInputs(state,draw.registers,why,&standIn.maps))return fail(why,DrawFailureKind::resource);
                standIn.multiply=simplifiedMaterial(state,raster,material);
                if(standIn.multiply)++multiplyStandIns;
                simplified[unsigned(DrawVisualFallback::tevMaterial)]=true;
                standIn.kind=1;standIn.error=std::uint8_t(std::min<unsigned>(unsigned(planError),tevStandInByError.size()-1));
                ++tevStandInByError[standIn.error];
            }
        }
    }
    }
    } // Original planner fallback; exact I4 proof is separate.
    // A plan that needs a lowering rule (gx::MaterialLowering) took the
    // one-stage stand-in in Alpha 6, which always left room for the band
    // alpha test and the vertex fog stage. Where this plan leaves none and
    // the fog overlay pass cannot take the draw (blended), keep that stand-in
    // instead of rejecting the draw below (fogged MA09 water with K1 below 191).
    if(material.lowering&&!additiveTexture&&!carrier&&!exactI4Pending&&!raster.alphaInterval.enabled&&
       !simplified[unsigned(DrawVisualFallback::tevMaterial)]){
        const bool noRoom=loweredPlanNoRoom(material,raster,draw.registers,context,expanded);
        const char* why;std::uint8_t maps=0;
        if(noRoom&&allowsFallback(DrawVisualFallback::tevMaterial)&&validFallbackInputs(state,draw.registers,why,&maps)){
            const bool multiply=simplifiedMaterial(state,raster,material);
            if(multiply)++multiplyStandIns;
            simplified[unsigned(DrawVisualFallback::tevMaterial)]=true;
            standIn={2,maps,std::uint8_t(gx::MaterialPlanError::stages),multiply};
            ++tevStandInByError[standIn.error];
        }
    }
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
    // Warps that the SD switch indirect-constant.txt admits become constant
    // texture offsets, so the draw can take the multi-texture GPU route
    // (gx_indirect_gpu.hpp; exact plans only by default). A draw that does
    // not end there is translated again with its per-vertex warp.
    gx::IndirectConstantPlan indirectConstant;
    // What each warped unit's per-vertex part is made of: written by the
    // constant plan below and read by planVertexWarp for the same draw
    // (one render thread; static so that no draw pays for clearing it).
    static gx::IndirectWarpSources warpSources;
    if(indirect.enabled&&!additiveTexture&&!indirectConstantSuppressed&&context.allowRigidGpu&&!carrier&&!exactI4Pending&&!expanded&&
       !raster.alphaUnion&&!raster.alphaInterval.enabled&&
       gx::planIndirectConstant(draw.registers,originalMaterial,material,material.sourceTextures(),indirect,
           allowsFallback(DrawVisualFallback::sharedIndirect),context.allowMultiProjected&&context.allowProjectionMode,indirectConstant,
           &warpSources)){
#if defined(__3DS__)
        reportWarpKind(draw,originalMaterial,material,material.sourceTextures(),indirect);
#endif
        indirect={};
        if(indirectConstant.sharedPlain)simplified[unsigned(DrawVisualFallback::sharedIndirect)]=true;
    }
    unsigned originalTextureCount=material.sourceTextures();
    std::uint8_t coordinates=indirect.coordinates;
    std::array<gx::Texture,3> textures{};
    gx::Texture additionalImage{};
    for(unsigned unit=0;unit<originalTextureCount;++unit)coordinates|=1u<<material.textures[unit].coordinate;
    if(additiveTexture)coordinates|=1u<<additiveTexture->textures[0].coordinate;
    gx::VertexTransformPlan transform;gx::VertexTransformError transformError;
    { static const auto index=registerGxNamedProfile("adapter.transformPlan");GxNamedProfileScope detail(index);
    if(!gx::planVertexTransform(draw.registers,coordinates,transform,&transformError))return fail(gx::vertexTransformErrorText(transformError));
    }
    if(exactI4Pending&&transform.projectiveMask())return fail(exactI4OriginalFailure);
    if(carrier&&!lookupCarrier&&transform.projectiveMask())return fail("GX diagnostic raster carrier projective unsupported");
    const bool needsProjectorPair=!gx::routeProjectiveTexture(material,transform.projectiveMask());
    if(needsProjectorPair&&(originalTextureCount!=3||!context.allowProjectionMode||carrier||exactI4Pending||expanded||additiveTexture||
       originalMaterial.indirectCount||indirect.enabled||indirectConstant.enabled||raster.alphaInterval.enabled||
       raster.clipDisabled||simplified[unsigned(DrawVisualFallback::tevMaterial)]))
        return fail("GX multiple projected textures pending");
    std::optional<gx::ScreenProjectors> screenProjectors;
    std::array<gx::Texture,2> projectorTextures{};
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
    if(needsProjectorPair){
        if(!gx::planScreenProjectors(draw.registers,transform,textures,material,screenProjectors.emplace()))
            return fail("GX multiple projected textures pending");
        for(unsigned i=0;i<2;++i)projectorTextures[i]=textures[screenProjectors->units[i]];
        textures[1]=textures[screenProjectors->baseUnit];
        textures[0]={};textures[2]={};
        originalTextureCount=2;
    }
    if(additiveTexture){
        const char* textureFailure;
        if(!gx::resolveCommandTexture(draw.registers,additiveTexture->textures[0].map,additionalImage,textureFailure))
            return fail(textureFailure,DrawFailureKind::resource);
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
    bool triangleFog=additiveTexture.has_value();
    unsigned fogTextureUnit=0;
    if(context.allowVertexFog) {
        if(carrier&&!gx::currentTargetExtent.valid())return fail("GX diagnostic raster carrier target invalid");
        if(!gx::planVertexFog(draw.registers,fog))return fail("GX vertex fog parameters invalid");
        // A fog curve other than the linear one: the GPU vertex route makes
        // the linear coordinate only, so a factor carried in the primary
        // alpha needed the CPU vertex route for its curve (plan reject 20;
        // 17 microseconds a vertex: two or three draws a picture in Faron
        // Woods and Ordon Spring, reverse exponential fog on unlit materials
        // of two textures). With a texture unit free the ramp applies the
        // curve per pixel and the draw stays on the GPU, as for the red
        // carrier above. SD switch fog-alpha-ramp-off.txt.
        if(fog.enabled&&fog.function!=2&&fogPrimaryAlpha&&!carrier&&context.preferFogRamp&&context.preferFogRampForAlpha&&
           material.textureCount<3&&!indirect.enabled){fogPrimaryAlpha=false;++context.fogAlphaRampDraws;}
        if(!carrier&&fog.enabled&&!fogPrimaryAlpha&&!fogPrimaryRed&&material.textureCount==3&&!(colorPlan.lit&2)){
            // Only a constant alpha across the complete original draw can be
            // moved to TEV constants. Varying or lit alpha retains its input.
            float alpha=colorPlan.material[3];bool constant=true;
            if(colorPlan.vertex&2){
                gx::DecodedVertex vertex;
                for(unsigned i=0;i<draw.vertexCount;++i){
                    if(!draw.vertex(i,vertex)){constant=false;break;}
                    // The alpha the lighting evaluation reads (white without CLR0).
                    const float vertexAlpha=gx::rasterVertexColor(vertex,0)[3];
                    if(!i)alpha=vertexAlpha;
                    if(vertexAlpha!=alpha){constant=false;break;}
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
        if(!triangleFog&&!gx::appendVertexFog(material,fog,fogPrimaryAlpha,fogPrimaryRed)){
            if((carrier&&!lookupCarrier)||!context.allowTriangleFog||expanded||!gx::planTriangleFogPass(raster,fogRaster,fogMaterial))
                return fail("GX vertex fog TEV/texture capacity");
            triangleFog=true;
        }
        if(fog.enabled&&!triangleFog&&!fogPrimaryAlpha&&!fogPrimaryRed&&!fogRampTexture(textures[fogTextureUnit],fog.function))return fail("GX vertex fog ramp unavailable",carrier?DrawFailureKind::resource:DrawFailureKind::state);
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
    const unsigned firstOrdinaryUnit=screenProjectors?1:0;
    if(screenProjectors){
#if defined(__3DS__)
        if(!prepareGxScreenProjectors(*screenProjectors,projectorTextures,scales[0]))
            return fail(gxPicaFailure(),DrawFailureKind::submission);
#else
        return fail("PICA screen projector backend unavailable");
#endif
    }
    // Optional: a draw that translates itself again ends this scope first.
    std::optional<GxTextureDrawScope> textureScope;
    std::array<float,2> additionalScale{};
    if(additiveTexture){
        const std::array protectedImages{textures[0],textures[1],textures[2],additionalImage};
        textureScope.emplace(protectedImages);
        if(!*textureScope||!bindGxTexture(additionalImage,additionalScale[0],additionalScale[1]))
            return fail(gxTextureFailure(),DrawFailureKind::resource);
    }else textureScope.emplace(std::span<const gx::Texture>{textures}.subspan(firstOrdinaryUnit,material.textureCount-firstOrdinaryUnit));
    if(!*textureScope)return fail(gxTextureFailure(),DrawFailureKind::resource);
    // worldMips: only the material's own textures in their own units; a draw
    // that rearranges or replaces them (projector pair, derived I4 image,
    // diagnostic carrier, added texture, indirect warp) keeps base levels.
    const bool worldDraw=!screenProjectors&&!exactI4Pending&&!carrier&&!additiveTexture&&!indirect.enabled&&
        !originalMaterial.indirectCount&&!indirectConstant.enabled&&
        gx::worldMipDraw(draw.registers,gx::staticGeometryHint.generation&&gx::staticGeometryHint.list);
    for(unsigned unit=firstOrdinaryUnit;unit<material.textureCount;++unit)
        if(!bindGxTexture(textures[unit],scales[unit][0],scales[unit][1],unit,unit==0&&material.projectiveTexture0,
            unit<originalTextureCount?gx::worldMipUse(draw.registers,worldDraw,material.textures[unit].coordinate):GxWorldMipUse{}))
            return fail(gxTextureFailure(),DrawFailureKind::resource);
    const bool signedQDiagnostic=!screenProjectors&&context.allowDirectSignedQDiagnostic&&
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
    // A merged skin list (runtime/skin_palette.hpp): only the multi-texture
    // route with its palette draws it; anything else refuses it whole and the
    // caller draws its groups (skinRefuse).
    const bool skinDraw=skin::mergedGeneration(gx::staticGeometryHint.generation)&&gx::staticGeometryHint.list;
    gx::SkinRefusal skinWhy{};
    bool paletteSplit=false,paletteSplitDone=false; // see drawPaletteSplit
    bool signedQPositiveRigidAttempt=false;
    auto recordSignedQPositiveRigid=[&](SignedQPositiveRigidCounts::Total& total){++total.draws;total.vertices+=count;};
    const bool rigidProjected=originalTextureCount==2&&material.projectiveTexture0&&
        material.textures[0].coordinate==1&&material.textures[1].coordinate==0;
    const auto& rigidScale=scales[rigidProjected?1:0];
    // Alpha-union layers whose blend ignores the fragment order take the rigid
    // GPU routes as two whole-mesh passes (gx_pica.cpp, picaAlphaUnionMesh).
    // Triangle fog never meets one (its pass needs an always-pass alpha test).
    const bool unionGpu=raster.alphaUnion&&!triangleFog&&picaAlphaUnionGpu(raster);
    const unsigned cpuReason=(raster.alphaUnion||raster.alphaInterval.enabled)?0:triangleFog?1:expanded?3:raster.clipDisabled?4:
        indirect.enabled?5:!originalTextureCount?6:(originalTextureCount>1&&!rigidProjected)?7:
        (!rigidProjected&&material.textures[0].coordinate!=0)?8:9;
    midSpan.stop();
    // Orthographic (2D) draws are unlit; never attempt the lit rigid shaders.
    const bool orthographicDraw=draw.registers.xfValid[0x1026]&&draw.registers.xf[0x1026]==1;
    // Constant offsets off the GPU route: translate again with the per-vertex
    // warp. This pass keeps only its own time in the draw-class profile, and
    // the repeat skips the immediate records (an indirect draw is never
    // recorded), so the draw's diagnostics count once.
    auto restartWithWarp=[&]{
        ++gx::indirectConstantCounters.restarts;textureScope.reset();
        const bool immediate=immediateEnabled;immediateEnabled=false;
        const auto nested=drawClass.start?gxProfileTick():0;
        indirectConstantSuppressed=true;const bool restarted=drawGxCommand(draw,ptr);indirectConstantSuppressed=false;
        immediateEnabled=immediate;
        if(drawClass.start)drawClass.start+=gxProfileTick()-nested;
        return restarted;
    };
    { static const auto index=registerGxNamedProfile("adapter.rigidPlan");GxNamedProfileScope detail(index);
    // Triangle fog on the GPU: the draw is planned without fog; the fog
    // overlay pass reuses its geometry and uniform block (see the submit).
    const bool rigidFogCandidate=triangleFog&&!additiveTexture&&rigidTriangleFogEnabled&&!rigidTriangleFogSuppressed&&picaRigidTriangleFogAvailable()&&
        fog.function==2&&!fog.orthographic&&fog.a>=0&&fog.b>1&&!orthographicDraw&&!signedQDiagnostic;
    static const gx::VertexFogPlan noFog{};
    // The shaders write the fog factor to primary red when u[12].z is 2 (the
    // material does not read the vertex colour: its red channel is free).
    const bool fogRedCarrier=fog.enabled&&fogPrimaryRed&&!triangleFog;
    if(!skinDraw&&!screenProjectors&&context.allowSingleRigidShader&&!orthographicDraw&&!exactI4Pending&&!carrier&&(!signedQDiagnostic||context.allowSignedQPositiveRigidGpu)&&context.allowRigidGpu&&(!raster.alphaUnion||unionGpu)&&!raster.alphaInterval.enabled&&(!triangleFog||rigidFogCandidate)&&!expanded&&!raster.clipDisabled&&!indirect.enabled&&!indirectConstant.enabled&&
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
    if(!screenProjectors&&!rigidGpu&&!exactI4Pending&&!carrier&&!signedQDiagnostic&&context.allowRigidGpu&&(!raster.alphaUnion||unionGpu)&&!raster.alphaInterval.enabled&&
       (!triangleFog||rigidFogCandidate)&&!expanded&&(!raster.clipDisabled||!orthographicDraw)&&!indirect.enabled&&
       originalTextureCount<=3&&(!material.projectiveTexture0||(context.allowMultiProjected&&!orthographicDraw))&&(!orthographicDraw||!colorPlan.lit)){
        gx::DecodedVertex first;
        const int fogUnit=!triangleFog&&fog.enabled&&!fogPrimaryAlpha&&!fogPrimaryRed?int(originalTextureCount):-1;
#if defined(__3DS__)
        const auto selectorRejects=gx::gxRigidPlanRejects[2];
#endif
        // A merged list's matrix bytes name palette entries: the planner takes
        // slot 0 as its first matrix and the palette replaces it below.
        if(draw.vertex(0,first)&&(!skinDraw||(first.matrix[0]=0,!triangleFog)))rigidGpu=gx::planRigidMultiVertex(draw.registers,first,transform,colorPlan,triangleFog?noFog:fog,fogPrimaryAlpha,
            gx::currentTargetExtent.logicalWidth,material,originalTextureCount,scales,fogUnit,rigid,context.allowMultiProjected);
#if defined(__3DS__)
        if(gx::gxRigidPlanRejects[2]!=selectorRejects)reportTextureMatrixSelectors(draw,material,originalTextureCount,count);
#endif
        if(rigidGpu&&fogRedCarrier)rigid.uniforms[12][2]=2;
        // Constant warp offsets go into the unit rows; not with the fog
        // overlay pass, which reuses this uniform block.
        if(rigidGpu&&indirectConstant.enabled&&
           (triangleFog||!gx::applyIndirectConstant(rigid,indirectConstant,material,originalTextureCount,scales)))rigidGpu=false;
        // What the warp map adds at each vertex to those constants: the
        // decoder samples it into the records. A draw it cannot serve keeps
        // the constants alone.
        if(rigidGpu&&indirectConstant.enabled)
            gx::planVertexWarp(draw.registers,first,rigid,indirectConstant,warpSources,material,originalTextureCount,scales);
        // Native clip-disabled draws (perspective only, see the gate above):
        // the shader clamps depth into the PICA's range so its mandatory
        // near/far clipping keeps them, as GX with clipping disabled does.
        // Until 2026-11-16 this sat in the triangle-fog branch below, where
        // rigidGpu is always false, so these draws were near/far clipped.
        if(rigidGpu&&raster.clipDisabled){gx::setMultiDepthClamp(rigid);++clipDisabledGpuDraws;}
        rigidTriangleFog=rigidGpu&&triangleFog;
        if(triangleFog&&!rigidGpu&&!indirectConstant.enabled)++rigidTriangleFogGate[7];
    }else if(triangleFog&&!rigidGpu&&!indirectConstant.enabled){
        // Diagnostic: first gate that kept a triangle-fog draw on the CPU.
        ++rigidTriangleFogGate[(rigidTriangleFogSuppressed||!rigidTriangleFogEnabled)?0:!picaRigidTriangleFogAvailable()?1:fog.function!=2?2:
            (fog.orthographic||fog.a<0||fog.b<=1||orthographicDraw)?3:material.projectiveTexture0?4:
            originalTextureCount>3?5:6];
    }
    if(indirectConstant.enabled&&!rigidGpu)return restartWithWarp();
    if(!rigidGpu&&!orthographicDraw){gateCounted=true;
        // Diagnostic: first gate that kept a perspective draw off the GPU paths.
        const unsigned why=exactI4Pending?1:carrier?2:signedQDiagnostic?3:!context.allowRigidGpu?4:((raster.alphaUnion&&!unionGpu)||raster.alphaInterval.enabled)?5:
            triangleFog?6:expanded?8:indirect.enabled?9:originalTextureCount>3?10:rigidProjected?11:
            material.projectiveTexture0?12:raster.clipDisabled?7:13;
        ++gxPerspCpuGate[why];gxPerspCpuGateVertices[why]+=count;
        ++gxPerspCpuGateTotal[why];gxPerspCpuGateVerticesTotal[why]+=count;
    }
    if(skinDraw){
        if(rigidGpu&&(!rigid.indexedMatrices||indirectConstant.enabled||raster.clipDisabled))rigidGpu=false,skinWhy=gx::SkinRefusal::program;
        else if(rigidGpu)rigidGpu=gx::placeSkinPalette(draw.registers,gx::staticGeometryHint.generation,rigid,&skinWhy);
        else skinWhy=gx::SkinRefusal::program;
        // Too many entries for this material's rows: the game thread plans the
        // shape again with what fits here.
        if(!rigidGpu&&skinWhy==gx::SkinRefusal::rows)
            if(auto* header=skin::headerOf(gx::staticGeometryHint.list,gx::staticGeometryHint.generation))
                header->capacity.store(skin::capacity(rigid.paletteLimit()),std::memory_order_relaxed);
        // Nothing reserved or submitted yet.
        if(!rigidGpu)return skinRefuse(skinWhy);
    }else if(rigidGpu&&rigid.indexedMatrices){
        // PNMTXIDX is the first direct byte in a validated GX layout. Gather
        // only matrices actually used by this draw; larger palettes fall back.
        bool placed=true;
        for(unsigned i=0;i<count;++i)if(!gx::addRigidMatrix(draw.registers,draw.vertices[i*stride],rigid)){placed=false;break;}
        if(!placed&&!rigid.projectedTexture){
            gx::restartCompactPalette(rigid);placed=true;
            for(unsigned i=0;i<count;++i)if(!gx::addRigidMatrix(draw.registers,draw.vertices[i*stride],rigid)){placed=false;break;}
        }
        // Palette overflow: runs of triangles with their own palettes (see
        // drawPaletteSplit), planned once the topology is in the reservation.
        if(!placed&&paletteSplitEnabled&&rigid.multiTexture&&!rigid.multiProjected&&!triangleFog&&!signedQPositiveRigidAttempt){
            paletteSplit=true;placed=true;
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
    const bool additiveMesh=additiveTexture&&indexCount<=196605/2&&picaRigidTriangleFogAvailable();
    if(!reserveGxGeometry(cpuTriangleFog?pairedCount*2:outputCount,additiveMesh?indexCount*2:indexCount,reservation))
        return fail(gxGeometryFailure(),DrawFailureKind::geometry);
    reservation.qPolicy=signedQDiagnostic?gx::GeometryQPolicy::directTexture0Diagnostic:gx::GeometryQPolicy::positiveOnly;
    auto batch=reservation;if(cpuTriangleFog)batch.vertices=batch.vertices.first(outputCount);
    if(additiveMesh)batch.indices=batch.indices.first(indexCount);
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
    unsigned negativeSourceMask=0;
    auto transformOne=[&](unsigned i,gx::GpuVertex& target,bool requirePositiveQ=true,bool* invalidDivisor=nullptr,
                          unsigned* allNegativeSources=nullptr)->const char*{
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
        if(screenProjectors){
            if(vertex.clip[3]<=0||!gx::screenProjectorsMatchPosition(draw.registers,transform.projectiveMask(),decoded))
                return "GX paired screen projectors do not share the visible position";
            target.textureQ=vertex.clip[3];
            target.texcoord[0]=gx::screenProjectorUV(vertex.clip,scales[0]);
        }
        bool invalidQ=false,invalidIndirectQ=false;
        for(unsigned unit=firstOrdinaryUnit;unit<originalTextureCount;++unit){
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
                auto source=vertex.texcoord[effect->coordinate];
                for(float value:source)if(!std::isfinite(value))return "GX vertex indirect coordinate invalid";
                const unsigned sourceBit=1u<<effect->coordinate;
                const bool projected=transform.projectiveMask()&sourceBit;
                if(negativeSourceMask&sourceBit){
                    for(float& value:source)value=-value;
                }
                const float sourceQ=projected?source[2]:1;
                if(allNegativeSources&&sourceQ>=0)*allNegativeSources&=~sourceBit;
                if(sourceQ<=0||q==0||(q<0&&!(cameraAlignedProved&&unit==0))){
                    // Defer finite divisors for the whole-draw proofs below.
                    // An unevaluated warp must never reach a visible triangle.
                    invalidIndirectQ=true;
                }else{
                    std::array<float,2> adjusted;
                    if(!gx::evaluateVertexIndirect(*effect,source,uv,
                        projected,unit==0&&material.projectiveTexture0,textures[unit],adjusted,cameraAlignedProved&&unit==0))
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
        if(additiveTexture){
            auto uv=vertex.texcoord[additiveTexture->textures[0].coordinate];
            const auto& warp=indirect.stages[originalMaterial.stageCount-1];
            if(warp.enabled){
                std::array<float,2> warped;
                if(!gx::evaluateVertexIndirect(warp,vertex.texcoord[warp.coordinate],uv,
                    transform.projectiveMask()&(1u<<warp.coordinate),false,additionalImage,warped))
                    return "GX additive texture indirect coordinate invalid";
                uv[0]=warped[0];uv[1]=warped[1];
            }
            if(!std::isfinite(uv[0])||!std::isfinite(uv[1]))return "GX additive texture coordinate invalid";
            auto& extra=reservation.vertices[pairedCount+i];
            extra={};
            extra.texcoord[0]={uv[0]*additionalScale[0],1-uv[1]*additionalScale[1]};
        }
        if(fog.enabled&&!triangleFog) {
            GxProfileScope section(GxProfilePart::vertexFog,sample);
            // A fog unit samples the curve from its ramp (fogRampTexture):
            // its coordinate is the ramp coordinate. A fog factor carried in
            // the vertex colour has the curve applied here.
            const float factor=gx::evaluateVertexFog(fog,vertex.clip,gx::currentTargetExtent.logicalWidth,fogPrimaryAlpha||fogPrimaryRed);
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
    enum class ProjectiveClip { unproved, empty, trimmed, retrySignedSource };
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
        unsigned allNegativeSources=0;
        for(const auto* effect:indirectUnits){
            if(effect)allNegativeSources|=1u<<effect->coordinate;
        }
        allNegativeSources&=transform.projectiveMask()&~negativeSourceMask;
        for(unsigned i=0;i<count;++i){
            if(transformOne(i,batch.vertices[i],false,nullptr,&allNegativeSources))return ProjectiveClip::unproved;
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
        // A source whose Q is strictly negative at every vertex cannot cross
        // zero on a clipped triangle. Negating all three source components
        // preserves S/Q and T/Q. The destination Q keeps its original sign.
        if(allNegativeSources){
            negativeSourceMask|=allNegativeSources;
            return ProjectiveClip::retrySignedSource;
        }
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
                // The first 8 refusals of the session (4 lines each); Alpha 6
                // printed about 160 per Lakebed zone into the SD log.
                static unsigned refusals;
                if(++refusals<=8){
                OSReport("Native projected clip refused %u/8: triangle=%u indices=%u,%u,%u q=%.9g,%.9g,%.9g side-masks=%x,%x,%x\n",
                    refusals,i/3,unsigned(batch.indices[i]),unsigned(batch.indices[i+1]),unsigned(batch.indices[i+2]),
                    double(a.textureQ),double(b.textureQ),double(c.textureQ),maskA,maskB,maskC);
                const gx::GpuVertex* vertices[]{&a,&b,&c};
                for(unsigned n=0;n<3;++n){const auto& p=vertices[n]->position;
                    OSReport("Native projected clip vertex %u: %.9g %.9g %.9g %.9g\n",unsigned(batch.indices[i+n]),
                        double(p[0]),double(p[1]),double(p[2]),double(p[3]));}
                }
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
    auto cpuProjection=[&]{if(!screenProjectors&&material.projectiveTexture0&&originalTextureCount&&context.allowProjectionMode)setGxTextureProjection(true);};
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
        if(paletteSplit){
            const char* why=nullptr;
            const int split=drawPaletteSplit(draw,raster,material,rigid,reservation,why);
            // A later run failed after the first was drawn: never redrawn.
            if(split<0)return fail(why,DrawFailureKind::submission);
            paletteSplitDone=split>0;
            if(!paletteSplitDone){
                // Not expressible as runs: the CPU route, counted as before.
                rigidGpu=false;
                unsigned used=0;for(unsigned k=0;k<count;++k){const unsigned m=draw.vertices[k*stride];if(m<=27&&m%3==0)used|=1u<<(m/3);}
                ++gx::gxRigidPlanRejects[40+std::min(rigid.lights,9u)];++gx::gxRigidPlanRejects[50+std::min(unsigned(__builtin_popcount(used)),10u)];
            }
        }
        // A split draw was decoded and submitted run by run above.
        if(!paletteSplitDone){
            const bool rigidPackingAttempt=rigidGpu;
            bool nativeRigid=false;
            // Immediate draw record: the planned uniform block, before the decode
            // folds this draw's fixed-point texture fractions into it.
            if(immediateSlot&&rigidGpu&&rigid.multiTexture&&!rigid.indexedMatrices&&!rigid.compactPalette&&rigid.matrices==1){
                immediateCapturedPlan()=rigid;immediateCaptured=true;
            }
            if(rigidGpu&&rigid.multiTexture){GxProfileScope section(GxProfilePart::vertexRigid);
#if defined(__3DS__)
                const bool watched=multiDecodeReports<12;std::array<std::uint32_t,64> refusals;
                if(watched)refusals=gx::gxCompactDecodeRejects;
#endif
                nativeRigid=gx::decodeCompactRigidBatchMT(draw.plan,draw.vertices,draw.arrays,batch.vertices,rigid);
                if(!nativeRigid){rigidGpu=false;++gx::gxRigidPlanRejects[63];} // no float fallback layout for the multi-texture shader
#if defined(__3DS__)
                if(!nativeRigid&&watched)reportMultiDecodeRefusal(draw,count,refusals);
#endif
            }else if(rigidGpu){GxProfileScope section(GxProfilePart::vertexRigid);
                nativeRigid=gx::decodeCompactRigidBatch(draw.plan,draw.vertices,draw.arrays,batch.vertices,rigidScale,rigid,context.compactCache);
                if(!nativeRigid&&!rigid.compactPalette)nativeRigid=gx::decodeRigidBatch(draw.plan,draw.vertices,draw.arrays,batch.vertices,rigid.color,rigidScale,rigid.matrixOffsets);}
            if(nativeRigid)gxProfileTotals.rigidDecodedVertices+=count;
            if(rigidGpu&&!nativeRigid)++gx::gxCompactDecodeRejects[60];
            if(rigidGpu&&!nativeRigid)for(unsigned i=0;i<count;++i){
                gx::DecodedVertex decoded;
                // Own code 62 since 2026-11-16 (it shared 60 with "10 matrices").
                if(!draw.vertex(i,decoded)||!gx::packRigidVertex(rigid,decoded,rigidScale,batch.vertices[i])){rigidGpu=false;++gx::gxRigidPlanRejects[62];break;}
            }
            if(signedQPositiveRigidAttempt&&rigidPackingAttempt&&!rigidGpu)recordSignedQPositiveRigid(context.signedQPositiveRigid.packingFailures);
            if(rigidGpu){
                // A warped unit projected through the camera and divided per
                // pixel: vertices behind the camera plane are the PICA's to
                // clip, so their divisors are not examined (Ordon Spring: all
                // 990 water draws of a run cross that plane).
                const bool cameraDivisor=indirectConstant.enabled&&context.allowCameraDivisor&&gx::indirectConstantPerPixel(rigid)&&
                    gx::projectedUnitFollowsCamera(rigid);
                if(cameraDivisor)++gx::indirectConstantCounters.cameraDivisor;
                const bool positive=cameraDivisor||gx::validateRigidProjection(rigid,batch.vertices);
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
                if(clampDivisor){
                    // A warp folded in as a constant offset divides unit 0 per
                    // pixel, which cannot clamp a divisor: this draw divides
                    // per vertex instead (gx_rigid_mt.v.pica, u[92].z), the
                    // offset rows hold for both. Until Alpha 8 such a draw
                    // went back to the CPU route with its warp (Lake Hylia
                    // water, plan reject 61). Only a projector that is not
                    // the camera gets here (cameraDivisor above).
                    if(indirectConstant.enabled&&gx::indirectConstantPerPixel(rigid))rigid.uniforms[gx::multiUnitRow(0)+3][2]=0;
                    simplified[unsigned(DrawVisualFallback::projectiveDivisor)]=true;
                }
                rigidGpu=positive||clampDivisor;if(!rigidGpu)++gx::gxRigidPlanRejects[61];
            }
        }
        if(signedQPositiveRigidAttempt&&!rigidGpu){
            reservation.qPolicy=batch.qPolicy=gx::GeometryQPolicy::directTexture0Diagnostic;
            // The complete CPU loop below resets every GpuVertex, overwriting
            // all speculative raw/compact bytes before scanning/submission.
        }
        if(skinDraw&&!rigidGpu){
            if(!cancelGxGeometry(reservation))return fail("GX geometry cancellation failed",DrawFailureKind::geometry);
            return skinRefuse(gx::SkinRefusal::decode);
        }
        if(rigidTriangleFog&&!rigidGpu){
            // The reservation has no fog half: translate again without the GPU form.
            if(!cancelGxGeometry(reservation))return fail("GX geometry cancellation failed",DrawFailureKind::geometry);
            textureScope.reset();
            rigidTriangleFogSuppressed=true;const bool restarted=drawGxCommand(draw,ptr);rigidTriangleFogSuppressed=false;
            return restarted;
        }
        if(indirectConstant.enabled&&!rigidGpu){
            // Off the GPU route after the reservation (decode, palette or a
            // projector crossing the vertices): translate again with the warp.
            if(!cancelGxGeometry(reservation))return fail("GX geometry cancellation failed",DrawFailureKind::geometry);
            return restartWithWarp();
        }
        // A projected unit 0 is divided per pixel, as on the CPU route.
        if(indirectConstant.enabled&&gx::indirectConstantPerPixel(rigid))setGxTextureProjection(true);
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
                    if(clipped==ProjectiveClip::retrySignedSource){i=unsigned(-1);continue;}
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
    if(!rigidGpu&&!orthographicDraw){
        const unsigned gate=gateCounted?15:14;
        ++gxPerspCpuGate[gate];gxPerspCpuGateVertices[gate]+=count;
        ++gxPerspCpuGateTotal[gate];gxPerspCpuGateVerticesTotal[gate]+=count;
    }
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
    if(paletteSplitDone){
        // Submitted run by run (drawPaletteSplit). A record cannot hold the
        // runs: counted with the batches whose vertices stay in the arena.
        gxProfileTotals.rigidVertices+=count;gxProfileTotals.compactVertices+=count;gxProfileTotals.rigidDecodedVertices+=count;
        if(gx::staticGeometryHint.generation&&gx::staticGeometryHint.list)++nativeRecordOutcome[3];
    }
    else if(rigidTriangleFog){
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
        for(unsigned i=outputCount;i<pairedCount;++i){
            reservation.vertices[i]=batch.vertices[0];
            if(additiveTexture)reservation.vertices[pairedCount+i]=reservation.vertices[pairedCount];
        }
        for(unsigned i=0;i<pairedCount;++i){auto& vertex=reservation.vertices[pairedCount+i];
            if(!additiveTexture)vertex={};
            vertex.position=reservation.vertices[i].position;
            for(unsigned c=0;c<3;++c)vertex.color[c]=float(fog.color[c])/255;
            if(fog.enabled)vertex.color[3]=gx::evaluateVertexFog(fog,vertex.position,gx::currentTargetExtent.logicalWidth);
        }
        const bool submitted=additiveTexture?
            submitGxAdditiveTextureGeometry(raster,material,fogRaster,fog.enabled?&fogMaterial:nullptr,
                                            *additiveTexture,textures[0],additionalImage,reservation,additiveMesh):
            submitGxTriangleFogGeometry(raster,material,fogRaster,fogMaterial,reservation);
        if(!submitted)return cancel(gxPicaFailure(),DrawFailureKind::submission);
        if(fog.enabled)++context.triangleFogDraws;
    }
    else if(rigidGpu){
        // Resident static vertices: keep only the indices in the frame arena.
        if(rigid.staticVertices&&batch.vertices.size()>3){
            if(!shrinkGxGeometry(batch,3,batch.indices.size(),true))return cancel(gxGeometryFailure(),DrawFailureKind::geometry);
            reservation=batch;
        }
        // Compact records decoded into the arena fill 32 or 36 bytes of each
        // 60-byte vertex the reservation holds: the rest is given back. A
        // full arena waits for the GPU in the middle of the picture; a room
        // of warped water (9,000 vertices a picture, 570 KB reserved of the
        // 512 KB arena) did so once per picture, as the grass and flowers of
        // Faron Woods did in Alpha 8. SD switch arena-shrink-off.txt.
        else if(context.shrinkCompactReservations&&rigid.compact&&rigid.multiTexture&&batch.vertices.size()==count&&count>=64){
            const std::size_t used=(std::size_t(count)*rigid.recordBytes()+sizeof(gx::GpuVertex)-1)/sizeof(gx::GpuVertex);
            if(!shrinkGxGeometry(batch,used,batch.indices.size(),true))return cancel(gxGeometryFailure(),DrawFailureKind::geometry);
            reservation=batch;context.compactReservationBytes+=(count-used)*sizeof(gx::GpuVertex);
        }
        if(!submitGxRigidGeometry(raster,material,batch,rigid))return cancel(gxPicaFailure(),DrawFailureKind::submission);
        gxProfileTotals.rigidVertices+=count;if(rigid.compact)gxProfileTotals.compactVertices+=count;
        bool anyFallback=false;for(bool v:simplified)anyFallback|=v;
        // Vertex fog (immediate-fog.txt): in the plans, plus a ramp unit
        // after the material's textures when the fog is not in a primary channel.
        const bool fogRecord=fog.enabled&&ImmediateFog(immediateFogMode.load(std::memory_order_relaxed))!=ImmediateFog::off;
        const bool fogRamp=fogRecord&&!fogPrimaryAlpha&&!fogPrimaryRed;
        if(immediateCaptured&&rigid.multiTexture&&rigid.compact&&!rigid.wideRecord&&!rigid.staticVertices&&!colorPlan.lit&&(!fog.enabled||fogRecord)&&!anyFallback&&
           !carrier&&!exactI4Pending&&!indirect.enabled&&!originalMaterial.indirectCount&&!signedQDiagnostic&&!cameraCpuClip&&
           !cameraAlignedProved&&!depthlessOrthoNormalized&&!positiveQDepthFallback&&!fogSubstituted&&!constantEarlyDepth&&
           material.textureCount==originalTextureCount+(fogRamp?1u:0u)&&!material.projectiveTexture0){
            auto& rec=*immediateSlot;
            if(auto* kept=immediateFogVerifyRecord){
                // The record this draw would have replayed, against the plans just made.
                immediateFogVerifyRecord=nullptr;++immediateFogVerified;
                const auto& now=immediateCapturedPlan();
                bool same=kept->fogged&&kept->fogRamp==fogRamp&&kept->fogFunction==fog.function&&kept->textureCount==originalTextureCount&&
                    !std::memcmp(&kept->raster,&raster,sizeof(raster))&&sameMaterial(kept->material,material)&&kept->scales==scales&&
                    kept->plan.lights==now.lights&&kept->plan.multiRowUnits==now.multiRowUnits&&kept->plan.unitSource==now.unitSource;
                // Every planned row but the palette (rows 0-5, placed per draw).
                for(unsigned row=6;same&&row<94;++row)
                    same=row>=immediateUnitRowBase&&row<immediateUnitRowBase+immediateUnitRowCount?
                        !std::memcmp(kept->unitRows[row-immediateUnitRowBase].data(),now.uniforms[row].data(),sizeof(now.uniforms[row])):
                        !std::memcmp(kept->plan.uniforms[row].data(),now.uniforms[row].data(),sizeof(now.uniforms[row]));
                if(!same){
                    ++immediateFogMismatches;
#if defined(__3DS__)
                    static unsigned reports;
                    if(reports<8){++reports;OSReport("Native immediate fogged record verify mismatch %llu: function=%u ramp=%d units=%u\n",
                        immediateFogMismatches,fog.function,int(fogRamp),originalTextureCount);}
#endif
                }
            }
            if(rec.used)++immediateEvictions;
            rec.plan=immediateCapturedPlan();rec.stamp=gx::textureFrameCounter;
            rec.fogged=fog.enabled;rec.fogRamp=fogRamp;rec.fogFunction=std::uint8_t(fog.function);
            if(fog.enabled)++immediateFogStores;
            rec.keyA=immediateKey.a;rec.keyB=immediateKey.b;rec.extent=gx::currentTargetExtent;
            rec.extra=immediateExtraKey.combined;rec.extraColor=immediateExtraKey.color;rec.extraOther=immediateExtraKey.other;
            rec.raster=raster;rec.material=material;rec.scales=scales;rec.textureCount=std::uint8_t(originalTextureCount);
            // rec.plan is the block as planned (captured before the decode).
            std::copy_n(rec.plan.uniforms.begin()+immediateUnitRowBase,immediateUnitRowCount,rec.unitRows.begin());
            rec.repeat.valid=false;rec.used=true;++immediateStores;
        }
        // Native shape record (direct J3D draw on later frames).
        const bool staticList=gx::staticGeometryHint.generation&&gx::staticGeometryHint.list;
        const unsigned listBatch=gx::staticGeometryHint.batch;
        // Topology must be the complete, view-independent expansion of the
        // list (pre-indexed by the batcher or expanded here from strips, fans
        // and quads); points and lines are widened per frame.
        // Fallbacks a record may keep: the TEV stand-in and the relaxed dual
        // alpha test, functions of the key's registers and the TEV colours
        // (see NativeShapeRecord::standIn). A stand-in needs the default
        // policy (perspective draws, no per-draw callback) for its replan.
        bool recordableFallback=nativeFallbackRecords&&!context.allowVisualFallback&&!context.diagnosticRasterCarrier&&
            (!simplified[unsigned(DrawVisualFallback::tevMaterial)]||standIn.kind);
        for(unsigned i=0;i<simplified.size();++i)
            if(simplified[i]&&i!=unsigned(DrawVisualFallback::tevMaterial)&&i!=unsigned(DrawVisualFallback::alphaCompare))recordableFallback=false;
        const unsigned refusal=!staticList?0:listBatch<1||listBatch>nativeMaxBatches?1:!rigid.compact?2:!rigid.staticVertices?3:
            (carrier||exactI4Pending||(anyFallback&&!recordableFallback))?5:(indirect.enabled||originalMaterial.indirectCount)?6:
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
                    // As the planner took it (see the multi-texture plan above).
                    if(rigid.skinEntries)first.matrix[0]=0;
                    rec={};rec.generation=gx::staticGeometryHint.generation;rec.list=gx::staticGeometryHint.list;
                    rec.keyA=key.a;rec.keyB=key.b;
                    rec.cacheEpoch=cache->epoch;rec.indexEpoch=residentIndexEpoch();
                    rec.raster=raster;rec.material=material;rec.colorMaterial=colorMaterial;rec.first=first;
                    rec.matrixOffsets=rigid.matrixOffsets;rec.normalOffsets=rigid.normalOffsets;rec.compactPalette=rigid.compactPalette;
                    for(unsigned m=0;m<10;++m)if(rigid.matrixOffsets[m]>=0)rec.matrixOrder[rec.matrixCount++]=std::uint8_t(m);
                    std::sort(rec.matrixOrder.begin(),rec.matrixOrder.begin()+rec.matrixCount,
                        [&](std::uint8_t x,std::uint8_t y){return rigid.matrixOffsets[x]<rigid.matrixOffsets[y];});
                    rec.normalScale=rigid.decodedNormalScale;rec.tex0Scale=rigid.decodedTex0Scale;rec.tex1Scale=rigid.decodedTex1Scale;
                    rec.vertices=rigid.staticVertices;rec.indices=indices;rec.indexCount=unsigned(batch.indices.size());rec.vertexCount=count;
                    rec.originalTextureCount=std::uint8_t(originalTextureCount);rec.multiTexture=rigid.multiTexture;
                    rec.fogEnabled=fog.enabled;rec.fogPrimaryAlpha=fogPrimaryAlpha;rec.fogPrimaryRed=fogPrimaryRed;
                    rec.dynamic=nativeDynamic(draw.registers);rec.constantEarlyDepth=constantEarlyDepth;rec.parts=keyParts;
                    rec.batch=std::uint8_t(listBatch);rec.batchCount=0;rec.pending=true;rec.rigidProjected=rigid.projectedTexture||rigid.multiProjected;
                    rec.origin=rigid.origin;rec.skinEntries=rigid.skinEntries;
                    if(rigid.skinEntries)++skinDrawState.recorded;
                    rec.standIn=simplified[unsigned(DrawVisualFallback::tevMaterial)]?standIn:RecordedStandIn{};
                    if(anyFallback)++nativeFallbackStored;
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
    for(unsigned bit=0;bit<loweredDraws.size();++bit)if(material.lowering&(1u<<bit))++loweredDraws[bit];
    reportDegradedReasons(context,classFilter.saved,clipDisabledGpuDraws);
    reportDrawGaps(classFilter.saved);
    if(carrier)++context.rasterCarrierDraws;
    ++context.submitted;if(material.quantizedConstants)++context.quantizedDraws;
    if(skinDraw)++skinDrawState.batches;
    if(fog.enabled)++context.vertexFogDraws;
    if(indirect.enabled)++context.vertexIndirectDraws;
    if(indirectConstant.enabled)gx::countIndirectConstant(rigid,count);
    gx::countIndirectFormat(indirectFormat);
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
// A replayed record that keeps a visual fallback counts as the complete
// path counted that draw (degraded draws by reason, stand-ins by error).
void countReplayedFallback(const NativeShapeRecord& rec,CommandDrawContext& context){
    if(!rec.standIn.kind&&!rec.raster.alphaRelaxed)return;
    if(rec.standIn.kind){
        ++context.simplifiedByReason[unsigned(DrawVisualFallback::tevMaterial)];
        ++tevStandInByError[rec.standIn.error];if(rec.standIn.multiply)++multiplyStandIns;
        ++nativeStandInReplays;
    }
    if(rec.raster.alphaRelaxed){++context.simplifiedByReason[unsigned(DrawVisualFallback::alphaCompare)];++nativeRelaxedAlphaReplays;}
    ++context.simplifiedDraws;
}
// ---------------------------------------------------------------------------
// Native PICA blocks (runtime/pica_blocks.hpp): a recorded shape replayed
// under inputs proven unchanged since its words were kept sends those words
// again, with its palette rows placed from the live matrices. SD switch
// pica-blocks.txt: 0 off (default), 1 on, v verify (today's path draws every
// shape a block would have drawn, and its words are compared with the block's).
// The facts of a record a block's words were made from: everything of the
// record they depend on. Compared as bytes up to the material plans (built
// over zeroed storage, so no indeterminate padding), the plans with
// sameMaterial (a PicaMaterialPlan has a padding byte); the target extent
// first, for the miss reason.
struct BlockFacts {
    gx::TargetExtent extent;
    const void* list;const void* vertices;const void* indices;
    std::uint32_t generation,keyA,keyB,cacheEpoch,indexEpoch,indexCount,vertexCount,firstPresent;
    std::array<float,3> origin;float normalScale,tex0Scale,tex1Scale;
    std::array<std::int8_t,10> matrixOffsets,normalOffsets;std::array<std::uint8_t,10> matrixOrder;
    std::uint8_t matrixCount,batch,firstMatrix,firstNormalCount,originalTextureCount,switches,standInKind,standInMaps;
    bool compactPalette,multiTexture,fogEnabled,fogPrimaryAlpha,fogPrimaryRed,constantEarlyDepth;
    gx::RasterPlan raster;
    gx::PicaMaterialPlan material,colorMaterial; // last: compared with sameMaterial
};
static_assert(std::is_trivially_copyable_v<BlockFacts>&&offsetof(BlockFacts,material)>offsetof(BlockFacts,raster)&&
              offsetof(BlockFacts,colorMaterial)>offsetof(BlockFacts,material));
void buildBlockFacts(const NativeShapeRecord& rec,const CommandDrawContext& context,BlockFacts& f){
    std::memset(static_cast<void*>(&f),0,sizeof(f));
    f.extent=gx::currentTargetExtent;
    f.list=rec.list;f.vertices=rec.vertices;f.indices=rec.indices;
    f.generation=rec.generation;f.keyA=rec.keyA;f.keyB=rec.keyB;f.cacheEpoch=rec.cacheEpoch;f.indexEpoch=rec.indexEpoch;
    f.indexCount=rec.indexCount;f.vertexCount=rec.vertexCount;f.firstPresent=rec.first.present;
    f.origin=rec.origin;f.normalScale=rec.normalScale;f.tex0Scale=rec.tex0Scale;f.tex1Scale=rec.tex1Scale;
    f.matrixOffsets=rec.matrixOffsets;f.normalOffsets=rec.normalOffsets;f.matrixOrder=rec.matrixOrder;
    f.matrixCount=std::uint8_t(rec.matrixCount);f.batch=rec.batch;f.firstMatrix=rec.first.matrix[0];f.firstNormalCount=rec.first.normalCount;
    f.originalTextureCount=rec.originalTextureCount;
    f.switches=std::uint8_t(context.allowVertexFog|(context.allowMultiProjected<<1)|(context.allowSingleRigidShader<<2)|(nativeLeanReplay<<3));
    f.standInKind=rec.standIn.kind;f.standInMaps=rec.standIn.maps;
    f.compactPalette=rec.compactPalette;f.multiTexture=rec.multiTexture;f.fogEnabled=rec.fogEnabled;
    f.fogPrimaryAlpha=rec.fogPrimaryAlpha;f.fogPrimaryRed=rec.fogPrimaryRed;f.constantEarlyDepth=rec.constantEarlyDepth;
    f.raster=rec.raster;f.material=rec.material;f.colorMaterial=rec.colorMaterial;
}
std::atomic<std::uint8_t> blockModeValue{std::uint8_t(blocks::Mode::off)};
blocks::Store blockStore;
// The capture armed for the replay in progress (its record slot and the
// register words its rows are made from); verify: the words the block would
// have sent are in blockVerifyWords.
struct BlockRequest {bool active{},verify{};unsigned slot{};blocks::InputPlan inputs{};} blockRequest;
// Rows mode armed for the replay in progress: its inputs changed, so today's
// planner makes every uniform row and the block takes them all at the submit.
struct BlockRows {bool active{},verify{};unsigned slot{};} blockRows;
std::uint32_t* blockVerifyWords{};unsigned blockVerifyCount{};
// Palette rows of the block being replayed or checked.
gx::RigidVertexPlan blockPlan;
std::uint32_t blockReportWindow{},blockProfiledFrame{};std::uint64_t blockProfiledPictures{},blockReportSum{};
// Verify reports: one line per place (register and uniform row), 16 places.
std::array<std::uint32_t,16> blockMismatchPlaces{};unsigned blockMismatchReported{};
bool blockKind(const NativeShapeRecord& rec){
    // Kept blocks hold the slot palette's rows (BlockFacts); merged skin
    // records place theirs from the bank.
    return !rec.skinEntries&&!rec.rigidProjected&&!rec.raster.alphaUnion&&!rec.raster.alphaInterval.enabled&&!rec.material.projectiveTexture0&&
        rec.vertices&&rec.indices&&rec.indexCount&&rec.material.textureCount<=3&&rec.originalTextureCount<=rec.material.textureCount;
}
std::uint8_t blockCoordinates(const NativeShapeRecord& rec){
    std::uint8_t coordinates=0;
    for(unsigned unit=0;unit<rec.colorMaterial.textureCount;++unit)coordinates|=std::uint8_t(1u<<rec.colorMaterial.textures[unit].coordinate);
    return coordinates;
}
unsigned blockPosition(const NativeShapeRecord& rec,const gx::CommandRegisters& r){
    return rec.first.present&1?rec.first.matrix[0]:r.matrixIndex[0]&63;
}
// The textures a replay of `rec` binds, resolved as the complete replay
// resolves them. False when one does not resolve or its pixels can change
// (only immutable images: their storage moves only with the storage serial).
bool blockTextures(const NativeShapeRecord& rec,const gx::CommandRegisters& r,std::array<gx::Texture,3>& textures){
    for(unsigned unit=0;unit<rec.originalTextureCount;++unit){
        const char* why;
        if(!gx::resolveCommandTexture(r,rec.material.textures[unit].map,textures[unit],why)||!textures[unit].immutablePixels)return false;
    }
    for(unsigned map=0;rec.standIn.maps>>map;++map)if(rec.standIn.maps&(1u<<map)){
        gx::Texture texture;const char* why;if(!gx::resolveCommandTexture(r,map,texture,why))return false;
    }
    if(rec.material.textureCount>rec.originalTextureCount){
        // The fog ramp after the material's textures (its curve is among the inputs).
        if(rec.material.textureCount!=rec.originalTextureCount+1||!rec.fogEnabled||rec.fogPrimaryAlpha||rec.fogPrimaryRed)return false;
        const auto& fog=nativeFog(r);
        if(!fog.ok||!fog.plan.enabled||!fogRampTexture(textures[rec.originalTextureCount],fog.plan.function))return false;
    }
    return true;
}
void reportBlockMismatch(const std::uint32_t* now,unsigned count){
    const auto* kept=blockVerifyWords;
    unsigned where=0;const unsigned common=std::min(count,blockVerifyCount);
    while(where<common&&kept[where]==now[where])++where;
    const auto place=blocks::describeWord({now,count},where);
    const std::uint32_t key=(place.reg<<16)^std::uint32_t(place.row&0xffff)^(place.header?0x80000000u:0);
    for(unsigned n=0;n<blockMismatchReported;++n)if(blockMismatchPlaces[n]==key)return;
    if(blockMismatchReported>=blockMismatchPlaces.size())return;
    blockMismatchPlaces[blockMismatchReported++]=key;
#if defined(__3DS__)
    OSReport("Native PICA blocks verify mismatch: words block=%u now=%u first difference at %u (%08lx/%08lx) register %03lx%s uniform row %ld\n",
        blockVerifyCount,count,where,static_cast<unsigned long>(where<blockVerifyCount?kept[where]:0),
        static_cast<unsigned long>(where<count?now[where]:0),static_cast<unsigned long>(place.reg),place.header?" (header)":"",static_cast<long>(place.row));
#endif
}
// The block path of a replay. True: the shape was drawn from its block.
// False: today's path draws it, with a capture (or a verification) armed
// when that applies.
bool nativeBlockReplay(NativeShapeRecord& rec,const gx::CommandRegisters& r,CommandDrawContext& context,unsigned caster){
    const auto mode=blocks::Mode(blockModeValue.load(std::memory_order_acquire));
    if(mode==blocks::Mode::off||!blockStore.attached())return false;
    static const auto blockIndex=registerGxNamedProfile("native.block");GxNamedProfileScope blockScope(blockIndex);
    const auto started=gxSparseVertexProfiling?gxProfileTick():0;
    auto& store=blockStore;auto& s=store.stats;
    const unsigned slot=unsigned(&rec-nativeShapes.data());
    ++s.replays;
    if(!blockKind(rec)){++s.misses[unsigned(blocks::Miss::kind)];return false;}
    if(store.resting(slot)){++s.misses[unsigned(blocks::Miss::resting)];return false;}
    bool verify=false;
    if(auto* b=store.find(slot)){
        BlockFacts facts;buildBlockFacts(rec,context,facts);
        const auto* kept=store.facts(*b);
        auto why=blocks::Miss::none;
        // Byte views of the kept facts (pool storage: copied, never cast).
        auto keptMaterial=[&](std::size_t at){gx::PicaMaterialPlan m;std::memcpy(static_cast<void*>(&m),kept+at,sizeof(m));return m;};
        if(b->factBytes!=sizeof(facts)||std::memcmp(kept+offsetof(BlockFacts,extent),&facts.extent,sizeof(facts.extent)))why=blocks::Miss::target;
        else if(std::memcmp(kept,&facts,offsetof(BlockFacts,material))||!sameMaterial(keptMaterial(offsetof(BlockFacts,material)),rec.material)||
                !sameMaterial(keptMaterial(offsetof(BlockFacts,colorMaterial)),rec.colorMaterial))why=blocks::Miss::record;
        else if(b->storageSerial!=gxTextureStorageSerial)why=blocks::Miss::storage;
        else if(nativeDynamic(r)!=b->dynamic)why=blocks::Miss::dynamic;
        else if(const int changed=blocks::changedInput(r,store.plan(*b),store.inputs(*b));changed>=0){
            ++s.inputsChanged[unsigned(changed)];
            // Projection, fog, colours, lights or texture matrices moved
            // (the camera moved): today's planner makes every row, the block
            // keeps its state words (submitBlockRows). Same textures needed.
            ++s.misses[unsigned(blocks::Miss::inputs)];
            if(b->rowsMode){
                std::array<gx::Texture,3> textures{};
                bool same=blockTextures(rec,r,textures);
                for(unsigned unit=0;same&&unit<rec.material.textureCount;++unit)same=textures[unit].handle==b->handle(unit);
                if(same){blockRows={true,mode==blocks::Mode::verify,slot};return false;}
            }
            if(store.missed(slot))return false;
        }
        else{
            std::array<gx::Texture,3> textures{};
            if(!blockTextures(rec,r,textures)){++s.misses[unsigned(blocks::Miss::texture)];return false;}
            for(unsigned unit=0;unit<rec.material.textureCount;++unit)if(textures[unit].handle!=b->handle(unit))why=blocks::Miss::texture;
            if(why==blocks::Miss::none){
                // Bound as the complete replay binds them (cache use order,
                // re-decodes, protection against eviction during the draw).
                GxTextureDrawScope textureScope({textures.data(),rec.material.textureCount});
                if(!textureScope){++s.misses[unsigned(blocks::Miss::texture)];return false;}
                std::array<std::array<float,2>,3> scales{};
                for(unsigned unit=0;unit<rec.material.textureCount;++unit)
                    if(!bindGxTexture(textures[unit],scales[unit][0],scales[unit][1],unit,false)){++s.misses[unsigned(blocks::Miss::texture)];return false;}
                if(scales!=b->scales)why=blocks::Miss::texture;
                else if(b->storageSerial!=gxTextureStorageSerial)why=blocks::Miss::storage;
                else if(!blocks::placePalette(r,blockPosition(rec,r),b->palette,blockPlan)){++s.misses[unsigned(blocks::Miss::palette)];return false;}
                else{
                    const auto rows=blocks::paletteRows(blockPlan);
                    auto* words=store.words(*b);
                    if(mode==blocks::Mode::verify){
                        std::memcpy(blockVerifyWords,words,b->wordCount*sizeof(std::uint32_t));blockVerifyCount=b->wordCount;
                        blocks::patchRows(blockVerifyWords,store.layout(*b),b->uniformBase,blockPlan,rows);
                        verify=true;
                    }else{
                        blocks::patchRows(words,store.layout(*b),b->uniformBase,blockPlan,rows);
                        blockPlan.staticVertices=static_cast<const std::byte*>(rec.vertices);
                        if(!submitPicaBlock(words,b->wordCount,b->palette.multiTexture,blockPlan,rec.vertices,rec.indices,rec.indexCount,caster)){
                            ++s.misses[unsigned(blocks::Miss::room)];return false;
                        }
                        ++s.hits;s.wordsCopied+=b->wordCount;store.hit(slot);
                        // The PICA now holds the block's state, not the chain's.
                        nativeChain.valid=false;
                        ++context.submitted;++nativeHits;countReplayedFallback(rec,context);
                        if(rec.compactPalette)if(auto* sc=gx::staticCompactCache())++sc->paletteReplays;
                        gxProfileTotals.rigidVertices+=rec.vertexCount;gxProfileTotals.compactVertices+=rec.vertexCount;gxProfileTotals.vertices+=rec.vertexCount;
                        if(started){s.hitTicks+=gxProfileTick()-started;++s.profiledHits;}
                        return true;
                    }
                }
            }
        }
        if(why!=blocks::Miss::none){
            ++s.misses[unsigned(why)];
            // Inputs that keep changing rest the record (no capture for a while).
            if(why!=blocks::Miss::target&&why!=blocks::Miss::storage&&store.missed(slot))return false;
        }
    }else ++s.misses[unsigned(blocks::Miss::none)];
    // Today's path draws this shape: keep its words.
    auto& q=blockRequest;
    if(!blocks::planInputs(r,blockCoordinates(rec),q.inputs)){++s.refusals[unsigned(blocks::Refusal::kind)];return false;}
    q.active=true;q.verify=verify;q.slot=slot;
    requestPicaBlockCapture();
    return false;
}
// After today's path submitted the shape of an armed capture: its words
// become the record's block (verify: they are compared with the block's first).
void finishBlockCapture(const NativeShapeRecord& rec,const gx::CommandRegisters& r,const CommandDrawContext& context,
                        const std::array<std::array<float,2>,3>& scales,const gx::RigidVertexPlan& rigid){
    auto& q=blockRequest;
    if(!q.active)return;
    q.active=false;
    auto& store=blockStore;auto& s=store.stats;
    const bool verify=q.verify;
    auto refuse=[&](blocks::Refusal why){++s.refusals[unsigned(why)];if(verify)++s.verifySkipped;};
    PicaBlockCapture cap;
    if(!takePicaBlockCapture(cap))return refuse(blocks::Refusal::span);
    if(rigid.pairedUnits||rigid.multiProjected||rigid.projectedTexture||rigid.wideRecord||rigid.vertexWarp.enabled||
       cap.multiTexture!=rigid.multiTexture||rigid.multiTexture!=rec.multiTexture)return refuse(blocks::Refusal::kind);
    if(cap.count>blocks::maxBlockWords)return refuse(blocks::Refusal::size);
    blocks::UniformLayout layout;
    if(!blocks::findUniformWords({cap.words,cap.count},layout))return refuse(blocks::Refusal::rows);
    blocks::PaletteShape shape;
    shape.matrixOffsets=rec.matrixOffsets;shape.normalOffsets=rec.normalOffsets;shape.matrixOrder=rec.matrixOrder;
    shape.matrixCount=std::uint8_t(rec.matrixCount);shape.multiRowUnits=rigid.multiRowUnits;shape.lights=std::uint8_t(rigid.lights);
    shape.compactPalette=rec.compactPalette;shape.multiTexture=rigid.multiTexture;shape.normalsUnused=rigid.normalsUnused;shape.origin=rec.origin;
    // A replay of the block places exactly the palette rows this draw sent.
    if(!blocks::placePalette(r,blockPosition(rec,r),shape,blockPlan))return refuse(blocks::Refusal::rows);
    const auto rows=blocks::paletteRows(blockPlan),sent=blocks::paletteRows(rigid);
    if(rows.firstEnd!=sent.firstEnd||rows.second!=sent.second||rows.end!=sent.end||rows.end>blockPlan.uniforms.size())return refuse(blocks::Refusal::rows);
    for(const auto [first,end]:{std::pair<unsigned,unsigned>{0,rows.firstEnd},std::pair<unsigned,unsigned>{rows.second,rows.end}})
        for(unsigned row=first;row<end;++row)if(std::memcmp(blockPlan.uniforms[row].data(),rigid.uniforms[row].data(),sizeof(rigid.uniforms[row])))return refuse(blocks::Refusal::rows);
    if(!blocks::rowsCovered(layout,cap.uniformBase,rows))return refuse(blocks::Refusal::rows);
    const auto dynamic=nativeDynamic(r);
    if(dynamic!=rec.dynamic)return refuse(blocks::Refusal::kind);
    std::array<gx::Texture,3> textures{};
    if(!blockTextures(rec,r,textures))return refuse(blocks::Refusal::kind);
    if(verify){
        ++s.verified;
        // Equal: the kept block stays. Different: today's words replace it.
        if(cap.count==blockVerifyCount&&!std::memcmp(cap.words,blockVerifyWords,cap.count*sizeof(std::uint32_t))){store.hit(q.slot);return;}
        ++s.verifyMismatches;reportBlockMismatch(cap.words,cap.count);
    }
    BlockFacts facts;buildBlockFacts(rec,context,facts);
    auto* b=store.keep(q.slot,{cap.words,cap.count},r,q.inputs,layout,{reinterpret_cast<const std::byte*>(&facts),sizeof(facts)});
    if(!b){if(verify)++s.verifySkipped;return;}
    b->palette=shape;b->uniformBase=std::uint16_t(cap.uniformBase);b->textureUnits=rec.material.textureCount;
    b->dynamic=dynamic;b->storageSerial=gxTextureStorageSerial;
    b->uploaded=blocks::uploadRows(rigid);b->rowsMode=blocks::rowsCovered(layout,cap.uniformBase,b->uploaded);
    for(unsigned unit=0;unit<rec.material.textureCount;++unit)b->setHandle(unit,textures[unit].handle);
    for(unsigned unit=0;unit<3;++unit)b->scales[unit]=unit<rec.material.textureCount?scales[unit]:std::array<float,2>{};
    ++s.captures;
}
// SD switch pica-blocks.txt, read once by the painter on the game thread
// before it queues a shape (the storage is published before the mode). Its
// own function: the shared-state audit takes it as a barrier.
[[gnu::noinline]] void configurePicaBlocks(){
    static bool configured;
    if(configured)return;
    // Nothing queued for the worker while the store and the mode are set.
    syncGxWorker();
    configured=true;
    char text[8]{};
    if(auto* file=std::fopen("sdmc:/3ds/DuskLight/pica-blocks.txt","rb")){(void)std::fread(text,1,sizeof(text)-1,file);std::fclose(file);}
    auto mode=blocks::Mode::off;
    for(const char c:text){
        if(c=='0')mode=blocks::Mode::off;
        else if(c=='1')mode=blocks::Mode::on;
        else if(c=='v'||c=='V')mode=blocks::Mode::verify;
    }
    std::uint32_t poolWords=0;
    if(mode!=blocks::Mode::off){
        // Ordinary CPU heap (never linear memory: the PICA never reads it).
        std::uint32_t* pool=nullptr;
        for(const std::uint32_t words:{256u*1024u,128u*1024u,64u*1024u})
            if((pool=static_cast<std::uint32_t*>(std::malloc(words*sizeof(std::uint32_t))))){poolWords=words;break;}
        auto* offsets=static_cast<std::uint32_t*>(std::malloc(nativeShapeCapacity*sizeof(std::uint32_t)));
        auto* heat=static_cast<std::uint8_t*>(std::malloc(2*nativeShapeCapacity));
        blockVerifyWords=static_cast<std::uint32_t*>(std::malloc(blocks::maxBlockWords*sizeof(std::uint32_t)));
        if(!pool||!offsets||!heat||!blockVerifyWords){
            std::free(pool);std::free(offsets);std::free(heat);std::free(blockVerifyWords);blockVerifyWords=nullptr;
            mode=blocks::Mode::off;poolWords=0;
        }else blockStore.attach(pool,poolWords,nativeShapeCapacity,offsets,heat);
    }
#if defined(__3DS__)
    OSReport("Native PICA blocks: %s, pool %lu KiB (SD switch pica-blocks.txt: 0 off, 1 on, v verify)\n",
        mode==blocks::Mode::off?"off":mode==blocks::Mode::verify?"verify":"on",static_cast<unsigned long>(poolWords/256));
#endif
    blockModeValue.store(std::uint8_t(mode),std::memory_order_release);
    // Fogged immediate records (ImmediateRecord): their own switch, read here once too.
    char fogText[8]{};
    if(auto* file=std::fopen("sdmc:/3ds/DuskLight/immediate-fog.txt","rb")){(void)std::fread(fogText,1,sizeof(fogText)-1,file);std::fclose(file);}
    auto fogMode=ImmediateFog::off;
    for(const char c:fogText){
        if(c=='0')fogMode=ImmediateFog::off;
        else if(c=='1')fogMode=ImmediateFog::on;
        else if(c=='v'||c=='V')fogMode=ImmediateFog::verify;
    }
#if defined(__3DS__)
    OSReport("Native immediate fogged records: %s (SD switch immediate-fog.txt: 0 off, 1 on, v verify)\n",
        fogMode==ImmediateFog::off?"off":fogMode==ImmediateFog::verify?"verify":"on");
#endif
    immediateFogMode.store(std::uint8_t(fogMode),std::memory_order_release);
}
// Two lines every 300 pictures when a count moved; on the thread that owns
// the records (the counts are its own).
void reportPicaBlocks(){
    const auto mode=blocks::Mode(blockModeValue.load(std::memory_order_acquire));
    if(mode==blocks::Mode::off)return;
    const std::uint32_t frame=gx::textureFrameCounter;
    if(gxSparseVertexProfiling&&frame!=blockProfiledFrame){blockProfiledFrame=frame;++blockProfiledPictures;}
    if(frame/300==blockReportWindow)return;
    blockReportWindow=frame/300;
    const auto& s=blockStore.stats;
    const std::uint64_t sum=s.replays+s.captures+s.verified+s.recycles;
    if(sum==blockReportSum)return;
    blockReportSum=sum;
    constexpr double ticksPerMillisecond=268111.856;
    const double ms=blockProfiledPictures?double(s.hitTicks)/ticksPerMillisecond/double(blockProfiledPictures):0;
    char line[240];
    for(unsigned n=0;n<2;++n){
        blocks::formatReport(line,sizeof(line),n,mode,s,blockStore,ms);
#if defined(__3DS__)
        OSReport("%s",line);
#endif
    }
}
// Rows mode (armed by nativeBlockReplay): today's path planned every row of
// the shape; the block takes them all instead of Citro3D's emission. True:
// submitted from the block. False: submitNativeRigid draws it (verify: with
// a capture armed to compare its words with the block's).
bool submitBlockRows(const NativeShapeRecord& rec,const gx::CommandRegisters& r,const std::array<std::array<float,2>,3>& scales,
                     const gx::RigidVertexPlan& rigid,unsigned caster){
    auto& q=blockRows;
    if(!q.active)return false;
    q.active=false;
    auto& store=blockStore;auto& s=store.stats;
    auto* b=store.find(q.slot);
    const auto rows=blocks::uploadRows(rigid);
    // The same rows as the kept words upload, the same light loop count
    // (an int uniform in the words), the same textures and storage.
    if(!b||!b->rowsMode||!(rows==b->uploaded)||rigid.multiTexture!=b->palette.multiTexture||rigid.lights!=b->palette.lights||!rigid.compact||
       rigid.pairedUnits||rigid.multiProjected||rigid.projectedTexture||rigid.wideRecord||rigid.vertexWarp.enabled||
       rigid.matrixOffsets!=b->palette.matrixOffsets||rigid.normalOffsets!=b->palette.normalOffsets||
       b->storageSerial!=gxTextureStorageSerial){++s.rowsRefused;return false;}
    for(unsigned unit=0;unit<3;++unit)
        if(b->scales[unit]!=(unit<rec.material.textureCount?scales[unit]:std::array<float,2>{})){++s.rowsRefused;return false;}
    auto* words=store.words(*b);
    if(q.verify){
        std::memcpy(blockVerifyWords,words,b->wordCount*sizeof(std::uint32_t));blockVerifyCount=b->wordCount;
        blocks::patchRows(blockVerifyWords,store.layout(*b),b->uniformBase,rigid,rows);
        blockRequest.active=true;blockRequest.verify=true;blockRequest.slot=q.slot;blockRequest.inputs=store.plan(*b);
        requestPicaBlockCapture();
        return false;
    }
    blocks::patchRows(words,store.layout(*b),b->uniformBase,rigid,rows);
    if(!submitPicaBlock(words,b->wordCount,rigid.multiTexture,rigid,rec.vertices,rec.indices,rec.indexCount,caster)){
        ++s.misses[unsigned(blocks::Miss::room)];return false;
    }
    // The kept rows now follow these inputs.
    blocks::takeInputs(r,store.plan(*b),store.inputs(*b));
    ++s.hits;++s.rowsHits;s.wordsCopied+=b->wordCount;store.hit(q.slot);
    return true;
}
// Armed captures end with the replay that armed them.
struct BlockRequestEnd {
    ~BlockRequestEnd(){
        blockRows.active=false;
        auto& q=blockRequest;
        if(!q.active)return;
        if(q.verify)++blockStore.stats.verifySkipped;
        q.active=false;cancelPicaBlockCapture();
    }
};
// Replay one recorded batch from the live registers. False: nothing was
// submitted for this batch.
// `caster`: the model shadow slot the shape was issued under.
// Levels of detail of the record's list (runtime/model_lod.hpp), or null.
// submitNativeRigid chooses between them and the record's own list. A level
// never goes through a PICA block: a kept block holds the address and count
// of the list it was captured with.
bool nativeReplay(NativeShapeRecord& rec,const gx::CommandRegisters& r,CommandDrawContext& context,unsigned caster,const NativeRigidLevels* levels=nullptr){
    if(rec.raster.scissor.empty||rec.raster.cull==3){++context.culled;++nativeHits;return true;}
    BlockRequestEnd blockRequestEnd;
    if(!levels&&nativeBlockReplay(rec,r,context,caster))return true;
    auto& chain=nativeChain;
    auto& rigid=nativeReplayPlan;
    if(chain.valid&&chain.stateEpoch==r.stateEpoch&&chain.picaSerial==gxPicaStateSerial&&
       chain.matrixTextures0==(r.matrixIndex[0]>>6)&&chain.matrixTextures1==r.matrixIndex[1]&&
       rec.dynamic==chain.dynamic&&rec.multiTexture==chain.multiTexture&&rec.fogEnabled==chain.fogEnabled&&
       rec.fogPrimaryAlpha==chain.fogPrimaryAlpha&&rec.fogPrimaryRed==chain.fogPrimaryRed&&
       rec.rigidProjected==chain.rigidProjected&&
       rec.originalTextureCount==chain.originalTextureCount&&rec.first.present==chain.firstPresent&&
       rec.first.normalCount==chain.firstNormalCount&&
       !std::memcmp(&chain.extent,&gx::currentTargetExtent,sizeof(chain.extent))&&
       !std::memcmp(&rec.raster,&chain.raster,sizeof(rec.raster))&&
       sameMaterial(rec.material,chain.material)&&sameMaterial(rec.colorMaterial,chain.colorMaterial)){
        static const auto chainIndex=registerGxNamedProfile("native.chained");GxNamedProfileScope chainScope(chainIndex);
        if(chain.rowsOnly){
            // The shared plan holds the predecessor's block (see NativeChain).
            std::copy(chain.unitRows.begin(),chain.unitRows.end(),rigid.uniforms.begin()+81);
            rigid.foldedOrigin={};rigid.compact=false;rigid.staticVertices=nullptr;
        }else rigid=chain.plan;
        rigid.matrixOffsets.fill(-1);rigid.normalOffsets.fill(-1);rigid.matrices=0;rigid.compactPalette=false;rigid.paletteEnd=0;rigid.skinEntries=0;
        const unsigned position=rec.first.present&1?rec.first.matrix[0]:r.matrixIndex[0]&63;
        bool okay=position<=27&&position%3==0&&gx::addRigidMatrix(r,position,rigid);
        if(okay&&rec.compactPalette)gx::restartCompactPalette(rigid);
        for(unsigned n=0;okay&&n<rec.matrixCount;++n)okay=gx::addRigidMatrix(r,rec.matrixOrder[n]*3u,rigid);
        if(okay&&rec.skinEntries)okay=gx::placeSkinPalette(r,rec.generation,rigid)&&rigid.skinEntries==rec.skinEntries;
        if(okay&&rigid.matrixOffsets==rec.matrixOffsets&&rigid.normalOffsets==rec.normalOffsets){
            rigid.decodedNormalScale=rec.normalScale;rigid.decodedTex0Scale=rec.tex0Scale;rigid.decodedTex1Scale=rec.tex1Scale;
            rigid.origin=rec.origin;
            if(gx::finishCompactRigidUniforms(rigid,chain.scales[rec.rigidProjected?1:0])&&
               (rigid.staticVertices=rec.vertices,!rec.rigidProjected||nativeProjectionPositive(rigid,rec))){
                static const auto chainSubmit=registerGxNamedProfile("native.chained.submit");GxNamedProfileScope chainSubmitScope(chainSubmit);
                const bool byBlock=!levels&&submitBlockRows(rec,r,chain.scales,rigid,caster);
                if(!byBlock){
                    if(!submitNativeRigid(rec.raster,rec.material,rigid,rec.vertices,rec.indices,rec.indexCount,caster,!chain.afterBlock,levels)){
                        rec.valid=false;chain.valid=false;return false;
                    }
                    if(!levels)finishBlockCapture(rec,r,context,chain.scales,rigid);
                }
                // The state now in effect is this shape's (applied by Citro3D, or sent by the block).
                chain.picaSerial=gxPicaStateSerial;chain.afterBlock=byBlock;
                ++context.submitted;++nativeHits;++nativeChained;if(chain.rowsOnly)++nativeRowContinuations;
                if(rec.skinEntries)++skinDrawState.replayed;
                countReplayedFallback(rec,context);
                if(rec.compactPalette)if(auto* sc=gx::staticCompactCache())++sc->paletteReplays;
                gxProfileTotals.rigidVertices+=rec.vertexCount;gxProfileTotals.compactVertices+=rec.vertexCount;gxProfileTotals.vertices+=rec.vertexCount;
                return true;
            }
        }
        // Not expressible as a continuation: take the complete replay below.
    }
    chain.valid=false;
    // Lean: multi-texture records (single-texture records keep the complete
    // replay, whose planner reads the whole transform plan).
    const bool lean=nativeLeanReplay&&rec.multiTexture;
    static const auto planIndex=registerGxNamedProfile("native.plans");GxNamedProfileSpan planSpan(planIndex);
    gx::RasterColorPlan colorPlan;
    if(!gx::planRasterColor(r,rec.colorMaterial,colorPlan,nullptr))return false;
    std::uint8_t coordinates=0;
    for(unsigned unit=0;unit<rec.colorMaterial.textureCount;++unit)coordinates|=1u<<rec.colorMaterial.textures[unit].coordinate;
    // planVertexTransform succeeds when both its projection part (cached)
    // and its texgen part (replayTexgenMask) do.
    std::optional<gx::VertexTransformPlan> transform;
    const std::array<float,16>* projection{};std::uint8_t projectiveMask=0;
    if(lean){
        const auto& cached=nativeProjection(r);
        if(!cached.ok||!gx::replayTexgenMask(r,coordinates,projectiveMask)){++nativeLeanRefusals;return false;}
        projection=&cached.projection;
    }else{
        transform.emplace();
        if(!gx::planVertexTransform(r,coordinates,*transform,nullptr))return false;
        projection=&transform->clipProjection();projectiveMask=transform->projectiveMask();
    }
    planSpan.stop();
    static const auto texIndex=registerGxNamedProfile("native.textures");GxNamedProfileSpan texSpan(texIndex);
    std::array<gx::Texture,3> textures{};
    {static const auto i=registerGxNamedProfile("native.tex.resolve");GxNamedProfileScope sc(i);
    for(unsigned unit=0;unit<rec.originalTextureCount;++unit){
        const char* why;if(!gx::resolveCommandTexture(r,rec.material.textures[unit].map,textures[unit],why))return false;
    }
    // The textures a stand-in leaves out must still resolve: the complete
    // path refuses the draw otherwise (validFallbackInputs).
    for(unsigned map=0;rec.standIn.maps>>map;++map)if(rec.standIn.maps&(1u<<map)){
        gx::Texture texture;const char* why;if(!gx::resolveCommandTexture(r,map,texture,why))return false;
    }}
    static const gx::VertexFogPlan noFog{};
    const gx::VertexFogPlan* fogPlan=&noFog;
    std::optional<gx::VertexFogPlan> planned;
    if(context.allowVertexFog){
        bool okay;
        if(lean){const auto& cached=nativeFog(r);okay=cached.ok;fogPlan=&cached.plan;}
        else{okay=gx::planVertexFog(r,planned.emplace());fogPlan=&*planned;}
        if(!okay||fogPlan->enabled!=rec.fogEnabled)return false;
        if(fogPlan->enabled&&!rec.fogPrimaryAlpha&&!rec.fogPrimaryRed&&!fogRampTexture(textures[rec.originalTextureCount],fogPlan->function))return false;
    }else if(rec.fogEnabled)return false;
    const auto& fog=*fogPlan;
    // Animated TEV colours or fog colour: same combiner, new constants.
    if(const auto dynamic=nativeDynamic(r);dynamic!=rec.dynamic){
        if(!refreshNativeConstants(rec,r,context,fog,projectiveMask)){++nativeConstantRefusals;return false;}
        rec.dynamic=dynamic;++nativeConstantRefreshes;
    }
    std::array<std::array<float,2>,3> scales{};
    static const auto scopeIndex=registerGxNamedProfile("native.tex.scope");GxNamedProfileSpan scopeSpan(scopeIndex);
    GxTextureDrawScope textureScope({textures.data(),rec.material.textureCount});
    if(!textureScope)return false;
    scopeSpan.stop();
    {static const auto i=registerGxNamedProfile("native.tex.bind");GxNamedProfileScope sc(i);
    // A record exists only for a batch of a static display list (claimed under
    // the static geometry hint, found again by list and generation). The worker
    // replays records before it sets that hint, so it is not asked here.
    const bool worldDraw=gx::worldMipDraw(r,true);
    for(unsigned unit=0;unit<rec.material.textureCount;++unit)
        if(!bindGxTexture(textures[unit],scales[unit][0],scales[unit][1],unit,unit==0&&rec.material.projectiveTexture0,
            unit<rec.originalTextureCount?gx::worldMipUse(r,worldDraw,rec.material.textures[unit].coordinate):GxWorldMipUse{}))return false;}
    texSpan.stop();
    static const auto rigidIndex=registerGxNamedProfile("native.rigid");GxNamedProfileSpan rigidSpan(rigidIndex);
    const int fogUnit=fog.enabled&&!rec.fogPrimaryAlpha&&!rec.fogPrimaryRed?int(rec.originalTextureCount):-1;
    bool planOkay;
    if(lean){
        gx::prepareRigidMultiReuse(rigid);
        planOkay=gx::buildRigidMultiVertex(r,rec.first,projectiveMask,*projection,colorPlan,fog,rec.fogPrimaryAlpha,gx::currentTargetExtent.logicalWidth,
                                           rec.material,rec.originalTextureCount,scales,fogUnit,rigid,context.allowMultiProjected);
        if(planOkay)++nativeLeanPlans;else ++nativeLeanRefusals;
    }else{
        planOkay=rec.multiTexture?
            gx::planRigidMultiVertex(r,rec.first,*transform,colorPlan,fog,rec.fogPrimaryAlpha,gx::currentTargetExtent.logicalWidth,
                                     rec.material,rec.originalTextureCount,scales,fogUnit,rigid,context.allowMultiProjected):
            (context.allowSingleRigidShader&&gx::planRigidVertex(r,rec.first,*transform,colorPlan,fog,rec.fogPrimaryAlpha,gx::currentTargetExtent.logicalWidth,rigid,rec.rigidProjected));
        if(planOkay)++nativeFullPlans;
    }
    if(!planOkay)return false;
    if(rec.rigidProjected&&!rec.multiTexture)gx::setProjectedScales(rigid,scales[0],scales[1]);
    if(fog.enabled&&rec.fogPrimaryRed)rigid.uniforms[12][2]=2;
    // Before this shape's palette and decoded scales.
    if(nativeChainEnabled){
        chain.rowsOnly=lean;
        if(lean)std::copy_n(rigid.uniforms.begin()+81,chain.unitRows.size(),chain.unitRows.begin());
        else chain.plan=rigid;
    }
    // A compact palette is placed as the slow path placed it: the first
    // matrix kept, the rest compact in recorded order. A changed shared-normal
    // decision moves rows and refuses the replay (the normal path then draws
    // the list; the static cache keys each layout).
    if(rec.compactPalette)gx::restartCompactPalette(rigid);
    bool placed=true;
    for(unsigned n=0;placed&&n<rec.matrixCount;++n)placed=gx::addRigidMatrix(r,rec.matrixOrder[n]*3u,rigid);
    if(placed&&rec.skinEntries&&!(gx::placeSkinPalette(r,rec.generation,rigid)&&rigid.skinEntries==rec.skinEntries)){
        ++skinDrawState.replayRefusals;return false;
    }
    if(!placed||rigid.matrixOffsets!=rec.matrixOffsets||rigid.normalOffsets!=rec.normalOffsets){
        if(rec.compactPalette)if(auto* sc=gx::staticCompactCache())++sc->paletteRefusals;
        return false;
    }
    rigid.decodedNormalScale=rec.normalScale;rigid.decodedTex0Scale=rec.tex0Scale;rigid.decodedTex1Scale=rec.tex1Scale;
    rigid.origin=rec.origin;
    if(!gx::finishCompactRigidUniforms(rigid,scales[rec.rigidProjected?1:0]))return false;
    rigid.staticVertices=rec.vertices;
    // The projector may now cross these vertices: the complete path then
    // takes its CPU route for this frame.
    if(rec.rigidProjected&&!nativeProjectionPositive(rigid,rec))return false;
    rigidSpan.stop();
    static const auto submitIndex=registerGxNamedProfile("native.submit");GxNamedProfileScope submitScope(submitIndex);
    const bool byBlock=!levels&&submitBlockRows(rec,r,scales,rigid,caster);
    if(!byBlock){
        if(!submitNativeRigid(rec.raster,rec.material,rigid,rec.vertices,rec.indices,rec.indexCount,caster,false,levels)){
            rec.valid=false;return false;
        }
        if(!levels)finishBlockCapture(rec,r,context,scales,rigid);
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
            chain.scales=scales;chain.afterBlock=byBlock;chain.valid=true;
        }
    }
    ++context.submitted;++nativeHits;countReplayedFallback(rec,context);
    if(rec.skinEntries)++skinDrawState.replayed;
    if(rec.compactPalette)if(auto* sc=gx::staticCompactCache())++sc->paletteReplays;
    gxProfileTotals.rigidVertices+=rec.vertexCount;gxProfileTotals.compactVertices+=rec.vertexCount;gxProfileTotals.vertices+=rec.vertexCount;
    return true;
}
}
}
// ---------------------------------------------------------------------------
// J3D shapes: by their native records, and through the second-core worker
// (runtime/gx_worker.hpp).
namespace dusk::native {
namespace {
gx::CommandProcessor* nativeListProcessor{};
std::uint64_t nativeQueued{},nativeQueuedLists{},nativeQueuedListFailures{};
// Primitive runs of the game thread's processor (grass, sky, particles and
// whatever else is not a J3D shape) go to the worker's queue only while the
// painter walks its 3D lists (duskNativeDeferRuns): the 2D passes behind
// them record the GPU commands of their draws as they are issued (hud_tape,
// screen_tape). A run whose arrays are not in immutable memory is taken only
// where the code that issues it declared them stable for the rest of the
// picture (duskNativeArraysStable).
bool nativeDeferRuns{},nativeLateRuns{true};
// Alpha 15 (gx-overlap.txt 2): the deferral goes on through the bloom and the
// 2D passes up to the painter's end, so the painter ends without a GPU use of
// the game thread and the worker can go on drawing into the next logic tick.
// The command tapes of those passes are then queued jobs (HUD tape) or not
// used (retained screens are drawn as plain runs).
bool nativeLate2D{};
// A retained screen being recorded draws its runs here (suspendNativeRuns).
unsigned nativeRunsSuspended{};
unsigned nativeStableArrays{};
std::uint64_t nativeQueuedRuns{},nativeRunsRefusedArrays{};
// Conditional sections (gx_worker_sync.hpp beginGxSection). Game side: the
// switch and whether a section is open (its begin queued, its end not yet).
// Drawing side: whether the open section's condition failed, so its shapes
// are left out. It moves between the two threads with the queue, like the
// copy of the register bank: written by the thread that consumes records,
// read by the game thread only after a sync.
bool nativeSectionsOn{true},nativeSectionOpen{};
bool nativeSectionSkipping{};
std::uint64_t nativeSectionsQueued{},nativeSectionRunsDropped{};
std::atomic<std::uint32_t> nativeSectionsSkipped{},nativeSectionShapesSkipped{};
// Shapes are queued only while the painter paints (setNativePainterOpen):
// actors run after it, and they free and rewrite what a queued shape reads.
bool nativePainterOpen{true};
// A static shape (generation != 0) by its records, under the bank `r` it was
// issued with. True: the shape needs nothing more in this picture (every
// batch was replayed, or a part of them: the list is then left out of this
// picture and recorded again in the next). False: nothing was submitted and
// the display list has to be drawn.
// Levels of detail of one recorded batch (the thread that draws): the
// level's index list in the resident pool, after a one-time check that the
// runtime's batch is the one the levels were cut for. Null: the full list.
const std::uint16_t* lodIndices(model_lod::Batch& b,unsigned level,const NativeShapeRecord& rec,unsigned& count,model_lod::DrawCounters& counted){
    if(!level||level>model_lod::levels)return nullptr;
    if(!b.checked){
        b.checked=rec.vertexCount==b.vertexCount&&rec.indexCount==b.fullIndices&&rec.indices&&
            modelLodIndexHash(rec.indices,rec.indexCount)==b.fullHash?1:2;
    }
    if(b.checked!=1){++counted.refusedBatch;return nullptr;}
    // A level that is the full list for this batch.
    unsigned l=level-1;
    while(!b.count[l]&&l>0)--l;
    if(!b.count[l])return nullptr;
    if(!b.resident[l]||b.epoch[l]!=residentIndexEpoch()){
        b.resident[l]=allocateResidentIndices(b.source[l],b.count[l]);
        b.epoch[l]=residentIndexEpoch();
        if(!b.resident[l]){++counted.refusedPool;return nullptr;}
    }
    count=b.count[l];
    return b.resident[l];
}
bool drawRecordedShape(std::uint32_t generation,const void* list,const gx::CommandRegisters& r,unsigned caster,
                       const void* lodGroup=nullptr,unsigned lodLevels=0){
    auto* cache=gx::staticCompactCache();
    if(!nativeContext||!cache){++nativeMisses;return false;}
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
    // Levels apply to the list they were looked up for, cut into as many
    // batches as the records have.
    auto* lod=static_cast<model_lod::Group*>(const_cast<void*>(lodGroup));
    if(lod&&(lod->list!=list||lod->batches!=batches))lod=nullptr;
    const unsigned sceneLevel=lodLevels&15,shadowLevel=caster?(lodLevels>>4)&15:0;
    model_lod::DrawCounters counted;
    for(unsigned n=0;n<batches;++n){
        NativeRigidLevels levels;
        if(lod&&(sceneLevel||shadowLevel)){
            auto& b=lod->batch[n];
            const auto& rec=*records[n];
            ++counted.batches;
            levels.scene=lodIndices(b,sceneLevel,rec,levels.sceneCount,counted);
            // The caster takes the coarser of the two (it is never culled).
            if(shadowLevel>sceneLevel)levels.caster=lodIndices(b,shadowLevel,rec,levels.casterCount,counted);
            if(!levels.caster&&caster){levels.caster=levels.scene;levels.casterCount=levels.sceneCount;}
        }
        if(nativeReplay(*records[n],r,context,caster,levels.scene||levels.caster?&levels:nullptr))continue;
        // Nothing submitted yet: the normal path draws the whole list. After a
        // partial replay the list must not be drawn twice; drop its records
        // and let the next frame take the normal path.
        for(unsigned m=0;m<batches;++m)records[m]->valid=false;
        ++nativeMisses;
        if(counted.batches)model_lod::countDraw(counted);
        return n!=0;
    }
    if(counted.batches)model_lod::countDraw(counted);
    return true;
}
// The groups of a merged skin list one by one, after the skin route refused
// the list (skinDrawState.refused): each group's slots get the palette entries
// its vertices name, as the game thread computed them (position rows, and the
// same 3x3 as normal rows: the bones of a merged list are not scaled), then
// its own list runs on the list processor under the issued bank. Their
// vertices are decoded each time (no static hint): an exception, not a path
// to keep records for. A list of which a batch was drawn already is not
// drawn again (merged lists are planned as one batch: `partial` stays 0).
bool skinFallback(const gxworker::QueuedShape& shape,const gx::CommandRegisters& bank,
                  const gx::CommandProcessor::ArrayBindings& arrays,gx::CommandProcessor* processor){
    if(skinDrawState.batches){++skinDrawState.partial;return true;}
    const auto* header=skin::headerOf(shape.list,shape.generation);
    const unsigned entries=bank.xf[skin::xfHeader]&0xffff;
    if(!processor||!header||!bank.xfValid[skin::xfHeader]||(bank.xf[skin::xfHeader]>>16)!=skin::headerMagic||
       bank.xf[skin::xfList]!=shape.generation||entries!=header->entries||!processor->adopt(bank,arrays)){++skinDrawState.lost;return false;}
    ++skinDrawState.fallbacks;
    for(unsigned g=0;g<header->groups;++g){
        const auto& group=header->group[g];
        for(unsigned slot=0;slot<skin::slots;++slot){
            const unsigned entry=group.entry[slot];
            if(entry==0xff)continue;
            if(entry>=entries){++skinDrawState.lost;return false;}
            const std::uint32_t* position=bank.xf.data()+skin::xfMatrices+12*entry;
            std::array<std::uint32_t,9> normal;
            for(unsigned row=0;row<3;++row)for(unsigned c=0;c<3;++c)normal[3*row+c]=position[4*row+c];
            if(!processor->writeXf(std::uint16_t(12*slot),{position,12})||!processor->writeXf(std::uint16_t(0x400+9*slot),normal)){
                ++skinDrawState.lost;return false;
            }
        }
        duskNativeStaticGeometry(0,nullptr);
        if(!processor->execute({group.list,group.bytes})){++skinDrawState.lost;return false;}
        ++skinDrawState.fallbackGroups;
    }
    return true;
}
// The consumer of the worker's queue (the worker, or the game thread at a
// sync): one shape as J3DShape draws it on the game thread, under the bank
// and the array bindings it was issued with. By its records when it has
// them; otherwise its display list runs on the list processor, which adopts
// that bank and those bindings (the game thread's own processor is not
// involved: it went on with the next material long ago).
void drawQueuedShape(const gxworker::QueuedShape& shape,const gx::CommandRegisters& bank,
                     const gx::CommandProcessor::ArrayBindings& arrays,const gx::TargetExtent& extent){
    // Measured dump: the draws of this shape belong to its owner, and so does
    // the time this thread spends on it (gx_profile.hpp, kind 3). A job has
    // no owner: its draws and its time go to "nobody's model". Everything
    // the consumer draws is inside this scope, so the thread that owns the
    // GPU never labels a draw with what the game thread is painting now.
    struct OwnerScope {
        const bool outerReplaying=gxReplaying;const std::uint16_t outerOwner=gxReplayOwner;
        const bool timed=gxProcessProfiling||gxSparseVertexProfiling;std::uint64_t start{};
        explicit OwnerScope(std::uint32_t owner){
            gxReplaying=true;gxReplayOwner=std::uint16_t(owner<gxProcessProfileCapacity?owner:0);
            if(timed)start=gxProfileTick();
        }
        ~OwnerScope(){
            if(timed&&!outerReplaying){gxProcessTicks[3][gxReplayOwner]+=std::uint32_t(gxProfileTick()-start);++gxProcessCalls[3][gxReplayOwner];}
            gxReplaying=outerReplaying;gxReplayOwner=outerOwner;
        }
    } ownerScope(shape.owner);
    if(shape.job){
        // A job between two shapes (queueNativeJob): the same target.
        const auto savedExtent=gx::currentTargetExtent;gx::currentTargetExtent=extent;
        if(shape.job==gxJobSectionBegin){
            // Every draw issued before the section has been drawn: the
            // condition reads what they left (gx_worker_sync.hpp).
            unsigned condition=0;
            if(shape.listBytes==sizeof(condition))std::memcpy(&condition,shape.list,sizeof(condition));
            nativeSectionSkipping=condition==gxSectionFrameCopyMade&&!gx::watchedCaptureUsedThisFrame();
            if(nativeSectionSkipping)nativeSectionsSkipped.fetch_add(1,std::memory_order_relaxed);
        }else if(shape.job==gxJobSectionEnd)nativeSectionSkipping=false;
        // Jobs inside a section run whatever its condition: they are not
        // part of what the game would have left out.
        else if(shape.job==gxJobShadowCasters)runGxShadowCasterJob(shape.list,shape.listBytes);
        else if(shape.job==gxJobCopyTex)runGxCopyTexJob(shape.list,shape.listBytes,bank);
        else if(shape.job==gxJobInvalidateTextures)invalidateGxTextures(); // GXInvalidateTexAll; this thread has the GPU
        else if(shape.job==gxJobBloom)runGxBloomJob(shape.list,shape.listBytes);
        else if(shape.job==gxJobShadowArm)runGxShadowArmJob();
        else if(shape.job==gxJobHudTape)runHudTapeJob(shape.list,shape.listBytes);
        else if(shape.job==gxJobScreenTape)runScreenTapeJob(shape.list,shape.listBytes);
        gx::currentTargetExtent=savedExtent;
        return;
    }
    // A shape of a section whose condition failed: the game would not have
    // issued it (its register words were applied all the same).
    if(nativeSectionSkipping){nativeSectionShapesSkipped.fetch_add(1,std::memory_order_relaxed);return;}
    // Report lines first: their cost stays out of the measured scope.
    if(gxSparseVertexProfiling){reportNativeReplay();reportDrawGaps(true);}
    reportPicaBlocks();
    static const auto index=registerGxNamedProfile("native.replay");GxNamedProfileScope profile(index);
    // Each thread has its own current target, static geometry hint and
    // shadow caster (gx_target_extent.hpp, gx_compact_cache.hpp, gx_pica.cpp).
    const auto savedExtent=gx::currentTargetExtent;gx::currentTargetExtent=extent;
    if(!shape.generation||!drawRecordedShape(shape.generation,shape.list,bank,shape.casterSlot,shape.lodGroup,shape.lodLevels)){
        ++nativeQueuedLists;
        auto* processor=nativeListProcessor;
        const auto savedHint=gx::staticGeometryHint;const unsigned savedCaster=gxShadowCasterSlot();
        setGxShadowCasterSlot(shape.casterSlot);
        duskNativeStaticGeometry(shape.generation,shape.list);
        skinDrawState.refused=false;skinDrawState.batches=0;
        bool drawn=processor&&shape.list&&shape.listBytes&&processor->executeQueued(
            {static_cast<const u8*>(shape.list),shape.listBytes},bank,arrays);
        duskNativeStaticGeometry(0,nullptr);
        if(skinDrawState.refused&&skin::mergedGeneration(shape.generation))drawn=skinFallback(shape,bank,arrays,processor);
        gx::staticGeometryHint=savedHint;setGxShadowCasterSlot(savedCaster);
        if(!drawn){
            ++nativeQueuedListFailures;
            static unsigned reports;
            if(reports<4){++reports;OSReport("Native GX worker: list %p (%lu bytes, generation %lu) not drawn, command error %u at %lu\n",shape.list,
                static_cast<unsigned long>(shape.listBytes),static_cast<unsigned long>(shape.generation),
                processor?unsigned(processor->error()):0u,static_cast<unsigned long>(processor?processor->errorOffset():0));}
        }
    }
    gx::currentTargetExtent=savedExtent;
}
}
namespace {
#if defined(__3DS__)
// Native packets (runtime/native_packets.hpp): the second-core worker's copy
// of the game bank lives in this processor (gx_worker.hpp Adapter::mirror).
gx::CommandProcessor* nativeMirror{};
void nativeApply(const std::uint32_t* body,std::size_t words,gx::CommandProcessor& mirror,const gx::TargetExtent& extent){
    static const auto index=registerGxNamedProfile("native.packetReplay");GxNamedProfileScope profile(index);
    packets::replay(body,words,mirror,extent,drawQueuedShape,nullptr);
}
#endif
// The levels J3DShape chose for the matrix group it draws next (game
// thread, duskNativeLodSelect): queued with the shape, or used right here.
const void* nativeLodGroup{};
unsigned nativeLodLevels{};
// duskNativeDrawShape's own path: queue the shape for the worker, or draw it
// from its records here. False: the caller draws its display list.
bool drawShapeNormal(std::uint32_t generation,const void* list,std::uint32_t size){
    auto* stream=gx::commandStream();
    if(!stream||!stream->idle()){if(generation)++nativeMisses;return false;}
    auto& queue=gxworker::queue();
    if(nativePainterOpen&&queue.accepting()&&size&&nativeContext){
        // The worker decides between the records and the list: it owns the
        // record table while shapes are queued.
        static const auto queueIndex=registerGxNamedProfile("native.queue");GxNamedProfileScope profile(queueIndex);
        gxworker::QueuedShape shape;
        shape.list=list;shape.listBytes=size;shape.generation=generation;shape.casterSlot=gxShadowCasterSlot();shape.owner=gxPaintOwner;
        shape.lodGroup=nativeLodGroup;shape.lodLevels=nativeLodLevels;
        if(queue.push(stream->registers(),stream->bankStores(),stream->arrayBindings(),gx::currentTargetExtent,shape)){
            stream->restartBankLog();++nativeQueued;return true;
        }
    }
    if(!generation)return false;
    queue.sync(gxworker::SyncCause::draw);
    // Report lines first: their cost stays out of the measured scope.
    if(gxSparseVertexProfiling){reportNativeReplay();reportDrawGaps(true);}
    reportPicaBlocks();
    static const auto index=registerGxNamedProfile("native.shape");GxNamedProfileScope profile(index);
    return drawRecordedShape(generation,list,stream->registers(),gxShadowCasterSlot(),nativeLodGroup,nativeLodLevels);
}
}
#if defined(__3DS__)
gxworker::Adapter nativeWorkerAdapter(){return {drawQueuedShape,nativeMirror,nativeMirror?nativeApply:nullptr};}
#else
gxworker::Adapter nativeWorkerAdapter(){return {drawQueuedShape};}
#endif
// CommandProcessor::RunDeferral of the game thread's processor (`context`).
namespace {
bool queueNativeRun(std::span<const u8> run,bool arraysFixed,void* context){
    auto& queue=gxworker::queue();
    if(!nativeDeferRuns||nativeRunsSuspended||!nativePainterOpen||!queue.accepting()||!nativeContext)return false;
    if(!arraysFixed&&!nativeStableArrays){++nativeRunsRefusedArrays;return false;}
    // A J3D list on the normal path: its batches are numbered as they are drawn.
    if(gx::staticGeometryHint.list)return false;
    auto& processor=*static_cast<gx::CommandProcessor*>(context);
    gxworker::QueuedShape shape;
    shape.casterSlot=gxShadowCasterSlot();shape.owner=gxPaintOwner;
    if(!queue.push(processor.registers(),processor.bankStores(),processor.currentArrayBindings(),gx::currentTargetExtent,shape,run.data(),run.size()))
        return false;
    processor.restartBankLog();++nativeQueuedRuns;
    return true;
}
}
bool deferNativeRun(std::span<const u8> run,bool arraysFixed,void* context){
    if(queueNativeRun(run,arraysFixed,context))return true;
    // Drawn right here: inside a conditional section only when its condition
    // held. The sync has every earlier record drawn, the section's begin
    // among them, on whichever thread (its answer travels with the queue).
    if(nativeSectionOpen)[[unlikely]]{
        gxworker::sync(gxworker::SyncCause::draw);
        if(nativeSectionSkipping){++nativeSectionRunsDropped;return true;}
    }
    return false;
}
void nativeRunStatistics(std::uint64_t& queued,std::uint64_t& refusedForArrays){queued=nativeQueuedRuns;refusedForArrays=nativeRunsRefusedArrays;}
// gxWorkerJobHook (gx_worker_sync.hpp): the job goes behind everything the
// game thread has issued, so what its command stream still holds is issued
// first.
bool queueNativeJob(unsigned job,const void* arguments,std::size_t bytes){
#if defined(__3DS__)
    packets::endCapture(); // the job goes behind the shapes of a packet being captured
#endif
    auto* stream=gx::commandStream();auto& queue=gxworker::queue();
    if(!job||!arguments||!bytes||!stream||!nativePainterOpen||!queue.accepting()||!nativeContext||!stream->finish())return false;
    gxworker::QueuedShape shape;shape.job=job;
    if(!queue.push(stream->registers(),stream->bankStores(),stream->arrayBindings(),gx::currentTargetExtent,shape,arguments,bytes))return false;
    stream->restartBankLog();
    return true;
}
// gxWorkerSectionHook (gx_worker_sync.hpp): `condition` 0 ends the open
// section. A section is opened only where the draws inside it can be queued
// (the painter's deferred runs): elsewhere each would sync anyway.
bool nativeSection(unsigned condition){
    if(!condition){
        if(!nativeSectionOpen)return false;
        nativeSectionOpen=false;
        const unsigned none=0;
        if(queueNativeJob(gxJobSectionEnd,&none,sizeof(none)))return true;
        // Not queued (cannot happen while the painter paints): the game thread
        // is the drawing side after the sync and ends the section itself.
        gxworker::sync(gxworker::SyncCause::other);
        nativeSectionSkipping=false;
        return true;
    }
    if(!nativeSectionsOn||nativeSectionOpen||!nativeDeferRuns)return false;
    if(!queueNativeJob(gxJobSectionBegin,&condition,sizeof(condition)))return false;
    nativeSectionOpen=true;++nativeSectionsQueued;
    return true;
}
void setNativeSections(bool on){nativeSectionsOn=on;}
void nativeSectionStatistics(std::uint64_t& queued,std::uint64_t& skipped,std::uint64_t& shapesSkipped,std::uint64_t& runsDropped){
    queued=nativeSectionsQueued;skipped=nativeSectionsSkipped.load(std::memory_order_relaxed);
    shapesSkipped=nativeSectionShapesSkipped.load(std::memory_order_relaxed);runsDropped=nativeSectionRunsDropped;
}
void setNativeListProcessor(gx::CommandProcessor* processor){
    nativeListProcessor=processor;
#if defined(__3DS__)
    // The worker's processor for native packet records: the arrays resolve
    // as for the list processor (the game processor's registry and
    // resolver); it never draws (no sink): its records hold register work
    // only, the shapes go through drawQueuedShape.
    if(processor&&!nativeMirror){
        nativeMirror=new(std::nothrow)gx::CommandProcessor(processor->memoryRegistry());
        if(nativeMirror)nativeMirror->setLiveArrayResolver(processor->liveResolver());
    }
    packets::Hooks hooks;
    hooks.canQueue=+[]{auto& queue=gxworker::queue();return nativePainterOpen&&nativeContext&&queue.accepting()&&queue.nativeAvailable();};
    hooks.drawShape=drawShapeNormal;
    hooks.casterSlot=gxShadowCasterSlot;hooks.setCasterSlot=setGxShadowCasterSlot;
    hooks.staticGeometry=duskNativeStaticGeometry;
    hooks.now=gxProfileTick;hooks.ticksPerMillisecond=268112; // ARM11 system ticks
    hooks.report=+[](const char* line){OSReport("%s",line);};
    if(!nativeMirror)OSReport("Native packets: no memory for the worker's processor; packets stay on the original path\n");
    else packets::install(hooks);
#endif
}
// The painter's end closes the queue for the picture and ends a deferral the
// 2D passes kept (setNativeLate2D): the next picture starts as before.
void setNativePainterOpen(bool open){nativePainterOpen=open;if(!open){nativeDeferRuns=false;nativeRunsSuspended=0;}}
void setNativeLate2D(bool on){nativeLate2D=on;}
void nativeWorkerStatistics(std::uint64_t& queued,std::uint64_t& lists,std::uint64_t& failures){
    queued=nativeQueued;lists=nativeQueuedLists;failures=nativeQueuedListFailures;
}
}
// J3DShape, for every display list of a shape: true when the list needs
// nothing more (it was queued for the worker, or replayed from its records
// right here, or it became part of a captured native packet); false means
// "draw it through the normal display-list path".
// `generation`: the vertex-data generation of a shape that draws its original
// archive arrays, 0 for one whose arrays the CPU rewrites. `size`: the bytes
// of `list`.
// The drawing side's skin palette counters (SkinDrawState), for the game
// thread's report line every 300 pictures (J3DNativeSkinPalette.h): totals
// since the start, read without a sync (a count one draw late at worst).
extern "C" void duskNativeSkinDrawReport(void){
#if defined(__3DS__)
    const auto& d=dusk::native::skinDrawState;
    OSReport("Native skin palette, drawing side since start: records %llu, replays %llu (refused %llu); lists refused: header %llu "
             "program %llu lights %llu rows %llu matrix %llu decode %llu; drawn group by group %llu (%llu groups), partial %llu, lost %llu\n",
             d.recorded,d.replayed,d.replayRefusals,d.refusals[0],d.refusals[1],d.refusals[2],d.refusals[3],d.refusals[4],d.refusals[5],
             d.fallbacks,d.fallbackGroups,d.partial,d.lost);
#endif
}
extern "C" bool duskNativeDrawShape(std::uint32_t generation,const void* list,std::uint32_t size){
    using namespace dusk::native;
#if defined(__3DS__)
    if(packets::capturing()&&packets::captureShape(generation,list,size,gxShadowCasterSlot()))return true;
#endif
    return drawShapeNormal(generation,list,size);
}
// J3DShape: the levels of the matrix group drawn next (runtime/model_lod.hpp).
extern "C" void duskNativeLodSelect(const void* group,unsigned sceneLevel,unsigned shadowLevel){
    dusk::native::nativeLodGroup=group;
    dusk::native::nativeLodLevels=(sceneLevel&15)|((shadowLevel&15)<<4);
}
#if defined(__3DS__)
// J3DMatPacket::draw around a material packet (runtime/native_packets.hpp).
extern "C" bool duskNativePacketBegin(int kind){return dusk::native::packets::begin(kind);}
extern "C" void duskNativePacketEnd(void){dusk::native::packets::end();}
// daBg_c::createHeap: the SD switch, once.
extern "C" void duskNativePacketsConfigure(void){dusk::native::packets::configureFromSd();}
// Direct records (runtime/native_packets.hpp; J3DPacket.cpp, J3DShape.cpp).
extern "C" bool duskNativeDirectEnabled(void){return dusk::native::packets::directEnabled();}
extern "C" bool duskNativeDirectVerifying(void){return dusk::native::packets::directVerifying();}
extern "C" bool duskNativeDirectBegin(int kind){return dusk::native::packets::directBegin(kind);}
extern "C" bool duskNativeDirectOpen(void){return dusk::native::packets::directOpen();}
extern "C" bool duskNativeDirectList(const void* list,std::uint32_t size){return dusk::native::packets::directList(list,size);}
extern "C" bool duskNativeDirectCurrentMtx(std::uint32_t a,std::uint32_t b){return dusk::native::packets::directCurrentMatrix(a,b);}
extern "C" bool duskNativeDirectBind(unsigned slot,const void* data,std::uint32_t size,std::uint32_t stride){
    return dusk::native::packets::directBind(slot,data,size,stride);
}
extern "C" bool duskNativeDirectIndexed(unsigned code,unsigned index,unsigned packed){return dusk::native::packets::directIndexed(code,index,packed);}
extern "C" bool duskNativeDirectShape(std::uint32_t generation,const void* list,std::uint32_t size){
    return dusk::native::packets::directShape(generation,list,size);
}
extern "C" void duskNativeDirectEnd(std::uint32_t stateHash){dusk::native::packets::directEnd(stateHash);}
extern "C" void duskNativeDirectAbort(const char* why){dusk::native::packets::directAbort(why);}
extern "C" void duskNativeDirectFallback(const char* why){dusk::native::packets::directFallback(why);}
extern "C" void duskNativeDirectRealState(std::uint32_t stateHash){dusk::native::packets::directRealState(stateHash);}
#endif
// DUSK_PICA_GAME_DRAW model dispatch (J3DPacket.cpp drawNativeGameShape),
// around each native model shape. The game thread is about to draw on the
// PICA itself and to reset the PICA state caches (applied state, bound
// program, uniform owner, texture and geometry bindings, alpha interval
// flags) that the second-core worker uses while it draws queued shapes: the
// worker is synced first (everything queued is drawn, the worker is parked;
// its writes are released by the queue's handshake and acquired here), so
// the game thread owns the GPU and those caches until it queues again. Their
// own functions: the shared-state audit takes them as barriers.
extern "C" [[gnu::noinline]] bool duskNativeGameDrawBegin(void** target){
    using namespace dusk::native;
    syncGxWorker();
    *target=activeFramebuffer();
    return alphaIntervalCopySafe();
}
extern "C" [[gnu::noinline]] void duskNativeGameDrawEnd(void){
    using namespace dusk::native;
    syncGxWorker();
    invalidatePicaCommandState();
}
// The painter, around its 3D lists: primitive runs issued in between may go
// to the second-core worker (deferNativeRun). 1 before the sky, 2 behind the
// translucent lists, 0 before the bloom and the 2D passes. With late runs
// (Alpha 14, default) 2 leaves them on through the 3D passes in between:
// projection and screen particles, filter lists, the frame copies. Those
// handed the whole queue back at their first particle (the refraction
// question, or a run drawn here); the 2D passes record their GPU commands as
// they are issued (hud_tape, screen_tape), so 0 comes before them.
extern "C" void duskNativeDeferRuns(int on){
    using namespace dusk::native;
    if(on==2){if(nativeLateRuns)return;on=0;}
    // 0 before the bloom and the 2D passes: kept on with the late 2D passes
    // (the painter's end turns it off, setNativePainterOpen).
    if(!on&&nativeLate2D&&nativeDeferRuns)return;
    nativeDeferRuns=on!=0;
    if(on)configurePicaBlocks();
}
namespace dusk::native {
void setNativeLateRuns(bool on){nativeLateRuns=on;}
// A command tape must not record while runs go to the queue: the words of
// those draws would be missing from it (hud_tape_ctru.cpp, screen_tape_ctru.cpp).
// Alpha 15: and the queue takes them (a resting worker draws them here, the
// tapes may record as without a worker).
bool nativeRunsDeferred(){return nativeDeferRuns&&!nativeRunsSuspended&&nativePainterOpen&&gxworker::queue().accepting();}
void suspendNativeRuns(bool on){if(on)++nativeRunsSuspended;else if(nativeRunsSuspended)--nativeRunsSuspended;}
}
// Game code, around draws whose vertex arrays stay as they are until the
// picture is painted (static tables that are not constant data): their runs
// may be read later by the second-core worker. Calls nest.
extern "C" void duskNativeArraysStable(int on){
    using namespace dusk::native;
    if(on)++nativeStableArrays;
    else if(nativeStableArrays)--nativeStableArrays;
}
