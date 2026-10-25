#pragma once
#include "runtime/gx_raster_plan.hpp"
#include <3ds/types.h>
#include <3ds/gpu/enums.h>
extern "C" {
#include <c3d/effect.h>
}
#include <cassert>
namespace dusk::native {
inline void applyPicaBlend(const gx::RasterPlan& p) {
    assert(p.blend.mode<=1&&p.blend.src<8&&p.blend.dst<8);
    // GX subtract ignores the stored factors and computes destination-source.
    // Its alpha equation is the same when the original EFB stores alpha.
    if(p.subtractBlend) {
        C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT,p.storesAlpha?GPU_BLEND_REVERSE_SUBTRACT:GPU_BLEND_ADD,
            GPU_ONE,GPU_ONE,GPU_ONE,p.storesAlpha?GPU_ONE:GPU_ZERO);
        return;
    }
    auto factor=[](unsigned f,bool destination,bool alpha){
        if(f==2)return destination?(alpha?GPU_SRC_ALPHA:GPU_SRC_COLOR):(alpha?GPU_DST_ALPHA:GPU_DST_COLOR);
        if(f==3)return destination?(alpha?GPU_ONE_MINUS_SRC_ALPHA:GPU_ONE_MINUS_SRC_COLOR):(alpha?GPU_ONE_MINUS_DST_ALPHA:GPU_ONE_MINUS_DST_COLOR);
        constexpr GPU_BLENDFACTOR other[]{GPU_ZERO,GPU_ONE,GPU_ZERO,GPU_ZERO,
            GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,GPU_DST_ALPHA,GPU_ONE_MINUS_DST_ALPHA};
        return other[f];
    };
    // RGB8 always reads destination alpha as one, including when PICA's
    // physical RGBA8 target still contains alpha from an earlier draw.
    auto rgbFactor=[&](unsigned f,bool destination){
        return !p.storesAlpha&&f==6?GPU_ONE:!p.storesAlpha&&f==7?GPU_ZERO:factor(f,destination,false);
    };
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,
        p.blend.mode?rgbFactor(p.blend.src,false):GPU_ONE,
        p.blend.mode?rgbFactor(p.blend.dst,true):GPU_ZERO,
        p.storesAlpha&&p.blend.mode?factor(p.blend.src,false,true):GPU_ONE,
        p.storesAlpha&&p.blend.mode?factor(p.blend.dst,true,true):GPU_ZERO);
}
}
