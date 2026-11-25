#include "../gx_command_draw.hpp"
#include "../gx_geometry_pica.hpp"
#include "../gx_texture_pica.hpp"
#include "../gx_pica.hpp"
#include "../runtime/gx_stream.hpp"
#include "../runtime/gx_raster_plan.hpp"
#include "../runtime/gx_raster_color.hpp"
#include "../runtime/gx_fog.hpp"
#include "../runtime/gx_fog_pass.hpp"
#include "../runtime/gx_indirect.hpp"
#include "../runtime/gx_early_alpha.hpp"
#include "../runtime/gx_clip_geometry.hpp"
#include "../runtime/gx_compact_cache.hpp"
#include "../runtime/gx_framebuffer.hpp"
#include "gx_rigid_reference.hpp"
#include <dolphin/gd.h>
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <cassert>
#include <bit>
#include <cstddef>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace dusk::native;
using namespace dusk::native::gx;
extern "C" void OSPanic(const char*,int,const char* message,...){throw std::runtime_error(message);}
extern "C" void DCFlushRange(void*,u32){}
namespace {
alignas(16) std::array<std::byte,8192> storage;
GpuGeometryArena arena;
unsigned submissions,binds,rigidSubmissions,compactSubmissions;
bool rigidTestMode;
bool signedQTestMode;
GeometryQPolicy capturedPolicy{};
bool carrierMode;
std::vector<GpuVertex> carrierVertices;
std::vector<u16> carrierIndices;
std::array<Texture,3> carrierTextures;
unsigned carrierFailBind=UINT32_MAX;
bool carrierInvalidScale;
PicaMaterialPlan carrierMaterial;
RasterPlan carrierRaster;
std::vector<GpuVertex> rigidExpected;
bool gpuCommittedFailure,gpuFailure,textureFailure,scopeFailure,reservationFailure,cancellationFailure,safetyMode;
const char* textureFailureReason="test texture failure";
float uvShiftS{},uvShiftT{};
std::array<GpuVertex,4> captured;
bool expandedMode;
bool fogMode;
bool fogPrimary;
bool constantFogTest;
bool redFogTest;
bool standInFogTest;
bool relaxedAlphaTest;
std::vector<GpuVertex> expandedVertices;
std::vector<u16> expandedIndices;
std::array<TextureHandle,3> protectedTextures{};
// Immediate record parity (end of main): what each rigid submission sent,
// as the multi-texture upload sees it (rows below the palette end and from
// paletteLimit up, light loop, matrices) plus the 32-byte vertex records.
struct RigidCapture {
    std::vector<std::array<float,4>> rows;unsigned lights{},matrices{};
    std::vector<std::byte> records;std::vector<u16> indices;
    RasterPlan raster;PicaMaterialPlan material;
};
std::vector<RigidCapture>* rigidCaptures;
RigidCapture captureRows(const RasterPlan& r,const PicaMaterialPlan& m,const RigidVertexPlan& p){
    RigidCapture c;const unsigned count=p.uniformCount();
    for(unsigned n=0;n<count;++n)c.rows.push_back(p.uniforms[n]);
    for(unsigned n=std::max(count,p.paletteLimit());n<94;++n)c.rows.push_back(p.uniforms[n]);
    c.lights=p.lights;c.matrices=p.matrices;c.raster=r;c.material=m;
    return c;
}
// Material plans compared member by member: `lowering` follows a padding
// byte whose value is indeterminate.
bool sameMaterialPlan(const PicaMaterialPlan& a,const PicaMaterialPlan& b){
    return a.lowering==b.lowering&&!std::memcmp(&a,&b,offsetof(PicaMaterialPlan,rasterCarrier)+sizeof(RasterCarrier));
}
// Native shape replay check (end of main): resident indices in host memory
// and every submitNativeRigid call as the multi-texture upload sees it.
bool nativeReplayFixture;
std::vector<std::vector<std::uint16_t>> residentIndices;
struct NativeCapture {RigidCapture rows;const void* vertices{};const std::uint16_t* indices{};unsigned indexCount{};bool chained{};};
std::vector<NativeCapture>* nativeCaptures;
}
extern "C" void duskNativeStaticGeometry(std::uint32_t generation,const void* list);
extern "C" bool duskNativeDrawShape(std::uint32_t generation,const void* list,std::uint32_t size);
// The model shadow pass belongs to the renderer (gx_pica.cpp): no caster here.
extern "C" void duskNativeShadowCaster(unsigned){}
// Texture-command diagnostics report through the game log on the console.
extern "C" void OSReport(const char*,...){}
namespace dusk::native {
std::uint64_t gxProfileTick(){return std::chrono::steady_clock::now().time_since_epoch().count();}
std::uint32_t residentIndexEpoch(){assert(nativeReplayFixture&&"resident replay is outside this adapter fixture");return 7;}
unsigned gxShadowCasterSlot(){return 0;}
void setGxShadowCasterSlot(unsigned){}
void runGxShadowCasterJob(const void*,std::size_t){}
void runGxCopyTexJob(const void*,std::size_t,const gx::CommandRegisters&){}
void runGxBloomJob(const void*,std::size_t){}
void runHudTapeJob(const void*,std::size_t){} // hud_tape_ctru.cpp: no HUD tape in this fixture
void runScreenTapeJob(const void*,std::size_t){} // screen_tape_ctru.cpp: no retained screens in this fixture
void runGxShadowArmJob(){}
void invalidateGxTextures(){}
// duskNativeGameDrawBegin/End (gx_command_draw.cpp, the DUSK_PICA_GAME_DRAW
// model dispatch) are never called here. Weak: a runner that compiles
// gx_pica.cpp (test_alpha_interval) links its own definitions.
[[gnu::weak]] FramebufferHandle activeFramebuffer(){return nullptr;}
[[gnu::weak]] bool alphaIntervalCopySafe(){return true;}
[[gnu::weak]] void invalidatePicaCommandState(){}
std::uint16_t* allocateResidentIndices(const std::uint16_t* indices,unsigned count){
    assert(nativeReplayFixture&&"resident replay is outside this adapter fixture");
    residentIndices.emplace_back(indices,indices+count);return residentIndices.back().data();
}
bool submitNativeRigid(const RasterPlan& r,const PicaMaterialPlan& m,const RigidVertexPlan& p,const void* vertices,const std::uint16_t* indices,unsigned count,unsigned,bool chained,
                       const NativeRigidLevels* levels){
    assert(nativeReplayFixture&&nativeCaptures&&"resident replay is outside this adapter fixture");
    // No levels of detail here (runtime/model_lod.hpp): the record's own list only.
    assert(!levels);
    assert(p.multiTexture&&p.compact&&p.staticVertices==vertices&&vertices&&indices&&count&&p.lights&&p.matrices);
    nativeCaptures->push_back({captureRows(r,m,p),vertices,indices,count,chained});
    return true;
}
// Native PICA blocks stay off here (no SD switch on the host): nothing is
// ever captured or replayed from a block.
void requestPicaBlockCapture(){}
void cancelPicaBlockCapture(){}
bool takePicaBlockCapture(PicaBlockCapture&){return false;}
bool submitPicaBlock(const std::uint32_t*,unsigned,bool,const RigidVertexPlan&,const void*,const std::uint16_t*,unsigned,unsigned){
    assert(!"PICA blocks are off in this adapter fixture");return false;
}
GxTextureDrawScope::GxTextureDrawScope(std::span<const gx::Texture> textures):active(!scopeFailure){
    assert((textures.size()<=3&&protectedTextures==std::array<TextureHandle,3>{}));
    for(unsigned n=0;n<textures.size();++n)protectedTextures[n]=textures[n].handle;
}
GxTextureDrawScope::~GxTextureDrawScope(){protectedTextures={};}
bool reserveGxGeometry(std::size_t v,std::size_t i,GpuGeometryBatch& b){return !reservationFailure&&arena.reserve(v,i,b);}
bool shrinkGxGeometry(GpuGeometryBatch& b,std::size_t v,std::size_t i,bool external){return arena.shrinkPending(b,v,i,external);}
bool cancelGxGeometry(const GpuGeometryBatch& b){return arena.cancel(b)&&!cancellationFailure;}
const char* gxGeometryFailure(){return "test arena failure";}
const char* gxPicaFailure(){return "test GPU failure";}
const char* gxTextureFailure(){return textureFailureReason;}
// Per-pixel projection switch of an already bound unit 0; the host model has
// no texture unit state to change.
bool setGxTextureProjection(bool){return true;}
// worldMips: the use each bind received, by unit (native replay fixture).
std::vector<std::pair<unsigned,bool>>* worldUses;
bool bindGxTexture(const Texture& texture,float& u,float& v,unsigned unit,bool projective,GxWorldMipUse use){
    if(worldUses)worldUses->push_back({unit,use.world});
    assert(!projective||unit==0);
    assert(texture.handle&&unit<3&&protectedTextures[unit]==texture.handle);++binds;u=1;v=.5;
    if(carrierMode){carrierTextures[unit]=texture;if(unit==carrierFailBind)return false;if(carrierInvalidScale)u=std::numeric_limits<float>::quiet_NaN();}
    return !textureFailure;
}
// The host has no stencil target: triangle fog keeps the CPU pair form.
bool picaRigidTriangleFogAvailable(){return false;}
// Alpha-union layers stay on the CPU vertex path here (the pair form these
// tests check); the predicate is tested with gx_pica.cpp (gx_alpha_interval_test).
bool picaAlphaUnionGpu(const RasterPlan&){return false;}
bool submitGxRigidFogGeometry(const RasterPlan&,const PicaMaterialPlan&,const RasterPlan&,const PicaMaterialPlan&,
    const GpuGeometryBatch&,const RigidVertexPlan&,const RigidVertexPlan&){assert(false);return false;}
bool submitGxRigidGeometry(const RasterPlan& r,const PicaMaterialPlan& m,const GpuGeometryBatch& b,const RigidVertexPlan& p){
    if(rigidCaptures){
        // Resident static vertices (native replay check): the arena keeps
        // only the indices and three placeholder records.
        assert(arena.owns(b)&&p.multiTexture&&p.compact&&(p.staticVertices?nativeReplayFixture:validateCompactRigidGeometry(b)));
        RigidCapture c=captureRows(r,m,p);
        if(!p.staticVertices){
            const auto bytes=std::as_bytes(b.vertices).first(b.vertices.size()*sizeof(CompactRigidVertexMT));
            c.records.assign(bytes.begin(),bytes.end());
        }
        c.indices.assign(b.indices.begin(),b.indices.end());
        rigidCaptures->push_back(std::move(c));
        return arena.commit(b);
    }
    assert(rigidTestMode&&arena.owns(b)&&(p.compact?validateCompactRigidGeometry(b):validateGpuGeometry(b))&&rigidExpected.size()==b.vertices.size());
    if(p.compact)++compactSubmissions;
    assert(p.uniformCount()<=94&&p.matrices<=10);
    auto close=[](float a,float expected){assert(std::abs(a-expected)<2e-4f*(1+std::abs(expected)));};
    // gx_rigid_reference.hpp models the single-texture shader only; the
    // multi-texture records (gx_rigid_mt.v.pica) are checked in Azahar.
    if(!p.multiTexture)for(unsigned i=0;i<b.vertices.size();++i){const auto actual=rigidReference(p,p.compact?expandCompactRigid(p,std::as_bytes(b.vertices),i):b.vertices[i]);const auto& expected=rigidExpected[i];
        for(unsigned c=0;c<4;++c){close(actual.position[c],expected.position[c]);close(actual.color[c],expected.color[c]);}
        for(unsigned t=0;t<3;++t)for(unsigned c=0;c<2;++c)close(actual.texcoord[t][c],expected.texcoord[t][c]);
        close(actual.textureQ,expected.textureQ);}
    ++submissions;++rigidSubmissions;return arena.commit(b);
}
bool submitGxTriangleFogGeometry(const RasterPlan& r,const PicaMaterialPlan& material,
    const RasterPlan& fog,const PicaMaterialPlan& fogMaterial,const GpuGeometryBatch& b){
    assert(constantFogTest&&arena.owns(b)&&validateGpuGeometry(b)&&b.vertices.size()%8==0);
    assert(r.depth.compare==3&&r.depth.write&&!r.blend.mode);
    assert(fog.depth.compare==2&&!fog.depth.write&&fog.blend.mode&&fog.blend.src==4&&fog.blend.dst==5&&!fog.writeAlpha);
    assert(material.textureCount==3&&fogMaterial.textureCount==0&&fogMaterial.stageCount==1);
    const auto half=b.vertices.size()/2;
    for(unsigned i=0;i<half;++i){const auto& a=b.vertices[i];const auto& f=b.vertices[half+i];
        assert(a.position==f.position&&std::abs(f.color[3]-.5f)<1e-6);
        assert(std::abs(f.color[0]-171.f/255)<1e-6&&std::abs(f.color[1]-205.f/255)<1e-6&&std::abs(f.color[2]-239.f/255)<1e-6);}
    for(auto index:b.indices)assert(index<half);
    return arena.commit(b);
}
bool submitGxAdditiveTextureGeometry(const RasterPlan&,const PicaMaterialPlan&,const RasterPlan&,
    const PicaMaterialPlan*,const PicaMaterialPlan&,const Texture&,const Texture&,const GpuGeometryBatch&,bool){
    assert(false);return false;
}
bool submitGxCommandGeometry(const RasterPlan& r,const PicaMaterialPlan& material,const GpuGeometryBatch& b){
    assert(arena.owns(b)&&validateGpuGeometry(b));
    if(signedQTestMode){
        capturedPolicy=b.qPolicy;
        expandedVertices.assign(b.vertices.begin(),b.vertices.end());
        expandedIndices.assign(b.indices.begin(),b.indices.end());
        if(gpuFailure)return false;
        ++submissions;return arena.commit(b);
    }
    if(carrierMode){
        if(gpuFailure)return false;
        carrierVertices.assign(b.vertices.begin(),b.vertices.end());carrierIndices.assign(b.indices.begin(),b.indices.end());carrierMaterial=material;carrierRaster=r;++submissions;const bool committed=arena.commit(b);return committed&&!gpuCommittedFailure;
    }
    if(safetyMode){
        if(gpuFailure)return false;
        ++submissions;const bool committed=arena.commit(b);return committed&&!gpuCommittedFailure;
    }
    if(rigidTestMode){rigidExpected.assign(b.vertices.begin(),b.vertices.end());expandedIndices.assign(b.indices.begin(),b.indices.end());++submissions;return arena.commit(b);}
    if(standInFogTest){
        // One-stage stand-in plus the vertex fog stage on its ramp unit.
        assert(material.stageCount==2&&material.textureCount==2&&material.stages[1].rgb.function==MaterialFunction::interpolate);
        return arena.commit(b);
    }
    if(constantFogTest){
        assert(material.textureCount==3&&material.stageCount==4);
        assert(material.stages[3].rgb.inputs[2].source==MaterialSource::primary);
        assert(material.stages[3].rgb.inputs[2].operand==(redFogTest?MaterialOperand::red:MaterialOperand::alpha));
        for(const auto& vertex:b.vertices)assert(std::abs(vertex.color[redFogTest?0:3]-.5f)<1e-6);
        if(redFogTest){assert(b.vertices.front().color[3]==1);assert(std::abs(b.vertices.back().color[3]-127.f/255)<1e-6);}
        return arena.commit(b);
    }
    assert(material.textureCount==(fogMode?(fogPrimary?1:2):3)&&material.stageCount==(fogMode?2:3));
    if(fogMode) {
        const auto& stage=material.stages[1];
        assert((stage.constant==std::array<u8,4>{0xab,0xcd,0xef,255}));
        assert(stage.rgb.function==MaterialFunction::interpolate&&stage.rgb.inputs[2].source==(fogPrimary?MaterialSource::primary:MaterialSource::texture1));
        assert(stage.alpha.function==MaterialFunction::replace&&stage.alpha.inputs[0].source==MaterialSource::previous);
    }
    assert(r.writeRgb&&!r.writeAlpha&&r.cull==0);
    assert(relaxedAlphaTest?r.alphaRelaxed&&r.alpha.comparison==4&&r.alpha.reference==75:!r.alphaRelaxed&&r.alpha.comparison==7);
    assert(r.depth.enabled&&r.depth.write&&r.depth.compare==GX_LEQUAL);
    assert(!r.scissor.empty&&r.scissor.left==0&&r.scissor.right==240&&r.scissor.top==0&&r.scissor.bottom==400);
    if(expandedMode) {
        expandedVertices.assign(b.vertices.begin(),b.vertices.end());
        expandedIndices.assign(b.indices.begin(),b.indices.end());
        ++submissions;return arena.commit(b);
    }
    assert(b.vertices.size()==4&&b.indices.size()==6);
    const u16 expected[]{0,1,2,2,3,0};assert(!std::memcmp(b.indices.data(),expected,sizeof(expected)));
    for(unsigned i=0;i<4;++i){
        captured[i]=b.vertices[i];const auto& v=captured[i];
        assert(std::abs(v.position[3]-1)<1e-6&&std::abs(v.position[2]+.5)<1e-6);
        assert(std::abs(v.color[0]-64.0/255)<1e-6&&std::abs(v.color[1]-128.0/255)<1e-6&&std::abs(v.color[2]-192.0/255)<1e-6&&v.color[3]==1);
        for(unsigned unit=0;unit<3;++unit){
            const auto coord=material.textures[unit].coordinate;
            const float s=uvShiftS+.1f+.2f*coord+(material.projectiveTexture0&&coord==0?.05f*i:0);
            const float t=uvShiftT+.2f+.2f*coord+(material.projectiveTexture0&&coord==0?.03f*i:0);
            const float q=unit==0&&material.projectiveTexture0?1+.1f*s+.2f*t:1;
            assert(std::abs(v.texcoord[unit][0]-s)<1e-6);
            assert(std::abs(v.texcoord[unit][1]-(q-t*.5f))<1e-6);
            if(unit==0)assert(std::abs(v.textureQ-q)<1e-6);
        }
    }
    if(gpuFailure)return false;
    ++submissions;return arena.commit(b);
}
}

static unsigned signedQHooks;
static SignedQDrawSample lastSignedQ;
static void signedQCases(const CommandDraw& draw){
    const auto savedArena=arena;const auto savedSubmissions=submissions;
    auto regs=draw.registers;regs.xf[0x1026]=0;regs.xf[0x1005]=0;
    const float projection[6]{1,0,1,0,0,-1};
    for(unsigned n=0;n<6;++n)regs.xf[0x1020+n]=std::bit_cast<u32>(projection[n]);
    const float identity[12]{1,0,0,0,0,1,0,0,0,0,1,0};
    for(unsigned n=0;n<12;++n)regs.xf[n]=std::bit_cast<u32>(identity[n]);
    regs.xf[0x1040]|=2;
    regs.matrixIndex[0]=(regs.matrixIndex[0]&~(63u<<6))|(30u<<6);
    const float matrix[12]{.5f,0,0,.125f,0,.25f,0,.0625f,1,0,0,0};
    for(unsigned n=0;n<12;++n){regs.xf[120+n]=std::bit_cast<u32>(matrix[n]);regs.xfValid.set(120+n);}
    const unsigned stride=draw.plan.packedStride();assert(stride==40);
    std::vector<u8> bytes(draw.vertices.begin(),draw.vertices.end());
    auto put=[&](unsigned i,unsigned offset,float value){const u32 bits=std::bit_cast<u32>(value);
        for(unsigned k=0;k<4;++k)bytes[i*stride+offset+k]=u8(bits>>(24-8*k));};
    const float x[]{-.5f,.5f,.5f,-.5f},y[]{-.5f,-.5f,.5f,.5f};
    const float qMixed[]{-.75f,.25f,.75f,-.25f};
    for(unsigned i=0;i<4;++i){put(i,0,x[i]);put(i,4,y[i]);put(i,8,-2);put(i,16,qMixed[i]);put(i,20,.5f);}
    CommandDraw candidate{GX_QUADS,4,0,bytes,{},draw.plan,draw.arrays,regs};
    signedQTestMode=true;
    auto enabled=[](){CommandDrawContext c;c.allowDirectSignedQDiagnostic=true;c.allowRigidGpu=true;c.allowDegradedDraws=true;
        c.reportSignedQ=[](const CommandDraw&,const PicaMaterialPlan&,const SignedQDrawSample& sample){++signedQHooks;lastSignedQ=sample;};return c;};
    auto reject=[&](bool opt=true){arena=savedArena;const auto sent=submissions;const auto used=arena.used();auto c=enabled();c.allowDirectSignedQDiagnostic=opt;
        assert(!drawGxCommand(candidate,&c)&&c.rejected==1&&!c.submitted&&!c.culled);
        assert(arena.used()==used&&submissions==sent&&!c.signedQNegativeDraws&&!c.signedQMixedDraws);};
    reject(false); // Default strict, visible mixed q is fatal.
    // Explicit degraded policy only: the same visible quad keeps positions,
    // indices, colour, s and t; nonpositive divisors become a small positive one.
    {arena=savedArena;auto c=enabled();c.allowDirectSignedQDiagnostic=false;c.allowProjectiveDivisorFallback=true;
        assert(drawGxCommand(candidate,&c)&&c.submitted==1&&!c.rejected&&!c.signedQNegativeDraws&&!c.signedQMixedDraws);
        assert(c.simplifiedDraws==1&&c.simplifiedByReason[unsigned(DrawVisualFallback::projectiveDivisor)]==1);
        assert((expandedIndices==std::vector<u16>{0,1,2,2,3,0})&&expandedVertices.size()==4);
        for(unsigned i=0;i<4;++i){const auto& v=expandedVertices[i];
            const float q=qMixed[i]>0?qMixed[i]:1.f/4096;
            assert((v.position==std::array<float,4>{y[i],-x[i],-1,2}));
            assert(v.textureQ==q&&v.texcoord[0][0]==.5f*qMixed[i]+.125f&&v.texcoord[0][1]==q-.09375f);}
        // The per-pass policy can still refuse it.
        arena=savedArena;c=enabled();c.allowDirectSignedQDiagnostic=false;c.allowProjectiveDivisorFallback=true;
        c.allowVisualFallback=[](const CommandDraw&,DrawVisualFallback reason){return reason!=DrawVisualFallback::projectiveDivisor;};
        assert(!drawGxCommand(candidate,&c)&&c.rejected==1&&!c.submitted);}
    auto oracle=[&](const float* qs,GeometryQPolicy expectedPolicy){
        assert(capturedPolicy==expectedPolicy);
        assert((expandedIndices==std::vector<u16>{0,1,2,2,3,0})&&expandedVertices.size()==4);
        for(unsigned i=0;i<4;++i){
            GpuVertex want{};want.position={y[i],-x[i],-1,2};want.color={64.f/255,128.f/255,192.f/255,1};
            // Explicit algebra of this fixture, independent from admission and
            // the production transform/packing helpers. Compare every byte.
            want.textureQ=qs[i];want.texcoord[0]={.5f*qs[i]+.125f,qs[i]-.09375f};
            want.texcoord[1]={.5f,1-.6f*.5f};want.texcoord[2]={.3f,1-.4f*.5f};
            if(std::memcmp(&want,&expandedVertices[i],sizeof(want))){
                std::array<float,15> a{},b{};std::memcpy(a.data(),&want,sizeof(want));std::memcpy(b.data(),&expandedVertices[i],sizeof(want));
                for(unsigned j=0;j<15;++j)if(std::bit_cast<u32>(a[j])!=std::bit_cast<u32>(b[j]))std::fprintf(stderr,"oracle vertex=%u field=%u expected=%a actual=%a\n",i,j,double(a[j]),double(b[j]));
                assert(false);
            }
        }
    };
    auto c=enabled();arena=savedArena;assert(drawGxCommand(candidate,&c)&&c.submitted==1&&!c.rejected);
    oracle(qMixed,GeometryQPolicy::directTexture0Diagnostic);
    assert(c.signedQNegativeDraws==1&&c.signedQMixedDraws==1&&c.signedQReports==1&&signedQHooks==1);
    assert(lastSignedQ.mixedTriangles==2&&lastSignedQ.negativeVertices==2&&lastSignedQ.samples==6&&lastSignedQ.vertex[0].textureMatrix==30);
    assert((lastSignedQ.target==std::array<unsigned,4>{400,240,400,240}));
    for(unsigned k=0;k<lastSignedQ.samples;++k){const auto& a=lastSignedQ.vertex[k];const auto& v=expandedVertices[a.index];
        assert(a.gpuBits[2]==std::bit_cast<u32>(qMixed[a.index])&&a.transformedBits[2]==a.gpuBits[2]);
        assert(a.transformedBits[0]==std::bit_cast<u32>(.5f*qMixed[a.index]+.125f));
        for(unsigned j=0;j<4;++j)assert(a.clipBits[j]==std::bit_cast<u32>(v.position[j]));
        assert(a.gpuBits[0]==std::bit_cast<u32>(v.texcoord[0][0])&&a.gpuBits[1]==std::bit_cast<u32>(v.texcoord[0][1]));
    }
    // Hard report budget survives repeated calls and resetting the arena.
    for(unsigned n=0;n<8;++n){arena=savedArena;assert(drawGxCommand(candidate,&c));}
    assert(c.signedQReports==4&&signedQHooks==4&&c.signedQNegativeDraws==9&&c.signedQMixedDraws==9);
    const float negative[]{-2,-2,-2,-2};for(unsigned i=0;i<4;++i)put(i,16,negative[i]);
    c=enabled();arena=savedArena;assert(drawGxCommand(candidate,&c));oracle(negative,GeometryQPolicy::directTexture0Diagnostic);
    assert(c.signedQNegativeDraws==1&&!c.signedQMixedDraws);reject(false);
    // Positive controls: OFF and ON must have identical full vertex/index bytes.
    const float positive[]{2,2,2,2};for(unsigned i=0;i<4;++i)put(i,16,2);
    c={};arena=savedArena;assert(drawGxCommand(candidate,&c));oracle(positive,GeometryQPolicy::positiveOnly);
    const auto control=expandedVertices;const auto hookBefore=signedQHooks;
    c=enabled();arena=savedArena;assert(drawGxCommand(candidate,&c));oracle(positive,GeometryQPolicy::directTexture0Diagnostic);
    assert(!std::memcmp(control.data(),expandedVertices.data(),4*sizeof(GpuVertex))&&!c.signedQNegativeDraws&&signedQHooks==hookBefore);
    for(float bad:{0.f,-0.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()}){put(3,16,bad);reject();}
    put(3,16,2);put(3,20,std::numeric_limits<float>::quiet_NaN());reject();put(3,20,.5f);
    // Tiny negative values are retained bit-for-bit, with no epsilon cutoff.
    const float tiny[]{-0x1p-126f,-0x1p-126f,-0x1p-126f,-0x1p-126f};
    for(unsigned i=0;i<4;++i)put(i,16,tiny[i]);c=enabled();arena=savedArena;assert(drawGxCommand(candidate,&c));oracle(tiny,GeometryQPolicy::directTexture0Diagnostic);
    for(unsigned i=0;i<4;++i)put(i,16,-2);
    regs.xf[0x1026]=1;reject();regs.xf[0x1026]=0; // UI remains strict.
    // Even with pre-existing visual fallbacks enabled, the diagnostic may
    // not combine signed-q admission with a possibly visible depth cut.
    regs.xfValid.set(0x1005);regs.xf[0x1005]=1;regs.xf[0x1025]=std::bit_cast<u32>(1.f);reject();
    // Positive-only handoff is byte-identical to the admitted non-diagnostic
    // world depth fallback; it never combines signed Q with the approximation.
    for(unsigned i=0;i<4;++i)put(i,16,2);
    arena=savedArena;CommandDrawContext depthControl;depthControl.allowDegradedDraws=true;
    assert(drawGxCommand(candidate,&depthControl));const auto depthVertices=expandedVertices;const auto depthIndices=expandedIndices;
    c=enabled();arena=savedArena;assert(drawGxCommand(candidate,&c));
    assert(c.positiveQDepthFallbackDraws==1&&c.positiveQDepthFallbackVertices==4&&c.simplifiedByReason[unsigned(DrawVisualFallback::clippedDepth)]==1);
    assert(capturedPolicy==GeometryQPolicy::positiveOnly&&depthVertices.size()==expandedVertices.size()&&
        !std::memcmp(depthVertices.data(),expandedVertices.data(),depthVertices.size()*sizeof(GpuVertex))&&depthIndices==expandedIndices);
    c=enabled();c.allowDegradedDraws=false;arena=savedArena;assert(!drawGxCommand(candidate,&c)&&!c.positiveQDepthFallbackDraws);
    c=enabled();c.allowVisualFallback=[](const CommandDraw&,DrawVisualFallback){return false;};arena=savedArena;
    assert(!drawGxCommand(candidate,&c)&&!c.positiveQDepthFallbackDraws);
    // An unreferenced last negative original still blocks the handoff.
    const std::array<u8,6> prefixIndices{0,0,1,0,2,0};
    CommandDraw unreferenced{GX_TRIANGLES,4,3,bytes,prefixIndices,draw.plan,draw.arrays,regs};
    put(3,16,-2);arena=savedArena;c=enabled();const auto beforeDepth=submissions;
    assert(!drawGxCommand(unreferenced,&c)&&c.rejected==1&&!c.positiveQDepthFallbackDraws&&submissions==beforeDepth&&arena.used()==savedArena.used());
    put(3,16,2);gpuFailure=true;arena=savedArena;c=enabled();assert(!drawGxCommand(candidate,&c)&&!c.positiveQDepthFallbackDraws);gpuFailure=false;
    for(unsigned i=0;i<4;++i)put(i,16,-2);
    regs.xf[0x1005]=0;regs.xf[0x1025]=std::bit_cast<u32>(-1.f);
    regs.xf[0x1041]|=2;reject();regs.xf[0x1041]&=~2u; // Additional projected unit is unsupported.
    const auto token=regs.bp[0x94];regs.bp[0x94]=0;reject();regs.bp[0x94]=token;
    gpuFailure=true;reject();gpuFailure=false;
    // Default-OFF proof boundary is preserved. Zero may be omitted ONLY with
    // a complete lateral invisibility proof, even with the experiment enabled.
    for(unsigned i=0;i<4;++i){put(i,0,10);put(i,16,0);}
    c=enabled();arena=savedArena;const auto sent=submissions;
    assert(drawGxCommand(candidate,&c)&&c.culled==1&&!c.submitted&&submissions==sent&&arena.used()==savedArena.used());
    put(3,20,std::numeric_limits<float>::infinity());reject();
    // A proved-invisible zero-q triangle may be removed without changing a
    // distinct visible negative-q triangle. The indexed survivor is exact.
    std::vector<u8> splitBytes(stride*6);
    auto splitPut=[&](unsigned i,unsigned offset,float value){const auto bits=std::bit_cast<u32>(value);
        for(unsigned k=0;k<4;++k)splitBytes[i*stride+offset+k]=u8(bits>>(24-8*k));};
    for(unsigned i=0;i<6;++i){std::memcpy(splitBytes.data()+i*stride,draw.vertices.data(),stride);
        splitPut(i,0,i<3?10.f:(i==4?.5f:-.5f));splitPut(i,4,i==5?.5f:-.5f);splitPut(i,8,-2);
        splitPut(i,16,i<3?0.f:-2.f);splitPut(i,20,.5f);}
    CommandDraw splitDraw{GX_TRIANGLES,6,0,splitBytes,{},draw.plan,draw.arrays,regs};
    c=enabled();arena=savedArena;assert(drawGxCommand(splitDraw,&c)&&c.submitted==1&&c.projectiveClipTriangles==1);
    assert((expandedIndices==std::vector<u16>{3,3,3,3,4,5}));
    for(unsigned i=3;i<6;++i){GpuVertex expected{};expected.position={i==5?.5f:-.5f,i==4?-.5f:.5f,-1,2};
        expected.color={64.f/255,128.f/255,192.f/255,1};expected.textureQ=-2;expected.texcoord[0]={-.875f,-2.09375f};
        expected.texcoord[1]={.5f,1-.6f*.5f};expected.texcoord[2]={.3f,1-.4f*.5f};
        assert(!std::memcmp(&expected,&expandedVertices[i],sizeof(expected)));}
    splitPut(4,16,0);c=enabled();arena=savedArena;const auto splitSent=submissions;
    assert(!drawGxCommand(splitDraw,&c)&&c.rejected==1&&!c.submitted&&!c.culled&&submissions==splitSent&&arena.used()==savedArena.used());
    signedQTestMode=false;arena=savedArena;submissions=savedSubmissions;
    std::puts("PASS signed-q direct: default strict; visible uniform/mixed exact bytes and topology; positive parity; bounded exact-bit telemetry; zero/nonfinite/UI/multi-projective/resource/submit failures; invisible proof boundary");
}

#include "raster_carrier_adapter.hpp"
static void negativeCases(const CommandDraw& draw){
    const auto used=arena.used();
    const auto submitted=submissions;
    auto reject=[&](const CommandDraw& d,const char* expected){
        for(bool degraded:{false,true}){
            CommandDrawContext context;context.allowDegradedDraws=degraded;
            context.allowVisualFallback=[](const CommandDraw&,DrawVisualFallback){return true;};
            assert(!drawGxCommand(d,&context));
            assert(context.rejected==1&&std::strcmp(context.failure,expected)==0);
            assert(context.submitted==0&&context.skippedDraws==0&&context.simplifiedDraws==0);
            std::uint64_t classified=0;for(auto n:context.rejectedByKind)classified+=n;assert(classified==1);
            assert(arena.used()==used&&submissions==submitted);
        }
    };
    auto shortDraw=draw;shortDraw.vertices=shortDraw.vertices.first(shortDraw.vertices.size()-1);
    reject(shortDraw,"GX command vertex extent invalid");
    auto aliased=draw;aliased.vertices={reinterpret_cast<const u8*>(storage.data()+((used+15)&~std::size_t(15))),draw.vertices.size()};
    reject(aliased,"GX command input aliases GPU reservation");
    // RGB and alpha have independent material sources, even for channel 0.
    MaterialState materialState; PicaMaterialPlan colorMaterial;
    assert(decodeMaterial(draw.registers,materialState));
    assert(planPicaMaterial(materialState,colorMaterial));
    auto colorRegisters=draw.registers;
    colorRegisters.xf[0x100e]&=~1u;
    colorRegisters.xf[0x100c]=0x12345678;
    colorRegisters.xfValid.set(0x100c);
    RasterColorPlan mixed; RasterColorError colorError;
    assert(planRasterColor(colorRegisters,colorMaterial,mixed,&colorError));
    assert(mixed.required==3&&mixed.vertex==2);
    DecodedVertex decoded; assert(draw.vertex(0,decoded));
    decoded.color[0][0]=std::numeric_limits<float>::quiet_NaN();
    std::array<float,4> actual;
    assert(evaluateRasterColor(mixed,decoded,actual,&colorError));
    assert(actual[0]==float(0x12)/255&&actual[1]==float(0x34)/255&&actual[2]==float(0x56)/255&&actual[3]==1);
    // No CLR0: channel 0 reads CLR1 when present, else Dolphin's missing
    // colour, opaque white (rasterVertexColor); RGB keeps its register source.
    decoded.present&=~((1u<<GX_VA_CLR0)|(1u<<GX_VA_CLR1));
    const auto missingBefore=gxMissingColorVertices;
    assert(evaluateRasterColor(mixed,decoded,actual,&colorError)&&actual[3]==1&&actual[0]==float(0x12)/255&&gxMissingColorVertices==missingBefore+1);
    decoded.present|=1u<<GX_VA_CLR1;decoded.color[1][3]=.25f;
    assert(evaluateRasterColor(mixed,decoded,actual)&&actual[3]==.25f&&gxMissingColorVertices==missingBefore+1);
    decoded.present&=~(1u<<GX_VA_CLR1);
    colorRegisters.xf[0x1010]&=~1u;
    assert(planRasterColor(colorRegisters,colorMaterial,mixed)&&mixed.vertex==0);
    assert(evaluateRasterColor(mixed,decoded,actual)&&actual[3]==float(0x78)/255);
    colorRegisters.xfValid.reset(0x100c);
    assert(!planRasterColor(colorRegisters,colorMaterial,mixed,&colorError)&&colorError==RasterColorError::missingState);
    auto regs=draw.registers;
    CommandDraw changed{draw.primitive,draw.vertexCount,draw.indexCount,draw.vertices,draw.indices,draw.plan,draw.arrays,regs};
    regs.xf[0x100e]|=2;regs.xfValid.reset(0x100a);reject(changed,"GX raster color state missing");regs=draw.registers;
    regs.bp[0xf1]=2u<<21;reject(changed,"GX fog effect pending");regs=draw.registers;
    regs.bp[0x94]=0;reject(changed,"GX command texture token missing/stale");regs=draw.registers;
    regs.xf[0x1040]|=2;regs.xf[0x1041]|=2;reject(changed,"GX multiple projected textures pending");regs=draw.registers;
    regs.bpValid.reset(0x42);reject(changed,"GX raster state missing");regs=draw.registers;
    regs.bp[0x43]|=1;reject(changed,"GX pixel/depth format unsupported");regs=draw.registers;
    regs.bp[0x42]|=256;reject(changed,"GX destination alpha override unsupported");regs=draw.registers;
    regs.bp[0xf5]|=4;reject(changed,"GX depth texture effect pending");regs=draw.registers;
    regs.bp[0x41]|=2;reject(changed,"GX logic blend unsupported");regs=draw.registers;
    regs.bp[0x43]|=64;regs.bp[0xf3]=128|(1u<<16)|(7u<<19);
    reject(changed,"GX early depth plus alpha unsupported");regs=draw.registers;
    regs.bp[0xf3]=50|(200u<<8)|(4u<<16)|(1u<<19);
    reject(changed,"GX dual alpha comparison unsupported");
    {   // Relaxed planning never turns that interior interval into one test:
        // it keeps the lower bound for the hardware and reports the upper
        // one, which the adapter folds into the combiner alpha.
        RasterPlan interior;RasterError error;
        assert(planRaster(regs,interior,&error,FogPolicy::vertexApproximation,ColorStoragePolicy::rgba8ForRgba6,
            ClipPolicy::validateGeometry,true,true,{},true)&&interior.alphaBand&&!interior.alphaRelaxed&&
            interior.alpha.comparison==6&&interior.alpha.reference==51&&interior.alphaBandUpper==199);
        // Three appended stages: 4*(upper+1-alpha), eight times that, then
        // the captured original alpha times the factor. Colour passes through.
        PicaMaterialPlan band;band.stageCount=2;
        assert(appendAlphaBand(band,199)&&band.stageCount==5&&band.bufferUpdateAlpha==2&&band.stages[2].constant[3]==200&&
            band.stages[2].alpha.function==MaterialFunction::subtract&&band.stages[2].alpha.scale==2&&
            band.stages[3].alpha.function==MaterialFunction::add&&band.stages[3].alpha.scale==2&&
            band.stages[4].alpha.function==MaterialFunction::modulate&&band.stages[4].alpha.inputs[1].source==MaterialSource::buffer&&
            band.stages[4].rgb.function==MaterialFunction::replace&&band.stages[4].rgb.inputs[0].source==MaterialSource::previous);
        // No room for three stages, or no upper bound to fold: unchanged.
        PicaMaterialPlan full;full.stageCount=4;const auto untouched=full;
        assert(!appendAlphaBand(full,199)&&!std::memcmp(&full,&untouched,sizeof(full)));
        full.stageCount=2;assert(!appendAlphaBand(full,255));
        // Hyrule Field BP state: alpha>75 AND alpha<255. Strict planning refuses
        // it; relaxed planning keeps the lower bound and loses only alpha 255.
        regs.bp[0xf3]=0x0cff4b;RasterPlan field;
        assert(!planRaster(regs,field,&error,FogPolicy::vertexApproximation,ColorStoragePolicy::rgba8ForRgba6,
            ClipPolicy::validateGeometry,true,true)&&error==RasterError::alpha);
        assert(planRaster(regs,field,&error,FogPolicy::vertexApproximation,ColorStoragePolicy::rgba8ForRgba6,
            ClipPolicy::validateGeometry,true,true,{},true)&&field.alphaRelaxed&&field.alpha.comparison==4&&field.alpha.reference==75);
        // The adapter needs the explicit context opt-in as well as degraded draws.
        reject(changed,"GX dual alpha comparison unsupported");
        const auto before=arena;
        CommandDrawContext c;c.allowDegradedDraws=c.allowAlphaCompareFallback=true;
        c.allowVisualFallback=[](const CommandDraw&,DrawVisualFallback reason){return reason!=DrawVisualFallback::alphaCompare;};
        assert(!drawGxCommand(changed,&c)&&!std::strcmp(c.failure,"GX dual alpha comparison unsupported"));
        c={};c.allowDegradedDraws=c.allowAlphaCompareFallback=true;
        c.allowVisualFallback=[](const CommandDraw&,DrawVisualFallback){return true;};
        relaxedAlphaTest=true;
        assert(drawGxCommand(changed,&c)&&c.submitted==1&&c.simplifiedDraws==1&&
            c.simplifiedByReason[unsigned(DrawVisualFallback::alphaCompare)]==1);
        relaxedAlphaTest=false;arena=before;submissions=submitted;
    }
    regs=draw.registers;
    // Exact Ordon spring BP state: XNOR(GE185,LE100) accepts 101..184.
    // Neither degraded rendering nor the disjoint-OR opt-in may turn an
    // interior interval into acceptance, including UI passes: only the
    // explicit alpha-comparison opt-in draws it (as a combiner band).
    for(unsigned perspective:{0u,1u}){
        regs.bp[0xf3]=0xde64b9;regs.bp[0x43]=0x41;regs.bp[0x40]=7;regs.bp[0x41]=0xf42d;
        regs.xf[0x1026]=perspective;
        RasterPlan spring;spring.alphaUnion=true;spring.alpha.reference=165;
        RasterError error;
        assert(!planRaster(regs,spring,&error,FogPolicy::vertexApproximation,ColorStoragePolicy::rgba8ForRgba6,
            ClipPolicy::validateGeometry,true,true)&&error==RasterError::alpha);
        assert(!spring.alphaUnion&&!spring.alpha.reference&&!spring.writeRgb&&!spring.writeAlpha);
        CommandDrawContext c;c.allowDegradedDraws=c.allowAlphaUnion=c.allowConstantEarlyDepth=c.allowRgba6Storage=true;
        c.allowVisualFallback=[](const CommandDraw&,DrawVisualFallback){return true;};
        assert(!drawGxCommand(changed,&c)&&c.rejected==1&&c.failureKind==DrawFailureKind::state);
        assert(!std::strcmp(c.failure,"GX dual alpha comparison unsupported"));
        assert(!c.submitted&&!c.culled&&!c.skippedDraws&&!c.simplifiedDraws&&!c.alphaUnionDraws);
        assert(arena.used()==used&&submissions==submitted);
    }
    regs=draw.registers;
    // Captured first new-game F_SP108 material: alpha<=65 OR alpha>=210.
    // Opt-in preserves both disjoint fragment ranges; strict path still rejects.
    regs.bp[0xf3]=0x73d241;RasterPlan tails;RasterError tailError;
    assert(!planRaster(regs,tails,&tailError)&&tailError==RasterError::alpha);
    assert(planRaster(regs,tails,&tailError,FogPolicy::requireSupported,ColorStoragePolicy::rgb8Only,ClipPolicy::requireEnabled,false,true));
    assert(tails.alphaUnion&&tails.alpha.comparison==3&&tails.alpha.reference==65&&tails.alphaSecond.comparison==6&&tails.alphaSecond.reference==210);
    regs.bp[0x43]|=64;
    assert(!planRaster(regs,tails,&tailError,FogPolicy::requireSupported,ColorStoragePolicy::rgb8Only,ClipPolicy::requireEnabled,true,true)&&tailError==RasterError::earlyDepth);
    regs=draw.registers;
    regs.bp[0x41]&=~24u;RasterPlan masked;assert(planRaster(regs,masked)&&!masked.writeRgb&&!masked.writeAlpha);regs=draw.registers;
    // Actual early-Z-only state from the Zant ambush. A mask of zero
    // still writes depth for a transparent texel; late Z must test alpha.
    regs.bp[0x40]=0x17;regs.bp[0x41]=0x04;regs.bp[0x43]=0x41;regs.bp[0xf3]=0x640000;
    auto planMasked=[&](){return planRaster(regs,masked,nullptr,FogPolicy::requireSupported,
        ColorStoragePolicy::rgba8ForRgba6,ClipPolicy::requireEnabled,true);};
    assert(planMasked()&&!masked.earlyDepthAlpha&&masked.alpha.comparison==7);
    assert(!masked.writeRgb&&!masked.writeAlpha&&masked.depth.enabled&&masked.depth.write&&masked.depth.compare==3);
    regs.bp[0x41]|=16;assert(planMasked()&&masked.earlyDepthAlpha&&masked.writeAlpha);
    regs.bp[0x41]=0x04;regs.bp[0x43]=1;assert(planMasked()&&masked.alpha.comparison!=7);
    regs=draw.registers;
    regs.bp[0]|=3u<<14;CommandDrawContext culled;assert(drawGxCommand(changed,&culled)&&culled.culled==1&&arena.used()==used&&submissions==submitted);regs=draw.registers;
    scopeFailure=true;reject(changed,"test texture failure");scopeFailure=false;
    for(const char* reason:{"test texture failure","injected texture allocation failure","injected GPU completion failure"}){
        textureFailureReason=reason;textureFailure=true;reject(changed,reason);textureFailure=false;
    }
    textureFailureReason="test texture failure";
    reservationFailure=true;reject(changed,"test arena failure");reservationFailure=false;
    cancellationFailure=true;reject(aliased,"GX geometry cancellation failed");cancellationFailure=false;
    gpuFailure=true;reject(changed,"test GPU failure");gpuFailure=false;
    const u16 invalidIndices[]{0,1,4};auto badIndex=draw;badIndex.indexCount=3;badIndex.primitive=GX_TRIANGLES;
    badIndex.indices={reinterpret_cast<const u8*>(invalidIndices),sizeof(invalidIndices)};
    reject(badIndex,"GX command index out of range");
}
static void intervalAdapterCases(const CommandDraw& draw){
    const auto savedArena=arena;const auto savedSubmissions=submissions;
    safetyMode=true;
    auto regs=draw.registers;
    regs.bp[0xf3]=0xde64b9;regs.bp[0x43]=1;regs.bp[0x40]=7;regs.bp[0x41]=0xf42d;
    regs.xfValid.set(0x1026);regs.xf[0x1026]=0;
    CommandDraw changed{draw.primitive,draw.vertexCount,draw.indexCount,draw.vertices,draw.indices,draw.plan,draw.arrays,regs};
    auto allowed=[](){CommandDrawContext c;c.allowRgba6Storage=c.allowAlphaUnion=c.allowConstantEarlyDepth=true;
        c.allowRigidGpu=c.allowVertexFog=c.allowTriangleFog=c.allowDegradedDraws=c.allowQuantizedConstants=true;
        c.alphaIntervalPolicy={true,[](const AlphaIntervalTarget&,void*){return true;},nullptr};
        c.allowVisualFallback=[](const CommandDraw&,DrawVisualFallback){return true;};return c;};
    auto reset=[&](){arena=savedArena;submissions=savedSubmissions;};
    auto c=allowed();assert(drawGxCommand(changed,&c));
    assert(c.submitted==1&&c.alphaIntervalDraws==1&&!c.rejected&&!c.alphaUnionDraws&&!c.simplifiedDraws&&!c.skippedDraws);
    assert(c.cpuVertexReasons[0]==draw.vertexCount&&!rigidSubmissions);reset();
    for(unsigned mode=0;mode<10;++mode){
        c=allowed();auto saved=regs;
        switch(mode){
        case 0:c.alphaIntervalPolicy.enabled=false;break;
        case 1:c.alphaIntervalPolicy.ownsTarget=nullptr;break;
        case 2:regs.xf[0x1026]=1;break;
        case 3:regs.bp[0x43]|=64;regs.bp[0x40]|=16;break;
        case 4:regs.bp[0xf1]=2u<<21;break;
        case 5:reservationFailure=true;break;
        case 6:textureFailure=true;break;
        case 7:gpuFailure=true;break;
        case 8:gpuCommittedFailure=true;break;
        case 9:regs.bp[0xc0]|=3u<<16|1u<<18;break; // R8_EQ: no PICA lowering
        }
        assert(!drawGxCommand(changed,&c)&&c.rejected==1&&!c.submitted&&!c.alphaIntervalDraws&&!c.simplifiedDraws&&!c.skippedDraws);
        assert(c.failureKind==((mode==7||mode==8)?DrawFailureKind::submission:mode==6?DrawFailureKind::resource:mode==5?DrawFailureKind::geometry:DrawFailureKind::state));
        assert(c.rejectedByKind[unsigned(c.failureKind)]==1);
        if(mode!=8)assert(arena.used()==savedArena.used());
        reservationFailure=textureFailure=gpuFailure=gpuCommittedFailure=false;regs=saved;reset();
    }
    safetyMode=false;
    std::puts("PASS alpha interval adapter opt-in, CPU routing, UI/fog/early/resource/reserve/committed-submit failures and success-only counters");
}
static void depthlessOrthoCases(const CommandDraw& draw){
    const auto savedArena=arena;const auto savedSubmissions=submissions;
    auto regs=draw.registers;
    regs.bp[0]=0x10;regs.bp[0x28]=0;regs.bp[0xc0]=0x8ffff;regs.bp[0xc1]=0x8fff0;
    regs.bp[0x40]=6;regs.xf[0x103f]=0;regs.xf[0x1026]=1;regs.xfValid.set(0x1005);
    regs.xf[0x1025]=std::bit_cast<u32>(4.f);regs.xf[0x1005]=0;
    CommandDraw candidate{draw.primitive,draw.vertexCount,draw.indexCount,draw.vertices,draw.indices,draw.plan,draw.arrays,regs};
    auto reset=[&](){arena=savedArena;submissions=savedSubmissions;};
    signedQTestMode=true;CommandDrawContext reference;
    if(!drawGxCommand(candidate,&reference)){std::fprintf(stderr,"DEPTHLESS FIXTURE reference failure=%s\n",reference.failure);assert(false);}
    assert(reference.submitted==1&&!reference.depthlessOrthoDraws);
    const auto original=expandedVertices;const auto indices=expandedIndices;
    assert(original.size()==4);bool outside=false;
    for(const auto& v:original){assert(v.position[3]==1.f);outside|=v.position[2]<-1.f||v.position[2]>0.f;}
    assert(outside);reset();regs.xf[0x1005]=1;
    CommandDrawContext exact;assert(drawGxCommand(candidate,&exact));
    assert(exact.submitted==1&&exact.depthlessOrthoDraws==1&&exact.depthlessOrthoVertices==4);
    assert(!exact.rejected&&!exact.simplifiedDraws&&!exact.skippedDraws&&expandedIndices==indices);
    for(unsigned i=0;i<4;++i){auto want=original[i];want.position[2]=-.5f;
        assert(!std::memcmp(&want,&expandedVertices[i],sizeof(want)));}
    const auto accepted=regs;
    // These consumers make Z observable or leave the bounded proof. The
    // existing strict UI policy stays in charge; no global fallback is added.
    for(unsigned mode=0;mode<7;++mode){
        reset();regs=accepted;CommandDrawContext c;
        switch(mode){
        case 0:regs.bp[0x40]|=1;break;
        case 1:regs.bp[0x40]|=16;break;
        case 2:regs=draw.registers;regs.xf[0x1025]=std::bit_cast<u32>(4.f);regs.xf[0x1005]=1;regs.xfValid.set(0x1005);regs.bp[0x40]=6;break;
        case 3:regs.xfValid.reset(0x1026);break;
        case 4:regs.bp[0xf1]=2u<<21;break;
        case 5:reservationFailure=true;break;
        case 6:gpuFailure=true;break;
        }
        assert(!drawGxCommand(candidate,&c)&&c.rejected==1&&!c.submitted&&!c.depthlessOrthoDraws&&!c.simplifiedDraws&&!c.skippedDraws);
        assert(arena.used()==savedArena.used()&&submissions==savedSubmissions);
        reservationFailure=gpuFailure=false;
    }
    // Healthy contained geometry retains all bytes and incurs no normalization.
    reset();regs=accepted;regs.xf[0x1025]=draw.registers.xf[0x1025];CommandDrawContext healthy;
    assert(drawGxCommand(candidate,&healthy)&&!healthy.depthlessOrthoDraws&&!healthy.simplifiedDraws);
    for(const auto& v:expandedVertices)assert(v.position[2]>=-1.f&&v.position[2]<=0.f);
    reset();regs=accepted;const u16 bad[]{0,1,4};auto invalid=candidate;
    invalid.indexCount=3;invalid.indices={reinterpret_cast<const u8*>(bad),sizeof(bad)};CommandDrawContext refused;
    assert(!drawGxCommand(invalid,&refused)&&refused.rejected==1&&!refused.depthlessOrthoDraws&&arena.used()==savedArena.used());
    signedQTestMode=false;reset();
    std::puts("PASS depthless orthographic exact Z-only transport, topology/attributes, consumer refusals, healthy path and failures");
}
static void safetyFallbackCases(const CommandDraw& draw){
    // Isolated host backend state: no GPU execution or emulated scene evidence.
    const auto savedArena=arena;const auto savedSubmissions=submissions;
    safetyMode=true;
    auto registers=draw.registers;
    CommandDraw changed{draw.primitive,draw.vertexCount,draw.indexCount,draw.vertices,draw.indices,draw.plan,draw.arrays,registers};
    registers.bp[0xc0]|=3u<<16|1u<<18; // Valid GX compare equation (R8_EQ); untranslatable TEV operation.
    MaterialState material;PicaMaterialPlan p;MaterialPlanError error;
    assert(decodeMaterial(registers,material)&&!planPicaMaterial(material,p,&error)&&error==MaterialPlanError::operation);
    CommandDrawContext ui;ui.allowDegradedDraws=true;
    assert(!drawGxCommand(changed,&ui)&&ui.rejected==1&&!ui.skippedDraws&&!ui.simplifiedDraws);
    auto allowed=[](){CommandDrawContext c;c.allowDegradedDraws=true;
        c.allowVisualFallback=[](const CommandDraw&,DrawVisualFallback){return true;};return c;};
    auto context=allowed();assert(drawGxCommand(changed,&context));
    assert(context.submitted==1&&context.simplifiedDraws==1&&!context.rejected&&!context.skippedDraws);
    assert(context.simplifiedByReason[unsigned(DrawVisualFallback::tevMaterial)]==1);
    // A permitted approximation cannot hide later lifetime/backend failures.
    auto reject=[&](const char* expected,DrawFailureKind kind){
        const auto used=arena.used();const auto sent=submissions;
        auto c=allowed();assert(!drawGxCommand(changed,&c));
        assert(c.rejected==1&&c.failureKind==kind&&std::strcmp(c.failure,expected)==0);
        assert(c.rejectedByKind[unsigned(kind)]==1&&!c.submitted&&!c.skippedDraws&&!c.simplifiedDraws);
        for(auto n:c.simplifiedByReason)assert(!n);
        assert(arena.used()==used&&sent==submissions);
    };
    const auto token=registers.bp[0x96];registers.bp[0x96]=0; // Later discarded map2 must still be live.
    reject("GX command texture token missing/stale",DrawFailureKind::resource);registers.bp[0x96]=token;
    std::array<u8,96> retired{};retired[0]=1;retired[3]=retired[5]=8;retired[24]=1;retired[31]=32;
    TextureHandle retiredHandle{};assert(registerTexture(retired,retiredHandle));
    std::uint32_t retiredToken{};assert(textureCommandToken(retired.data()+32,retiredToken));
    assert(unregisterTexture(retiredHandle));registers.bp[0x96]=retiredToken;
    reject("GX command texture token missing/stale",DrawFailureKind::resource);registers.bp[0x96]=token;
    const auto stage2=registers.bp[0xc4];
    registers.bp[0xc4]=(stage2&~(15u<<12))|(2u<<12);registers.tevColorValid.reset(2);registers.tevColorValid.reset(3);
    reject("GX fallback TEV source missing/unsupported",DrawFailureKind::resource);registers.bp[0xc4]=stage2;
    scopeFailure=true;reject("test texture failure",DrawFailureKind::resource);scopeFailure=false;
    for(const char* why:{"injected texture allocation failure","injected GPU completion failure"}){
        textureFailure=true;textureFailureReason=why;reject(why,DrawFailureKind::resource);textureFailure=false;
    }
    textureFailureReason="test texture failure";
    reservationFailure=true;reject("test arena failure",DrawFailureKind::geometry);reservationFailure=false;
    gpuFailure=true;reject("test GPU failure",DrawFailureKind::submission);gpuFailure=false;
    const auto beforePartial=arena.used();const auto beforePartialSubmissions=submissions;
    context=allowed();gpuCommittedFailure=true;assert(!drawGxCommand(changed,&context));gpuCommittedFailure=false;
    assert(context.rejected==1&&context.failureKind==DrawFailureKind::submission&&std::strcmp(context.failure,"test GPU failure")==0);
    assert(!context.submitted&&!context.simplifiedDraws&&!context.skippedDraws);
    assert(arena.used()>beforePartial&&submissions==beforePartialSubmissions+1);
    // Clipping compatibility is separately classified and subject to pass policy.
    registers=draw.registers;registers.xf[0x1005]=1;registers.xfValid.set(0x1005);registers.xf[0x1025]=std::bit_cast<u32>(4.f);
    context=allowed();assert(drawGxCommand(changed,&context));
    assert(context.submitted==1&&context.simplifiedDraws==1&&context.simplifiedByReason[unsigned(DrawVisualFallback::clippedDepth)]==1);
    ui={};ui.allowDegradedDraws=true;assert(!drawGxCommand(changed,&ui)&&ui.rejected==1&&!ui.skippedDraws);
    auto denied=allowed();denied.allowVisualFallback=[](const CommandDraw&,DrawVisualFallback){return false;};
    assert(!drawGxCommand(changed,&denied)&&denied.rejected==1&&!denied.simplifiedDraws);
    safetyMode=false;arena=savedArena;submissions=savedSubmissions;
    std::puts("PASS degraded safety: classified TEV/clip submissions, conservative UI policy, stale discarded map, scope/allocation/completion/reservation/submission failures never succeed");
}
// Finite invalid divisors are omittable only after a complete geometry and
// attribute proof. These use the captured Ordon clip quad, live indirect order
// (source map 2 / coord 1), and explicit synthetic q: the guest log lacks XF/q.
static void indirectInvisibleCases(const CommandDraw& draw){
    auto regs=draw.registers;
    regs.bp[0]=(regs.bp[0]&~(15u<<10))|(1u<<16); // One TEV stage, one indirect.
    regs.bp[0x28]=0x40; // Destination map 0 / coord 0, raster channel 0.
    regs.bp[0x10]=0x270;
    for(unsigned reg:{6u,7u,8u,0x25u,0x27u,0x30u,0x31u,0x32u,0x33u})regs.bpValid.set(reg);
    regs.bp[6]=0x400204;regs.bp[7]=0xc8f000;regs.bp[8]=0;
    regs.bp[0x25]=0;regs.bp[0x27]=0x6d224a;
    for(unsigned reg=0x30;reg<=0x33;++reg)regs.bp[reg]=63;
    regs.xf[0x1005]=0;regs.xfValid.set(0x1005);
    regs.xf[0x1026]=0;
    const float projection[6]{1,0,1,0,0,-1.000795f};
    for(unsigned n=0;n<6;++n)regs.xf[0x1020+n]=std::bit_cast<u32>(projection[n]);
    const float identity[12]{1,0,0,0,0,1,0,0,0,0,1,0};
    for(unsigned n=0;n<12;++n)regs.xf[n]=std::bit_cast<u32>(identity[n]);
    auto texgen=[&](unsigned coordinate,bool projected,float q,bool variable=false){
        regs.xf[0x1040+coordinate]=(regs.xf[0x1040+coordinate]&~2u)|(projected?2u:0u);
        const unsigned selector=30+3*coordinate,shift=6*(coordinate+1);
        regs.matrixIndex[0]=(regs.matrixIndex[0]&~(63u<<shift))|(selector<<shift);
        float matrix[12]{1,0,0,0,0,1,0,0,variable?1.f:0.f,0,0,q};
        for(unsigned n=0;n<12;++n){regs.xf[4*selector+n]=std::bit_cast<u32>(matrix[n]);regs.xfValid.set(4*selector+n);}
    };
    const auto stride=draw.plan.packedStride();assert(stride==40);
    std::vector<u8> bytes(draw.vertices.begin(),draw.vertices.end());
    auto put=[&](unsigned vertex,unsigned offset,float value){const auto bits=std::bit_cast<u32>(value);
        for(unsigned byte=0;byte<4;++byte)bytes[vertex*stride+offset+byte]=u8(bits>>(24-8*byte));};
    const std::array<std::array<float,4>,4> live{{
        {-13369.540039f,43137.718750f,-1.000795f,-126.270966f},
        {-13369.540039f,28353.994141f,-1.000795f,-126.270966f},
        {-31426.800781f,28353.994141f,-1.000795f,-126.270966f},
        {-31426.800781f,43137.718750f,-1.000795f,-126.270966f}}};
    for(unsigned i=0;i<4;++i){put(i,0,-live[i][1]);put(i,4,live[i][0]);put(i,8,-live[i][3]);}
    CommandDraw candidate{GX_QUADS,4,0,bytes,{},draw.plan,draw.arrays,regs};
    const auto savedArena=arena;const auto sent=submissions;const auto used=arena.used();
    rigidTestMode=true; // Capture CPU submissions without fixture-specific TEV assumptions.
    auto context=[](){CommandDrawContext c;c.allowVertexIndirect=true;c.allowDirectSignedQDiagnostic=true;return c;};
    unsigned fatalCases=0;
    auto fatal=[&](DrawFailureKind kind=DrawFailureKind::protocol){
        auto c=context();c.allowDegradedDraws=true;c.allowBehindCameraCull=true;
        c.allowVisualFallback=[](const CommandDraw&,DrawVisualFallback){return true;};
        const bool result=drawGxCommand(candidate,&c);++fatalCases;
        if(result)std::fprintf(stderr,"indirect fatal case %u unexpectedly succeeded\n",fatalCases);
        assert(!result&&c.rejected==1&&!c.submitted&&!c.culled&&!c.skippedDraws&&!c.simplifiedDraws);
        assert(c.failureKind==kind&&c.rejectedByKind[unsigned(kind)]==1);
        assert(!c.projectiveClipDraws&&!c.projectiveClipTriangles&&arena.used()==used&&submissions==sent);
    };
#if defined(DUSK_INDIRECT_INVISIBLE_BASE_ONLY)
    const unsigned modes[]{0};
#elif defined(DUSK_INDIRECT_INVISIBLE_SOURCE_ONLY)
    const unsigned modes[]{1};
#else
    const unsigned modes[]{0,1,2};
#endif
    for(unsigned mode:modes)for(float q:{-1.f,0.f}){
        texgen(0,mode!=1,mode!=1?q:1);texgen(1,mode!=0,mode!=0?q:1);
        VertexTransformPlan transform;assert(planVertexTransform(regs,3,transform));
        for(unsigned i=0;i<4;++i){DecodedVertex decoded;TransformedVertex vertex;
            assert(candidate.vertex(i,decoded)&&transformVertex(transform,decoded,vertex));
            assert(vertex.clip==live[i]);}
        for(unsigned clip:{0u,1u}){
            regs.xf[0x1005]=clip;auto c=context();
            const bool accepted=drawGxCommand(candidate,&c);
            if(!accepted)std::fprintf(stderr,"indirect invisible mode=%u q=%g failed: %s\n",mode,double(q),c.failure);
            assert(accepted&&c.culled==1&&!c.submitted&&!c.rejected&&!c.skippedDraws&&!c.simplifiedDraws);
            assert(!c.cameraCulled&&arena.used()==used&&submissions==sent);
        }
        regs.xf[0x1005]=0;
    }
#if defined(DUSK_INDIRECT_INVISIBLE_BASE_ONLY) || defined(DUSK_INDIRECT_INVISIBLE_SOURCE_ONLY)
    rigidTestMode=false;
    std::puts("PASS focused invisible indirect divisor class");
    return;
#endif
    // Invalid work stays fatal even if all geometry is far offscreen.
    texgen(0,true,-1);texgen(1,true,-1);
    regs.bpValid.set(0x22);regs.bp[0x22]=0x606;
    for(auto primitive:{GX_POINTS,GX_LINES,GX_LINESTRIP}){candidate.primitive=primitive;fatal();}
    candidate.primitive=GX_QUADS;
    for(bool* injection:{&scopeFailure,&textureFailure}){*injection=true;fatal(DrawFailureKind::resource);*injection=false;}
    for(const char* why:{"injected texture allocation failure","injected GPU completion failure"}){
        textureFailureReason=why;textureFailure=true;fatal(DrawFailureKind::resource);textureFailure=false;}
    textureFailureReason="test texture failure";
    reservationFailure=true;fatal(DrawFailureKind::geometry);reservationFailure=false;
    cancellationFailure=true;fatal(DrawFailureKind::geometry);cancellationFailure=false;
    const auto token=regs.bp[0x96];regs.bp[0x96]=0;fatal(DrawFailureKind::state);regs.bp[0x96]=token;
    regs.bpValid.reset(7);fatal(DrawFailureKind::state);regs.bpValid.set(7);
    auto shortDraw=candidate;candidate.vertices=candidate.vertices.first(bytes.size()-1);fatal();candidate.primitive=shortDraw.primitive;candidate.indexCount=shortDraw.indexCount;candidate.indices=shortDraw.indices;candidate.vertices=shortDraw.vertices;
    const u16 badIndices[]{0,1,4};candidate.primitive=GX_TRIANGLES;candidate.indexCount=3;
    candidate.indices={reinterpret_cast<const u8*>(badIndices),sizeof(badIndices)};fatal();candidate.primitive=shortDraw.primitive;candidate.indexCount=shortDraw.indexCount;candidate.indices=shortDraw.indices;candidate.vertices=shortDraw.vertices;
    auto alias=candidate;candidate.vertices={reinterpret_cast<const u8*>(storage.data()+((used+15)&~std::size_t(15))),bytes.size()};fatal();candidate.vertices=alias.vertices;
    for(float bad:{std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){
        // A later vertex/coordinate must be checked after the initial invalid q.
        const auto old=std::bit_cast<float>(regs.xf[143]);regs.xf[143]=std::bit_cast<u32>(bad);fatal();regs.xf[143]=std::bit_cast<u32>(old);
        put(3,4,bad);fatal();put(3,4,live[3][0]);
        put(3,24,bad);fatal();put(3,24,uvShiftS+.3f);
    }
    // Finite positive q whose division overflows also remains fatal.
    texgen(0,false,1);texgen(1,true,std::numeric_limits<float>::denorm_min());fatal();
    texgen(0,true,-1);texgen(1,true,-1);
    // Screen-overlapping negative-w geometry is not a clip-disabled proof;
    // remove compatibility to make sure it cannot justify a successful cut.
    for(unsigned i=0;i<4;++i){put(i,0,0);put(i,4,0);put(i,8,1);}
    regs.xf[0x1005]=1;auto strict=context();
    assert(!drawGxCommand(candidate,&strict)&&strict.rejected==1&&!strict.culled&&arena.used()==used);
    // Visible orthographic UI with invalid source/base q stays strict.
    regs.xf[0x1026]=1;regs.xf[0x1005]=0;
    for(unsigned i=0;i<4;++i){put(i,0,i==1||i==2?-.5f:0);put(i,4,i>=2?.5f:0);put(i,8,0);}
    fatal();regs.xf[0x1026]=0;
    // Mixed visible/invisible triangles: q is generated independently from
    // each texgen's input S. Preserve every surviving attribute bit-for-bit.
    bytes.resize(6*stride);
    for(unsigned i=0;i<6;++i){std::memcpy(bytes.data()+i*stride,draw.vertices.data(),stride);
        put(i,0,i==1||i==4?-.5f:0);put(i,4,i<3?-4.f:(i==5?.5f:0));put(i,8,-1);
        put(i,16,.5f+.0625f*i);put(i,24,.75f+.0625f*i);}
    candidate.primitive=GX_TRIANGLES;candidate.vertexCount=6;candidate.vertices=bytes;
    for(unsigned mode:{0u,1u,2u}){
        texgen(0,mode!=1,0,true);texgen(1,mode!=0,0,true);
        auto control=context();assert(drawGxCommand(candidate,&control)&&control.submitted==1);
        const auto expected=rigidExpected;arena=savedArena;submissions=sent;
        for(unsigned i=0;i<3;++i){if(mode!=1)put(i,16,-1);if(mode!=0)put(i,24,-1);}
        // Still reject if any invalid divisor reaches a visible triangle.
        for(float q:{-1.f,0.f}){put(4,mode==1?24:16,q);fatal();}
        put(4,mode==1?24:16,mode==1?1.f:.75f);
        regs.xf[0x1005]=1;fatal();regs.xf[0x1005]=0;
        safetyMode=true;gpuFailure=true;fatal(DrawFailureKind::submission);gpuFailure=false;
        gpuCommittedFailure=true;auto committed=context();assert(!drawGxCommand(candidate,&committed));
        assert(committed.failureKind==DrawFailureKind::submission&&!committed.submitted&&!committed.culled&&!committed.projectiveClipDraws);
        gpuCommittedFailure=false;safetyMode=false;arena=savedArena;submissions=sent;
        auto trimmed=context();assert(drawGxCommand(candidate,&trimmed)&&trimmed.submitted==1&&!trimmed.culled&&!trimmed.rejected);
        assert(trimmed.projectiveClipDraws==1&&trimmed.projectiveClipTriangles==1&&!trimmed.simplifiedDraws&&!trimmed.skippedDraws);
        assert((expandedIndices==std::vector<u16>{3,3,3,3,4,5}));
        for(unsigned i=0;i<3;++i)assert(!std::memcmp(&rigidExpected[i],&rigidExpected[3],sizeof(GpuVertex)));
        for(unsigned i=3;i<6;++i)assert(!std::memcmp(&rigidExpected[i],&expected[i],sizeof(GpuVertex)));
        arena=savedArena;submissions=sent;
        // Finite tiny positive q is consumed as supplied, including indirect
        // division; there is no positive epsilon cutoff or substituted q.
        const unsigned offset=mode==1?24:16;
        put(4,offset,0x1p-30f);auto tiny=context();
        assert(drawGxCommand(candidate,&tiny)&&tiny.submitted==1&&!tiny.rejected);
        assert(rigidExpected[4].textureQ==(mode==1?1.f:0x1p-30f));
        put(4,offset,mode==1?1.f:.75f);arena=savedArena;submissions=sent;
        // A shared vertex with invalid q cannot disappear while any incident
        // triangle might cover the screen, regardless of the other triangle.
        const u16 shared[]{0,1,2,0,4,5};candidate.indexCount=6;
        candidate.indices={reinterpret_cast<const u8*>(shared),sizeof(shared)};fatal();candidate.indexCount=0;candidate.indices={};
        for(unsigned i=0;i<3;++i){put(i,16,.5f+.0625f*i);put(i,24,.75f+.0625f*i);}
    }
    // The indirect source is sampled on the CPU. A single negative source
    // chart yields the same warp as multiplying its whole matrix by -1.
    texgen(0,true,2);
    texgen(1,true,1);
    auto chartControl=context();
    assert(drawGxCommand(candidate,&chartControl)&&chartControl.vertexIndirectDraws==1);
    const auto positiveChart=rigidExpected;
    arena=savedArena;submissions=sent;
    for(unsigned n=0;n<12;++n)regs.xf[4*33+n]^=0x80000000u;
    auto signedChart=context();
    assert(drawGxCommand(candidate,&signedChart)&&signedChart.vertexIndirectDraws==1);
    assert(signedChart.submitted==1&&!signedChart.skippedDraws&&!signedChart.simplifiedDraws);
    assert(positiveChart.size()==rigidExpected.size()&&
        !std::memcmp(positiveChart.data(),rigidExpected.data(),positiveChart.size()*sizeof(GpuVertex)));
    arena=savedArena;submissions=sent;
    // Even an unreferenced vertex with the opposite sign defeats the
    // whole-draw proof. The remaining visible negative-Q triangle stays strict.
    texgen(1,true,0,true);
    for(unsigned i=0;i<6;++i)put(i,24,-1);
    put(0,24,1);
    const u16 visible[]{3,4,5};
    candidate.indexCount=3;
    candidate.indices={reinterpret_cast<const u8*>(visible),sizeof(visible)};
    fatal();
    rigidTestMode=false;
    std::printf("PASS invisible indirect: captured clip quad, base/source/both nonpositive q, clip policies, exact surviving attributes; %u fatal injections\n",fatalCases);
}
int main(){
    // Every constant byte and comparison: preserve early depth when alpha
    // discards all color; admit known-passing tests, refuse unknown textures.
    for(unsigned value=0;value<256;++value)for(unsigned comparison=0;comparison<8;++comparison){
        PicaMaterialPlan m;m.stageCount=2;m.stages[0].constant[3]=value;
        m.stages[0].alpha.inputs[0]={MaterialSource::constant,MaterialOperand::alpha};
        m.stages[1].alpha.inputs[0]={MaterialSource::previous,MaterialOperand::alpha};
        RasterPlan r;r.earlyDepthAlpha=true;r.alpha={std::uint8_t(comparison),128};r.writeRgb=r.writeAlpha=true;r.depth={true,6,true};
        const bool expected[]={false,value<128,value==128,value<=128,value>128,value!=128,value>=128,true};
        assert(resolveConstantEarlyAlpha(m,r)&&r.alpha.comparison==7&&!r.earlyDepthAlpha);
        assert(r.depth.enabled&&r.depth.write&&r.depth.compare==6&&r.writeRgb==expected[comparison]&&r.writeAlpha==expected[comparison]);
        r.earlyDepthAlpha=true;m.stages[0].alpha.inputs[0].source=MaterialSource::texture0;
        assert(!resolveConstantEarlyAlpha(m,r)&&r.earlyDepthAlpha);
    }
    // RGB buffer captures cannot become a false constant-alpha proof when an
    // alpha stage selects R/G/B. Its untouched alpha constant remains usable.
    for(unsigned operand=1;operand<5;++operand){
        PicaMaterialPlan m;m.stageCount=3;m.bufferUpdateRgb=1;m.bufferConstant={128,128,128,128};
        m.stages[2].alpha.inputs[0]={MaterialSource::buffer,static_cast<MaterialOperand>(operand)};
        RasterPlan r;r.earlyDepthAlpha=true;r.alpha={2,128};
        assert(resolveConstantEarlyAlpha(m,r)==(operand==1));
        assert(r.earlyDepthAlpha==(operand!=1));
    }
    // Exact GX tiled intensity/alpha sampling at pixel centers, bilinear
    // boundaries, negative repeat and mirror. Distinct bytes catch ABG order.
    std::array<u8,128> indirectPixels{};
    Texture indirectImage{};indirectImage.image=indirectPixels;indirectImage.width=indirectImage.height=8;
    indirectImage.format=3;
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){
        const auto at=((y/4)*2+x/4)*32+((y%4)*4+x%4)*2;
        indirectPixels[at]=x*20;indirectPixels[at+1]=y*30;
    }
    assert(validIndirectImage(indirectImage));std::array<float,3> sample;
    assert(sampleIndirectImage(indirectImage,2.5f/8,3.5f/8,sample)&&(sample==std::array<float,3>{40,90,90}));
    indirectImage.minFilter=indirectImage.magFilter=1;
    assert(sampleIndirectImage(indirectImage,3.f/8,4.f/8,sample)&&(sample==std::array<float,3>{50,105,105}));
    indirectImage.wrapS=1;
    assert(sampleIndirectImage(indirectImage,-.5f/8,0,sample)&&sample[0]==140&&sample[1]==0);
    assert(sampleIndirectImage(indirectImage,0,0,sample)&&sample[0]==70);
    indirectImage.wrapS=2;
    assert(sampleIndirectImage(indirectImage,-.5f/8,0,sample)&&sample[0]==0);
    assert(sampleIndirectImage(indirectImage,8.5f/8,0,sample)&&sample[0]==140);
    assert(!sampleIndirectImage(indirectImage,std::numeric_limits<float>::infinity(),0,sample));
    indirectImage.image=indirectImage.image.first(127);assert(!validIndirectImage(indirectImage));
    indirectImage.image=indirectPixels;indirectImage.mipCount=2;assert(!validIndirectImage(indirectImage));
    indirectImage.mipCount=1;indirectImage.minFilter=6;assert(!validIndirectImage(indirectImage));
    indirectImage.minFilter=0;
    indirectImage.magFilter=0;indirectImage.wrapS=indirectImage.wrapT=0;
    for(unsigned format=0;format<3;++format){indirectImage.format=format;indirectPixels.fill(0xa3);
        assert(validIndirectImage(indirectImage)&&sampleIndirectImage(indirectImage,.5f/8,.5f/8,sample));
        assert(sample[0]==(format==1?163:170)&&sample[1]==(format==0?170:format==1?163:51));}
    std::array<u8,224> mipPixels{};mipPixels.fill(50);std::fill_n(mipPixels.data(),128,10);
    std::fill_n(mipPixels.data()+128,32,90);
    indirectImage.image=mipPixels;indirectImage.format=3;indirectImage.mipCount=3;
    indirectImage.minFilter=5;indirectImage.magFilter=1;indirectImage.maxLod=2;indirectImage.lodBias=.5;
    assert(validIndirectImage(indirectImage)&&sampleIndirectImage(indirectImage,.4,.7,sample)&&sample[0]==50);
    indirectImage.lodBias=1.5;
    assert(sampleIndirectImage(indirectImage,.4,.7,sample)&&sample[0]==70);
    indirectImage.minFilter=3;
    assert(sampleIndirectImage(indirectImage,.4,.7,sample)&&sample[0]==50);
    indirectImage.minLod=indirectImage.maxLod=1;indirectImage.lodBias=-4;
    assert(sampleIndirectImage(indirectImage,.4,.7,sample)&&sample[0]==90);
    // Fog coefficients and all five pinned curve functions, independently of
    // the GPU substitutions below. Range changes are measured off axis.
    CommandRegisters fogRegisters;VertexFogPlan fogPlan;
    for(unsigned reg:{0xe8u,0xeeu,0xefu,0xf0u,0xf1u,0xf2u})fogRegisters.bpValid.set(reg);
    fogRegisters.bp[0xee]=std::bit_cast<u32>(1.f)>>12;
    fogRegisters.bp[0xf1]=(2u<<21)|(1u<<20);
    assert(planVertexFog(fogRegisters,fogPlan));
    auto almost=[](double a,double b){assert(std::abs(a-b)<1e-6);};
    almost(evaluateVertexFog(fogPlan,{0,0,-1,1},400),0);
    almost(evaluateVertexFog(fogPlan,{0,0,-.5,1},400),.5);
    almost(evaluateVertexFog(fogPlan,{0,0,0,1},400),1);
    for(unsigned function:{4u,5u,6u,7u}) {
        fogRegisters.bp[0xf1]=(function<<21)|(1u<<20);
        assert(planVertexFog(fogRegisters,fogPlan));
        const double expected=function==4?.9375:function==5?.75:function==6?.0625:.25;
        almost(evaluateVertexFog(fogPlan,{0,0,-.5,1},400),expected);
    }
    fogRegisters.bp[0xf1]=2u<<21;
    fogRegisters.bp[0xef]=0x40000f;fogRegisters.bp[0xf0]=2;
    fogRegisters.bp[0xee]=std::bit_cast<u32>(.0625f)>>12;
    assert(planVertexFog(fogRegisters,fogPlan));
    almost(evaluateVertexFog(fogPlan,{0,0,-.5,1},400),.5);
    fogRegisters.bp[0xe8]=1024|542;fogRegisters.xfValid.set(0x101a);
    fogRegisters.xf[0x101a]=std::bit_cast<u32>(200.f);
    for(unsigned reg=0xe9;reg<=0xed;++reg){fogRegisters.bpValid.set(reg);fogRegisters.bp[reg]=64|(64u<<12);}
    assert(planVertexFog(fogRegisters,fogPlan));
    almost(evaluateVertexFog(fogPlan,{0,0,-.5,1},400),.5);
    almost(evaluateVertexFog(fogPlan,{0,1,-.5,1},400),std::sqrt(.5));
    // A=0,C>=0 makes only linear/exp/exp2 fog identically zero. Range
    // coefficients still require validation; reversed curves remain enabled.
    const auto ranged=fogRegisters;
    for(unsigned function:{2u,4u,5u,6u,7u}){
        fogRegisters=ranged;fogRegisters.bp[0xee]=0;
        fogRegisters.bp[0xf1]=(function<<21)|(std::bit_cast<u32>(.25f)>>12);
        assert(planVertexFog(fogRegisters,fogPlan));
        assert(fogPlan.enabled==(function==6||function==7));
        if(!fogPlan.enabled)for(float x:{-1.f,0.f,1.f})for(float z:{-1.f,-.5f,0.f})almost(evaluateVertexFog(fogPlan,{0,x,z,1},400),0);
    }
    fogRegisters=ranged;
    fogRegisters.bpValid.reset(0xed);assert(!planVertexFog(fogRegisters,fogPlan));
    PicaMaterialPlan full;full.textureCount=3;fogPlan.enabled=true;
    assert(!appendVertexFog(full,fogPlan)&&full.textureCount==3&&full.stageCount==0);
    assert(appendVertexFog(full,fogPlan,true)&&full.textureCount==3&&full.stageCount==1);
    full={};full.stageCount=6;assert(!appendVertexFog(full,fogPlan)&&full.stageCount==6);
    assert(arena.attach(storage)&&arena.begin());
    std::array<std::array<u8,96>,3> textures{};std::array<TextureHandle,3> handles{};GXTexObj objects[3];
    for(unsigned i=0;i<3;++i){auto& t=textures[i];t[0]=1;t[3]=t[5]=8;t[24]=1;t[31]=32;
        assert(registerTexture(t,handles[i]));GXInitTexObj(&objects[i],t.data()+32,8,8,GX_TF_I8,GX_CLAMP,GX_CLAMP,GX_FALSE);
        GXLoadTexObj(&objects[i],static_cast<GXTexMapID>(i));}
    CommandMemory memory;CommandProcessor processor(memory);u8 scratch[256];CommandStream stream(processor,scratch);assert(setCommandStream(&stream));
    // Rebind typed original resource objects while command mode is active.
    // A preceding complete NOP executes before mutation, preserving order.
    assert(stream.write({reinterpret_cast<const u8*>("\0"),1}));
    for(unsigned i=0;i<3;++i){
        GXInitTexObj(&objects[i],textures[i].data()+32,8,8,GX_TF_I8,GX_CLAMP,GX_CLAMP,GX_FALSE);
        GXLoadTexObj(&objects[i],static_cast<GXTexMapID>(i));
    }
    const float projection[]{.005f,0,0,-1,0,-2.0f/240,0,1,0,0,.5,-.5,0,0,0,1};
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0};
    GXSetProjection(projection,GX_ORTHOGRAPHIC);GXSetViewport(0,0,400,240,0,1);
    GXLoadPosMtxImm(identity,GX_PNMTX0);GXSetCurrentMtx(GX_PNMTX0);
    GXSetScissor(0,0,400,240);GXSetBlendMode(GX_BM_BLEND,GX_BL_SRCALPHA,GX_BL_INVSRCALPHA,GX_LO_COPY);
    GXSetZMode(GX_TRUE,GX_LEQUAL,GX_TRUE);GXSetZCompLoc(GX_FALSE);GXSetAlphaCompare(GX_ALWAYS,0,GX_AOP_AND,GX_ALWAYS,0);
    GXSetNumChans(1);GXSetChanCtrl(GX_COLOR0A0,GX_FALSE,GX_SRC_REG,GX_SRC_VTX,0,GX_DF_NONE,GX_AF_NONE);
    GXSetNumTexGens(3);GXClearVtxDesc();GXSetVtxDesc(GX_VA_POS,GX_DIRECT);GXSetVtxDesc(GX_VA_CLR0,GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,GX_F32,0);GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_CLR0,GX_CLR_RGBA,GX_RGBA8,0);
    for(unsigned i=0;i<3;++i){GXSetVtxDesc(static_cast<GXAttr>(GX_VA_TEX0+i),GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0,static_cast<GXAttr>(GX_VA_TEX0+i),GX_TEX_ST,GX_F32,0);
        GXSetTexCoordGen2(static_cast<GXTexCoordID>(i),GX_TG_MTX2x4,static_cast<GXTexGenSrc>(GX_TG_TEX0+i),GX_IDENTITY,GX_FALSE,GX_PTIDENTITY);}
    assert(stream.finish());
    assert(stream.writeBp(0x41,processor.registers().bp[0x41]|24));
    for(unsigned reg:{0x42u,0xf1u,0xf5u})assert(stream.writeBp(reg,0));
    alignas(32) u8 commands[1024];GDLObj gd;GDInitGDLObj(&gd,commands,sizeof(commands));GDSetCurrent(&gd);
    GDSetGenMode2(3,1,3,0,GX_CULL_NONE);
    for(unsigned i=0;i<4;++i)GDSetTevSwapModeTable(static_cast<GXTevSwapSel>(i),GX_CH_RED,GX_CH_GREEN,GX_CH_BLUE,GX_CH_ALPHA);
    GDSetTevOrder(GX_TEVSTAGE0,GX_TEXCOORD2,GX_TEXMAP0,GX_COLOR0A0,GX_TEXCOORD0,GX_TEXMAP1,GX_COLOR0A0);
    GDSetTevOrder(GX_TEVSTAGE2,GX_TEXCOORD1,GX_TEXMAP2,GX_COLOR0A0,GX_TEXCOORD_NULL,GX_TEXMAP_NULL,GX_COLOR_NULL);
    for(unsigned i=0;i<3;++i){auto stage=static_cast<GXTevStageID>(i);
        GDSetTevColorCalc(stage,GX_CC_ZERO,GX_CC_TEXC,i?GX_CC_CPREV:GX_CC_RASC,GX_CC_ZERO,GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV);
        GDSetTevAlphaCalcAndSwap(stage,GX_CA_ZERO,GX_CA_TEXA,i?GX_CA_APREV:GX_CA_RASA,GX_CA_ZERO,GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV,GX_TEV_SWAP0,GX_TEV_SWAP0);
        GDSetTevDirect(stage);}
    const auto bytes=GDGetCurrOffset();GDSetCurrent(nullptr);assert(processor.execute({commands,bytes}));
    CommandDrawContext context;
    processor.setSink([](const CommandDraw& draw,void* data){
        auto& context=*static_cast<CommandDrawContext*>(data);assert(drawGxCommand(draw,&context));negativeCases(draw);static bool safetyChecked=false;if(!safetyChecked){intervalAdapterCases(draw);safetyFallbackCases(draw);depthlessOrthoCases(draw);signedQCases(draw);carrierAdapterCases(draw);safetyChecked=true;}return true;
        
    },&context);
    std::vector<u8> list{u8(GX_QUADS),0,4};auto scalar=[&](float f){auto bits=std::bit_cast<u32>(f);for(int s=24;s>=0;s-=8)list.push_back(bits>>s);};
    for(unsigned i=0;i<4;++i){scalar(i==1||i==2?400:0);scalar(i>=2?240:0);scalar(0);
        list.insert(list.end(),{64,128,192,255});for(unsigned t=0;t<3;++t){scalar(.1f+.2f*t);scalar(.2f+.2f*t);}}
    assert(processor.execute(list));assert(context.submitted==1&&context.rejected==0&&submissions==1&&binds>=3);
    const float projectedMatrix[]{1,0,0,0,0,1,0,0,.1f,.2f,0,1};
    GXLoadTexMtxImm(projectedMatrix,GX_TEXMTX0,GX_MTX3x4);
    GXSetTexCoordGen2(GX_TEXCOORD0,GX_TG_MTX3x4,GX_TG_TEX0,GX_TEXMTX0,GX_FALSE,GX_PTIDENTITY);
    assert(stream.finish());
    list={u8(GX_QUADS),0,4};
    for(unsigned i=0;i<4;++i){scalar(i==1||i==2?400:0);scalar(i>=2?240:0);scalar(0);
        list.insert(list.end(),{64,128,192,255});for(unsigned t=0;t<3;++t){scalar(.1f+.2f*t+(t==0?.05f*i:0));scalar(.2f+.2f*t+(t==0?.03f*i:0));}}
    assert(processor.execute(list));assert(context.submitted==2&&context.rejected==0&&submissions==2);
    // Real materials tile beyond [0,1]. Vertex clamping changes interpolation;
    // preserve coordinates and padded V mapping for the fragment sampler.
    GXSetTexCoordGen2(GX_TEXCOORD0,GX_TG_MTX2x4,GX_TG_TEX0,GX_IDENTITY,GX_FALSE,GX_PTIDENTITY);
    assert(stream.finish());uvShiftS=-2.25f;uvShiftT=3.5f;
    list={u8(GX_QUADS),0,4};
    for(unsigned i=0;i<4;++i){scalar(i==1||i==2?400:0);scalar(i>=2?240:0);scalar(0);
        list.insert(list.end(),{64,128,192,255});for(unsigned t=0;t<3;++t){scalar(uvShiftS+.1f+.2f*t);scalar(uvShiftT+.2f+.2f*t);}}
    assert(processor.execute(list));assert(context.submitted==3&&context.rejected==0&&submissions==3);
    // Three original samplers plus fog use primary alpha only after proving
    // constant alpha over this entire draw. A single varying vertex rejects.
    processor.setSink([](const CommandDraw& draw,void*){
        auto regs=draw.registers;
        for(unsigned reg:{0xe8u,0xeeu,0xefu,0xf0u,0xf1u,0xf2u})regs.bpValid.set(reg);
        regs.bp[0xe8]=regs.bp[0xef]=regs.bp[0xf0]=0;
        regs.bp[0xee]=std::bit_cast<u32>(1.f)>>12;regs.bp[0xf1]=(2u<<21)|(1u<<20);regs.bp[0xf2]=0xabcdef;
        CommandDraw changed{draw.primitive,draw.vertexCount,draw.indexCount,draw.vertices,draw.indices,draw.plan,draw.arrays,regs};
        constantFogTest=true;CommandDrawContext constant;constant.allowVertexFog=true;
        assert(drawGxCommand(changed,&constant)&&constant.vertexFogDraws==1);
        // Constant material alpha need not equal one.
        regs.xf[0x1010]=0;regs.xfValid.set(0x100c);regs.xf[0x100c]=0xffffff49;
        assert(drawGxCommand(changed,&constant)&&constant.vertexFogDraws==2);
        const auto used=arena.used();
        regs.xf[0x1010]=1;
        std::vector<u8> varied(draw.vertices.begin(),draw.vertices.end());
        varied[(draw.vertexCount-1)*draw.plan.packedStride()+15]=127;changed.vertices=varied;
        CommandDrawContext varying;varying.allowVertexFog=true;
        assert(!drawGxCommand(changed,&varying)&&std::strcmp(varying.failure,"GX vertex fog TEV/texture capacity")==0&&arena.used()==used);
        // Transparent three-texture effects with unused primary RGB can
        // carry fog in red while preserving every varying alpha sample.
        const auto oldColor=regs.bp[0xc0];regs.bp[0xc0]=(oldColor&~0xffffu)|0x8fff;
        redFogTest=true;CommandDrawContext red;red.allowVertexFog=true;
        assert(drawGxCommand(changed,&red)&&red.vertexFogDraws==1&&!red.triangleFogDraws);
        redFogTest=false;regs.bp[0xc0]=oldColor;
        // Opt-in opaque LEQUAL fallback retains varying original alpha and
        // reserves a separate aligned fog stream with identical positions.
        regs.bp[0x41]&=~1u;
        CommandDrawContext paired;paired.allowVertexFog=paired.allowTriangleFog=true;
        assert(drawGxCommand(changed,&paired)&&paired.triangleFogDraws==1&&paired.vertexFogDraws==1);
        regs.bp[0x41]|=1;
        CommandDrawContext blended;blended.allowVertexFog=blended.allowTriangleFog=true;
        const auto afterPair=arena.used();
        assert(!drawGxCommand(changed,&blended)&&arena.used()==afterPair);
        changed.vertices=draw.vertices;regs.xf[0x1010]=2;regs.xfValid.set(0x100a);regs.xf[0x100a]=255;
        CommandDrawContext lit;lit.allowVertexFog=true;
        assert(!drawGxCommand(changed,&lit)&&std::strcmp(lit.failure,"GX vertex fog TEV/texture capacity")==0&&arena.used()==afterPair);
        // A plan that needs a lowering (halved TEX2*PREV on the last stage)
        // with no fog room under the blend keeps the Alpha 6 stand-in.
        {
            regs.xf[0x1010]=1;changed.vertices=varied; // the blended varying-alpha draw above
            const auto lastStage=regs.bp[0xc4];regs.bp[0xc4]|=3u<<20;
            MaterialState state;PicaMaterialPlan plan;
            assert(decodeMaterial(regs,state)&&planPicaMaterial(state,plan,nullptr,MaterialPolicy::quantizedConstants));
            assert((plan.lowering&MaterialLowering::halve)&&plan.textureCount==3);
            CommandDrawContext lowered;lowered.allowVertexFog=lowered.allowTriangleFog=lowered.allowQuantizedConstants=true;
            assert(!drawGxCommand(changed,&lowered)&&std::strcmp(lowered.failure,"GX vertex fog TEV/texture capacity")==0);
            lowered={};lowered.allowVertexFog=lowered.allowTriangleFog=lowered.allowQuantizedConstants=lowered.allowDegradedDraws=true;
            lowered.allowVisualFallback=[](const CommandDraw&,DrawVisualFallback){return true;};
            standInFogTest=true;
            assert(drawGxCommand(changed,&lowered)&&lowered.vertexFogDraws==1&&lowered.simplifiedDraws==1);
            assert(lowered.simplifiedByReason[unsigned(DrawVisualFallback::tevMaterial)]==1);
            standInFogTest=false;regs.bp[0xc4]=lastStage;
        }
        constantFogTest=false;return true;
    },nullptr);
    assert(processor.execute(list));
    std::puts("PASS three-texture fog: complete vertex alpha and register-alpha constants; varying alpha and lighting reject without allocation");
    processor.setSink([](const CommandDraw& draw,void*){indirectInvisibleCases(draw);return true;});
    assert(processor.execute(list));
#if defined(DUSK_INDIRECT_INVISIBLE_FOCUSED)
    return 0;
#endif
    // Real draw adapter lowers a static indirect matrix using the original
    // registered I8 image. Strict callers still reject. No extra GPU sampler.
    processor.setSink([](const CommandDraw& draw,void*){
        auto regs=draw.registers;regs.bp[0]|=1u<<16;regs.bp[0x10]=0x270;
        for(unsigned reg:{6u,7u,8u,0x25u,0x27u,0x30u,0x31u,0x32u,0x33u,0x34u,0x35u})regs.bpValid.set(reg);
        regs.bp[6]=512|(1u<<22);regs.bp[7]=0;regs.bp[8]=1u<<22;
        regs.bp[0x25]=0;regs.bp[0x27]=2|(1u<<3);
        for(unsigned reg=0x30;reg<=0x35;++reg)regs.bp[reg]=7;
        CommandDraw changed{draw.primitive,draw.vertexCount,draw.indexCount,draw.vertices,draw.indices,draw.plan,draw.arrays,regs};
        CommandDrawContext strict;assert(!drawGxCommand(changed,&strict)&&strict.rejected==1);
        CommandDrawContext allowed;allowed.allowVertexIndirect=true;expandedMode=true;
        assert(drawGxCommand(changed,&allowed)&&allowed.vertexIndirectDraws==1&&allowed.rejected==0);
        expandedMode=false;assert(expandedVertices.size()==4);
        for(const auto& v:expandedVertices){assert(std::abs(v.texcoord[0][0]-(uvShiftS+.5f-8))<1e-5f);
            assert(std::abs(v.texcoord[0][1]-(1-(uvShiftT+.6f)*.5f))<1e-5f);}
        const auto used=arena.used();regs.bp[0x10]|=1u<<13;allowed={};allowed.allowVertexIndirect=true;
        assert(!drawGxCommand(changed,&allowed)&&arena.used()==used&&std::strcmp(allowed.failure,"GX vertex indirect operation pending")==0);
        regs.bp[0x10]=0x270;regs.bpValid.reset(7);allowed={};allowed.allowVertexIndirect=true;
        assert(!drawGxCommand(changed,&allowed)&&arena.used()==used&&std::strcmp(allowed.failure,"GX vertex indirect matrix missing")==0);
        return true;
    });
    assert(processor.execute(list));
    // Clip-disable is exact only for a contained draw. Planning stays strict
    // by default; the real adapter proves containment before submitting.
    const u32 disabled=1;assert(stream.writeXf(0x1005,{&disabled,1}));
    assert(stream.finish());
    RasterPlan clipping;RasterError clippingError;
    assert(!planRaster(processor.registers(),clipping,&clippingError)&&clippingError==RasterError::clipMode);
    processor.setSink([](const CommandDraw& draw,void* data){
        assert(drawGxCommand(draw,data));
        auto registers=draw.registers;
        // Lateral clipping preserves framebuffer samples for positive-w,
        // depth-contained triangles. Depth crossings remain unsupported.
        registers.xf[0x1021]=std::bit_cast<u32>(4.f);
        CommandDraw outside{draw.primitive,draw.vertexCount,draw.indexCount,draw.vertices,draw.indices,draw.plan,draw.arrays,registers};
        CommandDrawContext lateral;assert(drawGxCommand(outside,&lateral)&&lateral.submitted==1);
        registers.xf[0x1021]=draw.registers.xf[0x1021];
        registers.xf[0x1025]=std::bit_cast<u32>(4.f);
        const auto before=arena.used();CommandDrawContext refused;
        assert(!drawGxCommand(outside,&refused)&&std::strcmp(refused.failure,"GX disabled clipping outside depth/camera volume")==0&&arena.used()==before);
        // The observed house camera turn produces an entirely offscreen
        // negative-w quad. Prove invisibility, release the reservation, and
        // still refuse a negative-w quad whose divided bounds touch the view.
        registers=draw.registers;registers.xf[0x1026]=0;
        const float projection[6]{0,100,0,100,0,-1};
        for(unsigned n=0;n<6;++n)registers.xf[0x1020+n]=std::bit_cast<u32>(projection[n]);
        registers.xf[11]=std::bit_cast<u32>(3.f);
        CommandDrawContext invisible;
        assert(drawGxCommand(outside,&invisible)&&invisible.culled==1&&invisible.submitted==0&&invisible.rejected==0&&arena.used()==before);
        // Same no-coverage geometry with a negative projective texture q must
        // be proven invisible before that unused divisor aborts the frame.
        registers.xf[0x1040]|=2;
        registers.matrixIndex[0]=(registers.matrixIndex[0]&~(63u<<6))|(30u<<6);
        const float textureProjection[12]{1,0,0,0,0,1,0,0,0,0,0,-1};
        for(unsigned n=0;n<12;++n){registers.xf[120+n]=std::bit_cast<u32>(textureProjection[n]);registers.xfValid.set(120+n);}
        invisible={};assert(drawGxCommand(outside,&invisible)&&invisible.culled==1&&invisible.submitted==0&&invisible.rejected==0&&arena.used()==before);
        registers.xf[0x1021]=registers.xf[0x1023]=0;refused={};
        assert(!drawGxCommand(outside,&refused)&&refused.rejected==1&&std::strcmp(refused.failure,"GX projected texture divisor invalid")==0&&arena.used()==before);
        CommandDrawContext compatibility;compatibility.allowBehindCameraCull=true;
        assert(drawGxCommand(outside,&compatibility)&&compatibility.cameraCulled==1&&compatibility.culled==1&&compatibility.submitted==0&&arena.used()==before);
        registers.xf[0x1005]=0;CommandDrawContext enabled;
        assert(drawGxCommand(outside,&enabled)&&enabled.culled==1&&enabled.cameraCulled==0&&arena.used()==before);

        // Kakariko blocker class: mixed camera-plane signs, but all triangles
        // outside a homogeneous lateral plane. Dividing endpoints or requiring
        // same-sign w misses this exact clipping-enabled no-coverage case.
        const float mixedProjection[6]{1,0,1,0,0,-1};
        for(unsigned n=0;n<6;++n)registers.xf[0x1020+n]=std::bit_cast<u32>(mixedProjection[n]);
        registers.xf[7]=std::bit_cast<u32>(1000.f);
        registers.xf[3]=std::bit_cast<u32>(-200.f);
        registers.xf[8]=std::bit_cast<u32>(.01f);registers.xf[11]=std::bit_cast<u32>(-2.f);
        const auto sent=submissions;
        auto exactCull=[&](const CommandDraw& candidate){
            CommandDrawContext c;c.allowRigidGpu=true;
            assert(drawGxCommand(candidate,&c)&&c.culled==1&&!c.submitted&&!c.rejected&&!c.skippedDraws&&!c.simplifiedDraws);
            assert(c.projectiveClipDraws==1&&c.projectiveClipTriangles==2);
            assert(arena.used()==before&&submissions==sent);
        };
        exactCull(outside);
        const u16 nativeIndices[]{0,1,2,2,3,0};auto indexed=outside;
        indexed.primitive=GX_TRIANGLES;indexed.indexCount=6;
        indexed.indices={reinterpret_cast<const u8*>(nativeIndices),sizeof(nativeIndices)};
        exactCull(indexed);
        for(auto primitive:{GX_TRIANGLESTRIP,GX_TRIANGLEFAN}){auto topology=outside;topology.primitive=primitive;exactCull(topology);}
        unsigned fatalCase=0;
        auto fatal=[&](const CommandDraw& candidate,DrawFailureKind kind=DrawFailureKind::protocol){
            CommandDrawContext c;c.allowDegradedDraws=true;c.allowBehindCameraCull=true;
            ++fatalCase;const bool result=drawGxCommand(candidate,&c);
            if(result||c.rejected!=1||c.submitted||c.skippedDraws||c.simplifiedDraws)
                std::fprintf(stderr,"projective fatal case %u returned %u: culled=%llu submitted=%llu reason=%s\n",fatalCase,unsigned(result),static_cast<unsigned long long>(c.culled),static_cast<unsigned long long>(c.submitted),c.failure);
            assert(!result&&c.rejected==1&&!c.submitted&&!c.skippedDraws&&!c.simplifiedDraws);
            assert(c.failureKind==kind&&c.rejectedByKind[unsigned(kind)]==1);
            assert(!c.projectiveClipDraws&&!c.projectiveClipTriangles);
            assert(arena.used()==before&&submissions==sent);
        };
        registers.xf[0x1005]=1;fatal(outside);registers.xf[0x1005]=0;
        // Geometry crossing the view cannot use this exception.
        registers.xf[7]=std::bit_cast<u32>(-120.f);fatal(outside);registers.xf[7]=std::bit_cast<u32>(1000.f);
        const auto q=registers.xf[131];registers.xf[131]=std::bit_cast<u32>(std::numeric_limits<float>::infinity());
        fatal(outside);registers.xf[131]=q;
        const auto textureToken=registers.bp[0x94];registers.bp[0x94]=0;fatal(outside,DrawFailureKind::resource);registers.bp[0x94]=textureToken;
        // A later consumed UV failure must not hide behind the first q<=0.
        const auto later=registers.xf[125];registers.xf[125]=std::bit_cast<u32>(std::numeric_limits<float>::quiet_NaN());
        fatal(outside);registers.xf[125]=later;
        reservationFailure=true;fatal(outside,DrawFailureKind::geometry);reservationFailure=false;
        cancellationFailure=true;fatal(outside,DrawFailureKind::geometry);cancellationFailure=false;
        const u16 badIndices[]{0,1,4};indexed.indexCount=3;
        indexed.indices={reinterpret_cast<const u8*>(badIndices),sizeof(badIndices)};fatal(indexed);
        // Visible orthographic UI is still strict, even when the caller has
        // enabled perspective visual cuts and behind-camera compatibility.
        auto uiRegisters=draw.registers;uiRegisters.xf[0x1005]=0;
        uiRegisters.xf[0x1040]|=2;
        uiRegisters.matrixIndex[0]=(uiRegisters.matrixIndex[0]&~(63u<<6))|(30u<<6);
        for(unsigned n=0;n<12;++n){uiRegisters.xf[120+n]=std::bit_cast<u32>(textureProjection[n]);uiRegisters.xfValid.set(120+n);}
        assert(uiRegisters.xf[0x1026]==1);
        CommandDraw uiDraw{draw.primitive,draw.vertexCount,draw.indexCount,draw.vertices,draw.indices,draw.plan,draw.arrays,uiRegisters};
        fatal(uiDraw);
        // A draw may also contain visible triangles with valid divisors. Keep
        // their exact data/order; only the invalid-q, clip-rejected primitive
        // becomes degenerate. No GPU call may consume the removed vertices.
        const auto stride=draw.plan.packedStride();assert(stride==40);
        std::vector<u8> mixedBytes(stride*6);
        auto put=[&](unsigned vertex,unsigned offset,float value){const auto bits=std::bit_cast<u32>(value);
            for(unsigned byte=0;byte<4;++byte)mixedBytes[vertex*stride+offset+byte]=u8(bits>>(24-byte*8));};
        for(unsigned i=0;i<6;++i){
            std::memcpy(mixedBytes.data()+i*stride,draw.vertices.data(),stride);
            put(i,0,i==1?390.f:i==4?20.f:10.f);put(i,4,i<3?float(i):-1000.f+float(i==5));
            put(i,16,i<3?-1.f:1.f);
        }
        registers.xf[0]=std::bit_cast<u32>(.001f);registers.xf[3]=0;registers.xf[128]=std::bit_cast<u32>(1.f);registers.xf[131]=0;
        auto partial=outside;partial.primitive=GX_TRIANGLES;partial.vertexCount=6;partial.vertices=mixedBytes;
        // A later visible triangle with zero/negative q must reject the whole
        // attempt before any primitive is replaced or submitted.
        for(float q:{-1.f,0.f}){put(4,16,q);fatal(partial);}put(4,16,1.f);
        safetyMode=true;gpuFailure=true;fatal(partial,DrawFailureKind::submission);gpuFailure=false;
        // A committed/partly emitted submission cannot be cancelled. Its
        // failure must stay fatal and must not record successful clipping.
        const auto savedArena=arena;gpuCommittedFailure=true;CommandDrawContext committed;
        assert(!drawGxCommand(partial,&committed));gpuCommittedFailure=false;safetyMode=false;
        assert(committed.rejected==1&&committed.failureKind==DrawFailureKind::submission);
        assert(!committed.submitted&&!committed.projectiveClipDraws&&!committed.simplifiedDraws&&!committed.skippedDraws);
        assert(arena.used()>before&&submissions==sent+1);arena=savedArena;submissions=sent;
        expandedMode=true;CommandDrawContext clipped;
        assert(drawGxCommand(partial,&clipped)&&clipped.submitted==1&&!clipped.culled&&!clipped.rejected&&!clipped.simplifiedDraws&&!clipped.skippedDraws);
        assert(clipped.projectiveClipDraws==1&&clipped.projectiveClipTriangles==1);
        expandedMode=false;
        assert((expandedIndices==std::vector<u16>{3,3,3,3,4,5}));
        for(unsigned i=0;i<3;++i)assert(!std::memcmp(&expandedVertices[i],&expandedVertices[3],sizeof(GpuVertex)));
        for(unsigned i=3;i<6;++i){const auto& vertex=expandedVertices[i];const float x=i==4?20.f:10.f;
            assert(std::abs(vertex.position[0]-float(i==5))<1e-5f&&std::abs(vertex.position[1]+x*.001f)<1e-5f);
            assert(std::abs(vertex.position[3]-(2-x*.01f))<1e-5f&&vertex.textureQ==1);
            assert(vertex.texcoord[0][0]==1&&vertex.color[0]==64.f/255&&vertex.color[3]==1);
        }
        // The divisor cutoff is exactly zero; keep small positive values in
        // surviving geometry without replacing them with a chosen epsilon.
        put(4,16,0x1p-30f);expandedMode=true;CommandDrawContext smallPositive;
        assert(drawGxCommand(partial,&smallPositive)&&smallPositive.submitted==1&&!smallPositive.rejected);
        assert(expandedVertices[4].textureQ==0x1p-30f);expandedMode=false;
        std::puts("PASS mixed-w projective clipping: quads/strips/fans/indexed triangles; clip-disabled, visible, nonfinite, stale resource, bounds, allocation and cancellation remain fatal");

        return true;
    },&context);
    list={u8(GX_QUADS),0,4};
    for(unsigned i=0;i<4;++i){scalar(i==1||i==2?390:10);scalar(i>=2?230:10);scalar(0);
        list.insert(list.end(),{64,128,192,255});for(unsigned t=0;t<3;++t){scalar(uvShiftS+.1f+.2f*t);scalar(uvShiftT+.2f+.2f*t);}}
    assert(processor.execute(list)&&context.submitted==4);
    std::array<GpuVertex,4> cameraTurn{};
    cameraTurn[0].position={-216.552505f,479.380035f,-1.000024f,-2.817139f};
    cameraTurn[1].position={-216.552505f,471.339691f,-1.000024f,-2.817139f};
    cameraTurn[2].position={-227.464432f,471.339691f,-1.000024f,-2.817139f};
    cameraTurn[3].position={-227.464432f,479.380035f,-1.000024f,-2.817139f};
    assert(proveNoScreenCoverage(cameraTurn));
    const auto distantTurn=cameraTurn;
    cameraTurn[0].position={84.862106f,516.300964f,-1.000978f,-155.507660f};
    cameraTurn[1].position={84.862106f,255.157730f,-1.000978f,-155.507660f};
    cameraTurn[2].position={-269.546570f,255.157730f,-1.000978f,-155.507660f};
    cameraTurn[3].position={-269.546570f,516.300964f,-1.000978f,-155.507660f};
    assert(proveNoScreenCoverage(cameraTurn));
    cameraTurn=distantTurn;
    for(unsigned direction=0;direction<4;++direction)for(bool negative:{false,true}){
        auto outside=cameraTurn;
        for(auto& v:outside){v.position={0,0,-.5f,negative?-1.f:1.f};
            v.position[direction/2]=(direction&1?-3.f:3.f)*v.position[3];}
        assert(proveNoScreenCoverage(outside));
        outside[0].position[direction/2]=1.0625f*outside[0].position[3];assert(!proveNoScreenCoverage(outside));
        outside[0].position[direction/2]=0;assert(!proveNoScreenCoverage(outside));
    }
    for(float badW:{0.f,2.817139f,std::numeric_limits<float>::infinity()}){
        auto ambiguous=cameraTurn;ambiguous[0].position[3]=badW;assert(!proveNoScreenCoverage(ambiguous));}
    auto ambiguous=cameraTurn;ambiguous[0].position[0]=std::numeric_limits<float>::quiet_NaN();assert(!proveNoScreenCoverage(ambiguous));
    assert(!proveNoScreenCoverage({}));
    assert(proveBehindCamera(cameraTurn)&&!proveBehindCamera({}));
    for(float w:{0.f,1.f,-1e-7f,std::numeric_limits<float>::quiet_NaN()}){
        auto crossing=cameraTurn;crossing[2].position[3]=w;assert(!proveBehindCamera(crossing));
    }
    auto nonfinite=cameraTurn;nonfinite[1].position[0]=std::numeric_limits<float>::infinity();
    assert(!proveBehindCamera(nonfinite));

    std::puts("PASS captured house camera-turn clip coordinates: conservative same-sign offscreen proof, reservation rollback, near/mixed-sign/nonfinite rejection");
    // Actual command adapter expands original GX line/point inputs. Check
    // rotated clip positions, preserved endpoint colors/depth, topology, and
    // per-coordinate texture-offset enables under nonuniform padded UV scale.
    assert(arena.end()&&arena.begin());expandedMode=true;uvShiftS=uvShiftT=0;
    processor.setSink([](const CommandDraw& draw,void* data){return drawGxCommand(draw,data);},&context);
    assert(stream.writeBp(0x22,120u|(60u<<8)|(5u<<16)|(5u<<19)));
    for(unsigned c=0;c<3;++c)assert(stream.writeBp(0x30+2*c,c==0?(3u<<18):0));
    auto makePrimitive=[&](GXPrimitive primitive,unsigned count) {
        list={u8(primitive),u8(count>>8),u8(count)};
        for(unsigned i=0;i<count;++i){scalar(100+80*i);scalar(80);scalar(0);
            list.insert(list.end(),{64,128,192,255});for(unsigned t=0;t<3;++t){scalar(.1f+.2f*t);scalar(.2f+.2f*t);}}
        assert(processor.execute(list));
    };
    auto close=[](float a,float b){assert(std::abs(a-b)<1e-5f);};
    makePrimitive(GX_LINES,2);
    assert(expandedVertices.size()==4&&expandedIndices==std::vector<u16>({0,1,2,1,3,2}));
    for(unsigned i=0;i<4;++i) {
        const auto& v=expandedVertices[i];
        close(v.position[0],1-2*(i&1?90.f:70.f)/240);
        close(v.position[1],1-2*(i>=2?180.f:100.f)/400);
        close(v.position[2],-.5);close(v.position[3],1);
        for(unsigned u=0;u<3;++u){const auto c=u==0?2:u==1?0:1;
            close(v.texcoord[u][0],.1f+.2f*c);
            close(v.texcoord[u][1],1-(.2f+.2f*c+(c==0&&(i&1)?1.f:0.f))*.5f);}
    }
    makePrimitive(GX_LINESTRIP,3);assert(expandedVertices.size()==8&&expandedIndices.size()==12);
    assert(stream.writeBp(0,processor.registers().bp[0]|(3u<<14)));
    makePrimitive(GX_POINTS,1);assert(expandedVertices.size()==4&&expandedIndices.size()==6);
    for(unsigned i=0;i<4;++i){const auto& v=expandedVertices[i];
        close(v.position[0],1-2*(i>=2?85.f:75.f)/240);
        close(v.position[1],1-2*(i&1?105.f:95.f)/400);
        close(v.texcoord[1][0],.1f+(i&1?1.f:0.f));
        close(v.texcoord[1][1],1-(.2f+(i>=2?1.f:0.f))*.5f);
    }
    assert(arena.end()&&arena.begin());fogMode=true;
    // A real typed/GD material with one consumed texture gains exactly one fog
    // ramp and RGB interpolation stage. Alpha is still the original material.
    assert(stream.writeBp(0,processor.registers().bp[0]&~((15u<<10)|(3u<<14))));
    for(unsigned reg:{0xe8u,0xefu,0xf0u})assert(stream.writeBp(reg,0));
    assert(stream.writeBp(0xee,std::bit_cast<u32>(1.f)>>12));
    assert(stream.writeBp(0xf1,(2u<<21)|(1u<<20)));
    assert(stream.writeBp(0xf2,0xabcdef));
    context.allowVertexFog=true;makePrimitive(GX_TRIANGLESTRIP,3);
    assert(context.vertexFogDraws==1&&expandedVertices.size()==3);
    for(const auto& v:expandedVertices){almost(v.texcoord[1][0],.5);almost(v.texcoord[1][1],.5);}
    // Removing the actual primary-alpha dependency frees that interpolation
    // component, preserving material output alpha without a utility texture.
    fogPrimary=true;assert(stream.writeBp(0xc1,0x08fff0));
    makePrimitive(GX_TRIANGLESTRIP,3);
    assert(context.vertexFogDraws==2);
    for(const auto& v:expandedVertices)almost(v.color[3],.5);
    assert(arena.end()&&arena.begin());rigidTestMode=true;
    // Execute identical original GX packets through CPU and rigid GPU packing.
    // The GPU substitute independently interprets the published uniform plan.
    // The single-texture shader is the fallback route in the game (multi-
    // texture is the default); these cases check its uniform plan.
    context.allowRigidGpu=false;context.allowVertexFog=true;context.allowSingleRigidShader=true;
    GXSetNumTexGens(1);GXSetTexCoordGen2(GX_TEXCOORD0,GX_TG_MTX2x4,GX_TG_TEX0,GX_IDENTITY,GX_FALSE,GX_PTIDENTITY);
    GXSetTevOrder(GX_TEVSTAGE0,GX_TEXCOORD0,GX_TEXMAP0,GX_COLOR0A0);GXSetTevOp(GX_TEVSTAGE0,GX_MODULATE);
    GXSetChanCtrl(GX_COLOR0,GX_TRUE,GX_SRC_REG,GX_SRC_VTX,GX_LIGHT0,GX_DF_CLAMP,GX_AF_SPOT);
    GXSetChanCtrl(GX_ALPHA0,GX_FALSE,GX_SRC_REG,GX_SRC_VTX,GX_LIGHT_NULL,GX_DF_NONE,GX_AF_NONE);
    GXSetChanAmbColor(GX_COLOR0A0,{30,60,90,128});
    GXLightObj light;GXInitLightPos(&light,0,0,100);GXInitLightDir(&light,0,0,1);
    GXInitLightColor(&light,{100,180,220,255});GXInitLightAttn(&light,1,.2f,.1f,1,.001f,.00001f);GXLoadLightObjImm(&light,GX_LIGHT0);
    // The preceding cases deliberately use a 2D orthographic UI. Select a
    // genuine, fully visible 3D draw before comparing the rigid GPU path.
    const float rigidProjection[16]{.5f,0,0,0,0,.7f,0,0,0,0,-1.01f,-10,0,0,-1,0};
    const float rigidPosition[12]{1,0,0,0,0,1,0,0,0,0,1,-100};
    GXSetProjection(rigidProjection,GX_PERSPECTIVE);
    GXLoadPosMtxImm(rigidPosition,GX_PNMTX0);GXLoadNrmMtxImm(identity,GX_PNMTX0);GXSetCurrentMtx(GX_PNMTX0);
    const u32 enabledClip=0;assert(stream.writeXf(0x1005,{&enabledClip,1}));
    GXClearVtxDesc();GXSetVtxDesc(GX_VA_POS,GX_DIRECT);GXSetVtxDesc(GX_VA_NRM,GX_DIRECT);GXSetVtxDesc(GX_VA_CLR0,GX_DIRECT);GXSetVtxDesc(GX_VA_TEX0,GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_NRM,GX_NRM_XYZ,GX_F32,0);
    list={u8(GX_TRIANGLES),0,3};
    for(unsigned i=0;i<3;++i){scalar(10.f+i*40);scalar(15.f+i*20);scalar(-float(i));scalar(.1f*i);scalar(.2f);scalar(1.f);
        list.insert(list.end(),{u8(64+i*12),128,192,255});scalar(.1f*i);scalar(.2f*i);}
    assert(stream.writeBp(0xf1,0)&&stream.finish());assert(processor.execute(list));
    context.allowRigidGpu=true;assert(processor.execute(list)&&rigidSubmissions==1);
    // Linear perspective range fog uses the same original register equations.
    assert(stream.writeBp(0xee,std::bit_cast<u32>(.2f)>>12));
    assert(stream.writeBp(0xef,0xffffff)&&stream.writeBp(0xf0,1));
    assert(stream.writeBp(0xf1,(2u<<21)|(std::bit_cast<u32>(.05f)>>12)));
    assert(stream.writeBp(0xe8,1024+542));for(unsigned i=0;i<5;++i)assert(stream.writeBp(0xe9+i,(64u+3*i)<<12|(67u+3*i)));
    for(unsigned primary=0;primary<2;++primary){
        if(primary)assert(stream.writeBp(0xc1,0x08fff0));
        context.allowRigidGpu=false;assert(processor.execute(list));context.allowRigidGpu=true;assert(processor.execute(list));}
    assert(rigidSubmissions==3);
    for(unsigned scenario=0;scenario<64;++scenario){
        assert(arena.end()&&arena.begin());
        const unsigned masks[]{1,64,124,255};const unsigned mask=masks[scenario%4];
        for(unsigned l=0;l<8;++l)if(mask&(1u<<l)){
            GXInitLightPos(&light,100+31*l,-30+float(l)*17,150+7*l);
            GXInitLightDir(&light,.03f*l,-.2f,1);
            GXInitLightColor(&light,{u8(10+l*12),u8(70+l*17),u8(170-l*13),255});
            GXInitLightAttn(&light,.9f,.2f,.1f,1,.001f,.00001f);GXLoadLightObjImm(&light,GXLightID(1u<<l));
        }
        const float f=.01f*scenario;
        const float position[12]{1+f,.2f,0,-20+f,0,1,.3f,7-f,.1f,0,1,-100-f};
        const float normals[12]{1,.1f,f,0,0,1,.2f,0,.1f,0,1,0};
        GXLoadPosMtxImm(position,GX_PNMTX0);GXLoadNrmMtxImm(normals,GX_PNMTX0);
        const float projection[16]{.5f,0,.05f,0,0,.7f,-.1f,0,0,0,-1.01f,-10,0,0,-1,0};GXSetProjection(projection,GX_PERSPECTIVE);
        GXSetChanCtrl(GX_COLOR0,GX_TRUE,scenario&32?GX_SRC_VTX:GX_SRC_REG,scenario&1?GX_SRC_VTX:GX_SRC_REG,GXLightID(mask),GX_DF_CLAMP,GX_AF_SPOT);
        GXSetChanMatColor(GX_COLOR0A0,{60,110,170,192});
        GXSetChanCtrl(GX_ALPHA0,GX_TRUE,GX_SRC_REG,scenario&2?GX_SRC_VTX:GX_SRC_REG,GX_LIGHT_NULL,GX_DF_NONE,GX_AF_NONE);
        GXSetTevOp(GX_TEVSTAGE0,GX_MODULATE);if(scenario&4)assert(stream.writeBp(0xc1,0x08fff0));
        assert(stream.writeBp(0xf1,scenario&8?0:(2u<<21)|(std::bit_cast<u32>(.05f)>>12)));
        assert(stream.writeBp(0xe8,scenario&16?1024+542:542));
        context.allowRigidGpu=false;assert(processor.execute(list));context.allowRigidGpu=true;assert(processor.execute(list));
    }
    assert(rigidSubmissions==67);
    // Original J3D PN matrix selectors, including an unused TEX1 selector.
    // Use 1..10 slots with 1/5 lights and 1..8 with 8 lights, reversed.
    const auto rigidList=list;
    for(unsigned scenario=0;scenario<28;++scenario){
        assert(arena.end()&&arena.begin());const unsigned matrices=1+(scenario<20?scenario%10:scenario-20);
        const unsigned mask=scenario<10?1:scenario<20?124:255;
        GXSetVtxDesc(GX_VA_PNMTXIDX,GX_DIRECT);GXSetVtxDesc(GX_VA_TEX1MTXIDX,GX_DIRECT);
        for(unsigned slot=0;slot<10;++slot){
            const float pos[12]{1+.03f*slot,.01f*slot,0,float(slot),0,1,.04f,2.f-slot,.02f,0,1,-100.f-slot};
            const float nrm[12]{1,.1f,.03f*slot,0,0,1,.2f,0,.1f,0,1,0};
            GXLoadPosMtxImm(pos,GX_PNMTX0+slot*3);GXLoadNrmMtxImm(nrm,GX_PNMTX0+slot*3);
        }
        list={u8(GX_TRIANGLES),0,30};
        const unsigned stride=(rigidList.size()-3)/3;
        for(unsigned i=0;i<30;++i){list.push_back(3*(9-i%matrices));list.push_back(3*(i%10));
            list.insert(list.end(),rigidList.begin()+3+(i%3)*stride,rigidList.begin()+3+(i%3+1)*stride);}
        GXSetChanCtrl(GX_COLOR0,GX_TRUE,GX_SRC_REG,scenario&1?GX_SRC_VTX:GX_SRC_REG,GXLightID(mask),GX_DF_CLAMP,GX_AF_SPOT);
        assert(stream.finish());
        context.allowRigidGpu=false;assert(processor.execute(list));context.allowRigidGpu=true;assert(processor.execute(list));
    }
    assert(rigidSubmissions==95);
    // With eight lights, a ninth live PN matrix must use the complete CPU
    // path instead of overflowing constants or submitting partial data.
    assert(arena.end()&&arena.begin());
    list={u8(GX_TRIANGLES),0,27};
    const unsigned paletteStride=(rigidList.size()-3)/3;
    for(unsigned i=0;i<27;++i){list.push_back(3*(9-i%9));list.push_back(0);
        list.insert(list.end(),rigidList.begin()+3+(i%3)*paletteStride,rigidList.begin()+3+(i%3+1)*paletteStride);}
    context.allowRigidGpu=false;assert(processor.execute(list));const auto expectedFallback=rigidExpected;
    context.allowRigidGpu=true;assert(processor.execute(list)&&rigidSubmissions==95);
    assert(expectedFallback.size()==rigidExpected.size());
    for(unsigned i=0;i<rigidExpected.size();++i){
        assert(expectedFallback[i].position==rigidExpected[i].position);
        assert(expectedFallback[i].color==rigidExpected[i].color);
        assert(expectedFallback[i].texcoord==rigidExpected[i].texcoord);
    }
    // Actual native arrays through the 28-byte PICA attribute layout, with native byte RGB/alpha.
    const std::array<float,9> compactPos{1,2,-3,4,5,-6,7,8,-9};
    const std::array<s16,9> compactShortPos{16,32,-48,64,80,-96,112,128,-144};
    const std::array<s16,9> compactNormals{100,200,16384,-100,100,16000,200,-300,16000};
    const std::array<s16,6> compactUv{128,64,256,-512,-256,1024};
    const std::array<u8,12> compactColors{1,67,234,98,249,124,4,222,99,42,184,17};
    std::array<MemoryHandle,5> compactHandles{};
    auto addArray=[&](const auto& values,unsigned slot){assert(memory.add({reinterpret_cast<const u8*>(values.data()),sizeof(values)},compactHandles[slot]));};
    addArray(compactPos,0);addArray(compactShortPos,1);addArray(compactNormals,2);addArray(compactUv,3);addArray(compactColors,4);
    for(unsigned colorSources=0;colorSources<4;++colorSources)for(unsigned scenario=0;scenario<6;++scenario){
        assert(arena.end()&&arena.begin());
        const unsigned masks[]{1,124,255},mask=masks[scenario%3],matrices=mask==255?8:10;
        GXSetChanCtrl(GX_COLOR0,GX_TRUE,scenario>=3?GX_SRC_VTX:GX_SRC_REG,(colorSources&1)?GX_SRC_VTX:GX_SRC_REG,GXLightID(mask),GX_DF_CLAMP,GX_AF_SPOT);
        GXSetChanCtrl(GX_ALPHA0,GX_TRUE,GX_SRC_REG,(colorSources&2)?GX_SRC_VTX:GX_SRC_REG,GX_LIGHT_NULL,GX_DF_NONE,GX_AF_NONE);
        GXSetVtxDesc(GX_VA_POS,GX_INDEX16);GXSetVtxDesc(GX_VA_NRM,GX_INDEX16);
        GXSetVtxDesc(GX_VA_CLR0,(colorSources||scenario>=3)?GX_INDEX16:GX_NONE);GXSetVtxDesc(GX_VA_TEX0,GX_INDEX16);
        GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_CLR0,GX_CLR_RGBA,GX_RGBA8,0);
        GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,scenario<3?GX_F32:GX_S16,4);
        GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_NRM,GX_NRM_XYZ,GX_S16,0);
        GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_TEX0,GX_TEX_ST,GX_S16,8);assert(stream.finish());
        if(scenario<3)assert(processor.bindArray(0,compactPos.data(),sizeof(compactPos),12,true));
        else assert(processor.bindArray(0,compactShortPos.data(),sizeof(compactShortPos),6,true));
        assert(processor.bindArray(1,compactNormals.data(),sizeof(compactNormals),6,true));
        assert(processor.bindArray(4,compactUv.data(),sizeof(compactUv),4,true));
        assert(processor.bindArray(2,compactColors.data(),sizeof(compactColors),4,true));
        list={u8(GX_TRIANGLES),0,30};
        for(unsigned i=0;i<30;++i){
            list.insert(list.end(),{u8(3*(i%matrices)),0,0,u8(i%3),0,u8(i%3)});
            if(colorSources||scenario>=3)list.insert(list.end(),{0,u8(i%3)});
            list.insert(list.end(),{0,u8(i%3)});
        }
        context.allowRigidGpu=false;assert(processor.execute(list));
        context.allowRigidGpu=true;assert(processor.execute(list));
    }
    assert(compactSubmissions==24&&rigidSubmissions==119);
    GXSetVtxDesc(GX_VA_PNMTXIDX,GX_NONE);GXSetVtxDesc(GX_VA_TEX1MTXIDX,GX_NONE);assert(stream.finish());
    // Eligibility cannot silently discard unsupported original state.
    auto regs=processor.registers();VertexTransformPlan rt;RasterColorPlan rc;PicaMaterialPlan rm;MaterialState rs;
    assert(decodeMaterial(regs,rs)&&planPicaMaterial(rs,rm)&&planRasterColor(regs,rm,rc)&&planVertexTransform(regs,1,rt));
    VertexFogPlan rf;assert(planVertexFog(regs,rf));DecodedVertex rv{};rv.present=(1u<<GX_VA_POS)|(1u<<GX_VA_NRM)|(1u<<GX_VA_CLR0)|(1u<<GX_VA_TEX0);rv.normalCount=1;
    RigidVertexPlan rp;assert(planRigidVertex(regs,rv,rt,rc,rf,false,608,rp));
    const auto initialPalette=rp;
    assert(!addRigidMatrix(regs,1,rp)&&!addRigidMatrix(regs,30,rp));
    assert(rp.matrices==initialPalette.matrices&&rp.matrixOffsets==initialPalette.matrixOffsets&&rp.uniforms==initialPalette.uniforms);
    auto missingPalette=regs;missingPalette.xfValid.reset(4*3);
    assert(!addRigidMatrix(missingPalette,3,rp));
    missingPalette=regs;missingPalette.xfValid.reset(0x400+3*3);
    assert(!addRigidMatrix(missingPalette,3,rp));
    missingPalette=regs;missingPalette.xf[4*3]=std::bit_cast<u32>(std::numeric_limits<float>::infinity());
    assert(!addRigidMatrix(missingPalette,3,rp));
    assert(rp.matrices==initialPalette.matrices&&rp.matrixOffsets==initialPalette.matrixOffsets&&rp.uniforms==initialPalette.uniforms);
    for(unsigned slot=1;slot<8;++slot)assert(addRigidMatrix(regs,3*slot,rp));
    const auto fullPalette=rp;assert(addRigidMatrix(regs,0,rp)&&!addRigidMatrix(regs,24,rp));
    assert(rp.matrices==fullPalette.matrices&&rp.matrixOffsets==fullPalette.matrixOffsets&&rp.uniforms==fullPalette.uniforms);
    auto badVertex=rv;badVertex.present|=2;assert(!planRigidVertex(regs,badVertex,rt,rc,rf,false,608,rp));
    auto badColor=rc;badColor.ambientVertex=2;assert(!planRigidVertex(regs,rv,rt,badColor,rf,false,608,rp));
    badColor=rc;badColor.channels[1].mask=1;assert(!planRigidVertex(regs,rv,rt,badColor,rf,false,608,rp));
    badColor=rc;badColor.lights[0].distance[0]=0;assert(!planRigidVertex(regs,rv,rt,badColor,rf,false,608,rp));
    auto badRegs=regs;badRegs.xfValid.reset(0);assert(!planRigidVertex(badRegs,rv,rt,rc,rf,false,608,rp));
    badRegs=regs;badRegs.xf[0x1050]|=256;assert(!planRigidVertex(badRegs,rv,rt,rc,rf,false,608,rp));
    auto badFog=rf;badFog.enabled=true;badFog.function=4;assert(!planRigidVertex(regs,rv,rt,rc,badFog,false,608,rp));
    assert(planRigidVertex(regs,rv,rt,rc,rf,false,608,rp));GpuVertex packed;
    rv.position[0]=std::numeric_limits<float>::infinity();assert(!packRigidVertex(rp,rv,{1,1},packed));
    // Original background TEX1 projective position matrix + identity TEX0.
    // Compare complete texture q and all three outputs, including fog routing.
    for(unsigned scenario=0;scenario<64;++scenario){
        assert(arena.end()&&arena.begin());
        GDInitGDLObj(&gd,commands,sizeof(commands));GDSetCurrent(&gd);
        GDSetGenMode2(2,1,2,0,GX_CULL_NONE);
        GDSetTevOrder(GX_TEVSTAGE0,GX_TEXCOORD0,GX_TEXMAP0,GX_COLOR0A0,GX_TEXCOORD1,GX_TEXMAP1,GX_COLOR0A0);
        for(unsigned i=0;i<2;++i){const auto stage=GXTevStageID(i);
            GDSetTevColorCalc(stage,GX_CC_ZERO,GX_CC_TEXC,i?GX_CC_CPREV:GX_CC_RASC,GX_CC_ZERO,GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV);
            GDSetTevAlphaCalcAndSwap(stage,GX_CA_ZERO,GX_CA_TEXA,i?GX_CA_APREV:(scenario&32?GX_CA_ZERO:GX_CA_RASA),GX_CA_ZERO,GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV,GX_TEV_SWAP0,GX_TEV_SWAP0);
            GDSetTevDirect(stage);
        }
        const auto size=GDGetCurrOffset();GDSetCurrent(nullptr);assert(processor.execute({commands,size}));
        GXSetTexCoordGen2(GX_TEXCOORD0,GX_TG_MTX2x4,GX_TG_TEX0,GX_IDENTITY,GX_FALSE,GX_PTIDENTITY);
        GXSetTexCoordGen2(GX_TEXCOORD1,GX_TG_MTX3x4,GX_TG_POS,GX_TEXMTX0,GX_FALSE,GX_PTIDENTITY);
        const float matrix[12]{.1f,.2f,.03f,.4f,.2f,.3f,.01f,.5f,.001f,.0005f,.0001f,1.25f};
        GXLoadTexMtxImm(matrix,GX_TEXMTX0,GX_MTX3x4);
        GXSetChanCtrl(GX_COLOR0,GX_TRUE,scenario&2?GX_SRC_VTX:GX_SRC_REG,scenario&4?GX_SRC_VTX:GX_SRC_REG,GX_LIGHT0,GX_DF_CLAMP,GX_AF_SPOT);
        GXSetChanCtrl(GX_ALPHA0,GX_TRUE,GX_SRC_REG,scenario&8?GX_SRC_VTX:GX_SRC_REG,GX_LIGHT_NULL,GX_DF_NONE,GX_AF_NONE);
        assert(stream.writeBp(0xf1,scenario&16?(2u<<21)|(std::bit_cast<u32>(.05f)>>12):0));
        GXClearVtxDesc();
        for(auto attr:{GX_VA_POS,GX_VA_NRM,GX_VA_CLR0,GX_VA_TEX0})GXSetVtxDesc(attr,scenario&1?GX_INDEX16:GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,GX_F32,0);
        GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_NRM,GX_NRM_XYZ,scenario&1?GX_S16:GX_F32,0);
        GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_CLR0,GX_CLR_RGBA,GX_RGBA8,0);
        GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_TEX0,GX_TEX_ST,scenario&1?GX_S16:GX_F32,8);
        assert(stream.finish());
        if(scenario&1){
            assert(processor.bindArray(0,compactPos.data(),sizeof(compactPos),12,true));
            list={u8(GX_TRIANGLES),0,3};
            for(unsigned i=0;i<3;++i)for(unsigned a=0;a<4;++a)list.insert(list.end(),{0,u8(i)});
        }else list=rigidList;
        context.allowRigidGpu=false;assert(processor.execute(list));
        const auto before=rigidSubmissions;
        context.allowRigidGpu=true;assert(processor.execute(list)&&rigidSubmissions==before+1);
        auto pr=processor.registers();VertexTransformPlan tp;RasterColorPlan cp;MaterialState ms;PicaMaterialPlan mp;
        assert(decodeMaterial(pr,ms)&&planPicaMaterial(ms,mp)&&planRasterColor(pr,mp,cp)&&planVertexTransform(pr,3,tp));
        VertexFogPlan fp;assert(planVertexFog(pr,fp));DecodedVertex dv{};dv.present=rv.present;dv.normalCount=1;
        RigidVertexPlan pp;assert(planRigidVertex(pr,dv,tp,cp,fp,!(cp.required&2),608,pp,true));
        std::array<GpuVertex,1> probe{};assert(validateRigidProjection(pp,probe));
        for(float q:{0.f,-1.f,1e-6f,std::numeric_limits<float>::infinity()}){
            auto invalid=pp;invalid.uniforms[pp.projectionBase()+2]={0,0,0,q};assert(!validateRigidProjection(invalid,probe));
        }
        auto cancellation=pp;cancellation.uniforms[pp.projectionBase()+2]={1e6f,0,0,-1e6f+1};
        probe[0].position[0]=1;assert(!validateRigidProjection(cancellation,probe));
        dv.present|=1u<<GX_VA_TEX1MTXIDX;assert(!planRigidVertex(pr,dv,tp,cp,fp,false,608,pp,true));
    }
    assert(rigidSubmissions==183&&compactSubmissions==56);
    std::puts("PASS 64 projected background adapter draws: raw/compact positions, independent material/ambient/alpha, homogeneous q, all UV/fog outputs; invalid divisor and per-vertex texture matrix fallback");
    std::puts("PASS 119 rigid/palette adapter/reference draws including 24 compact S16 stream draws: 1/5/8 compacted lights, affine position/normal/projection, material/vertex RGB and alpha, vertex ambient RGB, range/plain/no fog; ninth matrix/eight lights CPU fallback and missing/invalid/nonfinite palette rejection without mutation");
    // Immediate draw records (HUD panes): every replay, including the in-place
    // finish of the record's uniform block, the unchanged-draw repeats and
    // the lookup key memo, submits exactly what the complete path submits.
    {
        assert(arena.end()&&arena.begin());
        GXSetProjection(projection,GX_ORTHOGRAPHIC);GXSetViewport(0,0,400,240,0,1);GXSetScissor(0,0,400,240);
        GXSetCullMode(GX_CULL_NONE);GXSetBlendMode(GX_BM_BLEND,GX_BL_SRCALPHA,GX_BL_INVSRCALPHA,GX_LO_COPY);
        GXSetZMode(GX_FALSE,GX_ALWAYS,GX_FALSE);GXSetAlphaCompare(GX_ALWAYS,0,GX_AOP_AND,GX_ALWAYS,0);
        GXSetNumChans(1);GXSetChanCtrl(GX_COLOR0A0,GX_FALSE,GX_SRC_REG,GX_SRC_VTX,0,GX_DF_NONE,GX_AF_NONE);
        GXLoadTexObj(&objects[0],GX_TEXMAP0);GXLoadTexObj(&objects[1],GX_TEXMAP1);
        const float texMatrix[8]{.5f,0,0,.25f,0,2,0,-.5f};GXLoadTexMtxImm(texMatrix,GX_TEXMTX0,GX_MTX2x4);
        assert(stream.writeBp(0xf1,0));assert(stream.finish());
        // Two pane states: one texture (identity texgen), two textures (the
        // second through a texture matrix, S16 coordinates with 8 fraction bits).
        auto state=[&](bool second,u8 materialAlpha){
            GXSetNumTexGens(second?2:1);GXSetNumTevStages(second?2:1);
            GXSetTexCoordGen2(GX_TEXCOORD0,GX_TG_MTX2x4,GX_TG_TEX0,GX_IDENTITY,GX_FALSE,GX_PTIDENTITY);
            GXSetTexCoordGen2(GX_TEXCOORD1,GX_TG_MTX2x4,GX_TG_TEX1,GX_TEXMTX0,GX_FALSE,GX_PTIDENTITY);
            GXSetTevOrder(GX_TEVSTAGE0,GX_TEXCOORD0,GX_TEXMAP0,GX_COLOR0A0);GXSetTevOp(GX_TEVSTAGE0,GX_MODULATE);
            GXSetTevOrder(GX_TEVSTAGE1,GX_TEXCOORD1,GX_TEXMAP1,GX_COLOR0A0);GXSetTevOp(GX_TEVSTAGE1,GX_MODULATE);
            GXSetChanMatColor(GX_COLOR0A0,{200,180,160,materialAlpha});
            GXClearVtxDesc();GXSetVtxDesc(GX_VA_POS,GX_DIRECT);GXSetVtxDesc(GX_VA_CLR0,GX_DIRECT);GXSetVtxDesc(GX_VA_TEX0,GX_DIRECT);
            if(second)GXSetVtxDesc(GX_VA_TEX1,GX_DIRECT);
            GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,GX_F32,0);GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_CLR0,GX_CLR_RGBA,GX_RGBA8,0);
            GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_TEX0,GX_TEX_ST,GX_F32,0);GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_TEX1,GX_TEX_ST,GX_S16,8);
            assert(stream.finish());
        };
        // State changes only between steps of different states, so the
        // epoch stands across the others (key memo) and repeats can occur.
        struct Step {bool second;u8 materialAlpha;float tx,ty,scale,x,y,uv;u8 alpha;};
        const Step steps[]{
            {false,255,0,0,1,10,10,0,255},{false,255,0,0,1,10,10,0,255},{false,255,0,0,1,10,10,0,255},
            {false,255,30,5,1,10,10,0,255},{false,255,30,5,1,10,10,0,255},
            {false,255,-300,20,1,520,600,.25f,0},{false,255,-300,20,1,520,600,.25f,0}, // compact origin 512, hidden pane
            {true,255,5,5,2,20,30,.5f,200},{true,255,5,5,2,20,30,.5f,200},{true,255,5,5,2,20,30,.5f,200},
            {false,255,0,0,1,10,10,0,255},
            {false,128,0,0,1,10,10,0,255},{false,128,0,0,1,10,10,0,255}, // material colour only
            {false,255,0,0,1,10,10,0,255},
            {true,255,7,5,2,20,30,.5f,200},{true,255,7,5,2,21,30,.5f,200}};
        auto run=[&]{
            std::vector<RigidCapture> captures;rigidCaptures=&captures;
            CommandDrawContext immediate;immediate.allowRigidGpu=true;
            processor.setSink([](const CommandDraw& draw,void* data){return drawGxCommand(draw,data);},&immediate);
            const Step* previous=nullptr;
            for(const auto& step:steps){
                if(!previous||previous->second!=step.second||previous->materialAlpha!=step.materialAlpha)state(step.second,step.materialAlpha);
                previous=&step;
                const float position[12]{step.scale,0,0,step.tx,0,step.scale,0,step.ty,0,0,1,0};
                GXLoadPosMtxImm(position,GX_PNMTX0);GXSetCurrentMtx(GX_PNMTX0);
                list={u8(GX_QUADS),0,4};
                for(unsigned i=0;i<4;++i){
                    scalar(step.x+(i==1||i==2?40:0));scalar(step.y+(i>=2?20:0));scalar(0);
                    list.insert(list.end(),{64,128,192,step.alpha});
                    scalar(step.uv+(i==1||i==2?.5f:0));scalar(step.uv+(i>=2?.5f:0));
                    if(step.second){const u16 s=u16(32*i),t=u16(-16*int(i));list.insert(list.end(),{u8(s>>8),u8(s),u8(t>>8),u8(t)});}
                }
                assert(arena.end()&&arena.begin());assert(processor.execute(list));
            }
            assert(immediate.submitted==std::size(steps)&&!immediate.rejected&&captures.size()==std::size(steps));
            rigidCaptures=nullptr;return captures;
        };
        auto same=[](const std::vector<RigidCapture>& a,const std::vector<RigidCapture>& b){
            assert(a.size()==b.size());
            for(unsigned n=0;n<a.size();++n){
                const auto& x=a[n];const auto& y=b[n];
                if(x.rows.size()!=y.rows.size()||std::memcmp(x.rows.data(),y.rows.data(),x.rows.size()*sizeof(x.rows[0]))){
                    for(unsigned k=0;k<std::min(x.rows.size(),y.rows.size());++k)for(unsigned c=0;c<4;++c)
                        if(std::bit_cast<u32>(x.rows[k][c])!=std::bit_cast<u32>(y.rows[k][c]))
                            std::fprintf(stderr,"immediate parity step=%u row=%u.%u complete=%a replay=%a\n",n,k,c,double(x.rows[k][c]),double(y.rows[k][c]));
                    assert(false);
                }
                assert(x.lights==y.lights&&x.matrices==y.matrices&&x.records==y.records&&x.indices==y.indices);
                // Plans compare as bytes up to their last member: the host's
                // 64-bit pointers leave tail padding the ARM layout has not.
                constexpr auto rasterBytes=offsetof(RasterPlan,reserved)+sizeof(RasterPlan::reserved);
                assert(!std::memcmp(&x.raster,&y.raster,rasterBytes)&&sameMaterialPlan(x.material,y.material));
            }
        };
        setImmediateDrawRecords(false);
        const auto complete=run();
        std::uint64_t hits0,misses0,stores0,fallbacks0;immediateDrawStatistics(hits0,misses0,stores0,fallbacks0);
        ImmediateDrawDetail detail0;immediateDrawDetail(detail0);
        setImmediateDrawRecords(true);
        const auto replayed=run();
        std::uint64_t hits1,misses1,stores1,fallbacks1;immediateDrawStatistics(hits1,misses1,stores1,fallbacks1);
        ImmediateDrawDetail detail1;immediateDrawDetail(detail1);
        same(complete,replayed);
        // Misses: A, B, and A with another material colour (colour only).
        assert(hits1-hits0==13&&misses1-misses0==3&&stores1-stores0==3&&fallbacks1==fallbacks0);
        assert(detail1.repeats-detail0.repeats==5&&detail1.missColor-detail0.missColor==1&&detail1.missState-detail0.missState==2);
        assert(detail1.zeroVertexAlpha-detail0.zeroVertexAlpha==2&&detail1.keyMemoHits>detail0.keyMemoHits);
        // Shortcuts off (the A/B switch): the same submissions, no repeats.
        setImmediateDrawShortcuts(false);setImmediateDrawRecords(true);
        const auto plain=run();
        ImmediateDrawDetail detail2;immediateDrawDetail(detail2);
        same(complete,plain);
        assert(detail2.repeats==detail1.repeats&&detail2.keyMemoHits==detail1.keyMemoHits);
        setImmediateDrawShortcuts(true);
        std::printf("PASS immediate records: %zu pane draws, replays equal the complete path (uniform rows, vertex records, plans); "
                    "%llu repeats, %llu key memo hits, colour-only miss classified, shortcuts off equal too\n",std::size(steps),
                    static_cast<unsigned long long>(detail1.repeats-detail0.repeats),static_cast<unsigned long long>(detail1.keyMemoHits-detail0.keyMemoHits));
    }
    // Native shape replay (duskNativeDrawShape, the J3D static shapes):
    // two static lists of one lit, fogged two-texture material (TEX0, and an
    // environment map from the normal through a texture and a post matrix)
    // are recorded by the complete path, then replayed over frames that move
    // the lights, the texture matrix, the fog, the projection and the light
    // count, the last one under the narrow palette. The lean replay submits
    // exactly what the complete replay (switch off) submits, continuations
    // included, and its first replay equals the recorded draw.
    {
        assert(arena.end()&&arena.begin());
        static std::array<std::uint8_t,65536> poolBytes;static StaticCompactCache pool;
        assert(pool.attach(poolBytes,true,[](void*,std::size_t){}));setStaticCompactCache(&pool);nativeReplayFixture=true;
        GXSetProjection(projection,GX_ORTHOGRAPHIC);GXSetViewport(0,0,400,240,0,1);GXSetScissor(0,0,400,240);
        GXSetCullMode(GX_CULL_NONE);GXSetBlendMode(GX_BM_NONE,GX_BL_ONE,GX_BL_ZERO,GX_LO_COPY);
        GXSetZMode(GX_TRUE,GX_LEQUAL,GX_TRUE);GXSetAlphaCompare(GX_ALWAYS,0,GX_AOP_AND,GX_ALWAYS,0);
        GXSetNumChans(1);GXSetChanCtrl(GX_COLOR0,GX_TRUE,GX_SRC_REG,GX_SRC_REG,GX_LIGHT0|GX_LIGHT1,GX_DF_CLAMP,GX_AF_SPOT);
        GXSetChanCtrl(GX_ALPHA0,GX_FALSE,GX_SRC_REG,GX_SRC_REG,GX_LIGHT_NULL,GX_DF_NONE,GX_AF_NONE);
        GXSetChanMatColor(GX_COLOR0A0,{200,180,160,255});GXSetChanAmbColor(GX_COLOR0A0,{40,50,60,255});
        GXLoadTexObj(&objects[0],GX_TEXMAP0);GXLoadTexObj(&objects[1],GX_TEXMAP1);
        GXSetNumTexGens(2);
        GXSetTexCoordGen2(GX_TEXCOORD0,GX_TG_MTX2x4,GX_TG_TEX0,GX_IDENTITY,GX_FALSE,GX_PTIDENTITY);
        GXSetTexCoordGen2(GX_TEXCOORD1,GX_TG_MTX2x4,GX_TG_NRM,GX_TEXMTX0,GX_FALSE,GX_PTTEXMTX0);
        const float post[12]{1,.1f,0,.05f,0,1,.1f,-.05f,0,0,1,0};GXLoadTexMtxImm(post,GX_PTTEXMTX0,GX_MTX3x4);
        GXSetNumTevStages(2);
        GXSetTevOrder(GX_TEVSTAGE0,GX_TEXCOORD0,GX_TEXMAP0,GX_COLOR0A0);GXSetTevOp(GX_TEVSTAGE0,GX_MODULATE);
        GXSetTevOrder(GX_TEVSTAGE1,GX_TEXCOORD1,GX_TEXMAP1,GX_COLOR0A0);GXSetTevOp(GX_TEVSTAGE1,GX_MODULATE);
        GXClearVtxDesc();GXSetVtxDesc(GX_VA_POS,GX_INDEX16);GXSetVtxDesc(GX_VA_NRM,GX_INDEX16);GXSetVtxDesc(GX_VA_TEX0,GX_INDEX16);
        GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,GX_F32,0);GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_NRM,GX_NRM_XYZ,GX_S16,0);
        GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_TEX0,GX_TEX_ST,GX_S16,8);
        assert(stream.writeBp(0xee,std::bit_cast<u32>(.5f)>>12)&&stream.writeBp(0xef,0x600000)&&stream.writeBp(0xf0,2));
        for(unsigned reg=0xe9;reg<=0xed;++reg)assert(stream.writeBp(reg,(40u+reg)|((70u+reg)<<12)));
        GXLightObj lamp;
        auto frameState=[&](float lightX,u8 second,float env,u32 fogColor,u32 center,float near){
            GXInitLightPos(&lamp,lightX,40,-60);GXInitLightDir(&lamp,0,-.3f,1);GXInitLightColor(&lamp,{200,190,180,255});
            GXInitLightAttn(&lamp,1,0,0,1,.01f,0);GXLoadLightObjImm(&lamp,GX_LIGHT0);
            GXInitLightPos(&lamp,-50,lightX,-80);GXInitLightDir(&lamp,.2f,0,1);GXInitLightColor(&lamp,{second,second,second,255});
            GXInitLightAttn(&lamp,1,0,0,1,.02f,.001f);GXLoadLightObjImm(&lamp,GX_LIGHT1);
            const float m[8]{.5f*env,.1f,0,.5f,0,.5f,.2f*env,.5f};GXLoadTexMtxImm(m,GX_TEXMTX0,GX_MTX2x4);
            assert(stream.writeBp(0xf1,(2u<<21)|(std::bit_cast<u32>(.05f)>>12))&&stream.writeBp(0xf2,fogColor)&&stream.writeBp(0xe8,1024|center));
            const float p[16]{.5f,0,.05f,0,0,.7f,-.1f,0,0,0,-1.01f,-near,0,0,-1,0};GXSetProjection(p,GX_PERSPECTIVE);
        };
        auto shapeMatrix=[&](float x){
            const float m[12]{1,.2f,0,x,0,1,.3f,7,.1f,0,1,-100},n[12]{1,.1f,0,0,0,1,.2f,0,.1f,0,1,0};
            GXLoadPosMtxImm(m,GX_PNMTX0);GXLoadNrmMtxImm(n,GX_PNMTX0);GXSetCurrentMtx(GX_PNMTX0);assert(stream.finish());
        };
        frameState(100,170,1,0x406080,542,10);assert(stream.finish());
        assert(processor.bindArray(0,compactPos.data(),sizeof(compactPos),12,true));
        assert(processor.bindArray(1,compactNormals.data(),sizeof(compactNormals),6,true));
        assert(processor.bindArray(4,compactUv.data(),sizeof(compactUv),4,true));
        std::vector<u8> listA{u8(GX_TRIANGLES),0,6},listB{u8(GX_TRIANGLES),0,3};
        for(unsigned i=0;i<6;++i)listA.insert(listA.end(),{0,u8(i%3),0,u8((i+1)%3),0,u8((i+2)%3)});
        for(unsigned i=0;i<3;++i)listB.insert(listB.end(),{0,u8(2-i),0,u8(i),0,u8(i)});
        constexpr std::uint32_t generation=77;
        // Recording: a list's first draw stores its static pool entry, the
        // second draws from the pool and becomes the record.
        CommandDrawContext record;record.allowRigidGpu=true;record.allowVertexFog=true;
        processor.setSink([](const CommandDraw& draw,void* data){return drawGxCommand(draw,data);},&record);
        std::array<std::uint64_t,13> outcomesBefore,outcomesAfter;nativeShapeRecordOutcomes(outcomesBefore);
        std::vector<RigidCapture> recorded;rigidCaptures=&recorded;
        for(const auto* list:{&listA,&listB})for(unsigned pass=0;pass<2;++pass){
            shapeMatrix(list==&listA?-20.f:-14.f);assert(arena.end()&&arena.begin());
            duskNativeStaticGeometry(generation,list->data());assert(processor.execute(*list));duskNativeStaticGeometry(0,nullptr);
        }
        rigidCaptures=nullptr;nativeShapeRecordOutcomes(outcomesAfter);
        assert(recorded.size()==4&&record.submitted==4&&!record.rejected);
        assert(outcomesAfter[0]-outcomesBefore[0]==2&&outcomesAfter[3]-outcomesBefore[3]==2); // recorded, then not resident on first sight
        struct Frame {float lightX;u8 second;float env;u32 fogColor,center;float near;bool narrow;};
        const Frame frames[]{
            {100,170,1,0x406080,542,10,false},  // as recorded
            {100,170,1,0x406080,542,10,false},  // unchanged
            {130,170,1,0x406080,542,10,false},  // lights moved
            {130,170,1.5f,0x406080,542,10,false}, // environment matrix
            {130,170,1.5f,0x608040,600,10,false}, // fog colour and range centre
            {130,170,1.5f,0x608040,600,12,false}, // projection
            {130,0,1.5f,0x608040,600,12,false},   // second light black: one light record
            {130,170,1.5f,0x608040,600,12,true}}; // narrow palette (unit rows of absent units uploaded)
        auto run=[&](bool lean){
            setNativeLeanReplay(lean);
            std::vector<NativeCapture> captures;nativeCaptures=&captures;
            for(const auto& f:frames){
                multiTextureWidePalette=!f.narrow;
                frameState(f.lightX,f.second,f.env,f.fogColor,f.center,f.near);
                for(const auto* list:{&listA,&listB}){shapeMatrix(list==&listA?-20.f:-14.f);assert(duskNativeDrawShape(generation,list->data(),std::uint32_t(list->size())));}
            }
            multiTextureWidePalette=true;nativeCaptures=nullptr;
            assert(captures.size()==2*std::size(frames));
            return captures;
        };
        // A first pass leaves the records as the measured passes find them
        // (constants refreshed for the last frame's colours), so both measured
        // passes continue the same shapes.
        // worldMips: record replays run with no static geometry hint, as on
        // the worker; unit 0 (TEX0 through a 2x4 identity texgen, perspective)
        // is still a world use, unit 1 (from the normal) never is.
        std::vector<std::pair<unsigned,bool>> uses;worldUses=&uses;
        run(false);
        worldUses=nullptr;
        assert(!uses.empty());
        for(const auto& [unit,world]:uses)assert(world==(unit==0));
        NativeLeanReplayStatistics s0,s1,s2;nativeLeanReplayStatistics(s0);
        const auto complete=run(false);nativeLeanReplayStatistics(s1);
        const auto lean=run(true);nativeLeanReplayStatistics(s2);
        constexpr auto rasterBytes=offsetof(RasterPlan,reserved)+sizeof(RasterPlan::reserved);
        auto sameRows=[](const RigidCapture& x,const RigidCapture& y,const char* what,unsigned n){
            if(x.rows.size()==y.rows.size()&&!std::memcmp(x.rows.data(),y.rows.data(),x.rows.size()*sizeof(x.rows[0])))return true;
            std::fprintf(stderr,"native replay parity (%s) draw=%u rows %zu/%zu\n",what,n,x.rows.size(),y.rows.size());
            for(unsigned k=0;k<std::min(x.rows.size(),y.rows.size());++k)for(unsigned c=0;c<4;++c)
                if(std::bit_cast<u32>(x.rows[k][c])!=std::bit_cast<u32>(y.rows[k][c]))
                    std::fprintf(stderr,"  row=%u.%u %a %a\n",k,c,double(x.rows[k][c]),double(y.rows[k][c]));
            return false;
        };
        unsigned chained=0;
        for(unsigned n=0;n<complete.size();++n){
            const auto& x=complete[n];const auto& y=lean[n];
            assert(sameRows(x.rows,y.rows,"lean against complete replay",n));
            assert(x.rows.lights==y.rows.lights&&x.rows.matrices==y.rows.matrices&&x.vertices==y.vertices&&x.indices==y.indices&&
                   x.indexCount==y.indexCount&&x.chained==y.chained);
            assert(!std::memcmp(&x.rows.raster,&y.rows.raster,rasterBytes)&&sameMaterialPlan(x.rows.material,y.rows.material));
            chained+=x.chained;
        }
        // The first frame replays the recorded state: the recorded draws' rows.
        assert(sameRows(recorded[1],lean[0].rows,"replay against recording",0)&&sameRows(recorded[3],lean[1].rows,"replay against recording",1));
        // Light records: two, then one with the second light black.
        assert(lean[0].rows.lights==2&&lean[12].rows.lights==1&&lean[14].rows.lights==2);
        assert(s1.fullPlans-s0.fullPlans+chained==complete.size()&&s1.leanPlans==s0.leanPlans);
        assert(s2.leanPlans-s1.leanPlans+(s2.rowContinuations-s1.rowContinuations)==lean.size()&&s2.fullPlans==s1.fullPlans);
        // List B continues list A, except where the fog colour moved: its
        // constants are refreshed first (frames 1 and 5). Two continuations
        // cross frames whose state did not change (frame 2).
        assert(s2.rowContinuations-s1.rowContinuations==chained&&chained==7&&s2.leanRefusals==s1.leanRefusals);
        // Projection planned again when its words moved (and after the switch);
        // fog when its words moved; every other lean plan from the caches.
        assert(s2.projectionPlanned-s1.projectionPlanned==2&&s2.fogPlanned-s1.fogPlanned==2);
        assert(s2.projectionEpochHits+s2.projectionEqualHits>s1.projectionEpochHits+s1.projectionEqualHits);
        processor.setSink(nullptr);setStaticCompactCache(nullptr);nativeReplayFixture=false;
        std::printf("PASS native shape replay: %zu replays over %zu frames (%u continuations), lean equal to the complete replay "
                    "(uniform rows, plans, vertices, indices) and to the recorded draws\n",lean.size(),std::size(frames),chained);
    }
    // Fallback records (2026-11-16): a static list whose material takes the
    // TEV stand-in (an EQ compare, refused; under a multiply blend its floor
    // constant is TEV register C0) and the relaxed dual alpha test. With the
    // switch off every frame takes the complete path (record refusal 5); on,
    // the list is recorded and its replays, also after C0 moves (the stand-in
    // decision repeated), submit what the complete path submits.
    {
        assert(arena.end()&&arena.begin());
        static std::array<std::uint8_t,65536> fallbackPoolBytes;static StaticCompactCache fallbackPool;
        assert(fallbackPool.attach(fallbackPoolBytes,true,[](void*,std::size_t){}));setStaticCompactCache(&fallbackPool);nativeReplayFixture=true;
        const float perspective[16]{.5f,0,.05f,0,0,.7f,-.1f,0,0,0,-1.01f,-10,0,0,-1,0};GXSetProjection(perspective,GX_PERSPECTIVE);
        GXSetViewport(0,0,400,240,0,1);GXSetScissor(0,0,400,240);GXSetCullMode(GX_CULL_NONE);
        GXSetBlendMode(GX_BM_BLEND,GX_BL_DSTCLR,GX_BL_ZERO,GX_LO_COPY);GXSetZMode(GX_TRUE,GX_LEQUAL,GX_TRUE);
        GXSetAlphaCompare(GX_GREATER,75,GX_AOP_AND,GX_LESS,255); // relaxed: alpha > 75
        GXSetNumChans(1);GXSetChanCtrl(GX_COLOR0A0,GX_FALSE,GX_SRC_REG,GX_SRC_REG,GX_LIGHT_NULL,GX_DF_NONE,GX_AF_NONE);
        GXSetChanMatColor(GX_COLOR0A0,{200,180,160,255});
        GXLoadTexObj(&objects[0],GX_TEXMAP0);GXLoadTexObj(&objects[1],GX_TEXMAP1);
        GXSetNumTexGens(1);GXSetTexCoordGen2(GX_TEXCOORD0,GX_TG_MTX2x4,GX_TG_TEX0,GX_IDENTITY,GX_FALSE,GX_PTIDENTITY);
        GXSetNumTevStages(2);
        GXSetTevOrder(GX_TEVSTAGE0,GX_TEXCOORD0,GX_TEXMAP0,GX_COLOR0A0);GXSetTevOp(GX_TEVSTAGE0,GX_MODULATE);
        GXSetTevOrder(GX_TEVSTAGE1,GX_TEXCOORD0,GX_TEXMAP1,GX_COLOR0A0);
        GXSetTevColorIn(GX_TEVSTAGE1,GX_CC_TEXC,GX_CC_CPREV,GX_CC_RASC,GX_CC_C0);
        GXSetTevColorOp(GX_TEVSTAGE1,GX_TEV_COMP_RGB8_EQ,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV);
        GXSetTevAlphaIn(GX_TEVSTAGE1,GX_CA_ZERO,GX_CA_ZERO,GX_CA_ZERO,GX_CA_APREV);
        GXSetTevAlphaOp(GX_TEVSTAGE1,GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV);
        GXSetTevColor(GX_TEVREG0,{10,20,30,255});
        GXClearVtxDesc();GXSetVtxDesc(GX_VA_POS,GX_INDEX16);GXSetVtxDesc(GX_VA_NRM,GX_INDEX16);GXSetVtxDesc(GX_VA_TEX0,GX_INDEX16);
        GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,GX_F32,0);GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_NRM,GX_NRM_XYZ,GX_S16,0);
        GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_TEX0,GX_TEX_ST,GX_S16,8);
        const float shape[12]{1,.2f,0,-20,0,1,.3f,7,.1f,0,1,-100};
        GXLoadPosMtxImm(shape,GX_PNMTX0);GXLoadNrmMtxImm(identity,GX_PNMTX0);GXSetCurrentMtx(GX_PNMTX0);
        assert(stream.writeBp(0xf1,0)&&stream.finish());
        {MaterialState ms;PicaMaterialPlan mp;MaterialPlanError error;
         assert(decodeMaterial(processor.registers(),ms)&&!planPicaMaterial(ms,mp,&error)&&error==MaterialPlanError::operation);}
        assert(processor.bindArray(0,compactPos.data(),sizeof(compactPos),12,true));
        assert(processor.bindArray(1,compactNormals.data(),sizeof(compactNormals),6,true));
        assert(processor.bindArray(4,compactUv.data(),sizeof(compactUv),4,true));
        std::vector<u8> fallbackList{u8(GX_TRIANGLES),0,6};
        for(unsigned i=0;i<6;++i)fallbackList.insert(fallbackList.end(),{0,u8(i%3),0,u8((i+1)%3),0,u8((i+2)%3)});
        constexpr std::uint32_t generation=78;
        CommandDrawContext record;record.allowRigidGpu=true;record.allowVertexFog=true;record.allowDegradedDraws=true;record.allowAlphaCompareFallback=true;
        processor.setSink([](const CommandDraw& draw,void* data){return drawGxCommand(draw,data);},&record);
        std::vector<RigidCapture> unused;
        auto completeDraw=[&]{
            assert(arena.end()&&arena.begin());
            const bool keep=rigidCaptures;if(!keep)rigidCaptures=&unused;
            duskNativeStaticGeometry(generation,fallbackList.data());assert(processor.execute(fallbackList));duskNativeStaticGeometry(0,nullptr);
            if(!keep)rigidCaptures=nullptr;
        };
        const std::array<GXColor,5> c0{{{10,20,30,255},{10,20,30,255},{90,60,30,255},{90,60,30,255},{200,100,50,255}}};
        // Switch off: the complete path every frame (after a first draw that
        // stores the static pool entry), never recorded.
        setNativeFallbackRecords(false);completeDraw();
        std::array<std::uint64_t,13> outcomes0,outcomes1,outcomes2;nativeShapeRecordOutcomes(outcomes0);
        std::vector<RigidCapture> complete;rigidCaptures=&complete;
        for(const auto& color:c0){GXSetTevColor(GX_TEVREG0,color);assert(stream.finish());completeDraw();}
        rigidCaptures=nullptr;nativeShapeRecordOutcomes(outcomes1);
        assert(complete.size()==c0.size()&&outcomes1[5]-outcomes0[5]==c0.size()&&outcomes1[0]==outcomes0[0]);
        assert(!duskNativeDrawShape(generation,fallbackList.data(),std::uint32_t(fallbackList.size())));
        for(const auto& c:complete)assert(c.raster.alphaRelaxed&&c.material.stageCount==1&&c.material.textureCount==1);
        // The multiply stand-in's constant is C0 (its floor): it follows C0.
        assert((complete[0].material.stages[0].constant==std::array<u8,4>{10,20,30,255}));
        assert((complete[4].material.stages[0].constant==std::array<u8,4>{200,100,50,255}));
        // Switch on: recorded by the complete path, then replayed.
        DrawGapStatistics gaps0,gaps1;drawGapStatistics(gaps0);
        setNativeFallbackRecords(true);GXSetTevColor(GX_TEVREG0,c0[0]);assert(stream.finish());completeDraw();
        nativeShapeRecordOutcomes(outcomes2);assert(outcomes2[0]-outcomes1[0]==1);
        const auto simplifiedBefore=record.simplifiedByReason;
        std::vector<NativeCapture> replays;nativeCaptures=&replays;
        for(const auto& color:c0){GXSetTevColor(GX_TEVREG0,color);assert(stream.finish());assert(duskNativeDrawShape(generation,fallbackList.data(),std::uint32_t(fallbackList.size())));}
        nativeCaptures=nullptr;drawGapStatistics(gaps1);
        assert(replays.size()==c0.size());
        constexpr auto rasterBytes=offsetof(RasterPlan,reserved)+sizeof(RasterPlan::reserved);
        for(unsigned n=0;n<c0.size();++n){
            const auto& x=complete[n];const auto& y=replays[n].rows;
            assert(x.rows.size()==y.rows.size()&&!std::memcmp(x.rows.data(),y.rows.data(),x.rows.size()*sizeof(x.rows[0])));
            assert(x.lights==y.lights&&x.matrices==y.matrices&&!std::memcmp(&x.raster,&y.raster,rasterBytes)&&sameMaterialPlan(x.material,y.material));
        }
        // C0 moved twice: the stand-in decision repeated twice. Every replay
        // counts as the complete path counted the draw.
        assert(gaps1.fallbackStored-gaps0.fallbackStored==1&&gaps1.standInReplays-gaps0.standInReplays==c0.size());
        assert(gaps1.relaxedAlphaReplays-gaps0.relaxedAlphaReplays==c0.size()&&gaps1.standInRefreshes-gaps0.standInRefreshes==2);
        assert(gaps1.standInRefusals==gaps0.standInRefusals);
        assert(record.simplifiedByReason[unsigned(DrawVisualFallback::tevMaterial)]-simplifiedBefore[unsigned(DrawVisualFallback::tevMaterial)]==c0.size());
        assert(record.simplifiedByReason[unsigned(DrawVisualFallback::alphaCompare)]-simplifiedBefore[unsigned(DrawVisualFallback::alphaCompare)]==c0.size());
        // Switch off again: the records holding a fallback are dropped.
        setNativeFallbackRecords(false);assert(!duskNativeDrawShape(generation,fallbackList.data(),std::uint32_t(fallbackList.size())));setNativeFallbackRecords(true);
        processor.setSink(nullptr);setStaticCompactCache(nullptr);nativeReplayFixture=false;
        std::printf("PASS fallback records: TEV stand-in (multiply floor from C0) and relaxed alpha test recorded; %zu replays equal the complete path "
                    "(rows, raster, material), C0 moves repeat the stand-in decision; switch off refuses them (refusal 5)\n",replays.size());
    }
    // Missing vertex colour (2026-11-16): a channel reading the vertex colour
    // of a format without CLR0 reads Dolphin's missing colour, opaque white.
    // The GPU draw is the draw of the same vertices with a white CLR0 (same
    // rows, same records); the CPU route gives the same vertices too.
    {
        setImmediateDrawRecords(false);
        const float perspective[16]{.5f,0,.05f,0,0,.7f,-.1f,0,0,0,-1.01f,-10,0,0,-1,0};GXSetProjection(perspective,GX_PERSPECTIVE);
        GXSetBlendMode(GX_BM_NONE,GX_BL_ONE,GX_BL_ZERO,GX_LO_COPY);GXSetAlphaCompare(GX_ALWAYS,0,GX_AOP_AND,GX_ALWAYS,0);
        GXSetChanCtrl(GX_COLOR0A0,GX_FALSE,GX_SRC_REG,GX_SRC_VTX,GX_LIGHT_NULL,GX_DF_NONE,GX_AF_NONE);
        GXSetNumTevStages(1);GXSetTevOrder(GX_TEVSTAGE0,GX_TEXCOORD0,GX_TEXMAP0,GX_COLOR0A0);GXSetTevOp(GX_TEVSTAGE0,GX_MODULATE);
        const float shape[12]{1,.2f,0,-20,0,1,.3f,7,.1f,0,1,-100};GXLoadPosMtxImm(shape,GX_PNMTX0);GXSetCurrentMtx(GX_PNMTX0);
        auto format=[&](bool color){
            GXClearVtxDesc();GXSetVtxDesc(GX_VA_POS,GX_DIRECT);if(color)GXSetVtxDesc(GX_VA_CLR0,GX_DIRECT);GXSetVtxDesc(GX_VA_TEX0,GX_DIRECT);
            GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,GX_F32,0);GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_CLR0,GX_CLR_RGBA,GX_RGBA8,0);
            GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_TEX0,GX_TEX_ST,GX_F32,0);assert(stream.writeBp(0xf1,0)&&stream.finish());
            list={u8(GX_TRIANGLES),0,3};
            for(unsigned i=0;i<3;++i){scalar(10.f+i*40);scalar(15.f+i*20);scalar(-float(i));if(color)list.insert(list.end(),{255,255,255,255});scalar(.1f*i);scalar(.2f*i);}
        };
        CommandDrawContext missing;missing.allowRigidGpu=true;
        processor.setSink([](const CommandDraw& draw,void* data){return drawGxCommand(draw,data);},&missing);
        std::vector<RigidCapture> gpu;rigidCaptures=&gpu;
        const auto decodesBefore=gxMissingColorDecodes;
        for(bool color:{false,true}){format(color);assert(arena.end()&&arena.begin());assert(processor.execute(list));}
        rigidCaptures=nullptr;
        assert(gpu.size()==2&&!missing.rejected&&gxMissingColorDecodes==decodesBefore+1);
        assert(gpu[0].rows.size()==gpu[1].rows.size()&&!std::memcmp(gpu[0].rows.data(),gpu[1].rows.data(),gpu[0].rows.size()*sizeof(gpu[0].rows[0])));
        assert(gpu[0].records==gpu[1].records&&gpu[0].indices==gpu[1].indices&&gpu[0].rows[14][3]==1);
        // CPU route (the GPU route off): white too.
        rigidTestMode=true;missing.allowRigidGpu=false;std::array<std::vector<GpuVertex>,2> cpu;
        for(bool color:{false,true}){format(color);assert(arena.end()&&arena.begin());assert(processor.execute(list));cpu[color]=rigidExpected;}
        rigidTestMode=false;
        assert(cpu[0].size()==3&&!missing.rejected);
        for(unsigned i=0;i<3;++i)assert(cpu[0][i].position==cpu[1][i].position&&cpu[0][i].color==cpu[1][i].color&&cpu[0][i].texcoord==cpu[1][i].texcoord);
        processor.setSink(nullptr);setImmediateDrawRecords(true);
        std::puts("PASS missing vertex colour: no CLR0 reads opaque white; GPU rows and records equal a white CLR0 draw, CPU route equal too");
    }
    // One-attribute formats (2026-11-16, compact reject 58 before): a
    // position-only unlit draw takes the multi-texture GPU route. Its records
    // and rows place every vertex where the CPU route places it.
    {
        setImmediateDrawRecords(false);
        GXSetChanCtrl(GX_COLOR0A0,GX_FALSE,GX_SRC_REG,GX_SRC_REG,GX_LIGHT_NULL,GX_DF_NONE,GX_AF_NONE);
        GXSetChanMatColor(GX_COLOR0A0,{200,180,160,255});
        GXSetNumTexGens(0);GXSetNumTevStages(1);GXSetTevOrder(GX_TEVSTAGE0,GX_TEXCOORD_NULL,GX_TEXMAP_NULL,GX_COLOR0A0);GXSetTevOp(GX_TEVSTAGE0,GX_PASSCLR);
        GXClearVtxDesc();GXSetVtxDesc(GX_VA_POS,GX_DIRECT);GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,GX_F32,0);
        assert(stream.writeBp(0xf1,0)&&stream.finish());
        list={u8(GX_TRIANGLES),0,3};
        for(unsigned i=0;i<3;++i){scalar(10.f+i*40);scalar(15.f+i*20);scalar(-float(i));}
        CommandDrawContext single;single.allowRigidGpu=true;
        processor.setSink([](const CommandDraw& draw,void* data){return drawGxCommand(draw,data);},&single);
        gxCompactSingleAttribute=false;rigidTestMode=true;assert(arena.end()&&arena.begin());assert(processor.execute(list));
        const auto cpu=rigidExpected;rigidTestMode=false;gxCompactSingleAttribute=true;
        std::vector<RigidCapture> gpu;rigidCaptures=&gpu;const auto decodesBefore=gxCompactSingleAttributeDecodes;
        assert(arena.end()&&arena.begin());assert(processor.execute(list));rigidCaptures=nullptr;
        assert(gpu.size()==1&&cpu.size()==3&&!single.rejected&&gxCompactSingleAttributeDecodes==decodesBefore+1);
        const auto& c=gpu[0];
        for(unsigned i=0;i<3;++i){
            CompactRigidVertexMT v;std::memcpy(&v,c.records.data()+i*sizeof(v),sizeof(v));
            const unsigned m=unsigned(v.matrix[0]);std::array<float,4> view{0,0,0,1},clip{};
            for(unsigned r=0;r<3;++r)view[r]=c.rows[m+r][0]*v.position[0]+c.rows[m+r][1]*v.position[1]+c.rows[m+r][2]*v.position[2]+c.rows[m+r][3];
            for(unsigned r=0;r<4;++r)clip[r]=c.rows[6+r][0]*view[0]+c.rows[6+r][1]*view[1]+c.rows[6+r][2]*view[2]+c.rows[6+r][3];
            for(unsigned k=0;k<4;++k)assert(std::abs(clip[k]-cpu[i].position[k])<=1e-5f*(1+std::abs(cpu[i].position[k])));
            // Unlit: ambient one, the dummy light black, material colour.
            for(unsigned k=0;k<4;++k)assert(c.rows.back()[k]==cpu[i].color[k]);
        }
        processor.setSink(nullptr);setImmediateDrawRecords(true);
        std::puts("PASS one-attribute format: position-only unlit draw on the multi-texture GPU route, positions and colour as the CPU route");
    }
    // Palette split (2026-11-16, rigid plan rejects 45/46/59/60 before): a
    // lit multi-texture draw with six lights and ten distinct matrices whose
    // normal rows are not proportional does not fit one palette. It is drawn
    // as consecutive runs; every referenced vertex, through its run's rows,
    // gets the CPU route's position and colour, the runs' triangles are the
    // draw's in order, and every row outside the palette is the same in all runs.
    {
        setImmediateDrawRecords(false);
        GXSetNumTexGens(1);GXSetTexCoordGen2(GX_TEXCOORD0,GX_TG_MTX2x4,GX_TG_TEX0,GX_IDENTITY,GX_FALSE,GX_PTIDENTITY);
        GXLoadTexObj(&objects[0],GX_TEXMAP0);
        GXSetNumTevStages(1);GXSetTevOrder(GX_TEVSTAGE0,GX_TEXCOORD0,GX_TEXMAP0,GX_COLOR0A0);GXSetTevOp(GX_TEVSTAGE0,GX_MODULATE);
        GXSetChanCtrl(GX_COLOR0,GX_TRUE,GX_SRC_REG,GX_SRC_REG,GXLightID(63),GX_DF_CLAMP,GX_AF_SPOT);
        GXSetChanCtrl(GX_ALPHA0,GX_FALSE,GX_SRC_REG,GX_SRC_REG,GX_LIGHT_NULL,GX_DF_NONE,GX_AF_NONE);
        GXSetChanMatColor(GX_COLOR0A0,{180,160,140,255});GXSetChanAmbColor(GX_COLOR0A0,{30,40,50,255});
        GXLightObj lamp;
        for(unsigned l=0;l<6;++l){
            GXInitLightPos(&lamp,100+31*l,-30+float(l)*17,150+7*l);GXInitLightDir(&lamp,.03f*l,-.2f,1);
            GXInitLightColor(&lamp,{u8(10+l*12),u8(70+l*17),u8(170-l*13),255});
            GXInitLightAttn(&lamp,.9f,.2f,.1f,1,.001f,.00001f);GXLoadLightObjImm(&lamp,GXLightID(1u<<l));
        }
        for(unsigned slot=0;slot<10;++slot){
            const float pos[12]{1+.03f*slot,.01f*slot,0,float(slot),0,1,.04f,2.f-slot,.02f,0,1,-100.f-slot};
            const float nrm[12]{1,.1f,.03f*slot,0,0,1,.2f,0,.1f,0,1,0};
            GXLoadPosMtxImm(pos,GX_PNMTX0+slot*3);GXLoadNrmMtxImm(nrm,GX_PNMTX0+slot*3);
        }
        GXClearVtxDesc();GXSetVtxDesc(GX_VA_PNMTXIDX,GX_DIRECT);GXSetVtxDesc(GX_VA_POS,GX_DIRECT);GXSetVtxDesc(GX_VA_NRM,GX_DIRECT);GXSetVtxDesc(GX_VA_TEX0,GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,GX_F32,0);GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_NRM,GX_NRM_XYZ,GX_F32,0);
        GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_TEX0,GX_TEX_ST,GX_F32,0);
        assert(stream.writeBp(0xf1,0)&&stream.finish());
        list={u8(GX_TRIANGLES),0,30};
        for(unsigned i=0;i<30;++i){
            list.push_back(u8(3*(i/3)));scalar(10.f+(i%3)*40);scalar(15.f+(i%3)*20+i);scalar(-float(i%3));
            scalar(.1f*(i%3));scalar(.2f);scalar(1.f);scalar(.1f*(i%3));scalar(.2f*(i%3));
        }
        CommandDrawContext split;split.allowRigidGpu=true;
        processor.setSink([](const CommandDraw& draw,void* data){return drawGxCommand(draw,data);},&split);
        // CPU route: the split switched off.
        setPaletteSplit(false);rigidTestMode=true;assert(arena.end()&&arena.begin());assert(processor.execute(list));
        const auto cpu=rigidExpected;const auto cpuIndices=expandedIndices;rigidTestMode=false;setPaletteSplit(true);
        DrawGapStatistics gaps0,gaps1;drawGapStatistics(gaps0);
        std::vector<RigidCapture> runs;rigidCaptures=&runs;
        assert(arena.end()&&arena.begin());assert(processor.execute(list));rigidCaptures=nullptr;drawGapStatistics(gaps1);
        assert(!split.rejected&&cpu.size()==30&&runs.size()==2&&gaps1.paletteSplitDraws-gaps0.paletteSplitDraws==1&&gaps1.paletteSplitRuns-gaps0.paletteSplitRuns==2);
        std::vector<u16> joined;
        auto close=[](float a,float expected){assert(std::abs(a-expected)<2e-4f*(1+std::abs(expected)));};
        for(const auto& run:runs){
            joined.insert(joined.end(),run.indices.begin(),run.indices.end());
            // The shader's attribute and uniform contract, as rigidReference
            // models it (the light records and rows 0-18 share its layout).
            RigidVertexPlan plan;plan.lights=run.lights;plan.multiTexture=true;
            const unsigned count=unsigned(run.rows.size())-(94-89);
            for(unsigned k=0;k<count;++k)plan.uniforms[k]=run.rows[k];
            for(unsigned k=89;k<94;++k)plan.uniforms[k]=run.rows[count+k-89];
            for(auto index:run.indices){
                CompactRigidVertexMT v;std::memcpy(&v,run.records.data()+index*sizeof(v),sizeof(v));
                GpuVertex g{};g.position={v.position[0],v.position[1],v.position[2],float(v.normal[2])};
                g.texcoord[1]={float(v.normal[0]),float(v.normal[1])};g.texcoord[2]={float(v.matrix[0]),float(v.matrix[1])};
                g.color=plan.uniforms[93];
                const auto out=rigidReference(plan,g);
                for(unsigned k=0;k<4;++k)close(out.position[k],cpu[index].position[k]);
                for(unsigned k=0;k<3;++k)close(out.color[k],cpu[index].color[k]);
            }
        }
        assert(joined==cpuIndices);
        for(unsigned k=6;k<43;++k)assert(runs[0].rows[k]==runs[1].rows[k]);
        for(unsigned k=0;k<5;++k)assert(runs[0].rows[runs[0].rows.size()-5+k]==runs[1].rows[runs[1].rows.size()-5+k]);
        processor.setSink(nullptr);setImmediateDrawRecords(true);
        std::printf("PASS palette split: six lights and ten matrices drawn as %zu runs; every vertex as the CPU route (position, lit colour), "
                    "triangle order kept, rows outside the palette equal\n",runs.size());
    }
    assert(arena.end());
    Texture before,after;assert(textureBinding(0,before));
    const u8 incomplete[]{0x61,0x41};assert(stream.write(incomplete));
    bool refused=false;
    try{GXLoadTexObj(&objects[1],GX_TEXMAP0);}catch(const std::runtime_error&){refused=true;}
    assert(refused&&textureBinding(0,after)&&before.handle==after.handle);
    assert(stream.reset());
    for(unsigned i=0;i<3;++i){GXDestroyTexObj(&objects[i]);assert(unregisterTexture(handles[i]));}
    assert(setCommandStream(nullptr));
    std::puts("PASS original GD + typed GX -> command draw adapter -> indexed arena: mapped textures, projected binding reroute, original 3x4 matrix/variable homogeneous q, color/clip/index data and bounded rejection; GPU calls substituted on host");
}
