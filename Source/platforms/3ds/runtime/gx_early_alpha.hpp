#pragma once
#include "gx_pica_material.hpp"
#include "gx_raster_plan.hpp"
#include <algorithm>
namespace dusk::native::gx {
// Exact constant replace chains only. Texture/primary/combining arithmetic
// remains unknown; do not infer fragment alpha from vertex samples.
inline bool resolveConstantEarlyAlpha(const PicaMaterialPlan& material,RasterPlan& raster) {
    if(!raster.earlyDepthAlpha)return true;
    bool known=false;unsigned alpha=0;
    for(unsigned n=0;n<material.stageCount;++n){
        const auto& stage=material.stages[n];const auto& channel=stage.alpha;
        if(channel.function!=MaterialFunction::replace){known=false;continue;}
        const auto& input=channel.inputs[0];
        if(input.source==MaterialSource::constant||input.source==MaterialSource::buffer){
            const unsigned component=input.operand==MaterialOperand::red?0:input.operand==MaterialOperand::green?1:input.operand==MaterialOperand::blue?2:3;
            if(input.source==MaterialSource::buffer&&(component<3?material.bufferUpdateRgb:material.bufferUpdateAlpha)){known=false;continue;}
            alpha=input.source==MaterialSource::buffer?(n?material.bufferConstant[component]:0):stage.constant[component];known=true;
        }else if(input.source!=MaterialSource::previous||(input.operand!=MaterialOperand::alpha&&input.operand!=MaterialOperand::color))known=false;
        if(known){if(input.inverse)alpha=255-alpha;alpha=std::min(255u,alpha<<channel.scale);}
    }
    if(!known)return false;
    const unsigned reference=raster.alpha.reference;bool passed;
    switch(raster.alpha.comparison){
    case 0:passed=false;break;case 1:passed=alpha<reference;break;case 2:passed=alpha==reference;break;
    case 3:passed=alpha<=reference;break;case 4:passed=alpha>reference;break;case 5:passed=alpha!=reference;break;
    case 6:passed=alpha>=reference;break;case 7:passed=true;break;default:return false;
    }
    // An early GX depth write occurs even when constant alpha discards color.
    // With a proven passing test, ordinary PICA depth/alpha order is identical.
    if(!passed)raster.writeRgb=raster.writeAlpha=false;
    raster.alpha={7,0};raster.earlyDepthAlpha=false;return true;
}
}
