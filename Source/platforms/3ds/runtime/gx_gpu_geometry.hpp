#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace dusk::native::gx {
struct GpuVertex {
    std::array<float,4> position{},color{};
    std::array<std::array<float,2>,3> texcoord{};
    float textureQ=1;
};
static_assert(sizeof(GpuVertex)==60);
struct CompactRigidVertex {
    std::array<float,3> position{};
    std::array<std::int16_t,3> normal{};
    std::array<std::int16_t,2> texcoord{};
    // Never read (shader input v5). It gives this record the six-attribute,
    // 32-byte shape of CompactRigidVertexMT: on the console PICA every draw of
    // the five-attribute 28-byte layout stopped (2026-11-13), while all
    // six-attribute formats completed.
    std::array<std::int16_t,2> unused{};
    std::array<std::int8_t,2> matrix{0,3}; // position, normal uniform rows
    std::array<std::uint8_t,4> color{};
};
static_assert(sizeof(CompactRigidVertex)==32);
// Native multi-texture compact record (gx_rigid_mt.v.pica).
struct CompactRigidVertexMT {
    std::array<float,3> position{};
    std::array<std::int16_t,3> normal{};
    std::array<std::int16_t,2> texcoord0{},texcoord1{};
    std::array<std::int8_t,2> matrix{0,3}; // position, normal uniform rows
    std::array<std::uint8_t,4> color{};
};
static_assert(sizeof(CompactRigidVertexMT)==32);
// The same record with a third texture coordinate set after the second one,
// for a material whose texgens read three GX coordinates
// (RigidVertexPlan::wideRecord). The six attributes of the compact layout:
// the second coordinate attribute (shader input v5) is four shorts wide.
struct CompactRigidVertexMTWide {
    std::array<float,3> position{};
    std::array<std::int16_t,3> normal{};
    std::array<std::int16_t,2> texcoord0{},texcoord1{},texcoord2{};
    std::array<std::int8_t,2> matrix{0,3}; // position, normal uniform rows
    std::array<std::uint8_t,4> color{};
};
static_assert(sizeof(CompactRigidVertexMTWide)==36);
// Per-reservation admission, never global state. The CPU direct texture0
// diagnostic, or private camera-aligned indirect texture0 adapter after its
// complete original-source proof, may select this token. The latter experiment
// remains ACTIVATION HOLD pending numerical-domain proof (review107).
// Static/rigid storage and multi-pass wrappers do not carry this contract.
enum class GeometryQPolicy { positiveOnly, directTexture0Diagnostic };
struct GpuGeometryBatch {
    std::span<GpuVertex> vertices;
    std::span<std::uint16_t> indices;
    std::uint64_t ticket{};
    GeometryQPolicy qPolicy{}; // default strict; not vertex data or arena ownership.
};
// Single render-thread linear arena. Its owner calls begin only after the
// previous GPU queue has completed. Mid-frame end/begin is permitted only
// after an actual GPU completion fence, never merely a command frame split.
// One reservation may be written at a time. After commit, its spans must never
// be modified/retained; their bytes stay live until the next completed frame.
class GpuGeometryArena {
public:
    bool attach(std::span<std::byte> storage);
    bool begin();
    bool end();
    bool reserve(std::size_t vertices,std::size_t indices,GpuGeometryBatch&);
    bool owns(const GpuGeometryBatch&)const;
    // Main owner only, after CPU emission and all scratch/source-view use ends.
    // Retain vertex/index prefixes; move indices to the smaller aligned layout.
    // Success invalidates every retained source/scratch view and, when counts
    // change, every old batch copy. Use only the returned spans/ticket afterward.
    // Failure changes no handle, arena state or bytes. A no-op still checks the
    // retained index prefix. qPolicy is preserved, not admitted or validated here.
    bool shrinkPending(GpuGeometryBatch& inout,std::size_t newVertices,std::size_t newIndices,bool externalVertices=false);
    bool commit(const GpuGeometryBatch&);
    bool cancel(const GpuGeometryBatch&);
    std::size_t used()const{return cursor;}
    // End of the reservation being written, or of the committed bytes.
    std::size_t reservedEnd()const{return pending.ticket?pendingEnd:cursor;}
    std::size_t capacity()const{return storage.size();}
    bool inFrame()const{return active;}
private:
    std::span<std::byte> storage;
    GpuGeometryBatch pending{};
    std::size_t cursor{},pendingEnd{};
    std::uint64_t serial{};
    bool active{};
};
// Validate all attributes/indices before cache flush or GPU submission.
bool validateGpuGeometry(const GpuGeometryBatch&);
bool validateCompactRigidGeometry(const GpuGeometryBatch&);
}
