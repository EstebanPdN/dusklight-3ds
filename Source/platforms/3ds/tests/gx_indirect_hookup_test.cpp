// Host test of the A19 hookup in gx_command_draw.cpp: the real draw adapter,
// with the water-indirect patch applied and the GPU calls substituted, takes
// warped draws to the multi-texture GPU route with constant offsets, or
// translates them again with the per-vertex warp. Runner:
// Tools/3ds/test_gx_indirect.py --hookup-patch (builds the patched copy).
#include "../gx_command_draw.hpp"
#include "../gx_geometry_pica.hpp"
#include "../gx_texture_pica.hpp"
#include "../gx_pica.hpp"
#include "../runtime/gx_stream.hpp"
#include "../runtime/gx_indirect_gpu.hpp"
#include "../runtime/gx_profile.hpp"
#include "../runtime/gx_framebuffer.hpp"
#include <dolphin/gd.h>
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <bit>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
using namespace dusk::native;
using namespace dusk::native::gx;
extern "C" void OSPanic(const char*,int,const char* message,...){throw std::runtime_error(message);}
extern "C" void DCFlushRange(void*,u32){}
extern "C" void OSReport(const char*,...){}
// The model shadow pass belongs to the renderer (gx_pica.cpp): no caster here.
extern "C" void duskNativeShadowCaster(unsigned){}
namespace {
alignas(16) std::array<std::byte,32768> storage;
GpuGeometryArena arena;
std::array<TextureHandle,3> protectedTextures{};
// What one drawGxCommand call sent: the rigid block and records, or the
// CPU route's vertices.
struct Submission {
    bool rigid{};RigidVertexPlan plan;std::vector<CompactRigidVertexMT> records;std::vector<GpuVertex> vertices;
};
std::vector<Submission> submissions;
unsigned projectionCalls;
}
namespace dusk::native {
std::uint64_t gxProfileTick(){return std::chrono::steady_clock::now().time_since_epoch().count();}
std::uint32_t residentIndexEpoch(){assert(false&&"resident replay is outside this fixture");return 0;}
unsigned gxShadowCasterSlot(){return 0;}
void setGxShadowCasterSlot(unsigned){}
void runGxShadowCasterJob(const void*,std::size_t){}
void runGxCopyTexJob(const void*,std::size_t,const gx::CommandRegisters&){}
void runGxBloomJob(const void*,std::size_t){}
void runHudTapeJob(const void*,std::size_t){} // hud_tape_ctru.cpp: no HUD tape in this fixture
void runScreenTapeJob(const void*,std::size_t){} // screen_tape_ctru.cpp: no retained screens in this fixture
void runGxShadowArmJob(){}
void invalidateGxTextures(){}
std::uint16_t* allocateResidentIndices(const std::uint16_t*,unsigned){assert(false&&"resident replay is outside this fixture");return nullptr;}
bool submitNativeRigid(const RasterPlan&,const PicaMaterialPlan&,const RigidVertexPlan&,const void*,const std::uint16_t*,unsigned,unsigned,bool,
                       const NativeRigidLevels*){
    assert(false&&"resident replay is outside this fixture");return false;}
// As in gx_command_draw_test.cpp: the game draw dispatch and the native PICA
// blocks are never used by this fixture.
[[gnu::weak]] FramebufferHandle activeFramebuffer(){return nullptr;}
[[gnu::weak]] bool alphaIntervalCopySafe(){return true;}
[[gnu::weak]] void invalidatePicaCommandState(){}
void requestPicaBlockCapture(){}
void cancelPicaBlockCapture(){}
bool takePicaBlockCapture(PicaBlockCapture&){return false;}
bool submitPicaBlock(const std::uint32_t*,unsigned,bool,const RigidVertexPlan&,const void*,const std::uint16_t*,unsigned,unsigned){
    assert(!"PICA blocks are off in this fixture");return false;
}
// A restart must release the first pass's texture scope before the second
// pass opens its own (the binder asserts on a scope left open).
GxTextureDrawScope::GxTextureDrawScope(std::span<const gx::Texture> textures):active(true){
    assert((textures.size()<=3&&protectedTextures==std::array<TextureHandle,3>{}));
    for(unsigned n=0;n<textures.size();++n)protectedTextures[n]=textures[n].handle;
}
GxTextureDrawScope::~GxTextureDrawScope(){protectedTextures={};}
bool reserveGxGeometry(std::size_t v,std::size_t i,GpuGeometryBatch& b){return arena.reserve(v,i,b);}
bool shrinkGxGeometry(GpuGeometryBatch& b,std::size_t v,std::size_t i,bool external){return arena.shrinkPending(b,v,i,external);}
bool cancelGxGeometry(const GpuGeometryBatch& b){return arena.cancel(b);}
const char* gxGeometryFailure(){return "test arena failure";}
const char* gxPicaFailure(){return "test GPU failure";}
const char* gxTextureFailure(){return "test texture failure";}
bool setGxTextureProjection(bool on){projectionCalls+=on;return true;}
bool bindGxTexture(const Texture& texture,float& u,float& v,unsigned unit,bool projective,GxWorldMipUse){
    assert(!projective||unit==0);
    assert(texture.handle&&unit<3&&protectedTextures[unit]==texture.handle);u=1;v=.5;return true;
}
bool picaRigidTriangleFogAvailable(){return false;}
bool picaAlphaUnionGpu(const RasterPlan&){return false;}
bool submitGxRigidFogGeometry(const RasterPlan&,const PicaMaterialPlan&,const RasterPlan&,const PicaMaterialPlan&,
    const GpuGeometryBatch&,const RigidVertexPlan&,const RigidVertexPlan&){assert(false);return false;}
bool submitGxTriangleFogGeometry(const RasterPlan&,const PicaMaterialPlan&,const RasterPlan&,const PicaMaterialPlan&,
    const GpuGeometryBatch&){assert(false);return false;}
bool submitGxAdditiveTextureGeometry(const RasterPlan&,const PicaMaterialPlan&,const RasterPlan&,
    const PicaMaterialPlan*,const PicaMaterialPlan&,const Texture&,const Texture&,const GpuGeometryBatch&,bool){
    assert(false);return false;
}
bool submitGxRigidGeometry(const RasterPlan&,const PicaMaterialPlan&,const GpuGeometryBatch& b,const RigidVertexPlan& p){
    assert(arena.owns(b)&&p.multiTexture&&p.compact&&!p.staticVertices&&validateCompactRigidGeometry(b));
    Submission s;s.rigid=true;s.plan=p;
    const auto* records=reinterpret_cast<const CompactRigidVertexMT*>(b.vertices.data());
    s.records.assign(records,records+b.vertices.size());
    submissions.push_back(std::move(s));return arena.commit(b);
}
bool submitGxCommandGeometry(const RasterPlan&,const PicaMaterialPlan&,const GpuGeometryBatch& b){
    assert(arena.owns(b)&&validateGpuGeometry(b));
    Submission s;s.vertices.assign(b.vertices.begin(),b.vertices.end());
    submissions.push_back(std::move(s));return arena.commit(b);
}
}

namespace {
constexpr unsigned unit0=multiUnitRow(0);
// One translation of a register variant of the captured draw.
struct Outcome {
    bool ok{};std::string failure;std::uint64_t submitted{},rejected{},culled{},vertexIndirect{};
    std::vector<Submission> submissions;unsigned projection{};
    std::array<std::uint64_t,16> gate{};
    std::uint64_t immediateMisses{},immediateStores{};
    std::uint64_t planned{},gpu{},restarts{},vertices{},perPixel{};
    std::array<std::uint64_t,unsigned(IndirectConstantRefusal::count)> refused{};
};
Outcome run(const CommandDraw& base,const CommandRegisters& original,float limit,bool gpu,bool cameraDivisor=true){
    setIndirectConstantLimit(limit);
    // A fresh state epoch per run, as a changed command stream bank has: the
    // adapter caches register hashes (immediate record keys) per epoch.
    static std::uint32_t epoch=0x40000000;
    // The cases edit copies of the bank by hand: such a bank has no kept
    // state hash (gx_commands.hpp), so the adapter hashes its words.
    auto registers=original;registers.stateEpoch=++epoch;registers.stateHash.tracked=0;
    const CommandDraw draw{base.primitive,base.vertexCount,base.indexCount,base.vertices,base.indices,base.plan,base.arrays,registers};
    CommandDrawContext context;context.allowVertexIndirect=true;context.allowRigidGpu=gpu;context.allowCameraDivisor=cameraDivisor;
    submissions.clear();projectionCalls=0;
    const auto gate=gxPerspCpuGate;const auto counters=indirectConstantCounters;
    std::uint64_t hits,misses,stores,fallbacks;immediateDrawStatistics(hits,misses,stores,fallbacks);
    assert(arena.end()&&arena.begin());
    Outcome o;o.ok=drawGxCommand(draw,&context);o.failure=context.failure?context.failure:"";
    o.submitted=context.submitted;o.rejected=context.rejected;o.culled=context.culled;o.vertexIndirect=context.vertexIndirectDraws;
    o.submissions=submissions;o.projection=projectionCalls;
    for(unsigned k=0;k<16;++k)o.gate[k]=gxPerspCpuGate[k]-gate[k];
    {std::uint64_t h,m,st,f;immediateDrawStatistics(h,m,st,f);o.immediateMisses=m-misses;o.immediateStores=st-stores;}
    const auto& c=indirectConstantCounters;
    o.planned=c.planned-counters.planned;o.gpu=c.gpu-counters.gpu;o.restarts=c.restarts-counters.restarts;
    o.vertices=c.vertices-counters.vertices;o.perPixel=c.perPixel-counters.perPixel;
    for(unsigned k=0;k<o.refused.size();++k)o.refused[k]=c.refused[k]-counters.refused[k];
    return o;
}
bool sameVertices(const Outcome& a,const Outcome& b){
    if(a.submissions.size()!=b.submissions.size())return false;
    for(std::size_t n=0;n<a.submissions.size();++n){
        const auto& x=a.submissions[n].vertices;const auto& y=b.submissions[n].vertices;
        if(a.submissions[n].rigid||b.submissions[n].rigid||x.size()!=y.size()||
           std::memcmp(x.data(),y.data(),x.size()*sizeof(GpuVertex)))return false;
    }
    return true;
}
// The CPU route's result, and what the restart must reproduce exactly.
void assertSameCpuResult(const Outcome& restarted,const Outcome& reference){
    assert(restarted.ok==reference.ok&&restarted.failure==reference.failure);
    assert(restarted.submitted==reference.submitted&&restarted.rejected==reference.rejected&&restarted.culled==reference.culled);
    assert(restarted.vertexIndirect==reference.vertexIndirect&&sameVertices(restarted,reference));
    assert(restarted.gate==reference.gate&&restarted.projection==reference.projection);
    // The repeat skips the immediate records: one lookup per draw, never stored.
    assert(restarted.immediateMisses==reference.immediateMisses&&!restarted.immediateStores&&!reference.immediateStores);
}
// gx_rigid_mt.v.pica, unit 0: role 1 on in0=(TEX0,1,1), in1=(TEX1,0,0);
// role 2 on (position,1), divided per pixel when u[92].z is set.
std::array<float,3> shaderUnit0(const RigidVertexPlan& p,const CompactRigidVertexMT& v){
    const auto& u=p.uniforms;
    auto dot=[](const std::array<float,4>& a,const std::array<float,4>& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+a[3]*b[3];};
    if(multiRole(p,0)==1){
        const std::array<float,4> in0{float(v.texcoord0[0]),float(v.texcoord0[1]),1,1},in1{float(v.texcoord1[0]),float(v.texcoord1[1]),0,0};
        return {dot(u[unit0],in0)+dot(u[unit0+1],in1),dot(u[unit0+2],in0)+dot(u[unit0+3],in1),1};
    }
    assert(multiRole(p,0)==2&&u[unit0+3][2]>0);
    const std::array<float,4> position{v.position[0],v.position[1],v.position[2],1};
    float s=dot(u[unit0],position),t=dot(u[unit0+1],position);const float q=dot(u[unit0+2],position);
    // The warp of the vertex (the record's second coordinate), times q.
    s+=q*(u[unit0+3][3]*float(v.texcoord1[0]));t+=q*(u[unit0+3][3]*float(v.texcoord1[1]));
    return {u[unit0+3][0]*s,q-u[unit0+3][1]*t,q};
}
bool near(float a,float b,float tolerance=2e-5f){return std::abs(a-b)<=tolerance*std::max(1.f,std::abs(b));}
// GPU route coordinates of unit 0 against the CPU route's.
void assertGpuMatchesCpu(const Outcome& gpu,const Outcome& cpu,bool projected,float tolerance=2e-5f){
    assert(gpu.submissions.size()==1&&gpu.submissions[0].rigid&&cpu.submissions.size()==1&&!cpu.submissions[0].rigid);
    const auto& records=gpu.submissions[0].records;const auto& vertices=cpu.submissions[0].vertices;
    assert(records.size()==vertices.size());
    for(std::size_t i=0;i<records.size();++i){
        const auto g=shaderUnit0(gpu.submissions[0].plan,records[i]);const auto& c=vertices[i];
        assert(near(g[0],c.texcoord[0][0],tolerance)&&near(g[1],c.texcoord[0][1],tolerance));
        if(projected)assert(near(g[2],c.textureQ,tolerance));
    }
}
// Every uniform row equal but the listed unit-0 entries.
void assertRowsDiffer(const RigidVertexPlan& a,const RigidVertexPlan& b,const std::vector<std::array<unsigned,2>>& allowed){
    for(unsigned row=0;row<a.uniforms.size();++row)for(unsigned c=0;c<4;++c){
        bool listed=false;for(const auto& e:allowed)listed|=e[0]==row&&e[1]==c;
        if(!listed&&std::bit_cast<u32>(a.uniforms[row][c])!=std::bit_cast<u32>(b.uniforms[row][c])){
            std::fprintf(stderr,"row %u.%u differs: %a %a\n",row,c,double(a.uniforms[row][c]),double(b.uniforms[row][c]));assert(false);}
    }
}

// Register variants of the captured draw. Warp: one indirect stage on TEV
// stage 0, map `map` sampled with coordinate 1, matrix 1 = 0.25 * a on S and
// -0.125 * b on T, bias -128 on every channel, coordinate scales 8x8.
CommandRegisters warped(const CommandRegisters& base,unsigned map){
    auto r=base;
    r.bp[0]=(r.bp[0]&~(7u<<16))|(1u<<16);
    for(unsigned reg:{6u,7u,8u,0x10u,0x25u,0x27u,0x30u,0x31u,0x32u,0x33u})r.bpValid.set(reg);
    r.bp[0x10]=0x270;
    r.bp[6]=256u|(1u<<22);r.bp[7]=(2048u-128u)<<11;r.bp[8]=1u<<22; // exponent 17: scale 1
    r.bp[0x25]=0;r.bp[0x27]=map|(1u<<3);
    for(unsigned reg=0x30;reg<=0x33;++reg)r.bp[reg]=7;
    return r;
}
// Perspective camera: the quad at z = -1, w = 1, in view.
CommandRegisters perspective(const CommandRegisters& base){
    auto r=base;
    const float projection[6]{.005f,1,-2.f/240,-1,0,-.5f};
    for(unsigned n=0;n<6;++n)r.xf[0x1020+n]=std::bit_cast<u32>(projection[n]);
    r.xf[0x1026]=0;r.xf[11]=std::bit_cast<u32>(-1.f);
    return r;
}
// Unit 0 (coordinate 0) projected from the position through texture matrix 30.
CommandRegisters projectedUnit(const CommandRegisters& base,float qSlope){
    auto r=base;
    r.xf[0x1040]=2;r.matrixIndex[0]=(r.matrixIndex[0]&~(63u<<6))|(30u<<6);
    const float matrix[12]{.0025f,0,0,.0625f, 0,.004f,0,.125f, qSlope,0,0,1};
    for(unsigned n=0;n<12;++n){r.xf[120+n]=std::bit_cast<u32>(matrix[n]);r.xfValid.set(120+n);}
    return r;
}
// Texgen 0 from the position through a 2x4 matrix (input form ABC1): the
// multi-texture plan has no rows for it (reject 10), the CPU route does.
CommandRegisters fromPosition(const CommandRegisters& base){auto r=base;r.xf[0x1040]=4;return r;}
// Unit 0 projected through the camera: the view depth falls from 1 at x = 0
// to -1 at x = 400 (w = 1 - 0.005 x - z), and the texture matrix takes
// twice that w as its divisor.
CommandRegisters cameraProjector(const CommandRegisters& base){
    auto r=base;
    r.xf[8]=std::bit_cast<u32>(.005f);
    r.xf[0x1040]=2;r.matrixIndex[0]=(r.matrixIndex[0]&~(63u<<6))|(30u<<6);
    const float matrix[12]{.0025f,0,0,.0625f, 0,.004f,0,.125f, -.01f,0,-2,2};
    for(unsigned n=0;n<12;++n){r.xf[120+n]=std::bit_cast<u32>(matrix[n]);r.xfValid.set(120+n);}
    return r;
}

constexpr float o0=2.f/8,o1=-1.f/8; // uniform map 136: warp 0.25*8 = 2 and -0.125*8 = -1 texels on 8x8
constexpr float su=1,sv=.5f;        // the fixture binder's texture scales

unsigned cases;
// List A: POS, CLR0, TEX0 and TEX1 (the compact multi-texture decode takes it).
void casesA(const CommandDraw& draw){
    const auto plainRegisters=draw.registers;
    // 1. Exact mode (the default) with a uniform map: GPU route, the unit rows
    // are the plain plan's plus su*o and -sv*o, and the GPU coordinates equal
    // the CPU route's warped ones.
    const auto plainGpu=run(draw,plainRegisters,0,true),plainCpu=run(draw,plainRegisters,0,false);
    assert(plainGpu.ok&&plainGpu.submissions.size()==1&&plainGpu.submissions[0].rigid);
    assertGpuMatchesCpu(plainGpu,plainCpu,false); // the shader model itself
    const auto uniform=warped(plainRegisters,1);
    const auto off=run(draw,uniform,-1,true);
    assert(off.ok&&off.vertexIndirect==1&&off.submissions.size()==1&&!off.submissions[0].rigid&&!off.planned&&!off.restarts);
    for(std::size_t i=0;i<off.submissions[0].vertices.size();++i){
        const auto& w=off.submissions[0].vertices[i].texcoord[0];const auto& p=plainCpu.submissions[0].vertices[i].texcoord[0];
        assert(near(w[0],p[0]+su*o0)&&near(w[1],p[1]-sv*o1));
    }
    const auto exact=run(draw,uniform,0,true);
    assert(exact.ok&&exact.submitted==1&&!exact.vertexIndirect&&exact.planned==1&&exact.gpu==1&&!exact.restarts&&exact.vertices==4&&!exact.perPixel);
    assert(exact.submissions.size()==1&&exact.submissions[0].rigid&&!exact.projection);
    const auto& folded=exact.submissions[0].plan;const auto& plain=plainGpu.submissions[0].plan;
    assertRowsDiffer(folded,plain,{{unit0,3},{unit0+2,3}});
    assert(near(folded.uniforms[unit0][3],plain.uniforms[unit0][3]+su*o0,1e-6f));
    assert(near(folded.uniforms[unit0+2][3],plain.uniforms[unit0+2][3]-sv*o1,1e-6f));
    assert(!std::memcmp(exact.submissions[0].records.data(),plainGpu.submissions[0].records.data(),4*sizeof(CompactRigidVertexMT)));
    assertGpuMatchesCpu(exact,off,false);
    ++cases;
    // 1b. Stale texture coordinate scales (BP 0x30-0x33). This port leaves
    // the SDK's automatic update out, so after typed GX draws the registers
    // keep an older display list's values: Alpha 13 console dump 007 (file
    // select) held 304x224 on coordinate 0 while the selected-slot light
    // warped its 72x96 mask there, drawn as a whitish rectangle. The warp
    // takes the sampled textures' sizes, as the plain texture route does:
    // both routes give exactly the result of the bank with the true scales.
    {auto stale=uniform;
     for(unsigned coordinate=0;coordinate<2;++coordinate){stale.bp[0x30+2*coordinate]=0xc012f;stale.bp[0x31+2*coordinate]=0xdf;}
     const auto staleOff=run(draw,stale,-1,true),staleExact=run(draw,stale,0,true);
     assert(staleOff.ok&&staleOff.vertexIndirect==1&&sameVertices(staleOff,off));
     assert(staleExact.ok&&staleExact.planned==1&&staleExact.gpu==1&&!staleExact.vertexIndirect);
     assert(!staleExact.refused[unsigned(IndirectConstantRefusal::scale)]&&staleExact.submissions.size()==1&&staleExact.submissions[0].rigid);
     assertRowsDiffer(staleExact.submissions[0].plan,folded,{});
     assert(staleExact.submissions[0].records.size()==exact.submissions[0].records.size()&&
            !std::memcmp(staleExact.submissions[0].records.data(),exact.submissions[0].records.data(),
                         exact.submissions[0].records.size()*sizeof(CompactRigidVertexMT)));}
    ++cases;
    // 2. A map within 0.25 * (136 - 130) < 3 texels of its mean: exact mode
    // refuses it (CPU route, exactly the off result); approximate mode (3)
    // takes the mean.
    const auto varied=warped(plainRegisters,2);
    const auto variedOff=run(draw,varied,-1,true),variedExact=run(draw,varied,0,true);
    assert(variedExact.refused[unsigned(IndirectConstantRefusal::deviation)]==1&&!variedExact.planned);
    assertSameCpuResult(variedExact,variedOff);
    const auto approximate=run(draw,varied,3,true);
    assert(approximate.ok&&approximate.gpu==1&&!approximate.vertexIndirect&&approximate.submissions[0].rigid);
    // The offset is the map's mean warp (gx_indirect_gpu_test.cpp checks the
    // statistics): here 0.25 * (mean - 128) texels on S, -0.125 * (mean - 128) on T.
    const auto& last=indirectConstantCounters.last;
    double sum=0;for(unsigned n=0;n<64;++n)sum+=130+n%7;
    const float mean=float(sum/64)-128;
    assert(near(last.offset[0],.25f*mean/8,1e-6f)&&near(last.offset[1],-.125f*mean/8,1e-6f));
    assert(near(approximate.submissions[0].plan.uniforms[unit0][3],plain.uniforms[unit0][3]+su*last.offset[0],1e-6f));
    assert(near(approximate.submissions[0].plan.uniforms[unit0+2][3],plain.uniforms[unit0+2][3]-sv*last.offset[1],1e-6f));
    assert(last.deviation>0&&last.deviation<3);
    ++cases;
    // 2b. Alpha 9: the rest of the warp is sampled per vertex into the
    // record's second coordinate, so the GPU coordinates are the CPU route's
    // warped ones (the map read at each vertex). The mean alone, which the
    // switch restores, misses them.
    const auto& sampled=approximate.submissions[0].plan;
    assert(sampled.vertexWarp.enabled&&sampled.texcoordAttribute[1]==0xff&&!sampled.wideRecord);
    assertGpuMatchesCpu(approximate,variedOff,false,5e-4f);
    vertexWarpEnabled=false;
    const auto meanOnly=run(draw,varied,3,true);
    vertexWarpEnabled=true;
    assert(meanOnly.ok&&meanOnly.gpu==1&&meanOnly.submissions[0].rigid&&!meanOnly.submissions[0].plan.vertexWarp.enabled);
    {bool missed=false;
     for(std::size_t i=0;i<meanOnly.submissions[0].records.size();++i){
        const auto g=shaderUnit0(meanOnly.submissions[0].plan,meanOnly.submissions[0].records[i]);
        const auto& c=variedOff.submissions[0].vertices[i];
        missed|=!near(g[0],c.texcoord[0][0],5e-4f)||!near(g[1],c.texcoord[0][1],5e-4f);
     }
     assert(missed);}
    ++cases;
    // 3. Perspective: on the GPU route the draw counts in no perspective CPU
    // gate (with the path off it counts once, under the indirect gate 9).
    const auto view=warped(perspective(plainRegisters),1);
    const auto viewOff=run(draw,view,-1,true),viewExact=run(draw,view,0,true);
    assert(viewOff.ok&&viewOff.gate[9]==1&&viewOff.gate[15]==1);
    assert(viewExact.ok&&viewExact.gpu==1&&viewExact.submissions[0].rigid);
    for(auto v:viewExact.gate)assert(!v);
    assertGpuMatchesCpu(viewExact,viewOff,false);
    ++cases;
    // 4. Unit 0 projected from the position. Exact mode refuses it (the GPU
    // route would divide per vertex); approximate mode folds S += o*Q,
    // T += o*Q, divides per pixel (u[92].z, projection mode) and equals the
    // CPU route, which divides per pixel too.
    const auto projected=warped(projectedUnit(perspective(plainRegisters),.001f),1);
    const auto projectedOff=run(draw,projected,-1,true),projectedExact=run(draw,projected,0,true);
    assert(projectedOff.ok&&projectedOff.projection==1&&!projectedOff.submissions[0].rigid);
    assert(projectedExact.refused[unsigned(IndirectConstantRefusal::projected)]==1&&!projectedExact.planned);
    assertSameCpuResult(projectedExact,projectedOff);
    const auto perPixel=run(draw,projected,3,true);
    assert(perPixel.ok&&perPixel.gpu==1&&perPixel.perPixel==1&&perPixel.projection==1&&perPixel.submissions[0].rigid);
    const auto plainProjected=run(draw,projectedUnit(perspective(plainRegisters),.001f),3,true);
    assert(plainProjected.ok&&plainProjected.submissions[0].rigid&&plainProjected.submissions[0].plan.multiProjected);
    const auto& p=plainProjected.submissions[0].plan.uniforms;const auto& f=perPixel.submissions[0].plan.uniforms;
    for(unsigned c=0;c<4;++c){
        assert(near(f[unit0][c],p[unit0][c]+o0*p[unit0+2][c],1e-6f));
        assert(near(f[unit0+1][c],p[unit0+1][c]+o1*p[unit0+2][c],1e-6f));
    }
    assert(p[unit0+3][2]==0&&f[unit0+3][2]==1);
    assertRowsDiffer(perPixel.submissions[0].plan,plainProjected.submissions[0].plan,
        {{unit0,0},{unit0,1},{unit0,2},{unit0,3},{unit0+1,0},{unit0+1,1},{unit0+1,2},{unit0+1,3},{unit0+3,2}});
    assertGpuMatchesCpu(perPixel,projectedOff,true);
    ++cases;
    // 5. Late restart: a projector crossing the vertices (q from 1 to -1).
    // The GPU route refuses it after the reservation; the restart gives the
    // CPU route's own result, whatever it is.
    const auto crossing=warped(projectedUnit(perspective(plainRegisters),-.005f),1);
    const auto crossingOff=run(draw,crossing,-1,true),crossingRestart=run(draw,crossing,3,true);
    assert(crossingRestart.planned==1&&crossingRestart.restarts==1&&!crossingRestart.gpu);
    assertSameCpuResult(crossingRestart,crossingOff);
    ++cases;
    // 5b. Alpha 9: when the projector crossing the vertices is the camera
    // itself (q = 2 w at every position), the draw stays on the GPU route
    // and divides per pixel: the PICA clips the triangle before any pixel
    // divides. With the switch off it restarts as in case 5.
    const auto camera=warped(cameraProjector(perspective(plainRegisters)),1);
    const auto followed=indirectConstantCounters.cameraDivisor;
    const auto kept=run(draw,camera,3,true);
    assert(kept.ok&&kept.planned==1&&kept.gpu==1&&!kept.restarts&&kept.perPixel==1&&kept.projection==1);
    assert(kept.submissions.size()==1&&kept.submissions[0].rigid&&indirectConstantCounters.cameraDivisor==followed+1);
    {const auto& u=kept.submissions[0].plan.uniforms;
     assert(u[unit0+3][2]==1); // divided per pixel
     unsigned behind=0;
     for(const auto& record:kept.submissions[0].records){
        const float q=u[unit0+2][0]*record.position[0]+u[unit0+2][1]*record.position[1]+u[unit0+2][2]*record.position[2]+u[unit0+2][3];
        behind+=q<0;
     }
     assert(behind==2);} // the two vertices at x = 400
    const auto clamped=run(draw,camera,3,true,false);
    assert(clamped.planned==1&&clamped.restarts==1&&!clamped.gpu&&indirectConstantCounters.cameraDivisor==followed+1);
    ++cases;
}
// List B: TEX2 in the vertex as well, which no texgen reads.
void casesB(const CommandDraw& draw){
    const auto uniform=warped(draw.registers,1);
    // 6. An attribute no plan reads is skipped by the compact decode (since
    // Alpha 8; before, the format was refused and the draw restarted on the
    // CPU route): the GPU route's coordinates are the CPU route's.
    const auto off=run(draw,uniform,-1,true),gpu=run(draw,uniform,0,true);
    assert(off.ok&&off.vertexIndirect==1);
    assert(gpu.ok&&gpu.planned==1&&gpu.gpu==1&&!gpu.restarts&&!gpu.vertexIndirect);
    assertGpuMatchesCpu(gpu,off,false);
    ++cases;
    // 7-8. Early restart, before the reservation (texgen from the position:
    // no multi-texture plan, reject 10), orthographic and in perspective:
    // same result as off, and the perspective gate counts the draw once
    // (gate 9).
    for(const bool view:{false,true}){
        const auto registers=fromPosition(warped(view?perspective(draw.registers):draw.registers,1));
        const auto off=run(draw,registers,-1,true),restart=run(draw,registers,0,true);
        assert(off.ok&&off.vertexIndirect==1&&off.gate[9]==(view?1u:0u));
        assert(restart.planned==1&&restart.restarts==1&&!restart.gpu);
        assertSameCpuResult(restart,off);
        ++cases;
    }
}
bool listB;
}

int main(){
    assert(arena.attach(storage)&&arena.begin());
    // Map 0: the destination (I8 8x8). Map 1: a uniform warp map (136).
    // Map 2: a warp map of values 130-136.
    std::array<std::array<u8,96>,3> images{};std::array<TextureHandle,3> handles{};GXTexObj objects[3];
    for(unsigned i=0;i<3;++i){auto& t=images[i];t[0]=1;t[3]=t[5]=8;t[24]=1;t[31]=32;
        for(unsigned n=0;n<64;++n)t[32+n]=i==0?u8(n*4):i==1?u8(136):u8(130+n%7);
        assert(registerTexture(t,handles[i]));GXInitTexObj(&objects[i],t.data()+32,8,8,GX_TF_I8,GX_REPEAT,GX_REPEAT,GX_FALSE);}
    CommandMemory memory;CommandProcessor processor(memory);u8 scratch[256];CommandStream stream(processor,scratch);assert(setCommandStream(&stream));
    assert(stream.write({reinterpret_cast<const u8*>("\0"),1}));
    for(unsigned i=0;i<3;++i)GXLoadTexObj(&objects[i],static_cast<GXTexMapID>(i));
    const float projection[]{.005f,0,0,-1,0,-2.0f/240,0,1,0,0,.5,-.5,0,0,0,1};
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0};
    GXSetProjection(projection,GX_ORTHOGRAPHIC);GXSetViewport(0,0,400,240,0,1);
    GXLoadPosMtxImm(identity,GX_PNMTX0);GXSetCurrentMtx(GX_PNMTX0);
    GXSetScissor(0,0,400,240);GXSetBlendMode(GX_BM_BLEND,GX_BL_SRCALPHA,GX_BL_INVSRCALPHA,GX_LO_COPY);
    GXSetZMode(GX_TRUE,GX_LEQUAL,GX_TRUE);GXSetZCompLoc(GX_FALSE);GXSetAlphaCompare(GX_ALWAYS,0,GX_AOP_AND,GX_ALWAYS,0);
    GXSetNumChans(1);GXSetChanCtrl(GX_COLOR0A0,GX_FALSE,GX_SRC_REG,GX_SRC_VTX,0,GX_DF_NONE,GX_AF_NONE);
    GXSetNumTexGens(2);
    for(unsigned i=0;i<2;++i)GXSetTexCoordGen2(static_cast<GXTexCoordID>(i),GX_TG_MTX2x4,static_cast<GXTexGenSrc>(GX_TG_TEX0+i),GX_IDENTITY,GX_FALSE,GX_PTIDENTITY);
    assert(stream.finish());
    assert(stream.writeBp(0x41,processor.registers().bp[0x41]|24));
    for(unsigned reg:{0x42u,0xf1u,0xf5u})assert(stream.writeBp(reg,0));
    alignas(32) u8 commands[1024];GDLObj gd;GDInitGDLObj(&gd,commands,sizeof(commands));GDSetCurrent(&gd);
    GDSetGenMode2(2,1,1,0,GX_CULL_NONE);
    for(unsigned i=0;i<4;++i)GDSetTevSwapModeTable(static_cast<GXTevSwapSel>(i),GX_CH_RED,GX_CH_GREEN,GX_CH_BLUE,GX_CH_ALPHA);
    GDSetTevOrder(GX_TEVSTAGE0,GX_TEXCOORD0,GX_TEXMAP0,GX_COLOR0A0,GX_TEXCOORD_NULL,GX_TEXMAP_NULL,GX_COLOR_NULL);
    GDSetTevColorCalc(GX_TEVSTAGE0,GX_CC_ZERO,GX_CC_TEXC,GX_CC_RASC,GX_CC_ZERO,GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV);
    GDSetTevAlphaCalcAndSwap(GX_TEVSTAGE0,GX_CA_ZERO,GX_CA_TEXA,GX_CA_RASA,GX_CA_ZERO,GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV,GX_TEV_SWAP0,GX_TEV_SWAP0);
    GDSetTevDirect(GX_TEVSTAGE0);
    const auto bytes=GDGetCurrOffset();GDSetCurrent(nullptr);assert(processor.execute({commands,bytes}));
    processor.setSink([](const CommandDraw& draw,void*){if(listB)casesB(draw);else casesA(draw);return true;},nullptr);
    auto quad=[&](unsigned coordinates){
        GXClearVtxDesc();GXSetVtxDesc(GX_VA_POS,GX_DIRECT);GXSetVtxDesc(GX_VA_CLR0,GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,GX_F32,0);GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_CLR0,GX_CLR_RGBA,GX_RGBA8,0);
        for(unsigned t=0;t<coordinates;++t){GXSetVtxDesc(static_cast<GXAttr>(GX_VA_TEX0+t),GX_DIRECT);
            GXSetVtxAttrFmt(GX_VTXFMT0,static_cast<GXAttr>(GX_VA_TEX0+t),GX_TEX_ST,GX_F32,0);}
        assert(stream.finish());
        std::vector<u8> list{u8(GX_QUADS),0,4};auto scalar=[&](float f){auto bits=std::bit_cast<u32>(f);for(int s=24;s>=0;s-=8)list.push_back(u8(bits>>s));};
        for(unsigned i=0;i<4;++i){
            const bool right=i==1||i==2,bottom=i>=2;
            scalar(right?400:0);scalar(bottom?240:0);scalar(0);list.insert(list.end(),{64,128,192,255});
            // Multiples of 1/256: the compact records hold them exactly.
            for(unsigned t=0;t<coordinates;++t){scalar(.25f*t+(right?.5f:.125f));scalar(.125f*t+(bottom?.75f:.25f));}
        }
        assert(processor.execute(list));
    };
    indirectStatsCache={};indirectConstantCounters={};
    quad(2);
    listB=true;quad(3);
    assert(cases==11);
    assert(setCommandStream(nullptr));
    std::printf("PASS indirect hookup: %u cases on the real draw adapter: exact uniform warp on the GPU route (rows = plain plan + su*o, -sv*o; "
                "coordinates = CPU route), stale coordinate scale registers ignored on both routes, deviation refusal and approximate mean, the per-vertex part equal to the CPU route's warp (the mean "
                "alone misses it), projected unit refused in exact mode and folded per pixel at 3, a projector that is the camera kept per pixel "
                "across the camera plane, an unread attribute decoded, early and late restarts identical to the path off\n",cases);
    setIndirectConstantLimit(0);
    return 0;
}
