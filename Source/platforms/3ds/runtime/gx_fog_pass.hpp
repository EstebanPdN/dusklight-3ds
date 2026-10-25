#pragma once
#include "gx_raster_plan.hpp"
#include "gx_pica_material.hpp"
namespace dusk::native::gx {
// Bounded Tier D fallback: apply fog immediately after EACH opaque triangle.
// Applying it after the whole mesh is wrong for coplanar overlap. LEQUAL
// guarantees every equal-depth fog fragment also passed the original test.
// Framebuffer alpha is preserved; an extra UNORM8 RGB store can add rounding.
inline bool planTriangleFogPass(const RasterPlan& original,RasterPlan& fog,PicaMaterialPlan& material){
    fog={};material={};
    if(original.blend.mode||original.subtractBlend||!original.writeRgb||original.writeAlpha||
       original.alpha.comparison!=7||!original.depth.enabled||!original.depth.write||
       original.depth.compare!=3||original.earlyDepthAlpha)return false;
    fog=original;fog.depth.compare=2;fog.depth.write=false;fog.blend={1,4,5,3};
    material.stageCount=1;
    material.stages[0].rgb.inputs[0]={MaterialSource::primary,MaterialOperand::color,false};
    material.stages[0].alpha.inputs[0]={MaterialSource::primary,MaterialOperand::alpha,false};
    return true;
}
}
