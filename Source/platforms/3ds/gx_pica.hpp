#pragma once
#include <cstdint>
#include <array>
namespace dusk::native::gx { struct RasterPlan; struct PicaMaterialPlan; struct GpuGeometryBatch; struct RigidVertexPlan; }
namespace dusk::native {
bool submitStaticGxMesh(const gx::RasterPlan&,const gx::PicaMaterialPlan&,unsigned,const std::array<float,16>&);
bool initializeGxPica();
void destroyGxPica(); // Only after C3D_Fini / completed GPU work.
const char* gxPicaFailure();
std::uint64_t gxPicaDraws();
// Containment guard only, NOT a fence. Preserve all queue completion, flush and
// cache/lifetime operations. False forbids copy/clear/rebind/reuse/presentation.
bool alphaIntervalCopySafe();
// Forget the last applied raster/TEV state (render target or external change).
void invalidatePicaCommandState();
void picaStateFilterStatistics(std::uint64_t& skips,std::uint64_t& applies);
void picaUniformStatistics(std::uint64_t& skipped,std::uint64_t& written);
// Command adapter supplies pretransformed PICA clip positions and fully planned
// draw state. RGB8 destination-alpha blend factors are resolved as constants.
bool submitGxCommandGeometry(const gx::RasterPlan&,const gx::PicaMaterialPlan&,const gx::GpuGeometryBatch&);
bool submitGxTriangleFogGeometry(const gx::RasterPlan&,const gx::PicaMaterialPlan&,const gx::RasterPlan&,const gx::PicaMaterialPlan&,const gx::GpuGeometryBatch&);
bool submitGxRigidGeometry(const gx::RasterPlan&,const gx::PicaMaterialPlan&,const gx::GpuGeometryBatch&,const gx::RigidVertexPlan&);
bool submitNativeRigid(const gx::RasterPlan&,const gx::PicaMaterialPlan&,const gx::RigidVertexPlan&,const void* vertices,const std::uint16_t* indices,unsigned indexCount);
// Original J2D code, compiled in a separate TU without libctru typedefs.
void drawJ2dPrimitiveProbe();
struct ColorCopyPlan;
// GPU-resident EFB colour capture into a copy image's resident texture.
bool gpuCaptureCopy(void* framebuffer,std::uint64_t handle,const ColorCopyPlan& plan,unsigned orient);
extern std::uint64_t gpuCaptures;
}
