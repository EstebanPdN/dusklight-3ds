#pragma once
#include "gx_geometry.hpp"
#include "gx_gpu_geometry.hpp"
#include "gx_pica_material.hpp"
namespace dusk::native::gx {
inline bool validGeometryQBinding(const GpuGeometryBatch& batch,const PicaMaterialPlan& material){
    return batch.qPolicy==GeometryQPolicy::positiveOnly||
        (batch.qPolicy==GeometryQPolicy::directTexture0Diagnostic&&material.textureCount>0&&
         material.textureCount<=3&&material.projectiveTexture0);
}
enum class GeometryPackError { none, capacity, binding, projective, coordinate, color, position, index, alias };
// Pack prepared CPU geometry directly into the reserved linear GPU batch.
// Colors must come from the caller's correct raster/channel evaluation; this
// does not substitute white for missing lighting. Texture scales are validated
// width/padded-width and height/padded-height from bindGxTexture, by PICA unit.
// A single projective binding must already be routed to unit zero. Retained
// CPU q is sent to the fragment sampler; it is never divided per vertex.
// Positive finite divisors are the default. Explicit direct diagnostic batches
// retain finite signed nonzero q; this packer's caller establishes direct
// perspective eligibility. The CPU adapter also reuses the backend token for
// private camera-aligned indirect q only after complete original-source proof;
// that experiment remains ACTIVATION HOLD for review107 numerical-domain gaps.
// This binding helper does not prove that adapter contract. Padded-clamp bounds
// remain strict [0,1] in homogeneous form
// (q<=numerator<=0 for negative q). No division, allocation or GPU call.
bool packGpuGeometry(const PreparedGeometry&,std::span<const std::array<float,4>> colors,
                     const PicaMaterialPlan&,const std::array<std::array<float,2>,3>& scales,
                     const GpuGeometryBatch&,GeometryPackError* = nullptr);
}
