#pragma once
#include "gx_material.hpp"
#include "gx_pica_material.hpp"
#include "gx_raster_plan.hpp"
#include "gx_raster_color.hpp"
#include "gx_commands.hpp"
namespace dusk::native::gx {
// TEV alpha can be discarded only when no subsequent RGB computation, alpha
// test, blend or framebuffer write observes it. Raster alpha used by RGB is
// a separate input and is deliberately retained.
inline bool discardUnobservedTevAlpha(MaterialState& state,const RasterPlan& raster){
    if(raster.writeAlpha||raster.blend.mode||raster.alpha.comparison!=7||
       !state.stageCount||state.stageCount>state.stages.size())return false;
    for(unsigned i=0;i<state.stageCount;++i)
        for(auto input:state.stages[i].color)if(input<8&&(input&1))return false;
    for(unsigned i=0;i<state.stageCount;++i){
        state.stages[i].alpha={7,7,7,std::uint8_t(i?0:7)};
        state.stages[i].alphaOp={0,0,0,1,0};
    }
    return true;
}
// Replace a proven, draw-constant primary alpha with per-stage constant
// components. Existing constant operands are preserved, including swizzles.
// On allocation conflict leave the original material completely unchanged.
inline bool substituteConstantPrimaryAlpha(PicaMaterialPlan& material,std::uint8_t value){
    if(!material.stageCount||material.stageCount>material.stages.size())return false;
    auto copy=material;
    auto arity=[](MaterialFunction f)->unsigned{
        switch(f){case MaterialFunction::replace:return 1;
        case MaterialFunction::modulate:case MaterialFunction::add:case MaterialFunction::addSigned:case MaterialFunction::subtract:return 2;
        case MaterialFunction::interpolate:case MaterialFunction::multiplyAdd:return 3;}return 0;
    };
    constexpr MaterialOperand components[]{MaterialOperand::red,MaterialOperand::green,MaterialOperand::blue,MaterialOperand::alpha};
    for(unsigned i=0;i<copy.stageCount;++i){
        auto& stage=copy.stages[i];unsigned used=0;bool needed=false;
        for(unsigned alpha=0;alpha<2;++alpha){const auto& ch=alpha?stage.alpha:stage.rgb;
            const auto n=arity(ch.function);if(!n)return false;
            for(unsigned j=0;j<n;++j){const auto& input=ch.inputs[j];
                const bool a=input.operand==MaterialOperand::alpha||(alpha&&input.operand==MaterialOperand::color);
                needed|=input.source==MaterialSource::primary&&a;
                if(input.source!=MaterialSource::constant)continue;
                if(input.operand==MaterialOperand::color)used|=alpha?8:7;
                else for(unsigned c=0;c<4;++c)if(input.operand==components[c])used|=1u<<c;
            }
        }
        if(!needed)continue;
        unsigned slot=4;
        for(unsigned c=0;c<4;++c)if(stage.constant[c]==value){slot=c;break;}
        if(slot==4)for(unsigned c=0;c<4;++c)if(!(used&(1u<<c))){slot=c;break;}
        if(slot==4)return false;
        stage.constant[slot]=value;
        for(unsigned alpha=0;alpha<2;++alpha){auto& ch=alpha?stage.alpha:stage.rgb;
            for(unsigned j=0;j<arity(ch.function);++j){auto& input=ch.inputs[j];
                if(input.source==MaterialSource::primary&&(input.operand==MaterialOperand::alpha||(alpha&&input.operand==MaterialOperand::color))){
                    input.source=MaterialSource::constant;input.operand=components[slot];
                }
            }
        }
    }
    material=copy;return true;
}

// RGB counterpart: allocate the vector before scalar swizzles, retaining every
// live constant component and every inverse. Alpha/color in the alpha equation
// means primary alpha and is deliberately not replaced. Transactional on failure.
inline bool substituteConstantPrimaryRgb(PicaMaterialPlan& material,const std::array<std::uint8_t,3>& value){
    if(material.rasterCarrier!=RasterCarrier::none||!material.stageCount||material.stageCount>material.stages.size())return false;
    auto copy=material;
    auto arity=[](MaterialFunction f)->unsigned{
        switch(f){case MaterialFunction::replace:return 1;
        case MaterialFunction::modulate:case MaterialFunction::add:case MaterialFunction::addSigned:case MaterialFunction::subtract:return 2;
        case MaterialFunction::interpolate:case MaterialFunction::multiplyAdd:return 3;}return 0;
    };
    constexpr MaterialOperand components[]{MaterialOperand::red,MaterialOperand::green,MaterialOperand::blue,MaterialOperand::alpha};
    for(unsigned i=0;i<copy.stageCount;++i){
        auto& stage=copy.stages[i];unsigned used=0;bool vector=false;
        for(unsigned alpha=0;alpha<2;++alpha){const auto& ch=alpha?stage.alpha:stage.rgb;
            const auto n=arity(ch.function);if(!n)return false;
            for(unsigned j=0;j<n;++j){const auto& input=ch.inputs[j];
                if(unsigned(input.operand)>unsigned(MaterialOperand::blue))return false;
                vector|=!alpha&&input.source==MaterialSource::primary&&input.operand==MaterialOperand::color;
                if(input.source!=MaterialSource::constant)continue;
                if(input.operand==MaterialOperand::color)used|=alpha?8:7;
                else for(unsigned c=0;c<4;++c)if(input.operand==components[c])used|=1u<<c;
            }
        }
        if(vector)for(unsigned c=0;c<3;++c){
            if((used&(1u<<c))&&stage.constant[c]!=value[c])return false;
            stage.constant[c]=value[c];used|=1u<<c;
        }
        for(unsigned alpha=0;alpha<2;++alpha){auto& ch=alpha?stage.alpha:stage.rgb;
            for(unsigned j=0;j<arity(ch.function);++j){auto& input=ch.inputs[j];
                if(input.source!=MaterialSource::primary||input.operand==MaterialOperand::alpha||
                   (alpha&&input.operand==MaterialOperand::color))continue;
                if(input.operand==MaterialOperand::color){input.source=MaterialSource::constant;continue;}
                unsigned component=0;while(component<3&&input.operand!=components[component])++component;
                if(component==3)return false;
                unsigned slot=4;
                for(unsigned c=0;c<4;++c)if(stage.constant[c]==value[component]){slot=c;break;}
                if(slot==4)for(unsigned c=0;c<4;++c)if(!(used&(1u<<c))){slot=c;break;}
                if(slot==4)return false;
                stage.constant[slot]=value[component];used|=1u<<slot;
                input.source=MaterialSource::constant;input.operand=components[slot];
            }
        }
    }
    material=copy;return true;
}

// Proof covers EVERY original vertex, even when the caller's index stream does
// not reference it. Full decode retains validation of other encoded attributes.
// Candidate admission is endpoint-only: intermediate primary transport precision
// has not been established on PICA. This does not assert GPU equivalence.
inline bool proveConstantPrimaryRgb(const CommandDraw& draw,const RasterColorPlan& color,std::array<std::uint8_t,3>& value){
    const auto stride=draw.plan.packedStride();
    if(color.rasterChannel!=0||!(color.required&1)||(color.lit&1)||(color.ambientVertex&1)||
       !draw.vertexCount||!stride||draw.vertexCount>draw.vertices.size()/stride)return false;
    std::array<std::uint8_t,3> proven{};
    for(unsigned i=0;i<draw.vertexCount;++i){
        DecodedVertex vertex;if(!draw.vertex(i,vertex))return false;
        if((color.vertex&1)&&!(vertex.present&(1u<<GX_VA_CLR0)))return false;
        for(unsigned c=0;c<3;++c){
            const float f=(color.vertex&1)?vertex.color[0][c]:color.material[c];
            if(f!=0&&f!=1)return false; // Also rejects nonfinite/non-byte values.
            const auto byte=std::uint8_t(f==1?255:0);
            if(!i)proven[c]=byte;
            else if(proven[c]!=byte)return false;
        }
    }
    value=proven;return true;
}

}
