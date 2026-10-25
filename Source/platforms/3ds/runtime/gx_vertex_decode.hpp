#pragma once
#include <aurora/dl.hpp>
#include <array>
#include <cstdint>
#include <span>

namespace dusk::native::gx {
// Borrowed arrays are registered/resolved by the caller, never dereferenced
// directly from an address embedded in a command stream. Lifetime is external.
struct VertexArrayView { std::span<const u8> bytes; u32 stride = 0; bool littleEndian = false; };
using VertexArrays = std::array<VertexArrayView, GX_VA_MAX_ATTR>;
struct DecodedVertex {
    u32 present = 0;
    float position[3]{}, normal[3][3]{}, color[2][4]{}, texcoord[8][2]{};
    u8 normalCount{}; // 0 absent, 1 normal, 3 normal/binormal/tangent.
    u8 matrix[9]{}; // Raw GX matrix register indices; not divided/remapped here.
};
enum class VertexDecodeError { none, layout, format, packedBounds, arrayBounds, nonfinite };
struct GpuVertex;
struct RasterColorPlan;
struct RigidVertexPlan;
struct CommandRegisters;
class CompactVertexCache;
struct VertexDecodePlan {
    unsigned packedStride() const { return stride; }
private:
    friend bool planVertexDecode(const aurora::gx::dl::VtxLayout&, std::span<const GXVtxAttrFmtList>,
                                 VertexDecodePlan&, VertexDecodeError*);
    friend bool decodeVertex(const VertexDecodePlan&, std::span<const u8>, const VertexArrays&,
                             DecodedVertex&, VertexDecodeError*);
    friend bool decodeRigidBatch(const VertexDecodePlan&,std::span<const u8>,const VertexArrays&,std::span<GpuVertex>,const RasterColorPlan&,const std::array<float,2>&,std::span<const std::int8_t>);
    friend bool decodeCompactRigidBatch(const VertexDecodePlan&,std::span<const u8>,const VertexArrays&,std::span<GpuVertex>,const std::array<float,2>&,RigidVertexPlan&,CompactVertexCache*);
    friend bool decodeCompactRigidBatchMT(const VertexDecodePlan&,std::span<const u8>,const VertexArrays&,std::span<GpuVertex>,RigidVertexPlan&);
    friend bool decodePositionTransformBatch(const VertexDecodePlan&,std::span<const u8>,const VertexArrays&,std::span<GpuVertex>,const CommandRegisters&,const std::array<float,16>&);
    struct Attribute {
        u8 attribute=0, offset=0, packedSize=0, type=0, scalarSize=0, components=0;
        GXAttrType descriptor=GX_NONE;
        bool nbt3=false;
        float scale=1;
    };
    std::array<Attribute, GX_VA_TEX7+1> attributes{};
    u8 count=0, stride=0;
};
// Prepare once when the VAT/descriptor layout changes. Formats are explicitly
// bounded; no sentinel scan. Unused formats do not affect the plan.
bool planVertexDecode(const aurora::gx::dl::VtxLayout& layout,
                      std::span<const GXVtxAttrFmtList> formats, VertexDecodePlan& output,
                      VertexDecodeError* error=nullptr);
// Allocation-free, at most one attribute-record fetch for ordinary indexed data.
// Output clears on failure. NBT3 follows the pinned Aurora addressing convention:
// the three independent indices select records, then N/B/T uses its slice offset.
bool decodeVertex(const VertexDecodePlan& plan, std::span<const u8> packed,
                  const VertexArrays& arrays, DecodedVertex& output,
                  VertexDecodeError* error=nullptr);
// Native F32/S16/S8 indexed rigid records directly into the shared GPU stream.
// No cache or retained pointers; all referenced source extents are checked.
// False means unsupported/invalid input; caller rewrites through the general
// decoder before submission. Output may be partially filled on false.
bool decodeRigidBatch(const VertexDecodePlan&,std::span<const u8>,const VertexArrays&,
                      std::span<GpuVertex>,const RasterColorPlan&,const std::array<float,2>&,
                      std::span<const std::int8_t> matrixOffsets={});
// Original native S16 normals/UVs with uniform material or native RGBA8 color. The 28-byte
// records occupy the start of a normal arena reservation as raw object bytes;
// callers must submit using the compact layout, never as float GpuVertex data.
bool decodeCompactRigidBatch(const VertexDecodePlan&,std::span<const u8>,const VertexArrays&,
                             std::span<GpuVertex>,const std::array<float,2>&,RigidVertexPlan&,CompactVertexCache* cache=nullptr);
// Native multi-texture variant: 32-byte CompactRigidVertexMT records with TEX0
// and TEX1, for plans from planRigidMultiVertex. Folds the fixed-point texture
// fractions into the plan's texgen rows. Static J3D draws use the resident cache.
bool decodeCompactRigidBatchMT(const VertexDecodePlan&,std::span<const u8>,const VertexArrays&,
                               std::span<GpuVertex>,RigidVertexPlan&);
// Shared compact-uniform tail (decoders and native shape replay).
bool finishCompactRigidUniforms(RigidVertexPlan&,const std::array<float,2>& textureScale);
// Exact position-only CPU path for draws consuming no raster/texture/fog.
// Validates all indexed extents and unused F32 values before submitting.
bool decodePositionTransformBatch(const VertexDecodePlan&,std::span<const u8>,const VertexArrays&,
                                  std::span<GpuVertex>,const CommandRegisters&,const std::array<float,16>&);
}
