#include "gx_material_pica.hpp"
#include <3ds/types.h>
#include <3ds/gpu/enums.h>
extern "C" {
#include <c3d/texenv.h>
}
#include <cassert>
namespace dusk::native {
namespace {
GPU_TEVSRC source(gx::MaterialSource s){
    switch(s){
    case gx::MaterialSource::primary:return GPU_PRIMARY_COLOR;
    case gx::MaterialSource::texture0:return GPU_TEXTURE0;
    case gx::MaterialSource::texture1:return GPU_TEXTURE1;
    case gx::MaterialSource::texture2:return GPU_TEXTURE2;
    case gx::MaterialSource::previous:return GPU_PREVIOUS;
    case gx::MaterialSource::constant:return GPU_CONSTANT;
    case gx::MaterialSource::buffer:return GPU_PREVIOUS_BUFFER;
    }
    assert(false);return GPU_PREVIOUS;
}
GPU_COMBINEFUNC function(gx::MaterialFunction f){
    switch(f){
    case gx::MaterialFunction::replace:return GPU_REPLACE;
    case gx::MaterialFunction::modulate:return GPU_MODULATE;
    case gx::MaterialFunction::add:return GPU_ADD;
    case gx::MaterialFunction::interpolate:return GPU_INTERPOLATE;
    case gx::MaterialFunction::multiplyAdd:return GPU_MULTIPLY_ADD;
    case gx::MaterialFunction::addSigned:return GPU_ADD_SIGNED;
    case gx::MaterialFunction::subtract:return GPU_SUBTRACT;
    }
    assert(false);return GPU_REPLACE;
}
GPU_TEVOP_RGB rgbOperand(const gx::MaterialInput& in){
    unsigned op=0;
    switch(in.operand){
    case gx::MaterialOperand::color:op=GPU_TEVOP_RGB_SRC_COLOR;break;
    case gx::MaterialOperand::alpha:op=GPU_TEVOP_RGB_SRC_ALPHA;break;
    case gx::MaterialOperand::red:op=GPU_TEVOP_RGB_SRC_R;break;
    case gx::MaterialOperand::green:op=GPU_TEVOP_RGB_SRC_G;break;
    case gx::MaterialOperand::blue:op=GPU_TEVOP_RGB_SRC_B;break;
    }
    return static_cast<GPU_TEVOP_RGB>(op+in.inverse);
}
GPU_TEVOP_A alphaOperand(const gx::MaterialInput& in){
    unsigned op=0;
    switch(in.operand){
    case gx::MaterialOperand::color: // Only unused default operands may be color.
    case gx::MaterialOperand::alpha:op=GPU_TEVOP_A_SRC_ALPHA;break;
    case gx::MaterialOperand::red:op=GPU_TEVOP_A_SRC_R;break;
    case gx::MaterialOperand::green:op=GPU_TEVOP_A_SRC_G;break;
    case gx::MaterialOperand::blue:op=GPU_TEVOP_A_SRC_B;break;
    }
    return static_cast<GPU_TEVOP_A>(op+in.inverse);
}
void channel(C3D_TexEnv* env,C3D_TexEnvMode mode,const gx::PicaMaterialChannel& c){
    C3D_TexEnvSrc(env,mode,source(c.inputs[0].source),source(c.inputs[1].source),source(c.inputs[2].source));
    C3D_TexEnvFunc(env,mode,function(c.function));
    C3D_TexEnvScale(env,mode,static_cast<GPU_TEVSCALE>(c.scale));
    if(mode==C3D_RGB)C3D_TexEnvOpRgb(env,rgbOperand(c.inputs[0]),rgbOperand(c.inputs[1]),rgbOperand(c.inputs[2]));
    else C3D_TexEnvOpAlpha(env,alphaOperand(c.inputs[0]),alphaOperand(c.inputs[1]),alphaOperand(c.inputs[2]));
}
gx::MaterialSource legacy(gx::PicaTevSource s){
    switch(s){
    case gx::PicaTevSource::primary:return gx::MaterialSource::primary;
    case gx::PicaTevSource::texture0:return gx::MaterialSource::texture0;
    case gx::PicaTevSource::previous:return gx::MaterialSource::previous;
    case gx::PicaTevSource::constant:return gx::MaterialSource::constant;
    }
    assert(false);return gx::MaterialSource::previous;
}
}
PreparedPicaMaterial preparePicaMaterial(const gx::PicaMaterialPlan& p){
    assert(p.stageCount>0&&p.stageCount<=6);
    PreparedPicaMaterial result;
    for(auto& env:result.stages)C3D_TexEnvInit(&env);
    result.bufferUpdateRgb=p.bufferUpdateRgb;
    const auto& b=p.bufferConstant;
    result.bufferColor=u32(b[0])|(u32(b[1])<<8)|(u32(b[2])<<16)|(u32(b[3])<<24);
    for(unsigned i=0;i<p.stageCount;++i){
        auto* env=&result.stages[i];const auto& stage=p.stages[i];
        channel(env,C3D_RGB,stage.rgb);channel(env,C3D_Alpha,stage.alpha);
        C3D_TexEnvColor(env,u32(stage.constant[0])|(u32(stage.constant[1])<<8)|(u32(stage.constant[2])<<16)|(u32(stage.constant[3])<<24));
    }
    return result;
}
void applyPreparedPicaMaterial(const PreparedPicaMaterial& p){
    for(unsigned i=0;i<p.stages.size();++i)*C3D_GetTexEnv(i)=p.stages[i];
    C3D_TexEnvBufUpdate(C3D_Both,0);
    if(p.bufferUpdateRgb)C3D_TexEnvBufUpdate(C3D_RGB,p.bufferUpdateRgb);
    C3D_TexEnvBufColor(p.bufferColor);
}
void applyPicaMaterial(const gx::PicaMaterialPlan& p){
    assert(p.stageCount>0&&p.stageCount<=6);
    for(unsigned i=0;i<6;++i)C3D_TexEnvInit(C3D_GetTexEnv(i));
    C3D_TexEnvBufUpdate(C3D_Both,0);
    if(p.bufferUpdateRgb)C3D_TexEnvBufUpdate(C3D_RGB,p.bufferUpdateRgb);
    const auto& b=p.bufferConstant;C3D_TexEnvBufColor(u32(b[0])|(u32(b[1])<<8)|(u32(b[2])<<16)|(u32(b[3])<<24));
    for(unsigned i=0;i<p.stageCount;++i){
        auto* env=C3D_GetTexEnv(i);const auto& stage=p.stages[i];
        channel(env,C3D_RGB,stage.rgb);channel(env,C3D_Alpha,stage.alpha);
        C3D_TexEnvColor(env,u32(stage.constant[0])|(u32(stage.constant[1])<<8)|(u32(stage.constant[2])<<16)|(u32(stage.constant[3])<<24));
    }
}
void applyPicaTev(const gx::PicaTevPlan& original){
    gx::PicaMaterialPlan p{};p.stageCount=original.count;
    for(unsigned t=0;t<p.stageCount;++t){
        auto& stage=p.stages[t];const auto& old=original.stages[t];
        stage.constant={old.constant.r,old.constant.g,old.constant.b,old.constant.a};
        auto translate=[](const gx::PicaTevChannel& from,gx::PicaMaterialChannel& to,bool alpha){
            to.function=from.function==gx::PicaTevFunction::replace?gx::MaterialFunction::replace:gx::MaterialFunction::modulate;
            for(unsigned n=0;n<3;++n)to.inputs[n]={legacy(from.sources[n]),alpha?gx::MaterialOperand::alpha:gx::MaterialOperand::color,false};
        };
        translate(old.rgb,stage.rgb,false);translate(old.alpha,stage.alpha,true);
    }
    applyPicaMaterial(p);
}
}
