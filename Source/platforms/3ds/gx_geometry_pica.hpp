#pragma once
#include <cstdint>
#include "runtime/gx_gpu_geometry.hpp"
#include "runtime/gx_alpha_interval.hpp"
namespace dusk::native::gx { class StaticCompactCache; struct RigidVertexPlan; }
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
bool flushPendingGxGeometry();
// Console A/B switch: publish arena bytes with the kernel cache flush instead
// of a GSP request (same range, no IPC round trip). Off until hardware confirms it.
void setGxGeometryKernelFlush(bool enabled);
bool reserveGxGeometry(std::size_t vertices,std::size_t indices,gx::GpuGeometryBatch&);
// HUD tape (runtime/hud_tape.cpp). Between the two calls every reservation
// comes from a second buffer that frame begins do not reset. end returns
// false when the recording cannot be kept: the buffer overflowed (the rest of
// the pass used the frame arena), a flush failed or a draw left a reservation.
bool beginGxTapeGeometry();
bool endGxTapeGeometry();
// Retained 2D screens (runtime/screen_tape.hpp). A third buffer that frame
// begins do not reset, shared by every kept recording. A recording either
// takes the bytes after the last one (at = gxScreenGeometryAppend) or writes
// over a region an earlier recording of the same owner had (at, capacity),
// which must not be in any command list still to be run. begin is false when
// nothing is left or another recording is open; end returns false like
// endGxTapeGeometry and gives where the recording's bytes start and how many
// they are (appended bytes stay taken either way: commands of this frame
// refer to them). recycle starts the buffer again: only when no command list
// refers to it (frame begin) and every recording was dropped.
inline constexpr std::size_t gxScreenGeometryAppend=~std::size_t(0);
bool beginGxScreenGeometry(std::size_t at=gxScreenGeometryAppend,std::size_t capacity=0);
bool endGxScreenGeometry(std::size_t& at,std::size_t& bytes);
void recycleGxScreenGeometry();
std::size_t gxScreenGeometryUsed();
std::size_t gxScreenGeometryCapacity();
// The frame arena starts again from its beginning at each frame and when it
// is reclaimed within a frame. The caster diagnostics count how often a
// retained draw survives one of those restarts.
std::uint32_t gxGeometryArenaGeneration();
// Give transient caster inputs frame lifetime. Resident inputs are borrowed.
// Retained bytes are published before the atlas draws and recycled only at
// the next GPU-fenced frame begin.
bool retainGxCasterGeometry(const void*& vertices,std::size_t availableVertexBytes,
                            const std::uint16_t*& indices,unsigned indexCount);
bool publishGxCasterGeometry();
// The next draw sends its attribute layout and vertex buffers through
// Citro3D's getters again (complete re-send), as at frame begin. Between two
// such points a layout or buffer equal to Citro3D's copy is not sent again.
void invalidateGxGeometryBindings();
// What the draws sent: complete attribute layouts and buffer blocks, layouts
// and buffers left as they were, and buffer changes sent as the first buffer's
// three registers only. Cumulative.
struct GxBindingStatistics {std::uint64_t layoutsSent{},layoutsKept{},buffersSent{},buffersKept{},buffersFirstOnly{};};
GxBindingStatistics gxBindingStatistics();
bool cancelGxGeometry(const gx::GpuGeometryBatch&);
// Pending CPU-only layout change; never flushes, commits or waits.
bool shrinkGxGeometry(gx::GpuGeometryBatch&,std::size_t vertices,std::size_t indices,bool externalVertices=false);
// Caller binds shader, transform uniform, raster/material/texture state first.
// The shared GX shader consumes position4/color4/three independent UV2 streams.
// Success invalidates mutable batch access. No per-draw allocation or CPU copy.
// clipSpace: the positions are clip coordinates (the hardware clip guard
// applies); false for layouts a rigid shader transforms on the GPU.
bool submitGxGeometry(const gx::GpuGeometryBatch&,bool clipSpace=true);
// Two equal-size vertex halves share indices. The callback sets material/fog
// state per triangle, without changing attribute/buffer bindings or geometry.
// All bytes commit before any callback can flush the queue.
bool submitGxTrianglePairs(const gx::GpuGeometryBatch&,bool (*beforePass)(bool fog,void*),void*);
// A base and additive contribution use the two vertex halves. An optional
// third pass reuses the contribution half to apply fog after saturation.
// Mesh mode additionally reserves a second index span for reverse triangle order.
bool submitGxTrianglePasses(const gx::GpuGeometryBatch&,bool applyFog,bool mesh,
                            bool (*beforePass)(unsigned pass,void*),void*);
bool submitGxMeshPair(const gx::GpuGeometryBatch&,bool (*beforePass)(bool fog,void*),void*);
// Same immutable vertices for both disjoint alpha tests, interleaved per
// triangle to retain blending/depth order. Callback may only change draw state.
bool submitGxTriangleAlphaPairs(const gx::GpuGeometryBatch&,bool (*beforePass)(bool second,void*),void*);
// The same two alpha tests as whole-mesh passes (all triangles with the first,
// then all with the second): two draws. Only for blends whose result does not
// depend on the order of the fragments (see picaAlphaUnionMesh).
bool submitGxAlphaUnionMesh(const gx::GpuGeometryBatch&,bool (*beforePass)(bool second,void*),void*);
// Four passes per immutable triangle, committed before the first callback.
// All address/geometry checks precede commit. Failure after commit is fatal.
bool submitGxTriangleAlphaInterval(const gx::GpuGeometryBatch&,bool (*beforePass)(gx::AlphaIntervalPass,void*),void*);
bool submitGxCompactRigidGeometry(const gx::GpuGeometryBatch&,const void* resident=nullptr,bool multiTexture=false,bool wide=false);
bool submitGxCompactFogGeometry(const gx::GpuGeometryBatch&,const gx::RigidVertexPlan&,unsigned& indexCount);
void redrawGxGeometry(const gx::GpuGeometryBatch&);
// Native engine resident draws: persistent vertices/indices, no frame arena.
bool drawResidentRigid(const void* vertices,const std::uint16_t* indices,unsigned indexCount,bool multiTexture);
std::uint16_t* allocateResidentIndices(const std::uint16_t* source,unsigned count);
std::uint32_t residentIndexEpoch();
// Attach a GPU-readable linear pool to the native static geometry cache.
bool attachResidentStaticGeometry(gx::StaticCompactCache&,std::size_t bytes);
const char* gxGeometryFailure();
std::size_t gxGeometryBytes();
}
