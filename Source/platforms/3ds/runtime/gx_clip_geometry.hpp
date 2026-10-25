#pragma once
#include "gx_gpu_geometry.hpp"
#include <cmath>
#include <span>

namespace dusk::native::gx {
// A same-sign, nonzero homogeneous w makes every projected triangle a convex
// combination of its divided vertices. If every vertex is beyond the same
// screen edge, no color/depth sample can be reached even with GX clipping off.
// Keep a 1/16 NDC margin (7.5/12.5 physical pixels) beyond the full
// framebuffer. This exceeds float24 rounding and the raster sample footprint.
// Mixed signs, camera-plane points, nonfinite values and ambiguous geometry
// remain unproved. Positions already include the original viewport and tilt.
// Camera clipping discards strictly negative-w triangles. This also defines
// an explicit compatibility policy when original GX clipping is disabled;
// it does not claim to reproduce GX's undefined near-camera raster behavior.
inline bool proveBehindCamera(std::span<const GpuVertex> vertices) {
    if(vertices.empty())return false;
    for(const auto& vertex:vertices){
        for(float value:vertex.position)if(!std::isfinite(value))return false;
        if(vertex.position[3]>=-1e-6f)return false;
    }
    return true;
}
inline bool proveNoScreenCoverage(std::span<const GpuVertex> vertices) {
    if(vertices.empty())return false;
    const bool negative=vertices.front().position[3]<0;
    unsigned common=15;
    for(const auto& vertex:vertices){
        const auto& p=vertex.position;
        for(float value:p)if(!std::isfinite(value))return false;
        if(p[3]==0||(p[3]<0)!=negative)return false;
        const float limit=1.0625f*std::abs(p[3]);
        if(!std::isfinite(limit)||limit==0)return false;
        const float x=negative?-p[0]:p[0],y=negative?-p[1]:p[1];
        const unsigned outside=(x>limit?1u:0u)|(x<-limit?2u:0u)|
            (y>limit?4u:0u)|(y<-limit?8u:0u);
        common&=outside;
        if(!common)return false;
    }
    return common!=0;
}
}
