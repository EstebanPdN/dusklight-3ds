#pragma once
#include "contracts.hpp"
namespace dusk::pica {
// Game-authored continuous canvas/window data, not a hardware register bank.
// Projection input already has right-handed clip depth [-W,0]. The framebuffer
// is the standard sideways display target: physical width=logical height.
struct CameraRegion {
    float canvasWidth{},canvasHeight{};
    float x{},y{},width{},height{};
    float nearDepth=0,farDepth=1;
};
struct CameraProjection {
    Matrix rows{};
    unsigned left{},top{},right{},bottom{}; // Physical PICA scissor bounds.
};
// One per-pass matrix/region preparation. No vertices, source command decoding,
// SDK calls, allocation, projection reconstruction or division by vertex W.
// Scissor edges round outward; exact physical raster/edge precision is unproved.
bool prepareCamera(const Matrix& projection,const CameraRegion&,unsigned physicalWidth,unsigned physicalHeight,CameraProjection&);
}
