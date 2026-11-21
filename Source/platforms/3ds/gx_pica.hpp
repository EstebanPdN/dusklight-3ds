#pragma once
#include <cstdint>
#include <array>
namespace dusk::native::gx { struct RasterPlan; struct PicaMaterialPlan; struct GpuGeometryBatch; struct RigidVertexPlan; struct Texture; struct ScreenProjectors; }
namespace dusk::native {
bool submitStaticGxMesh(const gx::RasterPlan&,const gx::PicaMaterialPlan&,unsigned,const std::array<float,16>&);
bool initializeGxPica();
void destroyGxPica(); // Only after C3D_Fini / completed GPU work.
const char* gxPicaFailure();
// Containment guard only, NOT a fence. Preserve all queue completion, flush and
// cache/lifetime operations. False forbids copy/clear/rebind/reuse/presentation.
bool alphaIntervalCopySafe();
// Forget the last applied raster/TEV state (render target or external change).
void invalidatePicaCommandState();
// A/B switch: triangle fog as two whole-mesh passes (default) or one pair of
// draws per triangle.
void setTriangleFogMeshPass(bool enabled);
// Alpha-union draws ('LEQUAL a OR GEQUAL b' layers) whose blend makes the
// fragment order irrelevant (additive with a destination factor of one, no
// depth write): two whole-mesh passes instead of two draws per triangle.
// True only when that holds and the whole-mesh form is on.
bool picaAlphaUnionMesh(const gx::RasterPlan&);
// The same, and the rigid GPU submits may take the draw (two passes over the
// GPU-transformed mesh) instead of the CPU vertex path.
bool picaAlphaUnionGpu(const gx::RasterPlan&);
// A/B: mesh=false restores one pair of draws per triangle (and the CPU vertex
// path); gpu=false keeps the whole-mesh passes on the CPU vertex path.
void setAlphaUnionPasses(bool mesh,bool gpu);
// Profiled-frame lines: attribute/buffer re-sends saved, alpha-union draws by
// form, frame copies with filled padding. Cumulative counts.
void reportPicaSubmission();
void picaStateFilterStatistics(std::uint64_t& skips,std::uint64_t& applies);
void picaUniformStatistics(std::uint64_t& skipped,std::uint64_t& written);
// Command adapter supplies pretransformed PICA clip positions and fully planned
// draw state. RGB8 destination-alpha blend factors are resolved as constants.
bool submitGxCommandGeometry(const gx::RasterPlan&,const gx::PicaMaterialPlan&,const gx::GpuGeometryBatch&);
// Renders both I4 projectors to a transient texture, then binds it to unit 0.
bool prepareGxScreenProjectors(const gx::ScreenProjectors&,const std::array<gx::Texture,2>&,std::array<float,2>&);
bool submitGxTriangleFogGeometry(const gx::RasterPlan&,const gx::PicaMaterialPlan&,const gx::RasterPlan&,const gx::PicaMaterialPlan&,const gx::GpuGeometryBatch&);
bool submitGxAdditiveTextureGeometry(const gx::RasterPlan& original,const gx::PicaMaterialPlan& base,
    const gx::RasterPlan& fog,const gx::PicaMaterialPlan* fogMaterial,const gx::PicaMaterialPlan& contribution,
    const gx::Texture& baseTexture,const gx::Texture& extraTexture,const gx::GpuGeometryBatch&,bool mesh);
bool submitGxRigidGeometry(const gx::RasterPlan&,const gx::PicaMaterialPlan&,const gx::GpuGeometryBatch&,const gx::RigidVertexPlan&);
// Triangle fog on the GPU: the multi-texture rigid draw, then the same
// geometry again as a fog overlay (the shader writes the fog factor to the
// primary alpha), with the stencil mark of the whole-mesh passes. Available
// only with the whole-mesh switch on and a stencil plane on the target.
bool picaRigidTriangleFogAvailable();
bool submitGxRigidFogGeometry(const gx::RasterPlan&,const gx::PicaMaterialPlan&,const gx::RasterPlan& fogRaster,const gx::PicaMaterialPlan& fogMaterial,
    const gx::GpuGeometryBatch&,const gx::RigidVertexPlan&,const gx::RigidVertexPlan& fogRigid);
// Levels of detail of a record's list (runtime/model_lod.hpp): index lists
// over the same vertices. `scene` is drawn only when it has fewer indices
// than the full list leaves after the off-screen cull, so a level never
// submits more than the draw would without it (and never takes a cull
// entry); `caster` is what the shadow caster replays (casters are not culled).
struct NativeRigidLevels {
    const std::uint16_t* scene{};unsigned sceneCount{};
    const std::uint16_t* caster{};unsigned casterCount{};
};
bool submitNativeRigid(const gx::RasterPlan&,const gx::PicaMaterialPlan&,const gx::RigidVertexPlan&,const void* vertices,const std::uint16_t* indices,unsigned indexCount,unsigned caster,bool chained=false,
                       const NativeRigidLevels* levels=nullptr);
// Off-screen culling of resident rigid draws (Alpha 16): submitNativeRigid
// leaves out the blocks of 64 triangles of a resident index list whose
// bounding box lies wholly outside one plane of the clip volume, under the
// draw's own position matrix and projection. Such triangles produce no
// fragment, so the picture is the same; the PICA no longer runs the vertex
// program for them (the console's cost, about 0.3 us per index in the Alpha 15
// dumps). On by default; sdmc:/3ds/DuskLight/gpu-cull-off.txt turns it off
// (set once at start). gpu-cull-refine.txt enables smaller bounds for large
// lists and whole-list tests across several matrices. reportGxCulling:
// one line per 300 pictures.
void setGxCulling(bool on,bool refine);
void reportGxCulling();
// Multi-texture draws and indices per vertex plan kind and the program
// changes (gx_pica.cpp): one report per 300 pictures; SD file
// vertex-plans.txt records real plans for Tools/3ds/test_vertex_variants.py.
void reportVertexPlans();
// Variant vertex programs (features::Id::vertexVariants): false after a
// learned GPU hang of one (gpuCanaryVertexVariantsRefused); set at start.
void setVertexVariantsAllowed(bool allowed);
// Native PICA blocks (runtime/pica_blocks.hpp). After a request, the next
// submitNativeRigid of this thread sends its whole state and leaves its words
// in the command buffer: takePicaBlockCapture gives them (valid until the
// next PICA command) when they are one unbroken run. An alpha-union draw is
// never captured. cancel: no capture pending any more.
struct PicaBlockCapture {const std::uint32_t* words{};unsigned count{};unsigned uniformBase{};bool multiTexture{};};
void requestPicaBlockCapture();
void cancelPicaBlockCapture();
bool takePicaBlockCapture(PicaBlockCapture& out);
// Copies a kept block into the command buffer (never a jump into it) and
// marks Citro3D's record of the state changed. False: nothing was sent.
bool submitPicaBlock(const std::uint32_t* words,unsigned count,bool multiTexture,const gx::RigidVertexPlan& palette,
                     const void* vertices,const std::uint16_t* indices,unsigned indexCount,unsigned caster);
// Original J2D code, compiled in a separate TU without libctru typedefs.
void drawJ2dPrimitiveProbe();
struct ColorCopyPlan;
// GPU-resident EFB colour capture into a copy image's resident texture.
bool gpuCaptureCopy(void* framebuffer,std::uint64_t handle,const ColorCopyPlan& plan,unsigned orient);
extern std::uint64_t gpuCaptures;
}
