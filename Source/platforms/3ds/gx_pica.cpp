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
#include <3ds.h>
#include <citro3d.h>
#include <cstring>

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
    C3D_BindProgram(&program);
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
std::uint64_t gxPicaDraws() { return drawCount; }
namespace {
// Native PICA state filter: consecutive draws with an identical raster plan,
// PICA material and target extent do not re-send fixed-function state. Any
// other path that changes Citro3D raster/TEV state invalidates this record.
struct AppliedCommandState {bool valid{};gx::RasterPlan raster;gx::PicaMaterialPlan material;unsigned width{},height{};};
AppliedCommandState appliedState;
std::uint64_t appliedSkips{},appliedApplies{};
void invalidateAppliedState(){appliedState.valid=false;}
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
       fb->colorFmt!=GPU_RB_RGBA8||fb->colorMask!=15||fb->depthFmt!=GPU_RB_DEPTH24_STENCIL8||
       fb->block32||fb->depthMask!=3||fb->width!=gx::currentTargetExtent.pixelHeight||
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
    C3D_BindProgram(&program);C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,transformLocation,&identity);
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
static void uploadRigidUniforms(const gx::RigidVertexPlan& rigid,int uniforms,int loop,unsigned count,bool mt,bool fresh){
    for(unsigned n=0;n<count;++n)setUniformRow(uniforms+n,rigid.uniforms[n],fresh);
    if(mt){for(unsigned n=0;n<14;++n)setUniformRow(uniforms+80+n,rigid.uniforms[80+n],fresh);}
    else if(rigid.compact||rigid.color.ambientVertex)setUniformRow(uniforms+93,rigid.uniforms[93],fresh);
    const C3D_IVec want=IVec_Pack(rigid.lights-1,0,4,0);
    if(fresh||C3D_IVUnif[GPU_VERTEX_SHADER][loop-0x60]!=want)*C3D_IVUnifWritePtr(GPU_VERTEX_SHADER,loop)=want;
}
bool alphaIntervalCopySafe(){return !intervalPoisoned&&!intervalActive;}
void invalidatePicaCommandState(){invalidateAppliedState();uniformOwner=nullptr;invalidateGxTextureBindings();}
void picaUniformStatistics(std::uint64_t& skipped,std::uint64_t& written){skipped=uniformRowsSkipped;written=uniformRowsWritten;}
void picaStateFilterStatistics(std::uint64_t& skips,std::uint64_t& applies){skips=appliedSkips;applies=appliedApplies;}
bool submitStaticGxMesh(const gx::RasterPlan& p,const gx::PicaMaterialPlan& material,unsigned handle,const std::array<float,16>& matrix){
    if(!ready||p.alphaUnion||p.alphaInterval.enabled||!alphaIntervalCopySafe())return reject("PICA static state invalid");
    if(!platformPrepareGpuDraw())return reject("PICA command block completion failed");
    C3D_Mtx transform;
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)transform.r[r].c[c]=matrix[r*4+3-c];
    C3D_BindProgram(&program);C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,transformLocation,&transform);
    applyCommandState(p,material);
    if(!submitStaticGxGeometry(handle))return reject(gxGeometryFailure());
    ++drawCount;return true;
}
bool submitGxCommandGeometry(const gx::RasterPlan& p,const gx::PicaMaterialPlan& material,const gx::GpuGeometryBatch& batch){
    GxProfileScope profile(GxProfilePart::submit);
    if(!ready)return reject("PICA not initialized");
    if(!alphaIntervalCopySafe())return reject("PICA alpha interval scratch unsafe");
    if(batch.qPolicy!=gx::GeometryQPolicy::positiveOnly&&
       (!gx::validGeometryQBinding(batch,material)||p.alphaUnion||p.alphaInterval.enabled||!gx::validateGpuGeometry(batch)))
        return reject("PICA signed-q diagnostic contract invalid");
    if(p.alphaInterval.enabled)return submitInterval(p,material,batch);
    if(!platformPrepareGpuDraw())return reject("PICA command block completion failed");
    C3D_Mtx identity;Mtx_Identity(&identity);
    C3D_BindProgram(&program);C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,transformLocation,&identity);
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
    if(!ready)return reject("PICA not initialized");
    if(batch.qPolicy!=gx::GeometryQPolicy::positiveOnly)return reject("PICA signed-q fog pair unsupported");
    if(original.alphaInterval.enabled||fog.alphaInterval.enabled||!alphaIntervalCopySafe())
        return reject("PICA alpha interval plus fog unsupported/unsafe");
    struct State {const gx::RasterPlan& original;PreparedPicaMaterial material;
                  const gx::RasterPlan& fog;PreparedPicaMaterial fogMaterial;
                  bool initialized{},prepareFailed{};};
    State state{original,preparePicaMaterial(material),fog,preparePicaMaterial(fogMaterial)};
    auto prepare=[](bool fogPass,void* opaque){
        auto& s=*static_cast<State*>(opaque);
        if(!platformPrepareGpuDraw()){
            s.prepareFailed=true;return reject("PICA fog command block completion failed");
        }
        // Both halves use the same shader/identity. Queue completion preserves
        // the current Citro3D state; do not dirty four uniforms per triangle.
        if(!s.initialized){
            C3D_Mtx identity;Mtx_Identity(&identity);
            C3D_BindProgram(&program);C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,transformLocation,&identity);
            s.initialized=true;
        }
        applyCommandRaster(fogPass?s.fog:s.original);
        applyPreparedPicaMaterial(fogPass?s.fogMaterial:s.material);
        ++drawCount;return true;
    };
    const bool paired=submitGxTrianglePairs(batch,prepare,&state);
    invalidateAppliedState(); // both passes apply raster/TEV directly
    if(paired)return true;
    return state.prepareFailed?false:reject(gxGeometryFailure());
}
bool submitGxRigidGeometry(const gx::RasterPlan& p,const gx::PicaMaterialPlan& material,const gx::GpuGeometryBatch& batch,const gx::RigidVertexPlan& rigid){
    GxProfileScope profile(GxProfilePart::submit);
    if(batch.qPolicy!=gx::GeometryQPolicy::positiveOnly)return reject("PICA signed-q rigid unsupported");
    if(!ready||p.alphaUnion||p.alphaInterval.enabled||!alphaIntervalCopySafe()||!rigid.lights||rigid.lights>8)return reject("PICA rigid state invalid");
    if(!platformPrepareGpuDraw())return reject("PICA command block completion failed");
    const bool mt=rigid.multiTexture;
    if(mt&&!rigid.compact)return reject("PICA multi-texture requires compact records");
    shaderProgram_s* const bound=mt?&rigidMtProgram:&rigidProgram;
    const bool fresh=uniformOwner!=bound;
    C3D_BindProgram(bound);uniformOwner=bound;
    const int uniforms=mt?rigidMtUniforms:rigidUniforms,loop=mt?rigidMtLoop:rigidLoop;
    const unsigned count=rigid.uniformCount();
    if(!rigid.matrices||rigid.matrices>10||count>rigid.uniforms.size()||(mt&&count>80))return reject("PICA rigid palette invalid");
    uploadRigidUniforms(rigid,uniforms,loop,count,mt,fresh);
    applyCommandState(p,material);
    if(!(rigid.compact?submitGxCompactRigidGeometry(batch,rigid.staticVertices,mt):submitGxGeometry(batch)))return reject(gxGeometryFailure());
    ++drawCount;return true;
}
// Native shape replay: identical state/uniform setup, persistent buffers.
bool submitNativeRigid(const gx::RasterPlan& p,const gx::PicaMaterialPlan& material,const gx::RigidVertexPlan& rigid,const void* vertices,const std::uint16_t* indices,unsigned indexCount){
    GxProfileScope profile(GxProfilePart::submit);
    if(!ready||p.alphaUnion||p.alphaInterval.enabled||!alphaIntervalCopySafe()||!rigid.lights||rigid.lights>8)return reject("PICA rigid state invalid");
    if(!platformPrepareGpuDraw())return reject("PICA command block completion failed");
    const bool mt=rigid.multiTexture;
    if(mt&&!rigid.compact)return reject("PICA multi-texture requires compact records");
    shaderProgram_s* const bound=mt?&rigidMtProgram:&rigidProgram;
    const bool fresh=uniformOwner!=bound;
    C3D_BindProgram(bound);uniformOwner=bound;
    const int uniforms=mt?rigidMtUniforms:rigidUniforms,loop=mt?rigidMtLoop:rigidLoop;
    const unsigned count=rigid.uniformCount();
    if(!rigid.matrices||rigid.matrices>10||count>rigid.uniforms.size()||(mt&&count>80))return reject("PICA rigid palette invalid");
    uploadRigidUniforms(rigid,uniforms,loop,count,mt,fresh);
    applyCommandState(p,material);
    if(!rigid.compact||!drawResidentRigid(vertices,indices,indexCount,mt))return reject(gxGeometryFailure());
    ++drawCount;return true;
}


// GPU-resident EFB capture (native render-to-texture): the main colour buffer
// is copied tile-for-tile into a scratch texture by the transfer engine, then a
// quad resamples it (rotated: the 3DS framebuffer is portrait) straight into the
// resident RGBA8 texture of the capture image. No readback, encode or decode.
namespace {
C3D_Tex captureScratch{};
C3D_RenderTarget* captureTarget{};C3D_Tex captureVram{};
unsigned pow2(unsigned v){unsigned r=8;while(r<v)r<<=1;return r;}
}
std::uint64_t gpuCaptures;
extern "C" void OSReport(const char*,...);
namespace {bool captureFail(int code){static unsigned reports;if(reports<6){++reports;OSReport("Native GPU capture fallback reason=%d\n",code);}return false;}}
bool gpuCaptureCopy(void* framebuffer,std::uint64_t handle,const ColorCopyPlan& plan,unsigned orient){
    if(!ready||!framebuffer||!alphaIntervalCopySafe()||plan.redChannel||plan.format!=6)return captureFail(1);
    unsigned dw{},dh{};auto* dst=static_cast<C3D_Tex*>(gpuCaptureTexture(handle,dw,dh));
    if(!dst||dw!=plan.width||dh!=plan.height)return captureFail(2);
    auto& fb=static_cast<C3D_RenderTarget*>(framebuffer)->frameBuf;
    const auto& e=plan.extent;
    if(fb.colorFmt!=GPU_RB_RGBA8||fb.block32||(fb.width&7)||(fb.height&7)||fb.width!=e.pixelHeight||fb.height!=e.pixelWidth)return captureFail(3);
    const unsigned sw=pow2(fb.width),sh=pow2(fb.height);
    if(sw>1024||sh>1024)return captureFail(4);
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
    C3D_BindProgram(&program);C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,transformLocation,&identity);
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

}
