#include "gx_pica.hpp"
#include "runtime/gx_geometry_pack.hpp"
#include "runtime/gx_profile.hpp"
#include "runtime/gx_rigid_vertex.hpp"
#include "runtime/gx_pica_math.hpp"
#include "runtime/gx_pica_tev.hpp"
#include "gx_texture_pica.hpp"
#include "gx_material_pica.hpp"
#include "gx_blend_pica.hpp"
#include "gx_geometry_pica.hpp"
#include "runtime/gx_raster_plan.hpp"
#include "runtime/gpu_sync.hpp"
#include "runtime/gx_framebuffer.hpp"
#include "runtime/gx_copy_pixels.hpp"
#include "runtime/gx_state_serial.hpp"
#include "runtime/shadow_casters.hpp"
#include "runtime/screen_effects.hpp"
#include "runtime/gpu_canary.hpp"
#include "runtime/ctru_abi.hpp"
#include <3ds.h>
#include <citro3d.h>
#include <cstring>
#include <algorithm>
#include <cstdio>

extern const unsigned char gx_color_shader[];
extern const unsigned int gx_color_shader_size;
extern const unsigned char gx_rigid_shader[];
extern const unsigned int gx_rigid_shader_size;
extern const unsigned char gx_rigid_mt_shader[];
extern const unsigned int gx_rigid_mt_shader_size;
namespace dusk::native {
namespace {
DVLB_s* shader;
DVLB_s* rigidShader;
shaderProgram_s rigidProgram{};
bool rigidProgramCreated;
// Native multi-texture rigid program (gx_rigid_mt.v.pica).
DVLB_s* rigidMtShader{};
shaderProgram_s rigidMtProgram{};
bool rigidMtProgramCreated;
int rigidMtUniforms=-1,rigidMtLoop=-1;
int rigidUniforms,rigidLoop;
shaderProgram_s program{};
bool programCreated, ready;
int transformLocation;
const char* failure = "none";
std::uint64_t drawCount;
gx::AlphaCompare previousAlpha{};
gx::AlphaPlan alphaPlan{};
bool alphaCached;
bool intervalPoisoned{},intervalActive{};
void invalidateAppliedState();
void disableStencil(){
    C3D_StencilTest(false,GPU_ALWAYS,0,0xff,0);
    C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_KEEP);
}
bool reject(const char* reason) { failure = reason; return false; }
// Triangle fog (a fog overlay pass for materials with no TEV room for vertex
// fog) as two whole-mesh passes; false restores one pair of draws per
// triangle. The stencil bit is distinct from the alpha interval's.
bool triangleFogMeshPass=true;
constexpr unsigned triangleFogStencilBit=2;
static_assert(triangleFogStencilBit!=gx::alphaIntervalStencilBit);
// Citro3D reloads the shader's constant uniforms (and marks them for upload)
// on every C3D_BindProgram, also for the program already bound. Consecutive
// draws of one program bind once; frame begin and every state invalidation
// forget the record.
const shaderProgram_s* boundProgram;
inline void bindProgram(shaderProgram_s* next);
}
// The console GPU guard records which of these programs a hung draw used.
int gxBoundProgramKind();
namespace {
inline void bindProgram(shaderProgram_s* next){
    if(boundProgram==next)return;
    C3D_BindProgram(next);boundProgram=next;
}
constexpr GPU_TESTFUNC comparisons[8] = {
    GPU_NEVER, GPU_LESS, GPU_EQUAL, GPU_LEQUAL, GPU_GREATER, GPU_NOTEQUAL, GPU_GEQUAL, GPU_ALWAYS};
constexpr GPU_TESTFUNC reversedComparisons[8] = {
    GPU_NEVER, GPU_GREATER, GPU_EQUAL, GPU_GEQUAL, GPU_LESS, GPU_NOTEQUAL, GPU_LEQUAL, GPU_ALWAYS};
GPU_BLENDFACTOR factor(unsigned raw, bool destination) {
    if (raw == 2) return destination ? GPU_SRC_COLOR : GPU_DST_COLOR;
    if (raw == 3) return destination ? GPU_ONE_MINUS_SRC_COLOR : GPU_ONE_MINUS_DST_COLOR;
    constexpr GPU_BLENDFACTOR other[] = {GPU_ZERO, GPU_ONE, GPU_ZERO, GPU_ZERO,
        GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_DST_ALPHA, GPU_ONE_MINUS_DST_ALPHA};
    return other[raw];
}
bool draw(const gx::DrawPacket& p, void*) {
    ++gxPicaStateSerial;
    if(!alphaIntervalCopySafe())return reject("PICA alpha interval scratch unsafe");
    if(!platformPrepareGpuDraw())return reject("PICA command block completion failed");
    if (!ready) return reject("PICA not initialized");
    disableStencil();
    if (p.primitive == gx::Primitive::lines || p.primitive == gx::Primitive::lineStrip)
        return reject("GX lines pending tessellation");
    if(p.vertices.size()<3||
       (p.primitive==gx::Primitive::quads&&p.vertices.size()%4)||
       (p.primitive==gx::Primitive::triangles&&p.vertices.size()%3))
        return reject("GX primitive vertex count invalid");
    if (p.blend.mode > 1 || p.blend.src > 7 || p.blend.dst > 7)
        return reject("GX blend/logic mode unsupported");
    if (p.depth.compare > 7 || p.cull > 3) return reject("GX depth/cull invalid");
    if (!alphaCached || std::memcmp(&p.alpha, &previousAlpha, sizeof(previousAlpha))) {
        if (!gx::reduceAlpha(p.alpha, alphaPlan)) return reject("GX dual alpha test unsupported");
        previousAlpha = p.alpha; alphaCached = true;
    }
    if (p.zCompareBeforeTexture && p.depth.enabled && p.depth.write && alphaPlan.comparison != 7)
        return reject("GX early depth plus alpha unsupported");
    std::array<float, 16> matrix;
    if (!gx::picaTransform(p, matrix)) return reject("GX transform invalid");
    gx::PicaTevPlan tevPlan;
    gx::PicaTevError tevError;
    if (!gx::planPicaTev(p, tevPlan, &tevError)) return reject(gx::picaTevErrorText(tevError));
    float scaleU = 1, scaleV = 1;
    GxTextureDrawScope textureScope(p.textured?std::span<const gx::Texture>(&p.texture,1):std::span<const gx::Texture>{});
    if(!textureScope)return reject(gxTextureFailure());
    // Pinned Citro3D dereferences tex for units > 0 before recording it.
    // Only unit 0 accepts nullptr. Extra-unit cache images remain alive and
    // unused by this legacy material; never attempt null unbinding there.
    if(!p.textured){C3D_TexBind(0,nullptr);invalidateGxTextureBindings();}
    if (p.textured) {
        // Padded allocations do not reproduce repeated coordinates. Admit the
        // original bounded J2D rectangle until a fragment-level wrap path exists.
        for (const auto& v : p.vertices)
            if (v.u < 0 || v.u > 1 || v.v < 0 || v.v > 1)
                return reject("GX padded texture UV outside unit rectangle");
        if (!bindGxTexture(p.texture, scaleU, scaleV)) return reject(gxTextureFailure());
    }
    auto scissor = gx::picaScissor(p.scissor);
    if (scissor.empty || p.cull == 3) return true;
    C3D_Mtx transform;
    for (unsigned row = 0; row < 4; ++row)
        transform.r[row] = FVec4_New(matrix[row*4], matrix[row*4+1], matrix[row*4+2], matrix[row*4+3]);
    bindProgram(&program);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, transformLocation, &transform);
    invalidateAppliedState();
    C3D_SetViewport(0, 0, gx::currentTargetExtent.pixelHeight, gx::currentTargetExtent.pixelWidth);
    C3D_SetScissor(GPU_SCISSOR_NORMAL, scissor.left, scissor.top, scissor.right, scissor.bottom);
    C3D_CullFace(static_cast<GPU_CULLMODE>(gx::picaCull(p.cull)));
    C3D_DepthMap(true, -1, 0);
    C3D_EarlyDepthTest(false, GPU_EARLYDEPTH_GREATER, 0);
    C3D_DepthTest(p.depth.enabled, reversedComparisons[p.depth.compare],
        static_cast<GPU_WRITEMASK>(GPU_WRITE_COLOR | ((p.depth.enabled && p.depth.write) ? GPU_WRITE_DEPTH : 0)));
    C3D_AlphaTest(alphaPlan.comparison != 7, comparisons[alphaPlan.comparison], alphaPlan.reference);
    auto src = p.blend.mode == 0 ? GPU_ONE : factor(p.blend.src, false);
    auto dst = p.blend.mode == 0 ? GPU_ZERO : factor(p.blend.dst, true);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, src, dst, GPU_ONE, GPU_ZERO);
    applyPicaTev(tevPlan);
    const auto n=p.vertices.size();
    const auto indexCount=p.primitive==gx::Primitive::quads?n/4*6:
        p.primitive==gx::Primitive::triangles?n:(n-2)*3;
    gx::GpuGeometryBatch batch;
    if(!reserveGxGeometry(n,indexCount,batch))return reject(gxGeometryFailure());
    for(std::size_t i=0;i<n;++i){
        const auto& v=p.vertices[i];auto& out=batch.vertices[i];out={};
        out.position={v.x,v.y,v.z,1};
        out.color={v.r/255.0f,v.g/255.0f,v.b/255.0f,v.a/255.0f};
        out.texcoord[0]={v.u*scaleU,1-v.v*scaleV};
    }
    std::size_t at=0;
    auto triangle=[&](std::size_t a,std::size_t b,std::size_t c){
        batch.indices[at++]=a;batch.indices[at++]=b;batch.indices[at++]=c;
    };
    switch(p.primitive){
    case gx::Primitive::quads:
        for(std::size_t i=0;i<n;i+=4){triangle(i,i+1,i+2);triangle(i,i+2,i+3);}
        break;
    case gx::Primitive::triangles:
        for(std::size_t i=0;i<n;i+=3)triangle(i,i+1,i+2);
        break;
    case gx::Primitive::triangleStrip:
        for(std::size_t i=2;i<n;++i)triangle(i%2?i-1:i-2,i%2?i-2:i-1,i);
        break;
    case gx::Primitive::triangleFan:
        for(std::size_t i=2;i<n;++i)triangle(0,i-1,i);
        break;
    default:cancelGxGeometry(batch);return reject("GX indexed primitive unsupported");
    }
    if(!submitGxGeometry(batch)){cancelGxGeometry(batch);return reject(gxGeometryFailure());}
    ++drawCount;
    return true;
}
}
bool initializeGxPica() {
    if (ready) return true;
    shader = DVLB_ParseFile(reinterpret_cast<u32*>(const_cast<unsigned char*>(gx_color_shader)), gx_color_shader_size);
    if (!shader) return reject("GX shader parse failed");
    shaderProgramInit(&program); programCreated = true;
    if (R_FAILED(shaderProgramSetVsh(&program, &shader->DVLE[0]))) { destroyGxPica(); return reject("GX vertex shader failed"); }
    transformLocation = shaderInstanceGetUniformLocation(program.vertexShader, "transform");
    if (transformLocation < 0) { destroyGxPica(); return reject("GX transform uniform absent"); }
    rigidShader=DVLB_ParseFile(reinterpret_cast<u32*>(const_cast<unsigned char*>(gx_rigid_shader)),gx_rigid_shader_size);
    if(!rigidShader){destroyGxPica();return reject("GX rigid shader parse failed");}
    shaderProgramInit(&rigidProgram);rigidProgramCreated=true;
    if(R_FAILED(shaderProgramSetVsh(&rigidProgram,&rigidShader->DVLE[0]))){destroyGxPica();return reject("GX rigid vertex shader failed");}
    rigidUniforms=shaderInstanceGetUniformLocation(rigidProgram.vertexShader,"u");
    rigidLoop=shaderInstanceGetUniformLocation(rigidProgram.vertexShader,"lightLoop");
    if(rigidUniforms<0||rigidUniforms+94>96||rigidLoop<0x60||rigidLoop>0x63){destroyGxPica();return reject("GX rigid uniforms absent");}
    rigidMtShader=DVLB_ParseFile(reinterpret_cast<u32*>(const_cast<unsigned char*>(gx_rigid_mt_shader)),gx_rigid_mt_shader_size);
    if(!rigidMtShader){destroyGxPica();return reject("GX multi-texture shader parse failed");}
    shaderProgramInit(&rigidMtProgram);rigidMtProgramCreated=true;
    if(R_FAILED(shaderProgramSetVsh(&rigidMtProgram,&rigidMtShader->DVLE[0]))){destroyGxPica();return reject("GX multi-texture vertex shader failed");}
    rigidMtUniforms=shaderInstanceGetUniformLocation(rigidMtProgram.vertexShader,"u");
    rigidMtLoop=shaderInstanceGetUniformLocation(rigidMtProgram.vertexShader,"lightLoop");
    if(rigidMtUniforms<0||rigidMtUniforms+94>96||rigidMtLoop<0x60||rigidMtLoop>0x63){destroyGxPica();return reject("GX multi-texture uniforms absent");}
    if(!initializeGxGeometry()){destroyGxPica();return reject(gxGeometryFailure());}
    ready = true; gx::setDrawSink(draw); gx::resetState();
    return true;
}
void destroyGxPica() {
    ready = false; gx::setDrawSink(nullptr);
    intervalActive=false;
    // Teardown does not clear framebuffer pixels. A failed mask stays poisoned
    // for this process lifetime, including renderer reinitialization.
    destroyGxTextures(); destroyGxGeometry();
    if(rigidProgramCreated){shaderProgramFree(&rigidProgram);rigidProgramCreated=false;}
    if(rigidMtProgramCreated){shaderProgramFree(&rigidMtProgram);rigidMtProgramCreated=false;}
    if(rigidMtShader){DVLB_Free(rigidMtShader);rigidMtShader=nullptr;}
    if(rigidShader){DVLB_Free(rigidShader);rigidShader=nullptr;}
    if (programCreated) { shaderProgramFree(&program); programCreated = false; }
    if (shader) { DVLB_Free(shader); shader = nullptr; }
}
const char* gxPicaFailure() { return failure; }
namespace {
// Native PICA state filter: consecutive draws with an identical raster plan,
// PICA material and target extent do not re-send fixed-function state. Any
// other path that changes Citro3D raster/TEV state invalidates this record.
struct AppliedCommandState {bool valid{};gx::RasterPlan raster;gx::PicaMaterialPlan material;unsigned width{},height{};};
AppliedCommandState appliedState;
std::uint64_t appliedSkips{},appliedApplies{};
void invalidateAppliedState(){appliedState.valid=false;++gxPicaStateSerial;}
void applyCommandRaster(const gx::RasterPlan& p){
    disableStencil();
    // Native vertex fog is already lowered to final RGB TEV. The diagnostic
    // policy is explicit; no path may inherit stale PICA hardware fog state.
    C3D_FogGasMode(GPU_NO_FOG,GPU_PLAIN_DENSITY,false);
    C3D_SetViewport(0,0,gx::currentTargetExtent.pixelHeight,gx::currentTargetExtent.pixelWidth);
    C3D_SetScissor(GPU_SCISSOR_NORMAL,p.scissor.left,p.scissor.top,p.scissor.right,p.scissor.bottom);
    C3D_CullFace(static_cast<GPU_CULLMODE>(gx::picaCull(p.cull)));
    C3D_DepthMap(true,-1,0);C3D_EarlyDepthTest(false,GPU_EARLYDEPTH_GREATER,0);
    const unsigned writes=(p.writeRgb?(GPU_WRITE_RED|GPU_WRITE_GREEN|GPU_WRITE_BLUE):0)|
        (p.writeAlpha?GPU_WRITE_ALPHA:0)|((p.depth.enabled&&p.depth.write)?GPU_WRITE_DEPTH:0);
    C3D_DepthTest(p.depth.enabled,reversedComparisons[p.depth.compare],static_cast<GPU_WRITEMASK>(writes));
    C3D_AlphaTest(p.alpha.comparison!=7,comparisons[p.alpha.comparison],p.alpha.reference);
    applyPicaBlend(p);
}
void applyCommandState(const gx::RasterPlan& p,const gx::PicaMaterialPlan& material){
    auto& a=appliedState;
    if(a.valid&&a.width==gx::currentTargetExtent.pixelWidth&&a.height==gx::currentTargetExtent.pixelHeight&&
       !std::memcmp(&a.raster,&p,sizeof(p))&&!std::memcmp(&a.material,&material,sizeof(material))){++appliedSkips;return;}
    applyCommandRaster(p);
    applyPicaMaterial(material);
    a.raster=p;a.material=material;a.width=gx::currentTargetExtent.pixelWidth;a.height=gx::currentTargetExtent.pixelHeight;a.valid=true;
    ++appliedApplies;
}
}
namespace {
bool intervalTarget(gx::AlphaIntervalTarget& target){
    const auto* fb=C3D_GetFrameBuf();
    if(!gx::currentTargetExtent.valid()||!fb||!fb->colorBuf||!fb->depthBuf||
       abi::colorFormat(fb)!=GPU_RB_RGBA8||abi::colorWriteMask(fb)!=15||abi::depthFormat(fb)!=GPU_RB_DEPTH24_STENCIL8||
       abi::isBlock32(fb)||abi::depthWriteMask(fb)!=3||fb->width!=gx::currentTargetExtent.pixelHeight||
       fb->height!=gx::currentTargetExtent.pixelWidth)return false;
    target={fb->colorBuf,fb->depthBuf,fb->width,fb->height};return true;
}
bool submitInterval(const gx::RasterPlan& p,const gx::PicaMaterialPlan& material,const gx::GpuGeometryBatch& batch){
    if(p.alphaUnion||p.earlyDepthAlpha||material.quantizedConstants||!p.alphaIntervalPolicy.enabled||!p.alphaIntervalPolicy.ownsTarget||
       !p.alphaInterval.lower||p.alphaInterval.lower>p.alphaInterval.upper||p.alphaInterval.upper==255||
       p.alpha.comparison!=6||p.alpha.reference!=p.alphaInterval.lower||
       p.alphaSecond.comparison!=3||p.alphaSecond.reference!=p.alphaInterval.upper)
        return reject("PICA alpha interval plan invalid");
    gx::AlphaIntervalTarget target;
    if(!intervalTarget(target)||!p.alphaIntervalPolicy.ownsTarget(target,p.alphaIntervalPolicy.user))
        return reject("PICA alpha interval target ownership unproved");
    struct State {const gx::RasterPlan& raster;gx::AlphaIntervalTarget target;bool failed{},masked{};};
    State state{p,target};intervalActive=true;
    C3D_Mtx identity;Mtx_Identity(&identity);
    bindProgram(&program);C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,transformLocation,&identity);
    applyCommandState(p,material); // TEV alpha remains immutable for all four passes.
    auto prepare=[](gx::AlphaIntervalPass pass,void* opaque){
        auto& s=*static_cast<State*>(opaque);const auto& r=s.raster;
        gx::AlphaIntervalTarget current;
        if(!platformPrepareGpuDraw()){
            s.failed=true;return reject("PICA alpha interval command block completion failed");
        }
        if(!intervalTarget(current)||!(current==s.target)||
           !r.alphaIntervalPolicy.ownsTarget(current,r.alphaIntervalPolicy.user)){
            s.failed=true;return reject("PICA alpha interval target changed/ownership lost");
        }
        constexpr unsigned bit=gx::alphaIntervalStencilBit;
        if(pass==gx::AlphaIntervalPass::upper){
            // Original source alpha, blend, depth test/writes and color masks.
            // Stencil gates only coverage and cannot change the source color.
            const unsigned writes=(r.writeRgb?(GPU_WRITE_RED|GPU_WRITE_GREEN|GPU_WRITE_BLUE):0)|
                (r.writeAlpha?GPU_WRITE_ALPHA:0)|((r.depth.enabled&&r.depth.write)?GPU_WRITE_DEPTH:0);
            C3D_DepthTest(true,r.depth.enabled?reversedComparisons[r.depth.compare]:GPU_ALWAYS,
                          static_cast<GPU_WRITEMASK>(writes));
            C3D_AlphaTest(true,GPU_LEQUAL,r.alphaInterval.upper);
            C3D_StencilTest(true,GPU_EQUAL,bit,bit,0);
            C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_KEEP);
        }else{
            const bool lower=pass==gx::AlphaIntervalPass::lower;
            C3D_DepthTest(true,GPU_ALWAYS,static_cast<GPU_WRITEMASK>(0));
            C3D_AlphaTest(lower,lower?GPU_GEQUAL:GPU_ALWAYS,lower?r.alphaInterval.lower:0);
            C3D_StencilTest(true,GPU_ALWAYS,lower?bit:0,bit,bit);
            const auto op=lower?GPU_STENCIL_REPLACE:GPU_STENCIL_ZERO;
            C3D_StencilOp(op,op,op);
            if(lower)s.masked=true;
        }
        return true;
    };
    const bool submitted=submitGxTriangleAlphaInterval(batch,prepare,&state);
    // This only restores cached state; it cannot repair pixels after failed
    // queue completion. Poison prevents further draws, and the owner's copy
    // hook must contain the failure until process exit.
    applyCommandRaster(p);intervalActive=false;invalidateAppliedState();
    if(!submitted){if(state.masked)intervalPoisoned=true;return state.failed?false:reject(gxGeometryFailure());}
    drawCount+=batch.indices.size()/3*4;return true;
}
}
// Uniform filter: C3D_FVUnif mirrors what this program last uploaded, so a
// row only becomes dirty when its value changed. Any other program bind or a
// new frame forces a full upload (constants of other programs may overlap).
static const shaderProgram_s* uniformOwner;
static std::uint64_t uniformRowsSkipped,uniformRowsWritten;
inline void setUniformRow(int id,const std::array<float,4>& v,bool fresh){
    C3D_FVec& c=C3D_FVUnif[GPU_VERTEX_SHADER][id];
    if(!fresh&&c.x==v[0]&&c.y==v[1]&&c.z==v[2]&&c.w==v[3]){++uniformRowsSkipped;return;}
    c=FVec4_New(v[0],v[1],v[2],v[3]);C3D_FVUnifDirty[GPU_VERTEX_SHADER][id]=true;++uniformRowsWritten;
}
// Shadow casters (see runtime/shadow_casters.hpp). Only the rows the position
// needs are kept: the three position rows of every matrix the draw placed.
namespace {
struct CasterDraw {const void* vertices;const std::uint16_t* indices;std::uint32_t indexCount;std::uint16_t firstRow;
    std::array<std::int8_t,10> offsets;std::uint8_t matrices,slot;bool mt;};
constexpr unsigned casterDrawCapacity=320,casterRowCapacity=4096;
std::array<CasterDraw,casterDrawCapacity> casterDraws;
std::array<std::array<float,4>,casterRowCapacity> casterRows;
unsigned casterDrawCount{},casterRowCount{},casterSlot{};
bool casterArmed{};
unsigned casterRemembered{},casterDropped{},casterPasses{};
void rememberCaster(const gx::RigidVertexPlan& rigid,const void* vertices,const std::uint16_t* indices,unsigned indexCount){
    unsigned matrices=0;
    for(const auto offset:rigid.matrixOffsets)matrices+=offset>=0;
    if(!vertices||!indices||!indexCount||!matrices||casterDrawCount>=casterDrawCapacity||3*matrices>casterRowCapacity-casterRowCount){++casterDropped;return;}
    auto& d=casterDraws[casterDrawCount++];
    d={vertices,indices,indexCount,std::uint16_t(casterRowCount),{},0,std::uint8_t(casterSlot),rigid.multiTexture};
    for(const auto offset:rigid.matrixOffsets)if(offset>=0){
        if(unsigned(offset)+3>rigid.uniforms.size())continue;
        d.offsets[d.matrices++]=offset;
        for(unsigned row=0;row<3;++row)casterRows[casterRowCount++]=rigid.uniforms[unsigned(offset)+row];
    }
    ++casterRemembered;
}
}
// The rigid draw being emitted has its unit 0 projected from the position:
// the console GPU guard records it with a hang (gpuCanaryMultiProjectedRefused).
static bool multiProjectedDraw;
static void uploadRigidUniforms(const gx::RigidVertexPlan& rigid,int uniforms,int loop,unsigned count,bool mt,bool fresh){
    multiProjectedDraw=mt&&rigid.multiProjected;
    for(unsigned n=0;n<count;++n)setUniformRow(uniforms+n,rigid.uniforms[n],fresh);
    // Multi-texture: the unit rows and the material above the palette. The
    // shader reads the rows of a unit only when its role is 1 or 2, so the
    // rows below (palette or unused) need not be sent.
    if(mt){for(unsigned n=std::max(count,rigid.paletteLimit());n<94;++n)setUniformRow(uniforms+n,rigid.uniforms[n],fresh);}
    else if(rigid.compact||rigid.color.ambientVertex)setUniformRow(uniforms+93,rigid.uniforms[93],fresh);
    const C3D_IVec want=IVec_Pack(rigid.lights-1,0,4,0);
    if(fresh||C3D_IVUnif[GPU_VERTEX_SHADER][loop-0x60]!=want)*C3D_IVUnifWritePtr(GPU_VERTEX_SHADER,loop)=want;
}
bool alphaIntervalCopySafe(){return !intervalPoisoned&&!intervalActive;}
void invalidatePicaCommandState(){invalidateAppliedState();uniformOwner=nullptr;boundProgram=nullptr;invalidateGxTextureBindings();invalidateGxGeometryBindings();}
void picaUniformStatistics(std::uint64_t& skipped,std::uint64_t& written){skipped=uniformRowsSkipped;written=uniformRowsWritten;}
void picaStateFilterStatistics(std::uint64_t& skips,std::uint64_t& applies){skips=appliedSkips;applies=appliedApplies;}
void setTriangleFogMeshPass(bool enabled){triangleFogMeshPass=enabled;}
bool submitStaticGxMesh(const gx::RasterPlan& p,const gx::PicaMaterialPlan& material,unsigned handle,const std::array<float,16>& matrix){
    ++gxPicaStateSerial;
    if(!ready||p.alphaUnion||p.alphaInterval.enabled||!alphaIntervalCopySafe())return reject("PICA static state invalid");
    if(!platformPrepareGpuDraw())return reject("PICA command block completion failed");
    C3D_Mtx transform;
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)transform.r[r].c[c]=matrix[r*4+3-c];
    bindProgram(&program);C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,transformLocation,&transform);
    applyCommandState(p,material);
    if(!submitStaticGxGeometry(handle))return reject(gxGeometryFailure());
    ++drawCount;return true;
}
bool submitGxCommandGeometry(const gx::RasterPlan& p,const gx::PicaMaterialPlan& material,const gx::GpuGeometryBatch& batch){
    GxProfileScope profile(GxProfilePart::submit);
    ++gxPicaStateSerial;
    if(!ready)return reject("PICA not initialized");
    if(!alphaIntervalCopySafe())return reject("PICA alpha interval scratch unsafe");
    if(batch.qPolicy!=gx::GeometryQPolicy::positiveOnly&&
       (!gx::validGeometryQBinding(batch,material)||p.alphaUnion||p.alphaInterval.enabled||!gx::validateGpuGeometry(batch)))
        return reject("PICA signed-q diagnostic contract invalid");
    if(p.alphaInterval.enabled)return submitInterval(p,material,batch);
    if(!platformPrepareGpuDraw())return reject("PICA command block completion failed");
    C3D_Mtx identity;Mtx_Identity(&identity);
    bindProgram(&program);C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,transformLocation,&identity);
    applyCommandState(p,material);
    if(p.alphaUnion){
        struct State {const gx::RasterPlan& raster;bool failed{};};State state{p};
        auto prepare=[](bool second,void* opaque){
            auto& s=*static_cast<State*>(opaque);
            if(!platformPrepareGpuDraw()){
                s.failed=true;return reject("PICA alpha command block completion failed");
            }
            const auto alpha=second?s.raster.alphaSecond:s.raster.alpha;
            C3D_AlphaTest(true,comparisons[alpha.comparison],alpha.reference);
            ++drawCount;return true;
        };
        const bool paired=submitGxTriangleAlphaPairs(batch,prepare,&state);
        invalidateAppliedState(); // per-pass alpha tests bypass the filter
        if(paired)return true;
        return state.failed?false:reject(gxGeometryFailure());
    }
    if(!submitGxGeometry(batch))return reject(gxGeometryFailure());
    ++drawCount;return true;
}
bool submitGxTriangleFogGeometry(const gx::RasterPlan& original,const gx::PicaMaterialPlan& material,
    const gx::RasterPlan& fog,const gx::PicaMaterialPlan& fogMaterial,const gx::GpuGeometryBatch& batch){
    GxProfileScope profile(GxProfilePart::submit);
    ++gxPicaStateSerial;
    if(!ready)return reject("PICA not initialized");
    if(batch.qPolicy!=gx::GeometryQPolicy::positiveOnly)return reject("PICA signed-q fog pair unsupported");
    if(original.alphaInterval.enabled||fog.alphaInterval.enabled||!alphaIntervalCopySafe())
        return reject("PICA alpha interval plus fog unsupported/unsafe");
    struct State {const gx::RasterPlan& original;PreparedPicaMaterial material;
                  const gx::RasterPlan& fog;PreparedPicaMaterial fogMaterial;
                  bool initialized{},prepareFailed{},mesh{};};
    State state{original,preparePicaMaterial(material),fog,preparePicaMaterial(fogMaterial)};
    // The whole-mesh form needs a writable stencil plane on the current target.
    {const auto* fb=C3D_GetFrameBuf();
     state.mesh=triangleFogMeshPass&&fb&&fb->depthBuf&&abi::depthFormat(fb)==GPU_RB_DEPTH24_STENCIL8&&abi::depthWriteMask(fb)==3;}
    auto prepare=[](bool fogPass,void* opaque){
        auto& s=*static_cast<State*>(opaque);
        if(!platformPrepareGpuDraw()){
            s.prepareFailed=true;return reject("PICA fog command block completion failed");
        }
        // Both halves use the same shader/identity. Queue completion preserves
        // the current Citro3D state; do not dirty four uniforms per triangle.
        if(!s.initialized){
            C3D_Mtx identity;Mtx_Identity(&identity);
            bindProgram(&program);C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,transformLocation,&identity);
            s.initialized=true;
        }
        applyCommandRaster(fogPass?s.fog:s.original);
        applyPreparedPicaMaterial(fogPass?s.fogMaterial:s.material);
        if(s.mesh){
            // Whole-mesh passes. The original pass marks the pixels it wins;
            // the fog pass (depth EQUAL) needs the mark and clears it, so each
            // surviving pixel is fogged exactly once even where triangles of
            // the mesh overlap at equal depth: the same result as fogging
            // after each triangle, in two draws instead of two per triangle.
            constexpr unsigned bit=triangleFogStencilBit;
            C3D_StencilTest(true,fogPass?GPU_EQUAL:GPU_ALWAYS,bit,bit,bit);
            C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,fogPass?GPU_STENCIL_ZERO:GPU_STENCIL_REPLACE);
        }
        ++drawCount;return true;
    };
    const bool paired=state.mesh?submitGxMeshPair(batch,prepare,&state):submitGxTrianglePairs(batch,prepare,&state);
    if(state.mesh)disableStencil();
    invalidateAppliedState(); // both passes apply raster/TEV directly
    if(paired)return true;
    return state.prepareFailed?false:reject(gxGeometryFailure());
}
bool submitGxRigidGeometry(const gx::RasterPlan& p,const gx::PicaMaterialPlan& material,const gx::GpuGeometryBatch& batch,const gx::RigidVertexPlan& rigid){
    GxProfileScope profile(GxProfilePart::submit);
    ++gxPicaStateSerial;
    if(batch.qPolicy!=gx::GeometryQPolicy::positiveOnly)return reject("PICA signed-q rigid unsupported");
    if(!ready||p.alphaUnion||p.alphaInterval.enabled||!alphaIntervalCopySafe()||!rigid.lights||rigid.lights>8)return reject("PICA rigid state invalid");
    if(!platformPrepareGpuDraw())return reject("PICA command block completion failed");
    const bool mt=rigid.multiTexture;
    if(mt&&!rigid.compact)return reject("PICA multi-texture requires compact records");
    shaderProgram_s* const bound=mt?&rigidMtProgram:&rigidProgram;
    const bool fresh=uniformOwner!=bound;
    bindProgram(bound);uniformOwner=bound;
    const int uniforms=mt?rigidMtUniforms:rigidUniforms,loop=mt?rigidMtLoop:rigidLoop;
    const unsigned count=rigid.uniformCount();
    if(!rigid.matrices||rigid.matrices>10||count>rigid.uniforms.size()||(mt&&count>rigid.paletteLimit()))return reject("PICA rigid palette invalid");
    uploadRigidUniforms(rigid,uniforms,loop,count,mt,fresh);
    applyCommandState(p,material);
    if(!(rigid.compact?submitGxCompactRigidGeometry(batch,rigid.staticVertices,mt):submitGxGeometry(batch,false)))return reject(gxGeometryFailure());
    if(casterSlot&&rigid.compact)rememberCaster(rigid,rigid.staticVertices?rigid.staticVertices:static_cast<const void*>(batch.vertices.data()),batch.indices.data(),unsigned(batch.indices.size()));
    ++drawCount;return true;
}
bool picaRigidTriangleFogAvailable(){
    const auto* fb=C3D_GetFrameBuf();
    return ready&&triangleFogMeshPass&&fb&&fb->depthBuf&&abi::depthFormat(fb)==GPU_RB_DEPTH24_STENCIL8&&abi::depthWriteMask(fb)==3;
}
bool submitGxRigidFogGeometry(const gx::RasterPlan& p,const gx::PicaMaterialPlan& material,const gx::RasterPlan& fogRaster,const gx::PicaMaterialPlan& fogMaterial,
    const gx::GpuGeometryBatch& batch,const gx::RigidVertexPlan& rigid,const gx::RigidVertexPlan& fogRigid){
    GxProfileScope profile(GxProfilePart::submit);
    ++gxPicaStateSerial;
    if(batch.qPolicy!=gx::GeometryQPolicy::positiveOnly)return reject("PICA signed-q rigid fog unsupported");
    const bool mt=rigid.multiTexture;
    if(!picaRigidTriangleFogAvailable()||p.alphaUnion||p.alphaInterval.enabled||fogRaster.alphaInterval.enabled||!alphaIntervalCopySafe()||
       (mt&&!rigid.compact)||!rigid.lights||rigid.lights>8)return reject("PICA rigid fog state invalid");
    const unsigned count=rigid.uniformCount();
    if(!rigid.matrices||rigid.matrices>10||count>rigid.uniforms.size()||(mt&&count>rigid.paletteLimit())||fogRigid.uniformCount()!=count)return reject("PICA rigid fog palette invalid");
    if(!platformPrepareGpuDraw())return reject("PICA command block completion failed");
    shaderProgram_s* const bound=mt?&rigidMtProgram:&rigidProgram;
    const int uniforms=mt?rigidMtUniforms:rigidUniforms,loop=mt?rigidMtLoop:rigidLoop;
    const bool fresh=uniformOwner!=bound;
    bindProgram(bound);uniformOwner=bound;
    uploadRigidUniforms(rigid,uniforms,loop,count,mt,fresh);
    constexpr unsigned bit=triangleFogStencilBit;
    // Original pass: mark the pixels this mesh wins.
    applyCommandRaster(p);applyPicaMaterial(material);
    C3D_StencilTest(true,GPU_ALWAYS,bit,bit,bit);
    C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_REPLACE);
    if(!(rigid.compact?submitGxCompactRigidGeometry(batch,rigid.staticVertices,mt):submitGxGeometry(batch,false))){
        disableStencil();invalidateAppliedState();return reject(gxGeometryFailure());
    }
    // Fog pass: equal depth and the mark, cleared on use (one fog per pixel).
    bool okay=platformPrepareGpuDraw();
    if(okay){
        uploadRigidUniforms(fogRigid,uniforms,loop,count,mt,false);
        applyCommandRaster(fogRaster);applyPicaMaterial(fogMaterial);
        C3D_StencilTest(true,GPU_EQUAL,bit,bit,bit);
        C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_ZERO);
        redrawGxGeometry(batch);
    }
    disableStencil();invalidateAppliedState();
    if(!okay)return reject("PICA fog command block completion failed");
    drawCount+=2;return true;
}
// Native shape replay: identical state/uniform setup, persistent buffers.
// `chained` is the same-state continuation of the previous replayed shape: the
// program, raster, material and textures it applied are still in effect (the
// caller proved it with gxPicaStateSerial and the GX state epoch), so only the
// changed uniform rows and the draw itself are emitted.
bool submitNativeRigid(const gx::RasterPlan& p,const gx::PicaMaterialPlan& material,const gx::RigidVertexPlan& rigid,const void* vertices,const std::uint16_t* indices,unsigned indexCount,bool chained){
    GxProfileScope profile(GxProfilePart::submit);
    if(!chained)++gxPicaStateSerial;
    if(!ready||p.alphaUnion||p.alphaInterval.enabled||!alphaIntervalCopySafe()||!rigid.lights||rigid.lights>8)return reject("PICA rigid state invalid");
    {static const auto i=registerGxNamedProfile("submit.prepare");GxNamedProfileScope scope(i);
    if(!platformPrepareGpuDraw())return reject("PICA command block completion failed");}
    const bool mt=rigid.multiTexture;
    if(mt&&!rigid.compact)return reject("PICA multi-texture requires compact records");
    shaderProgram_s* const bound=mt?&rigidMtProgram:&rigidProgram;
    const bool fresh=uniformOwner!=bound;
    const int uniforms=mt?rigidMtUniforms:rigidUniforms,loop=mt?rigidMtLoop:rigidLoop;
    const unsigned count=rigid.uniformCount();
    if(!rigid.matrices||rigid.matrices>10||count>rigid.uniforms.size()||(mt&&count>rigid.paletteLimit()))return reject("PICA rigid palette invalid");
    {static const auto i=registerGxNamedProfile("submit.uniforms");GxNamedProfileScope scope(i);
    bindProgram(bound);uniformOwner=bound;
    uploadRigidUniforms(rigid,uniforms,loop,count,mt,fresh);}
    {static const auto i=registerGxNamedProfile("submit.state");GxNamedProfileScope scope(i);
    // A continuation runs under the state its predecessor applied (see above).
    if(chained)++appliedSkips;else applyCommandState(p,material);}
    static const auto drawIndex=registerGxNamedProfile("submit.draw");GxNamedProfileScope drawScope(drawIndex);
    if(!rigid.compact||!drawResidentRigid(vertices,indices,indexCount,mt))return reject(gxGeometryFailure());
    if(casterSlot)rememberCaster(rigid,vertices,indices,indexCount);
    ++drawCount;return true;
}


// GPU-resident EFB capture (native render-to-texture): the main colour buffer
// is copied tile-for-tile into a scratch texture by the transfer engine, then a
// quad resamples it (rotated: the 3DS framebuffer is portrait) straight into the
// resident RGBA8 texture of the capture image. No readback, encode or decode.
namespace {
// Two sizes stay resident: the scene capture and the shadow image are both
// taken in the same frame, and reallocating one for the other would wait for
// the GPU twice per frame.
struct CaptureSurfaces {C3D_Tex scratch{};C3D_RenderTarget* target{};C3D_Tex vram{};std::uint64_t used{};};
std::array<CaptureSurfaces,2> captureSurfaces{};std::uint64_t captureClock{};
unsigned pow2(unsigned v){unsigned r=8;while(r<v)r<<=1;return r;}
}
std::uint64_t gpuCaptures;
extern "C" void OSReport(const char*,...);
namespace {bool captureFail(int code){static unsigned reports;if(reports<6){++reports;OSReport("Native GPU capture fallback reason=%d\n",code);}return false;}}
bool gpuCaptureCopy(void* framebuffer,std::uint64_t handle,const ColorCopyPlan& plan,unsigned orient){
    if(!ready||!framebuffer||!alphaIntervalCopySafe()||plan.redChannel||plan.format!=6)return captureFail(1);
    unsigned dw{},dh{};auto* dst=static_cast<C3D_Tex*>(gpuCaptureTexture(handle,dw,dh));
    if(!dst||dw!=plan.width||dh!=plan.height)return captureFail(2);
    const auto& fb=*abi::frameBuf(framebuffer);
    const auto& e=plan.extent;
    if(abi::colorFormat(&fb)!=GPU_RB_RGBA8||abi::isBlock32(&fb)||(fb.width&7)||(fb.height&7)||fb.width!=e.pixelHeight||fb.height!=e.pixelWidth)return captureFail(3);
    const unsigned sw=pow2(fb.width),sh=pow2(fb.height);
    if(sw>1024||sh>1024)return captureFail(4);
    CaptureSurfaces* surfaces=nullptr;
    for(auto& c:captureSurfaces)if(c.scratch.data&&c.scratch.width==sw&&c.scratch.height==sh&&c.vram.data&&c.vram.width==dst->width&&c.vram.height==dst->height)surfaces=&c;
    if(!surfaces){surfaces=&captureSurfaces[0];for(auto& c:captureSurfaces)if(c.used<surfaces->used)surfaces=&c;}
    surfaces->used=++captureClock;
    auto& captureScratch=surfaces->scratch;auto& captureTarget=surfaces->target;auto& captureVram=surfaces->vram;
    if(!captureScratch.data||captureScratch.width!=sw||captureScratch.height!=sh){
        if(captureScratch.data){if(platformCompleteGpu(500000000)!=GpuCompletion::complete)return captureFail(5);C3D_TexDelete(&captureScratch);captureScratch={};}
        if(!C3D_TexInitWithParams(&captureScratch,nullptr,{u16(sw),u16(sh),0,GPU_RGBA8,GPU_TEX_2D,false}))return captureFail(6);
    }
    // Render targets must live in VRAM: render into a VRAM twin, then copy it
    // into the cached (linear) texture with the transfer engine.
    if(!captureVram.data||captureVram.width!=dst->width||captureVram.height!=dst->height){
        if(captureTarget||captureVram.data){if(platformCompleteGpu(500000000)!=GpuCompletion::complete)return captureFail(7);}
        if(captureTarget){C3D_RenderTargetDelete(captureTarget);captureTarget=nullptr;}
        if(captureVram.data){C3D_TexDelete(&captureVram);captureVram={};}
        if(!C3D_TexInitWithParams(&captureVram,nullptr,{dst->width,dst->height,0,GPU_RGBA8,GPU_TEX_2D,true}))return captureFail(10);
        C3D_DEPTHTYPE none(-1);
        captureTarget=C3D_RenderTargetCreateFromTex(&captureVram,GPU_TEXFACE_2D,0,none);
        if(!captureTarget){C3D_TexDelete(&captureVram);captureVram={};return captureFail(8);}
    }
    // Earlier draws must reach the queue before the transfer reads the target.
    if(!flushGxGeometry()||platformReserveGpuEntries(2,500000000)!=GpuCompletion::complete)return captureFail(9);
    const u32 line=fb.width*8*4/16,gap=(sw-fb.width)*8*4/16;
    C3D_SyncTextureCopy(static_cast<u32*>(fb.colorBuf),GX_BUFFER_DIM(line,0),static_cast<u32*>(captureScratch.data),
                        GX_BUFFER_DIM(line,gap),std::size_t(fb.width)*fb.height*4,8);
    if(!platformPrepareGpuDraw()||!C3D_FrameDrawOn(captureTarget))return captureFail(10);
    invalidatePicaCommandState();
    disableStencil();
    C3D_FogGasMode(GPU_NO_FOG,GPU_PLAIN_DENSITY,false);
    C3D_SetViewport(0,0,dst->width,dst->height);
    C3D_SetScissor(GPU_SCISSOR_DISABLE,0,0,0,0);
    C3D_CullFace(GPU_CULL_NONE);C3D_DepthMap(true,-1,0);C3D_EarlyDepthTest(false,GPU_EARLYDEPTH_GREATER,0);
    C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);C3D_AlphaTest(false,GPU_ALWAYS,0);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
    C3D_TexSetFilter(&captureScratch,GPU_NEAREST,GPU_NEAREST);C3D_TexSetWrap(&captureScratch,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
    C3D_TexBind(0,&captureScratch);
    C3D_TexEnv* env=C3D_GetTexEnv(0);C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env,C3D_Both,GPU_TEXTURE0,GPU_PRIMARY_COLOR,GPU_PRIMARY_COLOR);C3D_TexEnvFunc(env,C3D_Both,GPU_REPLACE);
    for(int stage=1;stage<6;++stage)C3D_TexEnvInit(C3D_GetTexEnv(stage));
    C3D_Mtx identity;Mtx_Identity(&identity);
    bindProgram(&program);C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,transformLocation,&identity);
    gx::GpuGeometryBatch batch;
    if(!reserveGxGeometry(4,6,batch)){platformBindFramebuffer(activeFramebuffer());return captureFail(11);}
    // Destination: GX row j is memory row j of the cached texture (t=1 at row 0).
    const float s1=float(dw)/dst->width,tb=1-float(dh)/dst->height;
    const float cornerS[4]{0,s1,s1,0},cornerT[4]{1,1,tb,tb};
    const unsigned ci[4]{0,dw,dw,0},cj[4]{0,0,dh,dh};
    for(unsigned n=0;n<4;++n){
        auto& v=batch.vertices[n];v={};
        float x=cornerS[n]*2-1,y=cornerT[n]*2-1;
        if(orient&1)x=-x;
        if(orient&2)y=-y;
        v.position={x,y,-0.5f,1};v.color={1,1,1,1};
        // GX destination texel corner -> logical EFB point -> 3DS screen pixel.
        const float lx=plan.left+float(ci[n])*plan.sourceWidth/dw,ly=plan.top+float(cj[n])*plan.sourceHeight/dh;
        const float px=lx*e.pixelWidth/e.logicalWidth,py=ly*e.pixelHeight/e.logicalHeight;
        // Portrait framebuffer: memory row = screen x, memory column = height-1-screen y.
        float ss=(float(fb.width)-py)/sw,tt=1-px/sh;
        if(orient&4)tt=px/sh;
        if(orient&8)ss=py/sw;
        v.texcoord[0]={ss,tt};
    }
    const std::uint16_t quad[6]{0,1,2,0,2,3};std::copy(quad,quad+6,batch.indices.begin());
    const bool drawn=submitGxGeometry(batch);
    if(!drawn)cancelGxGeometry(batch);
    // Back to the game's target; this also invalidates every filtered state.
    if(!platformBindFramebuffer(activeFramebuffer())||!drawn)return captureFail(12);
    if(!flushGxGeometry()||platformReserveGpuEntries(2,500000000)!=GpuCompletion::complete)return captureFail(14);
    C3D_SyncTextureCopy(static_cast<u32*>(captureVram.data),0,static_cast<u32*>(dst->data),0,C3D_TexCalcTotalSize(dst->size,0),8);
    gpuCaptureCommit(handle);++gpuCaptures;++drawCount;
    return true;
}
namespace {
struct BloomSurfaces {C3D_Tex scratch{},bright{},blurred{};C3D_RenderTarget* brightTarget{};C3D_RenderTarget* blurredTarget{};bool failed{};};
BloomSurfaces bloom;
// Full-target quad; texture coordinates of each unit are given per corner in
// the order (-1,-1), (1,-1), (1,1), (-1,1).
bool bloomQuad(const std::array<std::array<std::array<float,2>,4>,3>& coordinates){
    C3D_Mtx identityMatrix;Mtx_Identity(&identityMatrix);
    bindProgram(&program);C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,transformLocation,&identityMatrix);
    gx::GpuGeometryBatch batch;
    if(!reserveGxGeometry(4,6,batch))return false;
    const float corner[4][2]{{-1,-1},{1,-1},{1,1},{-1,1}};
    for(unsigned n=0;n<4;++n){
        auto& v=batch.vertices[n];v={};v.position={corner[n][0],corner[n][1],-0.5f,1};v.color={1,1,1,1};
        for(unsigned unit=0;unit<3;++unit)v.texcoord[unit]=coordinates[unit][n];
    }
    const std::uint16_t quad[6]{0,1,2,0,2,3};std::copy(quad,quad+6,batch.indices.begin());
    if(!submitGxGeometry(batch)){cancelGxGeometry(batch);return false;}
    ++drawCount;return true;
}
// Heat haze grid over the whole target: the frame copy with texture
// coordinates that sway inside the picture and stay put on its border.
bool hazeGrid(float us,float vt,float amplitude,unsigned phase){
    constexpr unsigned columns=12,rows=8;
    C3D_Mtx identityMatrix;Mtx_Identity(&identityMatrix);
    bindProgram(&program);C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,transformLocation,&identityMatrix);
    gx::GpuGeometryBatch batch;
    if(!reserveGxGeometry((columns+1)*(rows+1),columns*rows*6,batch))return false;
    const float time=float(phase%4096)*0.19f;
    for(unsigned j=0;j<=rows;++j)for(unsigned i=0;i<=columns;++i){
        const float a=float(i)/columns,b=float(j)/rows;
        const bool border=!i||!j||i==columns||j==rows;
        const float shiftS=border?0:amplitude*std::sin(time+b*19.0f+a*5.0f);
        const float shiftT=border?0:amplitude*std::cos(time*1.31f+a*23.0f-b*7.0f);
        auto& v=batch.vertices[j*(columns+1)+i];v={};
        v.position={a*2-1,b*2-1,-0.5f,1};v.color={1,1,1,1};
        v.texcoord[0]={(a+shiftS)*us,1-(1-(b+shiftT))*vt};
    }
    unsigned n=0;
    for(unsigned j=0;j<rows;++j)for(unsigned i=0;i<columns;++i){
        const std::uint16_t corner=std::uint16_t(j*(columns+1)+i);
        const std::uint16_t cell[6]{corner,std::uint16_t(corner+1),std::uint16_t(corner+columns+2),corner,std::uint16_t(corner+columns+2),std::uint16_t(corner+columns+1)};
        for(const auto index:cell)batch.indices[n++]=index;
    }
    if(!submitGxGeometry(batch)){cancelGxGeometry(batch);return false;}
    ++drawCount;return true;
}
u32 gray(unsigned value){value=std::min(value,255u);return value|(value<<8)|(value<<16)|(value<<24);}
}
bool drawGxBloom(const BloomSettings& settings){
    // The bloom copies the frame with the transfer engine: off on a console
    // where that engine stopped (GPU guard level 1).
    if(gpuCanaryTransferLevel())return false;
    auto* framebuffer=activeFramebuffer();
    if((!settings.glow&&!settings.mono[3]&&!settings.haze)||!ready||bloom.failed||!framebuffer||framebuffer!=mainFramebuffer()||!alphaIntervalCopySafe())return false;
    const auto& fb=*abi::frameBuf(framebuffer);
    if(abi::colorFormat(&fb)!=GPU_RB_RGBA8||abi::isBlock32(&fb)||(fb.width&7)||(fb.height&7))return false;
    const unsigned sw=pow2(fb.width),sh=pow2(fb.height);
    if(sw>1024||sh>1024)return false;
    if(!bloom.scratch.data){
        // The targets keep the frame's memory orientation (portrait).
        C3D_DEPTHTYPE none(-1);
        const bool created=C3D_TexInitWithParams(&bloom.scratch,nullptr,{u16(sw),u16(sh),0,GPU_RGBA8,GPU_TEX_2D,false})&&
            C3D_TexInitWithParams(&bloom.bright,nullptr,{64,128,0,GPU_RGBA8,GPU_TEX_2D,true})&&
            C3D_TexInitWithParams(&bloom.blurred,nullptr,{32,64,0,GPU_RGBA8,GPU_TEX_2D,true})&&
            (bloom.brightTarget=C3D_RenderTargetCreateFromTex(&bloom.bright,GPU_TEXFACE_2D,0,none))&&
            (bloom.blurredTarget=C3D_RenderTargetCreateFromTex(&bloom.blurred,GPU_TEXFACE_2D,0,none));
        if(!created){
            if(bloom.brightTarget)C3D_RenderTargetDelete(bloom.brightTarget);
            if(bloom.blurredTarget)C3D_RenderTargetDelete(bloom.blurredTarget);
            if(bloom.scratch.data)C3D_TexDelete(&bloom.scratch);
            if(bloom.bright.data)C3D_TexDelete(&bloom.bright);
            if(bloom.blurred.data)C3D_TexDelete(&bloom.blurred);
            bloom={};bloom.failed=true;return captureFail(30);
        }
        for(auto* texture:{&bloom.scratch,&bloom.bright,&bloom.blurred}){
            C3D_TexSetFilter(texture,GPU_LINEAR,GPU_LINEAR);C3D_TexSetWrap(texture,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
        }
    }
    // Earlier draws must reach the queue before the transfer reads the frame.
    if(!flushGxGeometry()||platformReserveGpuEntries(2,500000000)!=GpuCompletion::complete)return captureFail(31);
    const u32 line=fb.width*8*4/16,gap=(sw-fb.width)*8*4/16;
    C3D_SyncTextureCopy(static_cast<u32*>(fb.colorBuf),GX_BUFFER_DIM(line,0),static_cast<u32*>(bloom.scratch.data),
                        GX_BUFFER_DIM(line,gap),std::size_t(fb.width)*fb.height*4,8);
    if(!platformPrepareGpuDraw())return captureFail(32);
    if(settings.glow&&!C3D_FrameDrawOn(bloom.brightTarget))return captureFail(32);
    invalidatePicaCommandState();
    disableStencil();
    C3D_FogGasMode(GPU_NO_FOG,GPU_PLAIN_DENSITY,false);
    C3D_SetScissor(GPU_SCISSOR_DISABLE,0,0,0,0);
    C3D_CullFace(GPU_CULL_NONE);C3D_DepthMap(true,-1,0);C3D_EarlyDepthTest(false,GPU_EARLYDEPTH_GREATER,0);
    C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);C3D_AlphaTest(false,GPU_ALWAYS,0);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
    C3D_TexEnvBufUpdate(C3D_Both,0);
    auto stage=[](int index){auto* env=C3D_GetTexEnv(index);C3D_TexEnvInit(env);return env;};
    // Frame memory: column = s, row counted from t = 1.
    const float us=float(fb.width)/sw,vt=float(fb.height)/sh;
    std::array<std::array<std::array<float,2>,4>,3> coordinates{};
    const float cornerS[4]{0,1,1,0},cornerT[4]{0,0,1,1};
    bool okay=true;
    if(settings.glow){
        // 1. Brightness above the threshold, times the colour:
        //    (R/4 + G/4 + B/2 - point) * colour * gain, as the original's first filter.
        C3D_SetViewport(0,0,bloom.bright.width,bloom.bright.height);
        // Units 1 and 2 are never unbound: Citro3D reads the texture it is given
        // for those units. A unit no TEV stage names costs nothing.
        C3D_TexBind(0,&bloom.scratch);
        {
            auto* env=stage(0);
            C3D_TexEnvSrc(env,C3D_RGB,GPU_TEXTURE0,GPU_TEXTURE0,GPU_CONSTANT);
            C3D_TexEnvOpRgb(env,GPU_TEVOP_RGB_SRC_R,GPU_TEVOP_RGB_SRC_G,GPU_TEVOP_RGB_SRC_COLOR);
            C3D_TexEnvFunc(env,C3D_RGB,GPU_INTERPOLATE);C3D_TexEnvColor(env,gray(128));
            C3D_TexEnvSrc(env,C3D_Alpha,GPU_CONSTANT,GPU_CONSTANT,GPU_CONSTANT);C3D_TexEnvFunc(env,C3D_Alpha,GPU_REPLACE);
            env=stage(1);
            C3D_TexEnvSrc(env,C3D_RGB,GPU_TEXTURE0,GPU_PREVIOUS,GPU_CONSTANT);
            C3D_TexEnvOpRgb(env,GPU_TEVOP_RGB_SRC_B,GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_SRC_COLOR);
            C3D_TexEnvFunc(env,C3D_RGB,GPU_INTERPOLATE);C3D_TexEnvColor(env,gray(128));
            env=stage(2);
            C3D_TexEnvSrc(env,C3D_RGB,GPU_PREVIOUS,GPU_CONSTANT,GPU_CONSTANT);
            C3D_TexEnvFunc(env,C3D_RGB,GPU_SUBTRACT);C3D_TexEnvColor(env,gray(settings.point));
            env=stage(3);
            C3D_TexEnvSrc(env,C3D_RGB,GPU_TEXTURE0,GPU_PREVIOUS,GPU_PREVIOUS);
            C3D_TexEnvFunc(env,C3D_RGB,GPU_MODULATE);
            // Eight taps of weight ratio/255 in the original: gain 8*ratio/255.
            env=stage(4);
            C3D_TexEnvSrc(env,C3D_RGB,GPU_PREVIOUS,GPU_CONSTANT,GPU_CONSTANT);
            C3D_TexEnvFunc(env,C3D_RGB,GPU_MODULATE);C3D_TexEnvScale(env,C3D_RGB,GPU_TEVSCALE_4);
            C3D_TexEnvColor(env,gray(settings.blurRatio*2));
            stage(5);
        }
        for(unsigned n=0;n<4;++n)coordinates[0][n]={cornerS[n]*us,1-(1-cornerT[n])*vt};
        okay=bloomQuad(coordinates);
        // 2. Three taps around each texel into the half-size target.
        if(okay&&platformPrepareGpuDraw()&&C3D_FrameDrawOn(bloom.blurredTarget)){
            C3D_SetViewport(0,0,bloom.blurred.width,bloom.blurred.height);
            C3D_TexBind(0,&bloom.bright);C3D_TexBind(1,&bloom.bright);C3D_TexBind(2,&bloom.bright);
            auto* env=stage(0);
            C3D_TexEnvSrc(env,C3D_RGB,GPU_TEXTURE0,GPU_CONSTANT,GPU_CONSTANT);C3D_TexEnvFunc(env,C3D_RGB,GPU_MODULATE);C3D_TexEnvColor(env,gray(85));
            C3D_TexEnvSrc(env,C3D_Alpha,GPU_CONSTANT,GPU_CONSTANT,GPU_CONSTANT);C3D_TexEnvFunc(env,C3D_Alpha,GPU_REPLACE);
            env=stage(1);
            C3D_TexEnvSrc(env,C3D_RGB,GPU_TEXTURE1,GPU_CONSTANT,GPU_PREVIOUS);C3D_TexEnvFunc(env,C3D_RGB,GPU_MULTIPLY_ADD);C3D_TexEnvColor(env,gray(85));
            env=stage(2);
            C3D_TexEnvSrc(env,C3D_RGB,GPU_TEXTURE2,GPU_CONSTANT,GPU_PREVIOUS);C3D_TexEnvFunc(env,C3D_RGB,GPU_MULTIPLY_ADD);C3D_TexEnvColor(env,gray(86));
            stage(3);stage(4);stage(5);
            // Radius in frame pixels (the original offsets by size/6400 of a 640-wide image).
            const float radius=std::max(float(settings.blurSize)/16,2.0f)*float(fb.height)/640;
            const float tapS[3]{0,0.866f,-0.866f},tapT[3]{1,-0.5f,-0.5f};
            for(unsigned unit=0;unit<3;++unit)for(unsigned n=0;n<4;++n)
                coordinates[unit][n]={cornerS[n]+tapS[unit]*radius/fb.width,cornerT[n]+tapT[unit]*radius/fb.height};
            okay=bloomQuad(coordinates);
        }else okay=false;
    }
    // 3. Back on the frame: the monochrome filter, then the glow.
    if(!platformBindFramebuffer(framebuffer))return captureFail(33);
    if(okay&&platformPrepareGpuDraw()){
        disableStencil();
        C3D_FogGasMode(GPU_NO_FOG,GPU_PLAIN_DENSITY,false);
        C3D_SetViewport(0,0,fb.width,fb.height);
        C3D_SetScissor(GPU_SCISSOR_DISABLE,0,0,0,0);
        C3D_CullFace(GPU_CULL_NONE);C3D_DepthMap(true,-1,0);C3D_EarlyDepthTest(false,GPU_EARLYDEPTH_GREATER,0);
        C3D_DepthTest(false,GPU_ALWAYS,static_cast<GPU_WRITEMASK>(GPU_WRITE_RED|GPU_WRITE_GREEN|GPU_WRITE_BLUE));
        C3D_AlphaTest(false,GPU_ALWAYS,0);
        C3D_TexEnvBufUpdate(C3D_Both,0);
    }else okay=false;
    if(okay&&settings.haze){
        // Heat haze first: the filters below still read the copy made before it.
        C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,GPU_ZERO,GPU_ONE);
        C3D_TexBind(0,&bloom.scratch);
        auto* env=stage(0);
        C3D_TexEnvSrc(env,C3D_RGB,GPU_TEXTURE0,GPU_TEXTURE0,GPU_TEXTURE0);C3D_TexEnvFunc(env,C3D_RGB,GPU_REPLACE);
        C3D_TexEnvSrc(env,C3D_Alpha,GPU_CONSTANT,GPU_CONSTANT,GPU_CONSTANT);C3D_TexEnvFunc(env,C3D_Alpha,GPU_REPLACE);
        C3D_TexEnvColor(env,u32(std::min(settings.haze,255u))<<24);
        for(int index=1;index<6;++index)stage(index);
        okay=hazeGrid(us,vt,settings.hazeAmplitude,settings.hazePhase)&&platformPrepareGpuDraw();
    }
    if(okay&&settings.mono[3]){
        C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,GPU_ZERO,GPU_ONE);
        C3D_TexBind(0,&bloom.scratch);
        auto* env=stage(0);
        C3D_TexEnvSrc(env,C3D_RGB,GPU_TEXTURE0,GPU_CONSTANT,GPU_CONSTANT);
        C3D_TexEnvOpRgb(env,GPU_TEVOP_RGB_SRC_R,GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_SRC_COLOR);
        C3D_TexEnvFunc(env,C3D_RGB,GPU_MODULATE);
        C3D_TexEnvSrc(env,C3D_Alpha,GPU_CONSTANT,GPU_CONSTANT,GPU_CONSTANT);C3D_TexEnvFunc(env,C3D_Alpha,GPU_REPLACE);
        C3D_TexEnvColor(env,u32(settings.mono[0])|(u32(settings.mono[1])<<8)|(u32(settings.mono[2])<<16)|(u32(settings.mono[3])<<24));
        for(int index=1;index<6;++index)stage(index);
        for(unsigned n=0;n<4;++n)coordinates[0][n]={cornerS[n]*us,1-(1-cornerT[n])*vt};
        okay=bloomQuad(coordinates)&&platformPrepareGpuDraw();
    }
    if(okay&&settings.glow){
        C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,settings.mode==1?GPU_ONE_MINUS_DST_COLOR:GPU_ONE,GPU_SRC_ALPHA,GPU_ZERO,GPU_ONE);
        C3D_TexBind(0,&bloom.blurred);
        auto* env=stage(0);
        C3D_TexEnvSrc(env,C3D_RGB,GPU_TEXTURE0,GPU_CONSTANT,GPU_CONSTANT);C3D_TexEnvFunc(env,C3D_RGB,GPU_MODULATE);
        C3D_TexEnvSrc(env,C3D_Alpha,GPU_CONSTANT,GPU_CONSTANT,GPU_CONSTANT);C3D_TexEnvFunc(env,C3D_Alpha,GPU_REPLACE);
        C3D_TexEnvColor(env,u32(settings.blend[0])|(u32(settings.blend[1])<<8)|(u32(settings.blend[2])<<16)|(u32(settings.blend[3])<<24));
        for(int index=1;index<6;++index)stage(index);
        for(unsigned n=0;n<4;++n)coordinates[0][n]={cornerS[n],cornerT[n]};
        okay=bloomQuad(coordinates);
    }
    invalidatePicaCommandState();
    return okay;
}
bool gpuRenderDestination(const void* identity,gx::TextureHandle& handle); // gx_copy_execution.cpp
void armGxShadowCasters(){casterArmed=ready;casterDrawCount=casterRowCount=0;casterSlot=0;}
void selectGxShadowCaster(unsigned slot){casterSlot=casterArmed?slot:0;}
void gxShadowCasterStatistics(unsigned& remembered,unsigned& dropped,unsigned& passes){
    remembered=casterRemembered;dropped=casterDropped;passes=casterPasses;casterRemembered=casterDropped=casterPasses=0;
}
bool drawGxShadowCasters(const void* identity,unsigned width,unsigned height,std::span<const ShadowCasterView> views){
    const bool armed=casterArmed;const unsigned draws=casterDrawCount;
    casterArmed=false;casterSlot=0;casterDrawCount=casterRowCount=0;
    if(!ready||!armed||!activeFramebuffer()||!alphaIntervalCopySafe())return false;
    gx::TextureHandle handle{};
    if(!gpuRenderDestination(identity,handle))return captureFail(20);
    // The PICA renders straight into the image's own texture, moved once into
    // VRAM: no transfer-engine copy. Alpha 0 rendered into a VRAM twin and
    // copied it to the linear texture every frame; on the console that copy
    // never finished on the first gameplay frame (run 10, job type 4 silent).
    unsigned dw{},dh{};auto* dst=static_cast<C3D_Tex*>(gpuCaptureVramTexture(handle,dw,dh));
    if(!dst||dw!=width||dh!=height||dst->width!=width||dst->height!=height)return captureFail(21);
    // VRAM images are never evicted, so the target lives as long as the image.
    static C3D_RenderTarget* shadowTarget{};static const void* shadowTargetData{};
    if(!shadowTarget||shadowTargetData!=dst->data){
        C3D_DEPTHTYPE none(-1);
        shadowTarget=C3D_RenderTargetCreateFromTex(dst,GPU_TEXFACE_2D,0,none);
        if(!shadowTarget){shadowTargetData=nullptr;return captureFail(24);}
        shadowTargetData=dst->data;
    }
    if(!platformPrepareGpuDraw()||!C3D_FrameDrawOn(shadowTarget))return captureFail(25);
    invalidatePicaCommandState();
    disableStencil();
    C3D_FogGasMode(GPU_NO_FOG,GPU_PLAIN_DENSITY,false);
    C3D_SetViewport(0,0,dst->width,dst->height);
    C3D_SetScissor(GPU_SCISSOR_DISABLE,0,0,0,0);
    C3D_CullFace(GPU_CULL_NONE);C3D_DepthMap(true,-1,0);C3D_EarlyDepthTest(false,GPU_EARLYDEPTH_GREATER,0);
    C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);C3D_AlphaTest(false,GPU_ALWAYS,0);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
    C3D_TexEnv* env=C3D_GetTexEnv(0);C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env,C3D_Both,GPU_PRIMARY_COLOR,GPU_PRIMARY_COLOR,GPU_PRIMARY_COLOR);C3D_TexEnvFunc(env,C3D_Both,GPU_REPLACE);
    for(int stage=1;stage<6;++stage)C3D_TexEnvInit(C3D_GetTexEnv(stage));
    // Clear with a quad: a memory fill would need its own command-list split.
    bool okay=true;
    {
        C3D_Mtx identityMatrix;Mtx_Identity(&identityMatrix);
        bindProgram(&program);C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,transformLocation,&identityMatrix);
        gx::GpuGeometryBatch batch;
        if(reserveGxGeometry(4,6,batch)){
            const float corner[4][2]{{-1,-1},{1,-1},{1,1},{-1,1}};
            for(unsigned n=0;n<4;++n){auto& v=batch.vertices[n];v={};v.position={corner[n][0],corner[n][1],-0.5f,1};v.color={0,0,0,0};}
            const std::uint16_t quad[6]{0,1,2,0,2,3};std::copy(quad,quad+6,batch.indices.begin());
            if(!submitGxGeometry(batch)){cancelGxGeometry(batch);okay=false;}
        }else okay=false;
    }
    if(okay){
        // Fetch the stage again: Citro3D only re-sends a stage that was
        // fetched after the last draw.
        env=C3D_GetTexEnv(0);C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env,C3D_Both,GPU_CONSTANT,GPU_CONSTANT,GPU_CONSTANT);C3D_TexEnvFunc(env,C3D_Both,GPU_REPLACE);
        C3D_TexEnvColor(env,0xffffffff);
        unsigned scissorSlot=0;
        const float fw=float(width),fh=float(height);
        for(unsigned n=0;n<draws&&okay;++n){
            const auto& d=casterDraws[n];
            if(!d.slot||d.slot>views.size()||!views[d.slot-1].used)continue;
            const auto& view=views[d.slot-1];
            if(!platformPrepareGpuDraw()){okay=false;break;}
            if(scissorSlot!=d.slot){
                C3D_SetScissor(GPU_SCISSOR_NORMAL,view.x,height-view.y-view.size,view.x+view.size,height-view.y);
                scissorSlot=d.slot;
            }
            const bool mt=d.mt;multiProjectedDraw=false;
            bindProgram(mt?&rigidMtProgram:&rigidProgram);
            const int uniforms=mt?rigidMtUniforms:rigidUniforms;
            const auto* rows=&casterRows[d.firstRow];
            // Uniform registers are shared by every program: a row that
            // already holds the value is not sent again.
            for(unsigned m=0;m<d.matrices;++m)for(unsigned r=0;r<3;++r)setUniformRow(uniforms+d.offsets[m]+r,rows[3*m+r],false);
            // Camera space -> clip space of the tile; depth is not used.
            const float sx=2*view.size/fw,sy=-2*float(view.size)/fh;
            const float ox=2*view.x/fw-1,oy=1-2*view.y/fh;
            setUniformRow(uniforms+6,{sx*view.st[0][0],sx*view.st[0][1],sx*view.st[0][2],sx*view.st[0][3]+ox},false);
            setUniformRow(uniforms+7,{sy*view.st[1][0],sy*view.st[1][1],sy*view.st[1][2],sy*view.st[1][3]+oy},false);
            setUniformRow(uniforms+8,{0,0,0,-0.5f},false);
            setUniformRow(uniforms+9,{0,0,0,1},false);
            if(!drawResidentRigid(d.vertices,d.indices,d.indexCount,mt))okay=false;
            else ++drawCount;
        }
    }
    uniformOwner=nullptr;
    // The receivers sample this target next: write the PICA's framebuffer
    // cache back to VRAM before switching (rebinding the textures afterwards
    // clears the texture cache).
    if(okay){GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH,1);GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE,1);}
    // Back to the game's target; this also invalidates every filtered state.
    if(!platformBindFramebuffer(activeFramebuffer())||!okay)return captureFail(26);
    gpuCaptureCommit(handle);++casterPasses;
    // Development dump of the shadow image (SD switch shadow-dump.txt = pass number).
    static int dumpAt=-2;static unsigned dumpPass;
    if(dumpAt==-2){dumpAt=-1;if(auto* f=std::fopen("sdmc:/3ds/dusklight/shadow-dump.txt","rb")){int v=0;if(std::fscanf(f,"%d",&v)==1)dumpAt=v;std::fclose(f);}}
    if(dumpAt>=0&&int(++dumpPass)==dumpAt&&platformCompleteGpu(500000000)==GpuCompletion::complete){
        const std::size_t bytes=C3D_TexCalcTotalSize(dst->size,0);
        GSPGPU_InvalidateDataCache(dst->data,bytes);
        if(auto* f=std::fopen("sdmc:/3ds/dusklight/shadow-atlas.bin","wb")){std::fwrite(dst->data,1,bytes,f);std::fclose(f);}
        OSReport("Native shadow image dumped: %ux%u draws=%u\n",unsigned(dst->width),unsigned(dst->height),draws);
        for(unsigned n=0;n<views.size();++n)if(views[n].used)OSReport("Native shadow view %u tile=%u,%u,%u s=%f %f %f %f t=%f %f %f %f\n",n,views[n].x,views[n].y,views[n].size,
            views[n].st[0][0],views[n].st[0][1],views[n].st[0][2],views[n].st[0][3],views[n].st[1][0],views[n].st[1][1],views[n].st[1][2],views[n].st[1][3]);
        for(unsigned n=0;n<draws&&n<6;++n){const auto& d=casterDraws[n];
            OSReport("Native shadow draw %u slot=%u mt=%u indices=%u matrices=%u\n",n,d.slot,d.mt,unsigned(d.indexCount),d.matrices);}
    }
    return true;
}

int gxBoundProgramKind(){return boundProgram==&program?0:boundProgram==&rigidProgram?1:boundProgram==&rigidMtProgram?2:-1;}
bool gxDrawMultiProjected(){return multiProjectedDraw;}
std::uint32_t gxProgramCodeHash(int kind){
    const DVLB_s* blob=kind==0?shader:kind==1?rigidShader:kind==2?rigidMtShader:nullptr;
    if(!blob||!blob->DVLP.codeData)return 0;
    // Same mixing as the guard's hash of an uploaded program (gpu_canary_ctru.cpp).
    std::uint32_t h=0x811C9DC5u;
    for(u32 i=0;i<blob->DVLP.codeSize;++i){h=(h^blob->DVLP.codeData[i])*0x9E3779B1u;h^=h>>15;}
    return h;
}
}
extern "C" void duskNativeShadowCaster(unsigned slot){dusk::native::selectGxShadowCaster(slot);}
