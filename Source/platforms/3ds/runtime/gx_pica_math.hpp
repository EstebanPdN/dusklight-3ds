#pragma once
#include "gx_state.hpp"
#include "gx_target_extent.hpp"
#include <array>
namespace dusk::native::gx {
// Original GX/Aurora uses clockwise front faces. The native 90-degree clip
// rotation preserves winding. PICA mode 1 keeps clockwise, mode 2 keeps CCW.
// Cull-all is handled by the caller before submission, not by this register.
constexpr unsigned picaCull(unsigned gxCull){return gxCull==1?2:gxCull==2?1:0;}
// Output is row-major. GX projection already uses the pinned Aurora [-w,0]
// reversed depth convention; never apply an OpenGL [-w,w] conversion again.
bool picaTransform(const DrawPacket& packet, std::array<float, 16>& matrix, const TargetExtent& = currentTargetExtent);
// Same viewport/rotation/depth conversion for an already projected view-space
// position. Supports input/output aliasing; avoids a full DrawPacket temporary.
bool picaProjection(const std::array<float,16>& projection,const Viewport&,std::array<float,16>& output, const TargetExtent& = currentTargetExtent);
struct PicaScissor { unsigned left, top, right, bottom; bool empty; };
PicaScissor picaScissor(const Scissor& input, const TargetExtent& = currentTargetExtent);
// Conservative homogeneous lateral clip outcodes, including mixed/zero w.
// A triangle sharing a bit cannot reach the clip volume when clipping is on.
// Nonfinite, float24 range limits or near-boundary positions give no proof.
// Never use with GX clipping disabled, where homogeneous clipping is not the
// original contract.
unsigned picaClipSideMask(const std::array<float,4>& position);
// Also proves corner rejection when no single plane rejects all vertices.
// Clips conservative affine half-space distances in barycentric space, with
// the same per-vertex 1/16 margin. Uncertain/invalid input gives no proof.
bool picaTriangleOutsideSides(const std::array<float,4>& a,
                              const std::array<float,4>& b,
                              const std::array<float,4>& c);
struct AlphaPlan { std::uint8_t comparison, reference; };
// Exact truth-table reduction to one GX comparison; false means unsupported.
bool reduceAlpha(const AlphaCompare& input, AlphaPlan& output);
// Exact OR of two disjoint low/high tails, for ordered per-triangle passes.
// Neither pass can accept the same fragment; bounded middle intervals reject.
bool splitAlphaUnion(const AlphaCompare&, std::array<AlphaPlan,2>&);
}
