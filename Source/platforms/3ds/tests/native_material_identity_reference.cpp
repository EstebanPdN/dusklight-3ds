// A reference copy of the material planner (Tools/3ds/test_native_material_identity.py
// copies it to <output>/reference/), compiled in its own namespace, with its
// plans converted to the current types field by field.
#include "../runtime/gx_pica_material.hpp"
#include <algorithm>
#include <cstring>
#define dusk dusk_reference
#include "reference/gx_pica_material.cpp"
#undef dusk
namespace identity_test {
using namespace dusk::native::gx;
namespace R=dusk_reference::native::gx;
namespace {
R::MaterialState referenceState(const MaterialState& state){
    static_assert(sizeof(R::MaterialState)==sizeof(MaterialState));
    R::MaterialState s;std::memcpy(&s,&state,sizeof s);return s;
}
void convert(const R::PicaMaterialPlan& p,PicaMaterialPlan& out){
    out={};
    auto channel=[](const R::PicaMaterialChannel& c,PicaMaterialChannel& d){
        d.function=MaterialFunction(unsigned(c.function));d.scale=c.scale;
        for(unsigned n=0;n<3;++n)
            d.inputs[n]={MaterialSource(unsigned(c.inputs[n].source)),MaterialOperand(unsigned(c.inputs[n].operand)),c.inputs[n].inverse};
    };
    for(unsigned i=0;i<p.stages.size();++i){
        channel(p.stages[i].rgb,out.stages[i].rgb);channel(p.stages[i].alpha,out.stages[i].alpha);
        out.stages[i].constant=p.stages[i].constant;
    }
    for(unsigned i=0;i<p.textures.size();++i)out.textures[i]={p.textures[i].map,p.textures[i].coordinate};
    out.stageCount=p.stageCount;out.textureCount=p.textureCount;out.bufferConstant=p.bufferConstant;
    out.bufferUpdateRgb=p.bufferUpdateRgb;out.bufferUpdateAlpha=p.bufferUpdateAlpha;out.rasterReadMask=p.rasterReadMask;
    out.projectiveTexture0=p.projectiveTexture0;out.quantizedConstants=p.quantizedConstants;
    out.rasterCarrier=RasterCarrier(unsigned(p.rasterCarrier));out.lowering=p.lowering;
}
}
bool referencePlan(const MaterialState& state,PicaMaterialPlan& out,MaterialPlanError* error,MaterialPolicy policy){
    R::PicaMaterialPlan p;R::MaterialPlanError e;
    const bool okay=R::planPicaMaterial(referenceState(state),p,&e,R::MaterialPolicy(unsigned(policy)));
    convert(p,out);if(error)*error=MaterialPlanError(unsigned(e));
    return okay;
}
bool referenceLookup(const MaterialState& state,PicaMaterialPlan& out,MaterialPlanError* error,MaterialPolicy policy){
    R::PicaMaterialPlan p;R::MaterialPlanError e;
    const bool okay=R::planRasterLookupMaterial(referenceState(state),p,&e,R::MaterialPolicy(unsigned(policy)));
    convert(p,out);if(error)*error=MaterialPlanError(unsigned(e));
    return okay;
}
bool referenceCarrier(const MaterialState& state,PicaMaterialPlan& out,MaterialPlanError* error){
    R::PicaMaterialPlan p;R::MaterialPlanError e;
    const bool okay=R::planRasterCarrierMaterial(referenceState(state),p,&e);
    convert(p,out);if(error)*error=MaterialPlanError(unsigned(e));
    return okay;
}
}
