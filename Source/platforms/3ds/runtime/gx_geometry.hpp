#pragma once
#include "gx_vertex_transform.hpp"
namespace dusk::native::gx {
struct PreparedGeometry {
    std::span<const TransformedVertex> vertices;
    std::span<const u16> indices;
    std::uint8_t coordinateMask{},projectiveMask{};
};
enum class GeometryError { none, primitive, capacity, index, alias, decode, transform };
// Prepare one indexed triangle draw in caller-owned storage, retaining one
// transformed record per original vertex. Existing optimized indices remain
// identical; ordinary quads/strips/fans get triangle indices, not duplicated
// vertices. No allocation or GPU call. Inputs/registers must stay immutable;
// outputs must not overlap them or each other. On failure the published view
// clears; scratch may contain partial data after a decode/transform failure.
// The caller owns buffer lifetime through GPU completion, raster color/lighting,
// texture packing/projective support, draw-state admission and GPU submission.
bool prepareGeometry(const CommandDraw&,std::uint8_t coordinates,
                     std::span<TransformedVertex>,std::span<u16>,PreparedGeometry&,
                     GeometryError* = nullptr,VertexTransformError* = nullptr);
}
