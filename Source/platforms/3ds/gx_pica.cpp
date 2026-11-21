#include "runtime/gx_worker_sync.hpp"
#include "gx_pica.hpp"
#include "runtime/gx_geometry_pack.hpp"
#include "runtime/gx_profile.hpp"
#include "runtime/gx_rigid_vertex.hpp"
#include "runtime/skin_palette.hpp"
#include "runtime/gx_resident_cull.hpp"
#include "runtime/gx_compact_cache.hpp"
#include "runtime/gx_pica_math.hpp"
#include "runtime/gx_pica_tev.hpp"
#include "runtime/gx_screen_projectors.hpp"
#include "gx_texture_pica.hpp"
#include "gx_material_pica.hpp"
#include "gx_blend_pica.hpp"
#include "gx_geometry_pica.hpp"
#include "gx_uniform_row.hpp"
#include "runtime/gx_raster_plan.hpp"
#include "runtime/gpu_sync.hpp"
#include "runtime/gx_framebuffer.hpp"
#include "runtime/gx_copy_pixels.hpp"
#include "runtime/gx_state_serial.hpp"
#include "runtime/shadow_casters.hpp"
#include "runtime/model_lod.hpp"
#include "runtime/screen_effects.hpp"
#include "runtime/gpu_canary.hpp"
#include "runtime/ctru_abi.hpp"
#include "runtime/ctru_shader_abi.hpp"
#include "runtime/gx_vertex_variant.hpp"
#include "runtime/perf_features.hpp"
#include "runtime/tape_context.hpp"
#include <3ds.h>
#include <citro3d.h>
#include <atomic>
#include <type_traits>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdlib>

extern "C" void OSReport(const char*,...);
extern const unsigned char gx_color_shader[];
extern const unsigned int gx_color_shader_size;
extern const unsigned char gx_rigid_shader[];
extern const unsigned int gx_rigid_shader_size;
extern const unsigned char gx_rigid_mt_shader[];
extern const unsigned int gx_rigid_mt_shader_size;
extern const unsigned char gx_caster_shader[];
extern const unsigned int gx_caster_shader_size;
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
// Variants of the multi-texture program (features::Id::vertexVariants,
// runtime/gx_vertex_variant.hpp): programs 1.. of its binary. They share its
// code, uniform layout and constants, so a change between them sends an
// entry point and the output map, never code (C3D_BindProgram sends code
// only for another binary), and the uniform rows on the PICA stay valid.
std::array<shaderProgram_s,gx::vertexVariantCount> variantPrograms{};
unsigned variantProgramsCreated{};
bool variantsReady{},variantsAllowed{true};
// Model-shadow casters (gx_caster.v.pica): position only, see drawGxShadowCasters.
DVLB_s* casterShader{};
shaderProgram_s casterProgram{};
bool casterProgramCreated;
int casterUniforms=-1;
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
C3D_Tex screenProjectorTexture{};
C3D_RenderTarget* screenProjectorTarget{};
struct ScreenProjectorKey {
    gx::ScreenProjectors::Corners corners{};
    std::array<std::uint64_t,2> handles{};
    std::array<unsigned,2> samplers{};
    unsigned width{},height{};
    bool operator==(const ScreenProjectorKey&)const=default;
};
ScreenProjectorKey screenProjectorKey{};
void invalidateAppliedState();
void disableStencil(){
    C3D_StencilTest(false,GPU_ALWAYS,0,0xff,0);
    C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_KEEP);
}
bool reject(const char* reason) { failure = reason; return false; }
// Triangle fog (a fog overlay pass for materials with no TEV room for vertex
// fog) as two whole-mesh passes; false restores one pair of draws per
// triangle. The stencil bit is distinct from the alpha interval's.
bool cullOn=true;
bool triangleFogMeshPass=true;
constexpr unsigned triangleFogStencilBit=2;
static_assert(triangleFogStencilBit!=gx::alphaIntervalStencilBit);
constexpr unsigned additiveFogStencilBit=4;
static_assert(!(additiveFogStencilBit&(triangleFogStencilBit|gx::alphaIntervalStencilBit)));
// Alpha-union layers (water sparkle and foam: 'LEQUAL a OR GEQUAL b', additive,
// depth write off) as two whole-mesh passes instead of two draws per
// triangle, and on the rigid GPU shaders instead of the CPU vertex path. See
// picaAlphaUnionMesh for why the result is the same.
bool alphaUnionMeshPass=true,alphaUnionGpu=true;
struct AlphaUnionCounts {std::uint64_t meshes{},meshTriangles{},gpuMeshes{},perTriangle{};} alphaUnionCounts;
// Frame copies whose texture padding was filled with the copy's edge.
std::uint64_t capturePaddingFills{};
// Citro3D reloads the shader's constant uniforms (and marks them for upload)
// on every C3D_BindProgram, also for the program already bound. Consecutive
// draws of one program bind once; frame begin and every state invalidation
// forget the record.
const shaderProgram_s* boundProgram;
inline void bindProgram(shaderProgram_s* next);
// Program changes per picture (log "Native vertex plans"): binds, and the
// binds that make Citro3D send the shader code again (another binary).
// The binary is told by the program's identity, never read through it: a
// diagnostic must not be able to fault a bind.
std::atomic<std::uint32_t> programBinds{},programCodeSends{};
const shaderProgram_s* boundCode{};
const shaderProgram_s* codeOf(const shaderProgram_s* p){
    for(unsigned n=1;n<variantPrograms.size();++n)if(p==&variantPrograms[n])return &rigidMtProgram;
    return p;
}
}
// The console GPU guard records which of these programs a hung draw used.
int gxBoundProgramKind();
namespace {
inline void bindProgram(shaderProgram_s* next){
    if(boundProgram==next)return;
    C3D_BindProgram(next);boundProgram=next;
    // Counted as Citro3D sees it: a bind after an invalidation of the same
    // program changes nothing on the PICA.
    static const shaderProgram_s* previous;
    if(next==previous)return;
    previous=next;programBinds.fetch_add(1,std::memory_order_relaxed);
    if(const auto* code=codeOf(next);code!=boundCode){boundCode=code;programCodeSends.fetch_add(1,std::memory_order_relaxed);}
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
namespace {
// A variant may replace program 0 between two draws only if the rows those
// draws uploaded mean the same to it: the same uniform locations and the same
// constants (Citro3D loads a program's constants on every bind).
bool sameConstants(const shaderInstance_s& a,const shaderInstance_s& b){
    return a.numFloat24Uniforms==b.numFloat24Uniforms&&a.intUniformMask==b.intUniformMask&&a.boolUniformMask==b.boolUniformMask&&
        (!a.numFloat24Uniforms||!std::memcmp(a.float24Uniforms,b.float24Uniforms,a.numFloat24Uniforms*sizeof(float24Uniform_s)));
}
bool createVertexVariants(){
    if(rigidMtShader->numDVLE!=gx::vertexVariantCount)return false;
    for(unsigned n=1;n<gx::vertexVariantCount;++n){
        shaderProgramInit(&variantPrograms[n]);variantProgramsCreated=n+1;
        if(R_FAILED(shaderProgramSetVsh(&variantPrograms[n],abi::dvle(rigidMtShader,n))))return false;
        auto* variant=variantPrograms[n].vertexShader;
        if(shaderInstanceGetUniformLocation(variant,"u")!=rigidMtUniforms||shaderInstanceGetUniformLocation(variant,"lightLoop")!=rigidMtLoop||
           !sameConstants(*variant,*rigidMtProgram.vertexShader))return false;
    }
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
    variantsReady=createVertexVariants();
    OSReport("Native vertex variants: %s\n",variantsReady?"ready (features.txt vertex=1 selects them)":"unavailable, the general program draws every plan");
    casterShader=DVLB_ParseFile(reinterpret_cast<u32*>(const_cast<unsigned char*>(gx_caster_shader)),gx_caster_shader_size);
    if(!casterShader){destroyGxPica();return reject("GX caster shader parse failed");}
    shaderProgramInit(&casterProgram);casterProgramCreated=true;
    if(R_FAILED(shaderProgramSetVsh(&casterProgram,&casterShader->DVLE[0]))){destroyGxPica();return reject("GX caster vertex shader failed");}
    // Same row numbering as the rigid shaders: the remembered palette rows go
    // back to the rows the caster's vertices name.
    casterUniforms=shaderInstanceGetUniformLocation(casterProgram.vertexShader,"u");
    if(casterUniforms!=rigidUniforms||casterUniforms!=rigidMtUniforms){destroyGxPica();return reject("GX caster uniforms differ from the rigid shaders");}
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
    // C3D_Fini owns render targets. The texture owns its separate VRAM.
    if(screenProjectorTexture.data)C3D_TexDelete(&screenProjectorTexture);
    screenProjectorTexture={};screenProjectorTarget=nullptr;screenProjectorKey={};
    if(rigidProgramCreated){shaderProgramFree(&rigidProgram);rigidProgramCreated=false;}
    for(unsigned n=1;n<variantProgramsCreated;++n)shaderProgramFree(&variantPrograms[n]);
    variantProgramsCreated=0;variantsReady=false;
    if(rigidMtProgramCreated){shaderProgramFree(&rigidMtProgram);rigidMtProgramCreated=false;}
    if(rigidMtShader){DVLB_Free(rigidMtShader);rigidMtShader=nullptr;}
    if(casterProgramCreated){shaderProgramFree(&casterProgram);casterProgramCreated=false;}
    if(casterShader){DVLB_Free(casterShader);casterShader=nullptr;}
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
// Compared as words (gx_uniform_row.hpp): same decisions as the float compare.
inline void setUniformRow(int id,const std::array<float,4>& v,bool fresh){
    C3D_FVec& c=C3D_FVUnif[GPU_VERTEX_SHADER][id];
    if(!fresh&&sameUniformRow(c,v)){++uniformRowsSkipped;return;}
    storeUniformRow(c,v);C3D_FVUnifDirty[GPU_VERTEX_SHADER][id]=true;++uniformRowsWritten;
}
// Shadow casters (see runtime/shadow_casters.hpp). Only the rows the position
// needs are kept: the three position rows of every matrix the draw placed.
namespace {
// Resident inputs are borrowed; transient inputs point to snapshots with
// frame lifetime. Generation is used only to count replays across a reclaim.
// offsets: the first row of each matrix the draw placed (ten slots, or the
// entries of a merged skin draw, runtime/skin_palette.hpp).
struct CasterDraw {const void* vertices;const std::uint16_t* indices;std::uint32_t indexCount;std::uint16_t firstRow;
    std::array<std::int8_t,skin::maxMatrices> offsets;std::uint8_t matrices,slot;bool mt,retained;std::uint32_t generation;
    std::uint16_t owner;}; // owner: measured dump (gx_profile.hpp)
constexpr unsigned casterDrawCapacity=320,casterRowCapacity=4096;
std::array<CasterDraw,casterDrawCapacity> casterDraws;
std::array<std::array<float,4>,casterRowCapacity> casterRows;
unsigned casterDrawCount{},casterRowCount{};
// The caster the shapes issued now belong to. Per thread: the second-core
// worker draws a queued shape under the slot it was issued with while the
// game thread selects the next model's (gx_worker.hpp).
thread_local unsigned casterSlot{};
bool casterArmed{};
ShadowCasterStatistics casterStats;
void rememberCaster(const gx::RigidVertexPlan& rigid,const void* vertices,const std::uint16_t* indices,
                    unsigned indexCount,unsigned slot,std::size_t availableVertexBytes=0){
    unsigned matrices=rigid.skinEntries;
    for(const auto offset:rigid.matrixOffsets)matrices+=offset>=0;
    if(!vertices||!indices||!indexCount||!matrices||casterDrawCount>=casterDrawCapacity||3*matrices>casterRowCapacity-casterRowCount){++casterStats.dropped;return;}
    const void* sourceVertices=vertices;
    const auto* sourceIndices=indices;
    if(!retainGxCasterGeometry(vertices,availableVertexBytes,indices,indexCount)){
        ++casterStats.retainFailed;++casterStats.dropped;return;
    }
    const bool retained=vertices!=sourceVertices||indices!=sourceIndices;
    if(retained)++casterStats.retained;
    auto& d=casterDraws[casterDrawCount++];
    d={vertices,indices,indexCount,std::uint16_t(casterRowCount),{},0,std::uint8_t(slot),rigid.multiTexture,retained,gxGeometryArenaGeneration(),
       std::uint16_t(gxDrawOwnerNow())};
    auto keep=[&](unsigned offset){
        if(offset+3>rigid.uniforms.size())return;
        d.offsets[d.matrices++]=std::int8_t(offset);
        for(unsigned row=0;row<3;++row)casterRows[casterRowCount++]=rigid.uniforms[offset+row];
    };
    for(const auto offset:rigid.matrixOffsets)if(offset>=0)keep(unsigned(offset));
    for(unsigned k=0;k<rigid.skinEntries;++k)keep(skin::row(k));
    ++casterStats.remembered;
}
}
// The rigid draw being emitted has its unit 0 projected from the position:
// the console GPU guard records it with a hang (gpuCanaryMultiProjectedRefused).
static bool multiProjectedDraw;
extern "C" void OSReport(const char*,...);
// Uploads with a unit generated from the normal (role -1, J3D environment
// maps such as the Master Sword): the log shows they reach the GPU route
// instead of the CPU vertex path. The first three, then at every power of two.
static void reportNormalTexgen(const gx::RigidVertexPlan& rigid){
    static unsigned long uploads;
    ++uploads;
    if(uploads>3&&(uploads&(uploads-1)))return;
    const auto& refused=gx::gxRigidLastRefusedTexgen;
    OSReport("Native multi-texture texgen from the normal on the GPU: uploads=%lu plans=%lu roles %.1f %.1f %.1f lights=%u matrices=%u"
        " (last refused texgen gen=%08lx post=%08lx reject=%lu)\n",
        uploads,static_cast<unsigned long>(gx::gxRigidNormalTexgenPlans),
        double(gx::multiRole(rigid,0)),double(gx::multiRole(rigid,1)),double(gx::multiRole(rigid,2)),rigid.lights,rigid.matrices,
        static_cast<unsigned long>(refused.gen),static_cast<unsigned long>(refused.post),static_cast<unsigned long>(refused.code));
}
static void uploadRigidUniforms(const gx::RigidVertexPlan& rigid,int uniforms,int loop,unsigned count,bool mt,bool fresh){
    multiProjectedDraw=mt&&rigid.multiProjected;
    if(mt&&(gx::multiRole(rigid,0)<0||gx::multiRole(rigid,1)<0||gx::multiRole(rigid,2)<0))reportNormalTexgen(rigid);
    for(unsigned n=0;n<count;++n)setUniformRow(uniforms+n,rigid.uniforms[n],fresh);
    // Multi-texture: the unit rows and the material above the palette. The
    // shader reads the rows of a unit only when its role is 1, -1 or 2, so
    // the rows below (palette or unused) need not be sent.
    if(mt){for(unsigned n=std::max(count,rigid.paletteLimit());n<94;++n)setUniformRow(uniforms+n,rigid.uniforms[n],fresh);}
    else if(rigid.compact||rigid.color.ambientVertex)setUniformRow(uniforms+93,rigid.uniforms[93],fresh);
    const C3D_IVec want=IVec_Pack(rigid.lights-1,0,4,0);
    if(fresh||C3D_IVUnif[GPU_VERTEX_SHADER][loop-0x60]!=want)*C3D_IVUnifWritePtr(GPU_VERTEX_SHADER,loop)=want;
}
// The program a rigid draw runs (the one place it is chosen). A multi-texture
// plan whose key a variant accepts runs that variant while
// features::Id::vertexVariants is on; the general program otherwise. The
// uniform rows belong to the general program's layout either way (`layout`),
// which every variant shares: a change of variant re-sends no row.
namespace {
struct RigidProgram {shaderProgram_s* program;const shaderProgram_s* layout;std::uint32_t key;unsigned variant;};
RigidProgram rigidProgramFor(const gx::RigidVertexPlan& rigid,bool fogOnly,bool generalOnly=false){
    if(!rigid.multiTexture)return {&rigidProgram,&rigidProgram,0,0};
    const std::uint32_t key=gx::vertexPlanKey(rigid.uniforms,rigid.lights,fogOnly);
    const unsigned variant=!generalOnly&&variantsReady&&variantsAllowed&&features::on(features::Id::vertexVariants)?gx::vertexVariantFor(key):0;
    return {variant?&variantPrograms[variant]:&rigidMtProgram,&rigidMtProgram,key,variant};
}
}
void setVertexVariantsAllowed(bool allowed){variantsAllowed=allowed;}
// Census of the multi-texture program's draws by plan kind
// (runtime/gx_vertex_variant.hpp): draws and indices per key, reported every
// 300 pictures with the program changes. The drawing side counts; the game
// thread reads and clears at a picture end (a draw lands in the next report
// at worst).
namespace {
struct VertexPlanCount {std::atomic<std::uint32_t> key{},draws{},indices{};};
std::array<VertexPlanCount,64> vertexPlanCounts;
std::atomic<std::uint32_t> vertexPlanOverflow{};
std::array<std::atomic<std::uint32_t>,gx::vertexVariantCount> programIndices{};
// Real plans for Tools/3ds/test_vertex_variants.py (SD file vertex-plans.txt
// = picture number): from that picture on, up to four draws of each kind,
// with their rows and first vertices, into vertex-plans.bin.
struct VertexPlanRecord {
    std::uint32_t key,lights,stride,vertexCount;
    gx::UniformRows rows;
    std::array<std::uint8_t,16*sizeof(gx::CompactRigidVertexMTWide)> vertices;
};
constexpr unsigned vertexPlanRecordCapacity=256;
VertexPlanRecord* vertexPlanRecords{};
std::atomic<bool> vertexPlanRecording{};
std::atomic<std::uint32_t> vertexPlanRecorded{};
void recordVertexPlan(std::uint32_t key,const gx::RigidVertexPlan& rigid,const void* vertices,const std::uint16_t* indices,unsigned indexCount){
    const unsigned at=vertexPlanRecorded.load(std::memory_order_relaxed);
    if(at>=vertexPlanRecordCapacity||!vertices||!indices)return;
    unsigned same=0;
    for(unsigned n=0;n<at;++n)same+=vertexPlanRecords[n].key==key;
    if(same>=4)return;
    auto& r=vertexPlanRecords[at];
    r.key=key;r.lights=rigid.lights;r.stride=unsigned(rigid.recordBytes());r.rows=rigid.uniforms;
    r.vertexCount=std::min(indexCount,16u);
    for(unsigned n=0;n<r.vertexCount;++n)
        std::memcpy(r.vertices.data()+n*r.stride,static_cast<const std::uint8_t*>(vertices)+std::size_t(indices[n])*r.stride,r.stride);
    vertexPlanRecorded.store(at+1,std::memory_order_relaxed);
}
// A multi-texture draw of `drawn` indices was sent with this plan (key) by
// program `variant` (0 the general one).
void countVertexPlan(const RigidProgram& program,const gx::RigidVertexPlan& rigid,unsigned drawn,const void* vertices,const std::uint16_t* indices){
    const std::uint32_t key=program.key;
    programIndices[program.variant].fetch_add(drawn,std::memory_order_relaxed);
    unsigned slot=(key*0x9E3779B1u)>>26;
    bool counted=false;
    for(unsigned probe=0;probe<vertexPlanCounts.size()&&!counted;++probe,slot=(slot+1)&63){
        auto& c=vertexPlanCounts[slot];
        const auto held=c.key.load(std::memory_order_relaxed);
        if(held&&held!=key)continue;
        if(!held)c.key.store(key,std::memory_order_relaxed);
        c.draws.fetch_add(1,std::memory_order_relaxed);c.indices.fetch_add(drawn,std::memory_order_relaxed);counted=true;
    }
    if(!counted)vertexPlanOverflow.fetch_add(drawn,std::memory_order_relaxed);
    // Acquire: the buffer the game thread allocated before raising the flag.
    if(vertexPlanRecording.load(std::memory_order_acquire))recordVertexPlan(key,rigid,vertices,indices,drawn);
}
}
void reportVertexPlans(){
    static unsigned pictures,picture;static int recordAt=-2;
    ++picture;
    if(recordAt==-2){
        recordAt=-1;
        if(auto* f=std::fopen("sdmc:/3ds/DuskLight/vertex-plans.txt","rb")){int v=0;if(std::fscanf(f,"%d",&v)==1)recordAt=v;std::fclose(f);}
    }
    // Armed for ten pictures; written once the worker has drawn them (each
    // picture's close syncs the one before).
    if(recordAt>0&&int(picture)==recordAt){
        vertexPlanRecords=static_cast<VertexPlanRecord*>(std::calloc(vertexPlanRecordCapacity,sizeof(VertexPlanRecord)));
        if(vertexPlanRecords)vertexPlanRecording.store(true,std::memory_order_release);
    }
    if(recordAt>0&&int(picture)==recordAt+10)vertexPlanRecording.store(false,std::memory_order_relaxed);
    if(recordAt>0&&int(picture)==recordAt+12&&vertexPlanRecords){
        const std::uint32_t count=vertexPlanRecorded.load(std::memory_order_relaxed);
        if(auto* f=std::fopen("sdmc:/3ds/DuskLight/vertex-plans.bin","wb")){
            const std::uint32_t head[4]{0x52505644u,1u,count,std::uint32_t(sizeof(VertexPlanRecord))};
            std::fwrite(head,sizeof(head),1,f);std::fwrite(vertexPlanRecords,sizeof(VertexPlanRecord),count,f);std::fclose(f);
            OSReport("Native vertex plans recorded: %lu draws into vertex-plans.bin\n",static_cast<unsigned long>(count));
        }
        std::free(vertexPlanRecords);vertexPlanRecords=nullptr;
    }
    if(++pictures<300)return;
    const double n=pictures;pictures=0;
    struct Line {std::uint32_t key,draws,indices;};
    std::array<Line,64> lines{};unsigned used=0;double draws=0,indices=0;
    for(auto& c:vertexPlanCounts){
        const auto key=c.key.exchange(0,std::memory_order_relaxed);
        const auto d=c.draws.exchange(0,std::memory_order_relaxed),i=c.indices.exchange(0,std::memory_order_relaxed);
        if(!key)continue;
        lines[used++]={key,d,i};draws+=d;indices+=i;
    }
    const auto overflow=vertexPlanOverflow.exchange(0,std::memory_order_relaxed);
    const auto binds=programBinds.exchange(0,std::memory_order_relaxed),sends=programCodeSends.exchange(0,std::memory_order_relaxed);
    {
        char text[256];int at=0;
        for(unsigned v=0;v<gx::vertexVariantCount;++v)
            at+=std::snprintf(text+at,sizeof(text)-std::size_t(at),"%s %s %.0f",v?";":"",gx::vertexVariants[v].name,programIndices[v].exchange(0,std::memory_order_relaxed)/n);
        OSReport("Native vertex programs, indices per picture:%s\n",text);
    }
    std::sort(lines.begin(),lines.begin()+used,[](const Line& a,const Line& b){return a.indices>b.indices;});
    OSReport("Native vertex plans, last %u pictures, per picture: multi-texture draws %.1f, indices %.0f (+%.0f not keyed); program binds %.1f, code sends %.1f; %u kinds\n",
        unsigned(n),draws/n,indices/n,overflow/n,binds/n,sends/n,used);
    for(unsigned k=0;k<used;k+=4){
        char text[256];int at=0;
        for(unsigned m=k;m<used&&m<k+4;++m)
            at+=std::snprintf(text+at,sizeof(text)-std::size_t(at)," %07lx d=%.1f i=%.0f;",static_cast<unsigned long>(lines[m].key),lines[m].draws/n,lines[m].indices/n);
        OSReport("Native vertex plans:%s\n",text);
    }
}
bool alphaIntervalCopySafe(){return !intervalPoisoned&&!intervalActive;}
void invalidatePicaCommandState(){invalidateAppliedState();uniformOwner=nullptr;boundProgram=nullptr;invalidateGxTextureBindings();invalidateGxGeometryBindings();}
void picaUniformStatistics(std::uint64_t& skipped,std::uint64_t& written){skipped=uniformRowsSkipped;written=uniformRowsWritten;}
void picaStateFilterStatistics(std::uint64_t& skips,std::uint64_t& applies){skips=appliedSkips;applies=appliedApplies;}
void setTriangleFogMeshPass(bool enabled){triangleFogMeshPass=enabled;}
// The two tails of an alpha union are disjoint (splitAlphaUnion), so each
// fragment is drawn once in either order. Drawing all triangles with the first
// test and then all with the second only changes the ORDER in which fragments
// reach a pixel. That order cannot matter when:
// - nothing of the mesh writes depth (each pass tests the same depth buffer);
// - every written channel gets dst*1 + a term of the fragment alone: the
//   destination factor is one, and the source factor reads neither colour
//   nor alpha this mesh writes. A clamped sum of terms that are never
//   negative is the same in any order (min(255, d + sum)); with a
//   destination factor of one the blend unit's rounding acts on the
//   fragment's term only.
// Water sparkle and foam (SRCALPHA/ONE, z write off) are this case; ONE/ONE
// too. Multiplying or alpha-mixing blends are not and keep the per-triangle
// order.
bool picaAlphaUnionMesh(const gx::RasterPlan& p){
    if(!p.alphaUnion||!alphaUnionMeshPass||(p.depth.enabled&&p.depth.write)||p.blend.mode!=1||p.subtractBlend)return false;
    const unsigned s=p.blend.src,d=p.blend.dst;
    // GX destination factor 1 is one; 6 (destination alpha) is one on an RGB8
    // target (applyPicaBlend). A written alpha implies a stored alpha.
    const bool destinationOne=d==1||(d==6&&!p.storesAlpha);
    // 0 zero, 1 one, 4/5 source alpha; 6/7 read the destination alpha, which
    // is constant unless this mesh writes alpha. 2/3 read destination colour.
    const bool sourceAlone=s==0||s==1||s==4||s==5||((s==6||s==7)&&!p.writeAlpha);
    return destinationOne&&sourceAlone;
}
bool picaAlphaUnionGpu(const gx::RasterPlan& p){return alphaUnionGpu&&picaAlphaUnionMesh(p);}
namespace {
// Second whole-mesh pass of an alpha union on the rigid GPU route: the same
// geometry, uniforms and state with the second alpha test. `draw` repeats
// the draw just made (its buffers are still bound).
template<class Draw> bool drawAlphaUnionSecond(const gx::RasterPlan& p,Draw draw){
    bool okay=platformPrepareGpuDraw();
    if(okay){C3D_AlphaTest(true,comparisons[p.alphaSecond.comparison],p.alphaSecond.reference);okay=draw();}
    invalidateAppliedState(); // the alpha test differs from the applied raster
    if(!okay)return reject("PICA alpha-union second pass failed");
    ++alphaUnionCounts.gpuMeshes;++drawCount;return true;
}
}
void setAlphaUnionPasses(bool mesh,bool gpu){alphaUnionMeshPass=mesh;alphaUnionGpu=mesh&&gpu;}
void reportPicaSubmission(){
    const auto b=gxBindingStatistics();const auto& u=alphaUnionCounts;
    OSReport("Native submission state: attribute layouts sent=%llu kept=%llu; vertex buffers sent=%llu first-buffer-only=%llu kept=%llu\n",
        static_cast<unsigned long long>(b.layoutsSent),static_cast<unsigned long long>(b.layoutsKept),static_cast<unsigned long long>(b.buffersSent),
        static_cast<unsigned long long>(b.buffersFirstOnly),static_cast<unsigned long long>(b.buffersKept));
    OSReport("Native alpha-union draws: whole-mesh cpu=%llu (triangles %llu) gpu=%llu per-triangle=%llu; frame copy padding filled=%llu\n",
        static_cast<unsigned long long>(u.meshes),static_cast<unsigned long long>(u.meshTriangles),static_cast<unsigned long long>(u.gpuMeshes),
        static_cast<unsigned long long>(u.perTriangle),static_cast<unsigned long long>(capturePaddingFills));
}
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
        const bool mesh=picaAlphaUnionMesh(p);
        const bool paired=mesh?submitGxAlphaUnionMesh(batch,prepare,&state):submitGxTriangleAlphaPairs(batch,prepare,&state);
        invalidateAppliedState(); // per-pass alpha tests bypass the filter
        if(paired){
            if(mesh){++alphaUnionCounts.meshes;alphaUnionCounts.meshTriangles+=batch.indices.size()/3;}
            else ++alphaUnionCounts.perTriangle;
            return true;
        }
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
bool submitGxAdditiveTextureGeometry(const gx::RasterPlan& original,const gx::PicaMaterialPlan& base,
    const gx::RasterPlan& fog,const gx::PicaMaterialPlan* fogMaterial,const gx::PicaMaterialPlan& contribution,
    const gx::Texture& baseTexture,const gx::Texture& extraTexture,const gx::GpuGeometryBatch& batch,bool mesh){
    GxProfileScope profile(GxProfilePart::submit);
    ++gxPicaStateSerial;
    if(!ready||!alphaIntervalCopySafe()||(mesh&&!picaRigidTriangleFogAvailable()))
        return reject("PICA additive texture state invalid");
    struct State {
        const gx::RasterPlan& base;
        gx::RasterPlan addition;
        const gx::RasterPlan& fog;
        PreparedPicaMaterial materials[3];
        const gx::Texture& baseTexture;
        const gx::Texture& extraTexture;
        bool baseProjected{},fogEnabled{},mesh{},initialized{},failed{};
    };
    State state{original,fog,fog,{preparePicaMaterial(base),preparePicaMaterial(contribution),{}},
                baseTexture,extraTexture,base.projectiveTexture0,fogMaterial!=nullptr,mesh};
    state.addition.blend={1,1,1,3};
    if(fogMaterial)state.materials[2]=preparePicaMaterial(*fogMaterial);
    auto prepare=[](unsigned pass,void* opaque){
        auto& s=*static_cast<State*>(opaque);
        if(!platformPrepareGpuDraw()){
            s.failed=true;return reject("PICA additive texture command block completion failed");
        }
        if(!s.initialized){
            C3D_Mtx identity;Mtx_Identity(&identity);
            bindProgram(&program);C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,transformLocation,&identity);
            s.initialized=true;
        }
        if(pass<2){
            float scaleU,scaleV;
            if(!bindGxTexture(pass?s.extraTexture:s.baseTexture,scaleU,scaleV,0,!pass&&s.baseProjected)){
                s.failed=true;return reject(gxTextureFailure());
            }
        }
        applyCommandRaster(pass==0?s.base:pass==1?s.addition:s.fog);
        applyPreparedPicaMaterial(s.materials[pass]);
        if(s.mesh){
            const unsigned bits=pass==2?additiveFogStencilBit:triangleFogStencilBit|
                (!pass&&s.fogEnabled?additiveFogStencilBit:0);
            C3D_StencilTest(true,pass?GPU_EQUAL:GPU_ALWAYS,bits,bits,bits);
            C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,pass?GPU_STENCIL_ZERO:GPU_STENCIL_REPLACE);
        }
        ++drawCount;
        return true;
    };
    const bool submitted=submitGxTrianglePasses(batch,fogMaterial!=nullptr,mesh,prepare,&state);
    if(mesh)disableStencil();
    invalidateAppliedState();
    if(submitted||state.failed)return submitted;
    return reject(gxGeometryFailure());
}
bool submitGxRigidGeometry(const gx::RasterPlan& p,const gx::PicaMaterialPlan& material,const gx::GpuGeometryBatch& batch,const gx::RigidVertexPlan& rigid){
    GxProfileScope profile(GxProfilePart::submit);
    ++gxPicaStateSerial;
    if(batch.qPolicy!=gx::GeometryQPolicy::positiveOnly)return reject("PICA signed-q rigid unsupported");
    const bool unionMesh=p.alphaUnion&&picaAlphaUnionMesh(p);
    if(!ready||(p.alphaUnion&&!unionMesh)||p.alphaInterval.enabled||!alphaIntervalCopySafe()||!rigid.lights||rigid.lights>8)return reject("PICA rigid state invalid");
    // Profiled frames: the split of a rigid submission (immediate replays and
    // complete draws; native shape replays have submit.*).
    {static const auto i=registerGxNamedProfile("rigid.prepare");GxNamedProfileScope scope(i);
    if(!platformPrepareGpuDraw())return reject("PICA command block completion failed");}
    const bool mt=rigid.multiTexture;
    if(mt&&!rigid.compact)return reject("PICA multi-texture requires compact records");
    const auto chosen=rigidProgramFor(rigid,false);
    const bool fresh=uniformOwner!=chosen.layout;
    bindProgram(chosen.program);uniformOwner=chosen.layout;
    const int uniforms=mt?rigidMtUniforms:rigidUniforms,loop=mt?rigidMtLoop:rigidLoop;
    const unsigned count=rigid.uniformCount();
    if(!rigid.matrices||rigid.matrices>(rigid.skinEntries?skin::maxMatrices:10)||count>rigid.uniforms.size()||(mt&&count>rigid.paletteLimit()))
        return reject("PICA rigid palette invalid");
    {static const auto i=registerGxNamedProfile("rigid.uniforms");GxNamedProfileScope scope(i);
    uploadRigidUniforms(rigid,uniforms,loop,count,mt,fresh);}
    // An alpha union draws with its first test here (the raster's `alpha`).
    {static const auto i=registerGxNamedProfile("rigid.state");GxNamedProfileScope scope(i);
    applyCommandState(p,material);}
    {static const auto i=registerGxNamedProfile("rigid.draw");GxNamedProfileScope scope(i);
    if(!(rigid.compact?submitGxCompactRigidGeometry(batch,rigid.staticVertices,mt,mt&&rigid.wideRecord):submitGxGeometry(batch,false))){
        if(unionMesh)invalidateAppliedState();
        return reject(gxGeometryFailure());
    }
    if(unionMesh&&!drawAlphaUnionSecond(p,[&]{redrawGxGeometry(batch);return true;}))return false;}
    if(mt)countVertexPlan(chosen,rigid,unsigned(batch.indices.size())*(unionMesh?2:1),
                          rigid.staticVertices?static_cast<const void*>(rigid.staticVertices):batch.vertices.data(),batch.indices.data());
    // Wide records are left out of the caster pass (it draws 32-byte records).
    if(casterSlot&&rigid.compact&&!rigid.wideRecord)
        rememberCaster(rigid,rigid.staticVertices?rigid.staticVertices:static_cast<const void*>(batch.vertices.data()),
                       batch.indices.data(),unsigned(batch.indices.size()),casterSlot,batch.vertices.size_bytes());
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
    const auto chosen=rigidProgramFor(rigid,false);
    const int uniforms=mt?rigidMtUniforms:rigidUniforms,loop=mt?rigidMtLoop:rigidLoop;
    const bool fresh=uniformOwner!=chosen.layout;
    bindProgram(chosen.program);uniformOwner=chosen.layout;
    uploadRigidUniforms(rigid,uniforms,loop,count,mt,fresh);
    constexpr unsigned bit=triangleFogStencilBit;
    // Original pass: mark the pixels this mesh wins.
    applyCommandRaster(p);applyPicaMaterial(material);
    C3D_StencilTest(true,GPU_ALWAYS,bit,bit,bit);
    C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_REPLACE);
    unsigned visibleIndices=batch.indices.size();
    // The fog plan copies the base position palette and projection. Validated
    // resident compact records can share an ordered visible prefix for both draws.
    const bool filter=cullOn&&mt&&rigid.compact&&rigid.staticVertices&&rigid.matrices==1&&
        !std::memcmp(rigid.uniforms.data(),fogRigid.uniforms.data(),10*sizeof(rigid.uniforms[0]));
    if(!(filter?submitGxCompactFogGeometry(batch,rigid,visibleIndices):
         rigid.compact?submitGxCompactRigidGeometry(batch,rigid.staticVertices,mt,mt&&rigid.wideRecord):submitGxGeometry(batch,false))){
        disableStencil();invalidateAppliedState();return reject(gxGeometryFailure());
    }
    const void* const recordVertices=rigid.staticVertices?static_cast<const void*>(rigid.staticVertices):batch.vertices.data();
    if(mt)countVertexPlan(chosen,rigid,visibleIndices,recordVertices,batch.indices.data());
    // Fog pass: equal depth and the mark, cleared on use (one fog per pixel).
    bool okay=platformPrepareGpuDraw();
    if(okay){
        // This path supplies only alpha from the vertex shader. Its color is
        // a TEV constant and the one active stage references no texture.
        constexpr int fogOnlyBool=0x68; // PICA b0, SDK bool uniform base
        const auto& stage=fogMaterial.stages[0];
        const bool fogOnly=mt&&fogMaterial.stageCount==1&&fogMaterial.textureCount==0&&
            stage.rgb.function==gx::MaterialFunction::replace&&
            stage.rgb.inputs[0].source==gx::MaterialSource::constant&&
            stage.rgb.inputs[0].operand==gx::MaterialOperand::color&&
            stage.alpha.function==gx::MaterialFunction::replace&&
            stage.alpha.inputs[0].source==gx::MaterialSource::primary&&
            stage.alpha.inputs[0].operand==gx::MaterialOperand::alpha&&
            fogRigid.uniforms[12][0]==1&&fogRigid.uniforms[12][2]==1;
        // The fog pass has its own plan: its own program (b0 runs only in the general one).
        const auto fogChosen=rigidProgramFor(fogRigid,fogOnly);
        bindProgram(fogChosen.program);
        uploadRigidUniforms(fogRigid,uniforms,loop,count,mt,false);
        applyCommandRaster(fogRaster);applyPicaMaterial(fogMaterial);
        C3D_StencilTest(true,GPU_EQUAL,bit,bit,bit);
        C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_ZERO);
        if(fogOnly)C3D_BoolUnifSet(GPU_VERTEX_SHADER,fogOnlyBool,true);
        if(visibleIndices){
            auto visible=batch;
            visible.indices=batch.indices.first(visibleIndices);
            redrawGxGeometry(visible);
            if(mt)countVertexPlan(fogChosen,fogRigid,visibleIndices,recordVertices,batch.indices.data());
        }
        if(fogOnly){
            C3D_BoolUnifSet(GPU_VERTEX_SHADER,fogOnlyBool,false);
            // Raw replay can follow without passing through Citro3D's context update.
            C3D_UpdateUniforms(GPU_VERTEX_SHADER);
        }
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
// `caster`: the model shadow slot the shape was issued under (0 none). It is
// a parameter and not the slot selected last: a shape replayed by the
// second-core worker runs while the game thread selects slots for later ones.
// Native PICA blocks (runtime/pica_blocks.hpp): the capture of the next
// submitNativeRigid and the replay of a kept block. Citro3D 1.7.1 internals
// (tape_context.hpp checks the context layout): the state groups, the
// program pointer after the flags, and its own context update.
extern "C" void C3Di_UpdateContext(void);
namespace {
constexpr u32 c3dDrawUsed=BIT(1),c3dFrameBuf=BIT(5),c3dVshCode=BIT(11),c3dGshCode=BIT(12);
// Every group a native rigid draw sets (C3DiF_*): AttrInfo BufInfo Effect
// Viewport Scissor Program TexEnvBuf LightEnv TexAll TexEnvAll. Not the
// framebuffer, the shader code, fog/gas lookup tables or procedural textures.
constexpr u32 blockGroups=BIT(2)|BIT(3)|BIT(4)|BIT(6)|BIT(7)|BIT(8)|BIT(9)|BIT(10)|(7u<<23)|(0x3fu<<26);
static_assert(sizeof(DuskC3DContextHead)==offsetof(DuskC3DContextHead,flags)+sizeof(u32));
// C3D_Context::program follows the flags.
const shaderProgram_s* contextProgram(){
    const auto at=reinterpret_cast<std::uintptr_t>(&__C3D_Context)+sizeof(DuskC3DContextHead);
    return *reinterpret_cast<const shaderProgram_s* const*>(at);
}
struct BlockCapture {
    bool requested{},active{},held{},done{},multiTexture{};
    u32* buffer{};u32 start{},end{};std::uint32_t span{};unsigned uniformBase{};
};
BlockCapture blockCapture;
// Before the words of a block: whatever Citro3D still has to send goes out
// first (framebuffer, shader code, uniform rows), outside the block. Then
// every group the draw uses is marked changed, the program's constants and
// int/bool uniforms reloaded, and this file's filters forgotten, so that the
// draw sends its whole state. Texture units the material does not sample are
// switched off: a block names no image it does not read.
void beginBlockCapture(shaderProgram_s* bound,const gx::PicaMaterialPlan& material){
    auto& c=blockCapture;
    c.active=c.done=false;
    if(!tapeContextUsable())return;
    if(contextProgram()!=bound)C3D_BindProgram(bound);
    boundProgram=bound;
    C3Di_UpdateContext();
    for(unsigned unit=material.textureCount;unit<3;++unit)C3D_TexBind(int(unit),nullptr);
    invalidateGxTextureBindings();
    u32 size{};GPUCMD_GetBuffer(&c.buffer,&size,&c.start);
    c.span=platformGpuSpanSerial();
    platformHoldGpuChunks(true);c.held=true;
    __C3D_Context.flags|=blockGroups;
    C3D_BindProgram(bound);
    uniformOwner=nullptr;appliedState.valid=false;invalidateGxGeometryBindings();
    c.active=true;
}
// The words since beginBlockCapture are one unbroken run of the command
// buffer (no submission, reclaim or canary split in between).
void endBlockCapture(bool drawn,unsigned uniformBase,bool multiTexture){
    auto& c=blockCapture;
    if(c.held){platformHoldGpuChunks(false);c.held=false;}
    if(!c.active)return;
    c.active=false;
    u32* buffer{};u32 size{},offset{};GPUCMD_GetBuffer(&buffer,&size,&offset);
    if(!drawn||buffer!=c.buffer||offset<=c.start||platformGpuSpanSerial()!=c.span)return;
    c.end=offset;c.uniformBase=uniformBase;c.multiTexture=multiTexture;c.done=true;
}
// After words Citro3D did not send: its record of the hardware state no
// longer holds for those groups or for any uniform, so the next draw through
// it sends them whole (as after its own APT restore). The program and the
// framebuffer are unchanged; a draw was made into the framebuffer.
void blockReplayed(){
    __C3D_Context.flags|=blockGroups|c3dDrawUsed;
    C3Di_DirtyUniforms(GPU_VERTEX_SHADER);
    for(auto& dirty:C3D_IVUnifDirty[GPU_VERTEX_SHADER])dirty=true; // Citro3D 1.7.1 never marks int uniforms ever-dirty
    C3D_BoolUnifsDirty[GPU_VERTEX_SHADER]=true;
    ++gxPicaStateSerial;
}
}
void requestPicaBlockCapture(){blockCapture.requested=true;blockCapture.done=false;}
void cancelPicaBlockCapture(){blockCapture.requested=false;blockCapture.done=false;}
bool takePicaBlockCapture(PicaBlockCapture& out){
    auto& c=blockCapture;
    if(!c.done)return false;
    c.done=false;
    u32* buffer{};u32 size{},offset{};GPUCMD_GetBuffer(&buffer,&size,&offset);
    if(buffer!=c.buffer||offset<c.end||platformGpuSpanSerial()!=c.span)return false;
    out={c.buffer+c.start,c.end-c.start,c.uniformBase,c.multiTexture};
    return true;
}
// Off-screen culling (gx_pica.hpp setGxCulling). Drawing-side state: only
// submitNativeRigid reaches the cache, on the thread that draws.
namespace {
namespace residentCull = gx::resident_cull;
constexpr unsigned cullEntryCapacity=2048,cullBlockCapacity=8192,cullRefinementCapacity=16384;
constexpr unsigned cullIndices=residentCull::coarseIndices;
// Position matrix row -1 indicates a mixed palette. Refinement stores
// either ordered sub-blocks of one row or one bound per row of the list.
struct CullEntry {
    const void* vertices;
    const std::uint16_t* indices;
    unsigned count,first,blocks;
    std::int32_t row;
    std::uint32_t refinementFirst,refinementCount;
};
CullEntry* cullEntries{};
residentCull::Box* cullBlocks{};
residentCull::MatrixBox* cullRefinement{};
unsigned cullBlocksUsed{},cullRefinementUsed{},refinementCapacity{};
std::uint32_t cullVertexEpoch{};
constexpr unsigned edgeRefinementCapacity=2048;
constexpr unsigned minEdgeIndices=8*residentCull::coarseIndices;
std::uint32_t cullEpoch{};
bool cullRefine{},cullUnavailable{};
// Counters for reportGxCulling (game thread): atomics, written by the drawing side.
std::atomic<std::uint32_t> cullDraws{},cullDrawsWhole{},cullIndicesSeen{},cullIndicesCut{},cullDrawCalls{},cullMisses{},cullFull{};
void prepareCullRefinement(CullEntry& entry) {
    if (!cullRefine && (entry.row < 0 || entry.count < minEdgeIndices)) return;
    if (!refinementCapacity) {
        refinementCapacity = cullRefine ? cullRefinementCapacity : edgeRefinementCapacity;
        cullRefinement = static_cast<residentCull::MatrixBox*>(std::malloc(refinementCapacity*sizeof(residentCull::MatrixBox)));
    }
    if (!cullRefinement) return;
    constexpr unsigned stride = sizeof(gx::CompactRigidVertex);
    constexpr unsigned matrixOffset = offsetof(gx::CompactRigidVertex, matrix);
    static_assert(stride == sizeof(gx::CompactRigidVertexMT) && matrixOffset == offsetof(gx::CompactRigidVertexMT, matrix));
    const auto* base = static_cast<const std::uint8_t*>(entry.vertices);
    auto* boxes = cullRefinement + cullRefinementUsed;
    const unsigned remaining = refinementCapacity - cullRefinementUsed;
    unsigned written = 0;
    if (entry.row >= 0) {
        if (entry.count < residentCull::minFineIndices) return;
        const unsigned fineCount = (entry.count + residentCull::fineIndices - 1) / residentCull::fineIndices;
        if (fineCount >= remaining) return;
        residentCull::Bounds list;
        written = 1; // Whole bounds followed by the sub-blocks in draw order.
        for (unsigned first = 0; first < entry.count; first += residentCull::fineIndices) {
            residentCull::Bounds bounds;
            const unsigned end = std::min(first + residentCull::fineIndices, entry.count);
            for (unsigned i = first; i < end; ++i) {
                float position[3];
                std::memcpy(position, base + std::size_t(entry.indices[i]) * stride, sizeof(position));
                if (!bounds.add(position)) return;
            }
            list.include(bounds);
            boxes[written++] = {bounds.box(), unsigned(entry.row)};
        }
        boxes[0] = {list.box(), unsigned(entry.row)};
    } else {
        if (entry.count < residentCull::minMultiIndices) return;
        std::array<residentCull::Bounds, 10> bounds;
        std::array<unsigned, 10> rows;
        for (unsigned i = 0; i < entry.count; ++i) {
            const auto* vertex = base + std::size_t(entry.indices[i]) * stride;
            const unsigned row = vertex[matrixOffset];
            const auto rowEnd = rows.begin() + written;
            const auto found = std::find(rows.begin(), rowEnd, row);
            const unsigned slot = unsigned(found - rows.begin());
            if (found == rowEnd) {
                if (written == rows.size() || written == remaining) return;
                rows[written++] = row;
            }
            float position[3];
            std::memcpy(position, vertex, sizeof(position));
            if (!bounds[slot].add(position)) return;
        }
        for (unsigned i = 0; i < written; ++i) boxes[i] = {bounds[i].box(), rows[i]};
    }
    entry.refinementFirst = cullRefinementUsed;
    entry.refinementCount = written;
    cullRefinementUsed += written;
}
CullEntry* cullEntry(const void* vertices,const std::uint16_t* indices,unsigned count,unsigned vertexBytes){
    if(cullUnavailable)return nullptr;
    if(!cullEntries){
        cullEntries=static_cast<CullEntry*>(std::calloc(cullEntryCapacity,sizeof(CullEntry)));
        cullBlocks=static_cast<residentCull::Box*>(std::malloc(cullBlockCapacity*sizeof(residentCull::Box)));
        if(!cullEntries||!cullBlocks){std::free(cullEntries);std::free(cullBlocks);cullEntries=nullptr;cullBlocks=nullptr;cullUnavailable=true;return nullptr;}
        cullEpoch=residentIndexEpoch();
    }
    // A new fill of the resident index pool: every list may be another one.
    const auto* vertexCache=gx::staticCompactCache();
    const auto vertexEpoch=vertexCache?vertexCache->epoch:0;
    if(const auto epoch=residentIndexEpoch();epoch!=cullEpoch||vertexEpoch!=cullVertexEpoch){
        cullVertexEpoch=vertexEpoch;
        std::memset(cullEntries,0,cullEntryCapacity*sizeof(CullEntry));cullBlocksUsed=0;cullRefinementUsed=0;cullEpoch=epoch;
    }
    unsigned slot=unsigned((reinterpret_cast<std::uintptr_t>(indices)>>4)*2654435761u)%cullEntryCapacity;
    for(unsigned probe=0;probe<16;++probe,slot=(slot+1)%cullEntryCapacity){
        auto& e=cullEntries[slot];
        if(e.indices==indices&&e.vertices==vertices&&e.count==count)return &e;
        if(e.indices)continue;
        // A list seen for the first time: its blocks, read from the resident
        // records (position at 0, position matrix row at 26, gx_gpu_geometry.hpp).
        const unsigned blocks=(count+cullIndices-1)/cullIndices;
        if(blocks>cullBlockCapacity-cullBlocksUsed){cullFull.fetch_add(1,std::memory_order_relaxed);return nullptr;}
        cullMisses.fetch_add(1,std::memory_order_relaxed);
        const auto* base=static_cast<const std::uint8_t*>(vertices);
        std::int32_t row=-2;
        for(unsigned b=0;b<blocks;++b){
            float low[3]{1e30f,1e30f,1e30f},high[3]{-1e30f,-1e30f,-1e30f};
            const unsigned end=std::min(count,(b+1)*cullIndices);
            for(unsigned n=b*cullIndices;n<end;++n){
                const auto* record=base+std::size_t(indices[n])*vertexBytes;
                float position[3];std::memcpy(position,record,sizeof(position));
                for(unsigned c=0;c<3;++c){low[c]=std::min(low[c],position[c]);high[c]=std::max(high[c],position[c]);}
                const std::int32_t matrix=static_cast<std::int8_t>(record[26]);
                if(row==-2)row=matrix;else if(row!=matrix)row=-1;
            }
            auto& block=cullBlocks[cullBlocksUsed+b];
            for(unsigned c=0;c<3;++c){
                // A non-finite position is never culled (the box covers everything).
                if(!(std::isfinite(low[c])&&std::isfinite(high[c]))){low[c]=-1e30f;high[c]=1e30f;row=-1;}
                block.center[c]=0.5f*(low[c]+high[c]);block.extent[c]=0.5f*(high[c]-low[c]);
            }
        }
        e={vertices,indices,count,cullBlocksUsed,blocks,row<0?-1:row,0,0};cullBlocksUsed+=blocks;
        prepareCullRefinement(e);
        return &e;
    }
    cullFull.fetch_add(1,std::memory_order_relaxed);
    return nullptr;
}
// The runs of blocks to draw, as [first index, index count) pairs. False:
// draw the whole list. The planes are those of the PICA clip volume
// (-w <= x,y <= w, -w <= z <= 0), outside meaning every vertex of the block
// is beyond one of them (the far plane is not used). A multi-texture draw
// whose clip z its program clamps (u[10].w = 1, GX clip off: the sky) is
// tested against x and y only; in the single-texture program u[10] is the
// ambient colour and z is never clamped. The box must be outside by a margin
// of 1/1024 of the magnitudes involved (the PICA computes in 24-bit floats).
bool cullCoarseRuns(const gx::RigidVertexPlan& rigid,bool multiTexture,const CullEntry& e,residentCull::Runs& runs){
    if(e.row<0||unsigned(e.row)+3>rigid.uniforms.size())return false;
    const auto& u=rigid.uniforms;
    // Clip rows C = P * [M; 0 0 0 1].
    float clip[4][4];
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j){
        float sum=j==3?u[6+i][3]:0.0f;
        for(unsigned k=0;k<3;++k)sum+=u[6+i][k]*u[unsigned(e.row)+k][j];
        clip[i][j]=sum;
    }
    const bool depthClamped=multiTexture&&u[10][3]>0;
    float planes[5][4];unsigned planeCount=0;
    for(unsigned axis=0;axis<2;++axis)for(const float sign:{1.0f,-1.0f}){
        for(unsigned j=0;j<4;++j)planes[planeCount][j]=sign*clip[axis][j]-clip[3][j];
        ++planeCount;
    }
    if(!depthClamped){for(unsigned j=0;j<4;++j)planes[planeCount][j]=clip[2][j];++planeCount;}
    for(unsigned q=0;q<planeCount;++q)for(const float value:planes[q])if(!std::isfinite(value))return false;
    for(unsigned b=0;b<e.blocks;++b){
        const auto& block=cullBlocks[e.first+b];
        bool outside=false;
        for(unsigned q=0;q<planeCount&&!outside;++q){
            const float* r=planes[q];
            const float centre=r[0]*block.center[0]+r[1]*block.center[1]+r[2]*block.center[2]+r[3];
            const float reach=std::fabs(r[0])*block.extent[0]+std::fabs(r[1])*block.extent[1]+std::fabs(r[2])*block.extent[2];
            const float scale=std::fabs(r[0]*block.center[0])+std::fabs(r[1]*block.center[1])+std::fabs(r[2]*block.center[2])+std::fabs(r[3])+reach;
            outside=centre-reach>scale*(1.0f/1024.0f);
        }
        const unsigned first=b*cullIndices;
        if (!outside && !runs.add(first,std::min(e.count,first+cullIndices))) return false;
    }
    return true;
}
// A row the draw placed a matrix at (slot palette or merged skin palette).
bool placedRow(const gx::RigidVertexPlan& rigid, unsigned row) {
    if (rigid.skinEntries)
        return (row < 6 && row % 3 == 0 && row / 3 < rigid.skinEntries) ||
               (row >= skin::paletteRow && (row - skin::paletteRow) % 3 == 0 && 2 + (row - skin::paletteRow) / 3 < rigid.skinEntries);
    return std::find(rigid.matrixOffsets.begin(), rigid.matrixOffsets.end(), std::int8_t(row)) != rigid.matrixOffsets.end();
}
bool cullRuns(const gx::RigidVertexPlan& rigid, bool multiTexture, const CullEntry& entry, residentCull::Runs& runs) {
    if (cullRefine && entry.refinementCount) {
        const std::span boxes(cullRefinement + entry.refinementFirst, entry.refinementCount);
        const unsigned mask = multiTexture && rigid.uniforms[10][3] > 0 ?
            residentCull::sidePlanes : residentCull::allPlanes;
        if (entry.row < 0) {
            for (const auto& box : boxes)
                if (box.row >= rigid.uniforms.size() || !placedRow(rigid, unsigned(box.row)))
                    return false;
            return residentCull::outsidePalette(rigid.uniforms, boxes, mask);
        }
        residentCull::Planes planes;
        if (residentCull::makePlanes(rigid.uniforms, unsigned(entry.row), mask, planes) &&
            residentCull::refine(planes, mask, boxes.front().box, {cullBlocks + entry.first, entry.blocks}, boxes.subspan(1), entry.count, runs))
            return true;
        runs.size = runs.indices = 0;
    }
    if (!cullCoarseRuns(rigid,multiTexture,entry,runs)) return false;
    // Tighten the ends of each already ordered coarse run. Interior holes
    // remain in their draw, so this refinement never adds an index upload or a draw.
    if (!cullRefine && entry.refinementCount && runs.indices >= residentCull::minFineIndices) {
        const auto* fine=cullRefinement+entry.refinementFirst+1;
        residentCull::Planes planes;
        if (residentCull::makePlanes(rigid.uniforms, unsigned(entry.row), residentCull::sidePlanes, planes)) {
            const unsigned n=runs.size;
            runs.size=runs.indices=0;
            for (unsigned i=0;i<n;++i) {
                unsigned first=runs.ranges[i].first;
                unsigned end=first+runs.ranges[i].count;
                while (first<end && residentCull::classify(planes,fine[first/residentCull::fineIndices].box,residentCull::sidePlanes).outside)
                    first=std::min(end,first+residentCull::fineIndices);
                while (first<end && residentCull::classify(planes,fine[(end-1)/residentCull::fineIndices].box,residentCull::sidePlanes).outside)
                    end=(end-1)/residentCull::fineIndices*residentCull::fineIndices;
                if (first<end) runs.add(first,end);
            }
        }
    }
    return true;
}
}
void setGxCulling(bool on,bool refine){cullOn=on;cullRefine=on&&refine;}
void reportGxCulling(){
    static unsigned pictures;
    if(++pictures<300)return;
    const double n=pictures;pictures=0;
    const auto take=[](std::atomic<std::uint32_t>& value){return value.exchange(0,std::memory_order_relaxed);};
    const auto draws=take(cullDraws),whole=take(cullDrawsWhole),seen=take(cullIndicesSeen),cut=take(cullIndicesCut);
    const auto runs=take(cullDrawCalls),misses=take(cullMisses),full=take(cullFull);
    if(!cullOn&&!draws)return;
    OSReport("Native off-screen culling%s, last %u pictures, per picture: resident draws %.1f (%.1f left out whole), indices %.0f of %.0f left out (%.1f %%), "
        "draw calls %.1f; new lists %lu, cache full %lu\n",cullRefine?" (refined)":cullOn?"":" (off: SD switch)",unsigned(n),draws/n,whole/n,cut/n,seen/n,seen?100.0*cut/seen:0.0,runs/n,
        static_cast<unsigned long>(misses),static_cast<unsigned long>(full));
}
bool submitNativeRigid(const gx::RasterPlan& p,const gx::PicaMaterialPlan& material,const gx::RigidVertexPlan& rigid,const void* vertices,const std::uint16_t* indices,unsigned indexCount,unsigned caster,bool chained,
                       const NativeRigidLevels* levels){
    const unsigned fullCount=indexCount;
    GxProfileScope profile(GxProfilePart::submit);
    // An alpha union leaves its second alpha test behind (state invalidated):
    // it never runs as a continuation, and always applies its state.
    const bool unionMesh=p.alphaUnion&&picaAlphaUnionMesh(p);
    if(unionMesh)chained=false;
    // A block capture sends the whole state (the same state a continuation
    // runs under) and is never an alpha union (two draws).
    const bool capturing=blockCapture.requested&&!unionMesh;
    blockCapture.requested=false;blockCapture.done=false;
    if(!chained)++gxPicaStateSerial;
    if(!ready||(p.alphaUnion&&!unionMesh)||p.alphaInterval.enabled||!alphaIntervalCopySafe()||!rigid.lights||rigid.lights>8)return reject("PICA rigid state invalid");
    const bool mt=rigid.multiTexture;
    if(mt&&!rigid.compact)return reject("PICA multi-texture requires compact records");
    // A block capture keeps the general program: kept blocks replay its words.
    const auto chosen=rigidProgramFor(rigid,false,capturing);
    shaderProgram_s* const bound=chosen.program;
    const int uniforms=mt?rigidMtUniforms:rigidUniforms,loop=mt?rigidMtLoop:rigidLoop;
    const unsigned count=rigid.uniformCount();
    if(!rigid.matrices||rigid.matrices>(rigid.skinEntries?skin::maxMatrices:10)||count>rigid.uniforms.size()||(mt&&count>rigid.paletteLimit()))
        return reject("PICA rigid palette invalid");
    residentCull::Runs runs;
    bool bounded=false;
    // Block recording must retain the full list for replay under other cameras.
    // Alpha unions use the full list twice under different tests.
    if(cullOn&&rigid.compact&&!unionMesh&&!capturing&&indices&&indexCount>=3&&indexCount%3==0){
        static const auto i=registerGxNamedProfile("submit.cull");GxNamedProfileScope scope(i);
        if(const auto* entry=cullEntry(vertices,indices,indexCount,sizeof(gx::CompactRigidVertex)))
            bounded=cullRuns(rigid,mt,*entry,runs);
    }
    // Levels of detail: the scene's list only when it is shorter than what
    // the full list submits after the cull (a block capture keeps the full
    // list); the caster's list in any case.
    const std::uint16_t* casterIndices=indices;unsigned casterIndexCount=indexCount;
    if(levels){
        model_lod::DrawCounters counted;
        const unsigned fullSubmitted=bounded?runs.indices:indexCount;
        if(levels->scene&&!capturing&&levels->sceneCount<fullSubmitted){
            counted.reduced=1;counted.indicesSaved=fullSubmitted-levels->sceneCount;
            indices=levels->scene;indexCount=levels->sceneCount;bounded=false;
            casterIndices=indices;casterIndexCount=indexCount;
        }
        if(levels->caster&&levels->casterCount<casterIndexCount){casterIndices=levels->caster;casterIndexCount=levels->casterCount;}
        if(caster&&casterIndexCount<fullCount){counted.shadowReduced=1;counted.shadowIndicesSaved=fullCount-casterIndexCount;}
        model_lod::countDraw(counted);
    }
    if(bounded&&!runs.size){
        cullDraws.fetch_add(1,std::memory_order_relaxed);cullIndicesSeen.fetch_add(indexCount,std::memory_order_relaxed);
        cullIndicesCut.fetch_add(indexCount,std::memory_order_relaxed);cullDrawsWhole.fetch_add(1,std::memory_order_relaxed);
        // A continuation of this shape must apply its raster and TEV state if
        // the state-changing draw was skipped. Its caller has bound textures.
        if(!chained)appliedState.valid=false;
        // The light may still see a caster outside the scene camera.
        if(caster)rememberCaster(rigid,vertices,casterIndices,casterIndexCount,caster);
        ++drawCount;return true;
    }
    {static const auto i=registerGxNamedProfile("submit.prepare");GxNamedProfileScope scope(i);
    if(!platformPrepareGpuDraw())return reject("PICA command block completion failed");}
    struct CaptureEnd {bool on;bool drawn{};unsigned base;bool mt;~CaptureEnd(){if(on)endBlockCapture(drawn,base,mt);}} captureEnd{capturing,false,unsigned(uniforms),mt};
    if(capturing)beginBlockCapture(bound,material);
    const bool fresh=uniformOwner!=chosen.layout;
    {static const auto i=registerGxNamedProfile("submit.uniforms");GxNamedProfileScope scope(i);
    bindProgram(bound);uniformOwner=chosen.layout;
    uploadRigidUniforms(rigid,uniforms,loop,count,mt,fresh);}
    {static const auto i=registerGxNamedProfile("submit.state");GxNamedProfileScope scope(i);
    // A continuation runs under the state its predecessor applied (see above).
    if(chained&&!capturing&&appliedState.valid)++appliedSkips;else applyCommandState(p,material);}
    static const auto drawIndex=registerGxNamedProfile("submit.draw");GxNamedProfileScope drawScope(drawIndex);
    if(bounded){
        for(unsigned r=0;r<runs.size;++r){
            const auto& run=runs.ranges[r];
            if(!drawResidentRigid(vertices,indices+run.first,run.count,mt))return reject(gxGeometryFailure());
        }
        cullDraws.fetch_add(1,std::memory_order_relaxed);cullIndicesSeen.fetch_add(indexCount,std::memory_order_relaxed);
        cullIndicesCut.fetch_add(indexCount-runs.indices,std::memory_order_relaxed);cullDrawCalls.fetch_add(runs.size,std::memory_order_relaxed);
        if(mt)countVertexPlan(chosen,rigid,runs.indices,vertices,indices+runs.ranges[0].first);
    }else{
        if(!rigid.compact||!drawResidentRigid(vertices,indices,indexCount,mt)){
            if(unionMesh)invalidateAppliedState();
            return reject(gxGeometryFailure());
        }
        captureEnd.drawn=true;
        if(unionMesh&&!drawAlphaUnionSecond(p,[&]{C3D_DrawElements(GPU_TRIANGLES,int(indexCount),C3D_UNSIGNED_SHORT,indices);return true;}))return false;
        if(mt)countVertexPlan(chosen,rigid,indexCount*(unionMesh?2:1),vertices,indices);
    }
    if(caster)rememberCaster(rigid,vertices,casterIndices,casterIndexCount,caster);
    ++drawCount;return true;
}
// A kept block: the program is bound in Citro3D (its code must be on the
// PICA; a pending upload, framebuffer or other change is sent first through
// Citro3D), the words are copied into the command buffer, Citro3D's record
// is marked changed. `palette` gives the caster pass its rows.
bool submitPicaBlock(const std::uint32_t* words,unsigned count,bool multiTexture,const gx::RigidVertexPlan& palette,
                     const void* vertices,const std::uint16_t* indices,unsigned indexCount,unsigned caster){
    GxProfileScope profile(GxProfilePart::submit);
    if(!ready||!words||!count||!alphaIntervalCopySafe()||!tapeContextUsable())return false;
    if(!platformPrepareGpuDraw())return reject("PICA command block completion failed");
    // platformPrepareGpuDraw leaves at least 8192 words; Citro3D's own flush,
    // invalidate and end commands need a few more after the block.
    constexpr u32 margin=1024;
    u32* buffer{};u32 size{},offset{};GPUCMD_GetBuffer(&buffer,&size,&offset);
    if(!buffer||offset>size||size-offset<count+margin)return false;
    shaderProgram_s* const bound=multiTexture?&rigidMtProgram:&rigidProgram;
    if(contextProgram()!=bound)C3D_BindProgram(bound);
    boundProgram=bound;
    if(__C3D_Context.flags&(c3dFrameBuf|c3dVshCode|c3dGshCode)){
        C3Di_UpdateContext();
        GPUCMD_GetBuffer(&buffer,&size,&offset);
        if(!buffer||offset>size||size-offset<count+margin)return false;
    }
    GPUCMD_AddRawCommands(words,count);
    blockReplayed();
    multiProjectedDraw=false;
    if(caster)rememberCaster(palette,vertices,indices,indexCount,caster);
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
    // The copy is dw x dh texels of a dst->width x dst->height texture. GX
    // clamps a sample at the copy's own edge; the PICA clamps at the texture's,
    // so a filtered or warped sample past the last row or column reads the
    // padding, which was never written (stale VRAM: the bright or garbage
    // bottom row under refracting water, Alpha 6 F_SP108 r1). The padding is
    // filled with the edge: the right strip repeats the last column, the
    // bottom strip the last row and the corner the last texel, which is what
    // GX's clamp returns there.
    const bool padRight=dw<dst->width,padBottom=dh<dst->height;
    const unsigned quads=1+unsigned(padRight)+unsigned(padBottom)+unsigned(padRight&&padBottom);
    gx::GpuGeometryBatch batch;
    if(!reserveGxGeometry(4*quads,6*quads,batch)){platformBindFramebuffer(activeFramebuffer());return captureFail(11);}
    // Destination texel corner (ci,cj) sampled at the source point of the
    // copy's texel coordinates (si,sj). Destination: GX row j is memory row j
    // of the cached texture (t=1 at row 0).
    auto corner=[&](unsigned n,float ci,float cj,float si,float sj){
        auto& v=batch.vertices[n];v={};
        float x=ci/dst->width*2-1,y=(1-cj/dst->height)*2-1;
        if(orient&1)x=-x;
        if(orient&2)y=-y;
        v.position={x,y,-0.5f,1};v.color={1,1,1,1};
        // GX destination texel corner -> logical EFB point -> 3DS screen pixel.
        const float lx=plan.left+si*plan.sourceWidth/dw,ly=plan.top+sj*plan.sourceHeight/dh;
        const float px=lx*e.pixelWidth/e.logicalWidth,py=ly*e.pixelHeight/e.logicalHeight;
        // Portrait framebuffer: memory row = screen x, memory column = height-1-screen y.
        float ss=(float(fb.width)-py)/sw,tt=1-px/sh;
        if(orient&4)tt=px/sh;
        if(orient&8)ss=py/sw;
        v.texcoord[0]={ss,tt};
    };
    // One quad from (c0,r0) to (c1,r1) in destination texels. A strip past the
    // copy samples at the centre of the copy's last column or row. The first
    // quad is the copy itself, with the same vertices as before the padding.
    const float fw=float(dw),fh=float(dh),tw=float(dst->width),th=float(dst->height);
    unsigned quad=0;
    auto rectangle=[&](float c0,float r0,float c1,float r1){
        const float cs[4]{c0,c1,c1,c0},rs[4]{r0,r0,r1,r1};
        for(unsigned k=0;k<4;++k)corner(quad*4+k,cs[k],rs[k],c0>=fw?fw-0.5f:cs[k],r0>=fh?fh-0.5f:rs[k]);
        const std::uint16_t first=std::uint16_t(quad*4);
        const std::uint16_t indices[6]{first,std::uint16_t(first+1),std::uint16_t(first+2),first,std::uint16_t(first+2),std::uint16_t(first+3)};
        std::copy(indices,indices+6,batch.indices.begin()+quad*6);++quad;
    };
    rectangle(0,0,fw,fh);
    if(padRight)rectangle(fw,0,tw,fh);
    if(padBottom)rectangle(0,fh,fw,th);
    if(padRight&&padBottom)rectangle(fw,fh,tw,th);
    const bool drawn=submitGxGeometry(batch);
    if(drawn&&quads>1)++capturePaddingFills;
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
bool screenQuad(const std::array<std::array<std::array<float,2>,4>,3>& coordinates){
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
}
#if defined(__3DS__)
bool prepareGxScreenProjectors(const gx::ScreenProjectors& projectors,const std::array<gx::Texture,2>& textures,
                               std::array<float,2>& extent){
    auto* framebuffer=activeFramebuffer();
    if(!ready||!framebuffer||framebuffer!=mainFramebuffer()||!alphaIntervalCopySafe())
        return reject("PICA screen projector target unavailable");
    const auto& fb=*abi::frameBuf(framebuffer);
    const unsigned width=fb.width,height=fb.height;
    constexpr unsigned textureWidth=256,textureHeight=512;
    if(!width||!height||width>textureWidth||height>textureHeight||abi::isBlock32(&fb))
        return reject("PICA screen projector extent unsupported");
    const auto sampler=[](const gx::Texture& t){return (unsigned(t.wrapS)<<4)|(unsigned(t.wrapT)<<2)|t.minFilter;};
    const ScreenProjectorKey key{projectors.corners,{textures[0].handle,textures[1].handle},
                                 {sampler(textures[0]),sampler(textures[1])},width,height};
    const auto bind=[&]{
        C3D_TexBind(0,&screenProjectorTexture);
        invalidateGxTextureBindings();
        extent={float(width)/textureWidth,float(height)/textureHeight};
        return true;
    };
    // Each matrix group can reuse the same bytes. Both sources are immutable,
    // and the whole viewport is filled independently of the draw's geometry.
    if(screenProjectorKey==key)return bind();
    if(!screenProjectorTarget){
        if(!C3D_TexInitWithParams(&screenProjectorTexture,nullptr,
                                  {textureWidth,textureHeight,0,GPU_RGBA8,GPU_TEX_2D,true}))
            return reject("PICA screen projector VRAM allocation failed");
        screenProjectorTarget=C3D_RenderTargetCreateFromTex(&screenProjectorTexture,GPU_TEXFACE_2D,0,C3D_DEPTHTYPE(-1));
        if(!screenProjectorTarget){
            C3D_TexDelete(&screenProjectorTexture);screenProjectorTexture={};
            return reject("PICA screen projector target allocation failed");
        }
        C3D_TexSetFilter(&screenProjectorTexture,GPU_NEAREST,GPU_NEAREST);
        C3D_TexSetWrap(&screenProjectorTexture,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
        screenProjectorTexture.param=(screenProjectorTexture.param&~GPU_TEXTURE_MODE(7))|GPU_TEXTURE_MODE(GPU_TEX_PROJECTION);
    }
    GxTextureDrawScope sourceTextures(textures);
    if(!sourceTextures)return reject(gxTextureFailure());
    screenProjectorKey={}; // A failed pass may have overwritten the image.
    if(!platformPrepareGpuDraw()||!C3D_FrameDrawOn(screenProjectorTarget))
        return reject("PICA screen projector target bind failed");
    invalidatePicaCommandState();
    const bool drawn=[&]{
        std::array<std::array<std::array<float,2>,4>,3> coordinates{};
        for(unsigned i=0;i<2;++i){
            float u{},v{};
            if(!bindGxTexture(textures[i],u,v,i)||u!=1||v!=1)return false;
            for(unsigned n=0;n<4;++n)coordinates[i][n]={projectors.corners[i][n][0],1-projectors.corners[i][n][1]};
        }
        disableStencil();
        C3D_FogGasMode(GPU_NO_FOG,GPU_PLAIN_DENSITY,false);
        C3D_SetViewport(0,0,width,height);
        C3D_SetScissor(GPU_SCISSOR_DISABLE,0,0,0,0);
        C3D_CullFace(GPU_CULL_NONE);
        C3D_DepthMap(true,-1,0);
        C3D_EarlyDepthTest(false,GPU_EARLYDEPTH_GREATER,0);
        C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);
        C3D_AlphaTest(false,GPU_ALWAYS,0);
        C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
        C3D_TexEnvBufUpdate(C3D_Both,0);
        for(int n=0;n<6;++n)C3D_TexEnvInit(C3D_GetTexEnv(n));
        auto* env=C3D_GetTexEnv(0);
        C3D_TexEnvSrc(env,C3D_RGB,GPU_TEXTURE0,GPU_PRIMARY_COLOR,GPU_PRIMARY_COLOR);
        C3D_TexEnvSrc(env,C3D_Alpha,GPU_TEXTURE1,GPU_PRIMARY_COLOR,GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env,C3D_Both,GPU_REPLACE);
        return screenQuad(coordinates);
    }();
    if(!platformBindFramebuffer(framebuffer)||!drawn)return reject("PICA screen projector pass failed");
    // Target switches flush the previous framebuffer. Binding the completed
    // target for sampling also clears the PICA texture cache.
    screenProjectorKey=key;
    return bind();
}
#endif
namespace {
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
namespace {
// Bloom passes as queued jobs (gx_worker_sync.hpp gxJobBloom). The game is
// only told whether a pass was issued (a counter): once the surfaces exist a
// pass is queued behind the draws it copies, and the thread that draws runs
// it in its place. The surfaces are created by a pass run on the game thread
// after a sync (the first one), and `bloom` is written only there.
bool bloomJobs{true};
std::atomic<bool> bloomProven{};
std::uint64_t bloomQueued{};std::atomic<std::uint64_t> bloomJobFailures{};
bool runBloom(const BloomSettings& settings);
// The game thread's pass: every draw issued so far is drawn first (a sync),
// then the pass runs here. Its own function: the shared-state audit takes it
// as a barrier (Tools/3ds/shared_state_roots.tsv).
[[gnu::noinline]] bool drawGxBloomNow(const BloomSettings& settings){
    syncGxWorker();
    return runBloom(settings);
}
}
void setGxBloomJobs(bool on){bloomJobs=on;}
void gxBloomJobStatistics(std::uint64_t& queued,std::uint64_t& failed){queued=bloomQueued;failed=bloomJobFailures.load(std::memory_order_relaxed);}
void runGxBloomJob(const void* arguments,std::size_t bytes){
    BloomSettings settings;
    if(bytes!=sizeof(settings))return;
    std::memcpy(static_cast<void*>(&settings),arguments,sizeof(settings));
    if(!runBloom(settings)){bloomProven.store(false,std::memory_order_relaxed);bloomJobFailures.fetch_add(1,std::memory_order_relaxed);}
}
bool drawGxBloom(const BloomSettings& settings){
    if(!settings.glow&&!settings.mono[3]&&!settings.haze)return false;
    // Queued after a pass on the game thread succeeded (its surfaces exist);
    // one that fails later puts the passes back on the game thread.
    static_assert(std::is_trivially_copyable_v<BloomSettings>);
    if(bloomJobs&&bloomProven.load(std::memory_order_relaxed)&&queueGxJob(gxJobBloom,&settings,sizeof(settings))){++bloomQueued;return true;}
    // The frame it copies holds every draw issued so far (gx_worker_sync.hpp).
    const bool drawn=drawGxBloomNow(settings);
    bloomProven.store(drawn,std::memory_order_relaxed);
    return drawn;
}
namespace {
// The thread that draws: every draw issued before the pass has been drawn.
bool runBloom(const BloomSettings& settings){
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
        okay=screenQuad(coordinates);
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
            okay=screenQuad(coordinates);
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
        okay=screenQuad(coordinates)&&platformPrepareGpuDraw();
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
        okay=screenQuad(coordinates);
    }
    invalidatePicaCommandState();
    return okay;
}
}
bool gpuRenderDestination(const void* identity,gx::TextureHandle& handle); // gx_copy_execution.cpp
// The thread that draws: the shapes queued before the start were drawn, and
// what they remembered is forgotten.
void runGxShadowArmJob(){casterDrawCount=casterRowCount=0;}
namespace {
bool shadowArmJobs{true};
// The game thread's start: a sync, then the start here (a barrier of the
// shared-state audit, as drawGxBloomNow).
[[gnu::noinline]] void armGxShadowCastersNow(){
    syncGxWorker();
    runGxShadowArmJob();
}
}
void setGxShadowArmJobs(bool on){shadowArmJobs=on;}
void armGxShadowCasters(){
    // The game thread's side: shapes issued from here on belong to their
    // caster (selectGxShadowCaster reads it on this thread only).
    casterArmed=ready;casterSlot=0;
    // The list starts again behind every shape issued so far: in the queue,
    // or here once they are drawn (gx_worker_sync.hpp). The painter arms
    // before its 3D lists.
    const unsigned char none=0;
    if(shadowArmJobs&&queueGxJob(gxJobShadowArm,&none,sizeof(none)))return;
    armGxShadowCastersNow();
}
void selectGxShadowCaster(unsigned slot){casterSlot=casterArmed?slot:0;}
unsigned gxShadowCasterSlot(){return casterSlot;}
void setGxShadowCasterSlot(unsigned slot){casterSlot=slot;}
namespace {
// What the game thread gave drawGxShadowCasters, for a pass run later.
struct CasterPass {
    const void* identity{};
    unsigned width{},height{};
    std::uint8_t count{};
    GpuShadowProfileMode profile{};
    std::array<ShadowCasterView,8> views{};
};
// The job is copied within one process. Its texture identity is a native
// pointer, four bytes on the console and eight in the host renderer tests.
static_assert(sizeof(CasterPass) == (sizeof(void*) == 4 ? 400 : 408));
// A pass is queued only after one succeeded: the game is then told at once
// that it will draw (it does not wait), which is what a pass on the same
// image did last. A queued pass that fails after all leaves the receivers of
// that one picture with the previous shadow image, and the passes go back to
// the game thread until one succeeds again.
std::atomic<bool> casterPassProven{};
std::uint64_t casterPassesQueued{};std::atomic<std::uint64_t> casterPassFailures{};
bool runCasterPass(const void* identity,unsigned width,unsigned height,std::span<const ShadowCasterView> views,
                   GpuShadowProfileMode profile);
}
void runGxShadowCasterJob(const void* arguments,std::size_t bytes){
    CasterPass pass;
    if(bytes!=sizeof(pass))return;
    std::memcpy(&pass,arguments,sizeof(pass));
    if(pass.count>pass.views.size()||!runCasterPass(pass.identity,pass.width,pass.height,{pass.views.data(),pass.count},pass.profile)){
        casterPassProven.store(false,std::memory_order_relaxed);casterPassFailures.fetch_add(1,std::memory_order_relaxed);
    }
}
void gxShadowCasterJobStatistics(std::uint64_t& queued,std::uint64_t& failed){queued=casterPassesQueued;failed=casterPassFailures.load(std::memory_order_relaxed);}
ShadowCasterStatistics takeGxShadowCasterStatistics(){
    const auto result=casterStats;
    casterStats={};
    return result;
}
bool drawGxShadowCasters(const void* identity,unsigned width,unsigned height,std::span<const ShadowCasterView> views){
    // The game thread's side: shapes issued from here on belong to no caster.
    const bool armed=casterArmed;
    casterArmed=false;casterSlot=0;
    // Not armed: no shape was given a caster since the last pass emptied the
    // list, so there is nothing to draw and nothing to empty.
    if(!ready||!armed)return false;
    // The casters are remembered as their shapes are drawn, so the pass comes
    // after every shape issued so far: in the queue behind them, or here once
    // they are drawn (gx_worker_sync.hpp).
    const auto profile=gpuShadowProfileMode;
    if(casterPassProven.load(std::memory_order_relaxed)&&views.size()<=CasterPass{}.views.size()){
        CasterPass pass{identity,width,height,std::uint8_t(views.size()),profile,{}};
        std::copy(views.begin(),views.end(),pass.views.begin());
        if(queueGxJob(gxJobShadowCasters,&pass,sizeof(pass))){++casterPassesQueued;return true;}
    }
    syncGxWorker();
    const bool drawn=runCasterPass(identity,width,height,views,profile);
    casterPassProven.store(drawn,std::memory_order_relaxed);
    return drawn;
}
namespace {
unsigned casterProfilePassTag(unsigned draws,std::span<const ShadowCasterView> views,GpuShadowProfileMode profile){
    bool link=false,other=false;
    for(unsigned n=0;n<draws;++n){
        const auto slot=casterDraws[n].slot;
        if(!slot||slot>views.size()||!views[slot-1].used)continue;
        const auto owner=views[slot-1].owner;
        link|=owner!=ShadowCasterOwner::other;
        other|=owner!=ShadowCasterOwner::link;
    }
    if(!link)return DUSK_SHADOW_GPU_OTHER_PASS;
    if(!other)return DUSK_SHADOW_GPU_LINK_PASS;
    return profile==GpuShadowProfileMode::runs?DUSK_SHADOW_GPU_SHARED_CLEAR:DUSK_SHADOW_GPU_MIXED_PASS;
}
unsigned casterProfileRunTag(ShadowCasterOwner owner){
    switch(owner){
        case ShadowCasterOwner::link:return DUSK_SHADOW_GPU_LINK_RUN;
        case ShadowCasterOwner::other:return DUSK_SHADOW_GPU_OTHER_RUN;
        case ShadowCasterOwner::shared:return DUSK_SHADOW_GPU_SHARED_CASTER;
    }
    return DUSK_SHADOW_GPU_SHARED_CASTER;
}
struct CasterProfileTag {
    unsigned tag{};
    void set(unsigned next){tag=next;platformSetGpuShadowTag(next);}
    ~CasterProfileTag(){if(tag)platformSetGpuShadowTag(0);}
};
// The thread that draws: every shape issued before the pass has been drawn.
bool runCasterPass(const void* identity,unsigned width,unsigned height,std::span<const ShadowCasterView> views,
                   GpuShadowProfileMode profile){
    const unsigned draws=casterDrawCount;
    casterDrawCount=casterRowCount=0;
    if(!activeFramebuffer()||!alphaIntervalCopySafe())return false;
    if(!publishGxCasterGeometry()){++casterStats.publishFailed;return captureFail(27);}
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
    // The pass gets its own command lists: the scene before it is handed to
    // the GPU first, and the GPU guard can tell a stop in the pass from one in
    // the scene (it finds the shadow image as colour target in the list).
    const bool profiling=profile!=GpuShadowProfileMode::off;
    if(!(profiling?platformSubmitGpuShadowBoundary():platformSubmitGpu()))return captureFail(25);
    CasterProfileTag profileTag;
    const unsigned passTag=profiling?casterProfilePassTag(draws,views,profile):0;
    if(profiling)profileTag.set(passTag);
    gpuCanaryShadowTarget(osConvertVirtToPhys(dst->data));
    // Every list queued from here to the end of the pass belongs to it.
    struct PassMark {PassMark(){gpuCanaryShadowPass(true);}~PassMark(){gpuCanaryShadowPass(false);}} passMark;
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
        const bool outerReplaying=gxReplaying;const std::uint16_t outerOwner=gxReplayOwner;
        gxCasterPassNow=true;
        for(unsigned n=0;n<draws&&okay;++n){
            const auto& d=casterDraws[n];
            if(!d.slot||d.slot>views.size()||!views[d.slot-1].used)continue;
            const auto& view=views[d.slot-1];
            if(passTag==DUSK_SHADOW_GPU_SHARED_CLEAR){
                const unsigned next=casterProfileRunTag(view.owner);
                if(next!=profileTag.tag){
                    if(!platformSubmitGpuShadowBoundary()){okay=false;break;}
                    profileTag.set(next);
                }
            }
            // Measured dump: the caster's list carries the owner of its model.
            gxReplaying=true;gxReplayOwner=d.owner;
            if(!platformPrepareGpuDraw()){okay=false;break;}
            if(scissorSlot!=d.slot){
                C3D_SetScissor(GPU_SCISSOR_NORMAL,view.x,height-view.y-view.size,view.x+view.size,height-view.y);
                scissorSlot=d.slot;
            }
            const bool mt=d.mt;multiProjectedDraw=false;
            // Position only (gx_caster.v.pica): the rigid shaders also read
            // normal rows, flags, fog and texture rows this pass never sends.
            bindProgram(&casterProgram);
            const int uniforms=casterUniforms;
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
            else {
                ++drawCount;
                if(d.retained){
                    ++casterStats.retainedReplays;
                    if(d.generation!=gxGeometryArenaGeneration())++casterStats.reclaimedReplays;
                }
            }
        }
        gxReplaying=outerReplaying;gxReplayOwner=outerOwner;
        gxCasterPassNow=false;
    }
    uniformOwner=nullptr;
    // The receivers sample this target next: write the PICA's framebuffer
    // cache back to VRAM before switching (rebinding the textures afterwards
    // clears the texture cache).
    if(okay){GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH,1);GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE,1);}
    if(!(profiling?platformSubmitGpuShadowBoundary():platformSubmitGpu()))okay=false;
    if(profiling)profileTag.set(0);
    // Back to the game's target; this also invalidates every filtered state.
    if(!platformBindFramebuffer(activeFramebuffer())||!okay)return captureFail(26);
    gpuCaptureCommit(handle);++casterStats.passes;
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
}

int gxBoundProgramKind(){
    for(unsigned n=1;n<variantPrograms.size();++n)if(boundProgram==&variantPrograms[n])return 4;
    return boundProgram==&program?0:boundProgram==&rigidProgram?1:boundProgram==&rigidMtProgram?2:boundProgram==&casterProgram?3:-1;
}
bool gxDrawMultiProjected(){return multiProjectedDraw;}
std::uint32_t gxProgramCodeHash(int kind){
    // The variants (4) are programs of the multi-texture binary: its code.
    const DVLB_s* blob=kind==0?shader:kind==1?rigidShader:kind==2||kind==4?rigidMtShader:kind==3?casterShader:nullptr;
    if(!blob||!blob->DVLP.codeData)return 0;
    // Same mixing as the guard's hash of an uploaded program (gpu_canary_ctru.cpp).
    std::uint32_t h=0x811C9DC5u;
    for(u32 i=0;i<blob->DVLP.codeSize;++i){h=(h^blob->DVLP.codeData[i])*0x9E3779B1u;h^=h>>15;}
    return h;
}
}
extern "C" void duskNativeShadowCaster(unsigned slot){dusk::native::selectGxShadowCaster(slot);}
