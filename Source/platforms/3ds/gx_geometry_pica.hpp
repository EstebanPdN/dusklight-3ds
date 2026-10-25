#pragma once
#include "runtime/gx_gpu_geometry.hpp"
#include "runtime/gx_alpha_interval.hpp"
namespace dusk::native::gx { class StaticCompactCache; }
namespace dusk::native {
// Immutable GPU meshes: upload once, retain until destroyGxGeometry after GPU drain.
// Handles are process-lifetime slots, never recycled; total storage is bounded.
bool uploadStaticGxGeometry(const gx::GpuGeometryBatch&,unsigned& handle);
bool submitStaticGxGeometry(unsigned handle);
std::size_t staticGxGeometryBytes();
bool initializeGxGeometry();
void destroyGxGeometry(); // Only after C3D_Fini / completed GPU work.
// Successful blocking C3D_FrameBegin must precede begin. End precedes
// C3D_FrameEnd. Empty capture frames do not open the geometry arena.
bool beginGxGeometryFrame();
bool endGxGeometryFrame();
// Flush newly committed arena bytes before every GPU queue submission. Pending
// CPU reservations are excluded until commit; successful flushes alone advance
// the watermark. No draw reads the arena before this publication boundary.
bool flushGxGeometry();
bool reserveGxGeometry(std::size_t vertices,std::size_t indices,gx::GpuGeometryBatch&);
bool cancelGxGeometry(const gx::GpuGeometryBatch&);
// Pending CPU-only layout change; never flushes, commits or waits.
bool shrinkGxGeometry(gx::GpuGeometryBatch&,std::size_t vertices,std::size_t indices,bool externalVertices=false);
// Caller binds shader, transform uniform, raster/material/texture state first.
// The shared GX shader consumes position4/color4/three independent UV2 streams.
// Success invalidates mutable batch access. No per-draw allocation or CPU copy.
bool submitGxGeometry(const gx::GpuGeometryBatch&);
// Two equal-size vertex halves share indices. The callback sets material/fog
// state per triangle, without changing attribute/buffer bindings or geometry.
// All bytes commit before any callback can flush the queue.
bool submitGxTrianglePairs(const gx::GpuGeometryBatch&,bool (*beforePass)(bool fog,void*),void*);
// Same immutable vertices for both disjoint alpha tests, interleaved per
// triangle to retain blending/depth order. Callback may only change draw state.
bool submitGxTriangleAlphaPairs(const gx::GpuGeometryBatch&,bool (*beforePass)(bool second,void*),void*);
// Four passes per immutable triangle, committed before the first callback.
// All address/geometry checks precede commit. Failure after commit is fatal.
bool submitGxTriangleAlphaInterval(const gx::GpuGeometryBatch&,bool (*beforePass)(gx::AlphaIntervalPass,void*),void*);
bool submitGxCompactRigidGeometry(const gx::GpuGeometryBatch&,const void* resident=nullptr,bool multiTexture=false);
// Native engine resident draws: persistent vertices/indices, no frame arena.
bool drawResidentRigid(const void* vertices,const std::uint16_t* indices,unsigned indexCount,bool multiTexture);
std::uint16_t* allocateResidentIndices(const std::uint16_t* source,unsigned count);
std::uint32_t residentIndexEpoch();
// Attach a GPU-readable linear pool to the native static geometry cache.
bool attachResidentStaticGeometry(gx::StaticCompactCache&,std::size_t bytes);
const char* gxGeometryFailure();
std::size_t gxGeometryBytes();
}
