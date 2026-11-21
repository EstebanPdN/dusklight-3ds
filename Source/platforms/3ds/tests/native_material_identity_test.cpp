// Plan identity against a reference planner (default: the Alpha 7 wave 1
// one). The switch below turns the Alpha 12 operation rules off with the sum
// rules (they need them), so "off" is still the wave 1 planner. With the sum rules off the current planner must equal it on every
// material: acceptance, error and every plan field. With them on, every
// reference plan stays byte-identical and carries no sum-rule bit; a
// reference refusal either stays the same refusal (cleared output, same
// error) or becomes a plan with a sum-rule bit, checked against the GX
// reference evaluator (exact where no quantized constant is involved).
// Random materials over the whole state space, plus optional survey files
// (one material per line, numbers after the fourth tab).
#include "material_equivalence.hpp"
#include <cstring>
#include <fstream>
#include <string>
namespace identity_test {
using namespace dusk::native::gx;
bool referencePlan(const MaterialState&,PicaMaterialPlan&,MaterialPlanError*,MaterialPolicy);
bool referenceLookup(const MaterialState&,PicaMaterialPlan&,MaterialPlanError*,MaterialPolicy);
bool referenceCarrier(const MaterialState&,PicaMaterialPlan&,MaterialPlanError*);
}
using namespace material_test;
namespace {
struct Tally {
    unsigned materials=0,referencePlans=0,added=0,addedExact=0,addedQuantized=0,unevaluated=0,compares=0,stillRefused=0;
    std::array<unsigned,MaterialLowering::count> bits{};
    double worstQuantized=0,worstReferenceQuantized=0;
};
double quantizedError(const MaterialState& s,const PicaMaterialPlan& p,std::uint32_t& seed,bool quantizedInputs=false){
    double worst=0;
    for(unsigned k=0;k<24;++k){
        Samples in{};
        for(auto& v:in.raster)v=double(randomWord(seed)&255)/255;
        for(auto& v:in.secondary)v=double(randomWord(seed)&255)/255;
        for(auto& m:in.texture)for(auto& uv:m)for(auto& v:uv)v=double(randomWord(seed)&255)/255;
        const auto a=gxEvaluate(s,in,quantizedInputs),b=picaEvaluate(p,in);
        for(unsigned c=0;c<4;++c)worst=std::max(worst,std::abs(a[c]-b[c]));
    }
    return worst;
}
bool halfScale(const MaterialState& s){
    for(unsigned i=0;i<s.stageCount;++i)if(s.stages[i].colorOp.scale==3||s.stages[i].alphaOp.scale==3)return true;
    return false;
}
// The reference evaluator reads dead inputs too and accepts only konst
// selectors the hardware defines (the planner refuses the others where read).
// Alpha compares other than A8 too (a dead stage the Alpha 12 liveness rule
// drops may hold one).
bool evaluable(const MaterialState& s){return referenceEvaluable(s);}
void check(const MaterialState& s,MaterialPolicy policy,Tally& t,std::uint32_t& seed){
    ++t.materials;
    PicaMaterialPlan reference,off,on;MaterialPlanError re,oe,ne;
    const bool r=identity_test::referencePlan(s,reference,&re,policy);
    setMaterialSumRules(false);
    const bool o=planPicaMaterial(s,off,&oe,policy);
    setMaterialSumRules(true);
    const bool n=planPicaMaterial(s,on,&ne,policy);
    auto fail=[&](const char* what){
        std::fprintf(stderr,"Identity failure (%s): reference=%d/%u off=%d/%u on=%d/%u lowering=%u/%u policy=%u\n",what,r,unsigned(re),o,unsigned(oe),n,unsigned(ne),
            unsigned(reference.lowering),unsigned(on.lowering),unsigned(policy));
        reportMismatch(s,on,0,0,0,0);assert(false);
    };
    if(o!=r||oe!=re||!samePlan(off,reference))fail("sum rules off differ from the reference");
    if(r){
        ++t.referencePlans;
        if(!n||ne!=MaterialPlanError::none||!samePlan(on,reference)||(on.lowering&MaterialLowering::sumRules))fail("reference plan changed");
        // For scale: the quantized constants' own error in reference plans.
        if(reference.quantizedConstants&&evaluable(s)&&!(reference.lowering&MaterialLowering::compare)&&halfScale(s))
            t.worstReferenceQuantized=std::max(t.worstReferenceQuantized,quantizedError(s,reference,seed,true));
        return;
    }
    if(!n){
        if(ne!=re||!samePlan(on,PicaMaterialPlan{}))fail("refusal changed");
        ++t.stillRefused;return;
    }
    // Since Alpha 12 the third pass adds plans with an operation-rule bit
    // (test_native_material_operation_rules.py checks them against Alpha 11).
    if(ne!=MaterialPlanError::none||!(on.lowering&(MaterialLowering::sumRules|MaterialLowering::operationRules)))fail("added plan without a sum-rule bit");
    ++t.added;
    for(unsigned b=0;b<t.bits.size();++b)if(on.lowering&(1u<<b))++t.bits[b];
    if(!evaluable(s)){++t.unevaluated;return;}
    // The compare gain mask is exact for 8-bit operands, which the integer
    // hardware always has; this real-arithmetic reference gives computed
    // operands fractions (compareCases checks compares on integral ones).
    if(on.lowering&MaterialLowering::compare){++t.compares;return;}
    if(policy==MaterialPolicy::exactConstants||!on.quantizedConstants){compare(s,on,24);++t.addedExact;}
    // With the planner's own quantized inputs the only remaining error is
    // the half scale's (half a byte where it is applied, amplified by later
    // 2x/4x stages, as in wave 1 plans): the sum rules add none.
    else if(!halfScale(s)){
        if(quantizedError(s,on,seed,true)>=1e-12)fail("added quantized plan differs with quantized inputs");
        ++t.addedQuantized;
    }else t.worstQuantized=std::max(t.worstQuantized,quantizedError(s,on,seed,true));
}
// The raster-carrier routes are not given the sum rules: same as the reference.
void checkCarriers(const MaterialState& s){
    for(auto policy:{MaterialPolicy::exactConstants,MaterialPolicy::quantizedConstants}){
        PicaMaterialPlan a,b;MaterialPlanError ea,eb;
        const bool x=identity_test::referenceLookup(s,a,&ea,policy),y=planRasterLookupMaterial(s,b,&eb,policy);
        assert(x==y&&ea==eb&&samePlan(a,b));
    }
    PicaMaterialPlan a,b;MaterialPlanError ea,eb;
    const bool x=identity_test::referenceCarrier(s,a,&ea),y=planRasterCarrierMaterial(s,b,&eb);
    assert(x==y&&ea==eb&&samePlan(a,b));
}
unsigned pick(std::uint32_t& seed,unsigned modulo){return (randomWord(seed)>>16)%modulo;}
// The whole decoded state space, weighted towards valid TEV and PREV as D.
MaterialState randomMaterial(std::uint32_t& seed){
    MaterialState s{};
    s.stageCount=std::uint8_t(pick(seed,40)?1+pick(seed,6):pick(seed,2)*7);
    s.texgenCount=std::uint8_t(pick(seed,9));s.channelCount=std::uint8_t(pick(seed,8)?1+pick(seed,2):0);
    s.colorValid=std::uint8_t(pick(seed,12)?15:pick(seed,16));s.konstValid=std::uint8_t(pick(seed,12)?15:pick(seed,16));
    s.swapValid=std::uint8_t(pick(seed,12)?15:pick(seed,16));
    s.indirectCount=std::uint8_t(pick(seed,30)==0);
    for(auto& r:s.colors)for(auto& c:r){
        const unsigned kind=pick(seed,10);
        c=std::int16_t(kind<6?int(randomWord(seed)&255):kind<8?(pick(seed,2)?0:255):int(randomWord(seed)%2048)-1024);
    }
    for(auto& r:s.konst)for(auto& c:r)c=std::uint8_t(pick(seed,4)?randomWord(seed)&255:pick(seed,2)*255);
    for(unsigned r=0;r<4;++r){
        if(pick(seed,4))s.swaps[r]={0,1,2,3};
        else for(auto& c:s.swaps[r])c=std::uint8_t(pick(seed,4));
    }
    for(unsigned i=0;i<s.stageCount;++i){
        auto& st=s.stages[i];
        for(auto& v:st.color)v=std::uint8_t(pick(seed,16));
        for(auto& v:st.alpha)v=std::uint8_t(pick(seed,8));
        if(pick(seed,3)==0)st.color[3]=std::uint8_t(pick(seed,4)?0:1);
        if(pick(seed,3)==0)st.alpha[3]=0;
        for(auto* op:{&st.colorOp,&st.alphaOp}){
            const unsigned kind=pick(seed,12);
            op->operation=std::uint8_t(kind<8?0:kind<10?1:8+pick(seed,8));
            op->bias=std::uint8_t(op->operation>=8?0:pick(seed,6)?0:pick(seed,4));
            op->scale=std::uint8_t(op->operation>=8?0:pick(seed,4));
            op->clamp=std::uint8_t(pick(seed,5)!=0);
            op->output=std::uint8_t(pick(seed,5)?0:pick(seed,4));
        }
        st.coordinate=std::uint8_t(pick(seed,8));st.texture=std::uint8_t(pick(seed,8));
        st.textureEnabled=pick(seed,6)!=0;
        st.raster=std::uint8_t(pick(seed,5)?0:pick(seed,2)?1:7);
        st.rasterSwap=std::uint8_t(pick(seed,4)?0:pick(seed,4));st.textureSwap=std::uint8_t(pick(seed,4)?0:pick(seed,4));
        st.konstColor=std::uint8_t(pick(seed,32));st.konstAlpha=std::uint8_t(pick(seed,32));
        st.indirect=pick(seed,40)==0?624u:0u;
    }
    return s;
}
// Valid TEV only (defined sources, swaps, selectors and operations), with
// PREV as D and register outputs often, so that most materials plan.
MaterialState plausibleMaterial(std::uint32_t& seed){
    MaterialState s{};
    s.stageCount=std::uint8_t(pick(seed,4)?2+pick(seed,2):pick(seed,2)?1:4+pick(seed,3));s.texgenCount=8;s.channelCount=1;
    s.colorValid=s.konstValid=s.swapValid=15;
    for(auto& r:s.colors)for(auto& c:r)c=std::int16_t(pick(seed,8)?int(randomWord(seed)&255):int(randomWord(seed)%2048)-1024);
    for(auto& r:s.konst)for(auto& c:r)c=std::uint8_t(randomWord(seed)&255);
    for(unsigned r=0;r<4;++r)s.swaps[r]={0,1,2,3};
    if(pick(seed,4)==0){const auto c=std::uint8_t(pick(seed,4));s.swaps[1]={c,c,c,std::uint8_t(pick(seed,4))};}
    for(unsigned i=0;i<s.stageCount;++i){
        auto& st=s.stages[i];
        for(auto& v:st.color)v=std::uint8_t(pick(seed,16));
        for(auto& v:st.alpha)v=std::uint8_t(pick(seed,8));
        if(pick(seed,2)==0)st.color[3]=0;
        if(pick(seed,2)==0)st.alpha[3]=0;
        for(auto* op:{&st.colorOp,&st.alphaOp}){
            const unsigned kind=pick(seed,16);
            op->operation=std::uint8_t(kind<12?0:kind<14?1:pick(seed,2)?8:14);
            op->bias=std::uint8_t(op->operation>=8||pick(seed,4)?0:2);
            op->scale=std::uint8_t(op->operation>=8?0:pick(seed,3)?pick(seed,3):3);
            op->clamp=std::uint8_t(pick(seed,8)!=0);
            op->output=std::uint8_t(pick(seed,10)?0:1+pick(seed,3));
        }
        st.coordinate=std::uint8_t(pick(seed,3));st.texture=std::uint8_t(pick(seed,4));
        st.textureEnabled=pick(seed,8)!=0;st.raster=std::uint8_t(pick(seed,8)?0:7);
        st.textureSwap=std::uint8_t(pick(seed,4)?0:1);st.rasterSwap=std::uint8_t(pick(seed,6)?0:1);
        st.konstColor=std::uint8_t(pick(seed,8)?12+pick(seed,20):pick(seed,8));
        st.konstAlpha=std::uint8_t(pick(seed,8)?16+pick(seed,16):pick(seed,8));
    }
    return s;
}
void print(const char* what,const Tally& t){
    std::printf("%s: %u materials, %u reference plans unchanged, %u refusals unchanged, %u plans added (%u checked exact, %u exact with quantized inputs, "
        "%u with a compare and %u with undefined konst selectors unevaluated); bits",
        what,t.materials,t.referencePlans,t.stillRefused,t.added,t.addedExact,t.addedQuantized,t.compares,t.unevaluated);
    for(unsigned b=0;b<t.bits.size();++b)if(t.bits[b])std::printf(" %u:%u",1u<<b,t.bits[b]);
    std::printf("; with a half scale, worst error %.3f bytes in added plans, %.3f in reference plans\n",t.worstQuantized*255,t.worstReferenceQuantized*255);
}
}
int main(int argc,char** argv){
    // Materials and evaluation samples draw from separate sequences, so the
    // materials do not depend on which plans get evaluated.
    std::uint32_t seed=0x5eed2,samples=0x51de;
    Tally random[2];
    for(unsigned n=0;n<150000;++n){
        const auto s=randomMaterial(seed);
        check(s,MaterialPolicy::exactConstants,random[0],samples);
        check(s,MaterialPolicy::quantizedConstants,random[1],samples);
        if(n%8==0)checkCarriers(s);
    }
    print("PASS random materials, exact policy",random[0]);
    print("PASS random materials, quantized policy",random[1]);
    assert(random[0].added&&random[1].added&&random[0].referencePlans&&random[1].referencePlans);
    Tally plausible[2];
    for(unsigned n=0;n<300000;++n){
        const auto s=plausibleMaterial(seed);
        check(s,MaterialPolicy::exactConstants,plausible[0],samples);
        check(s,MaterialPolicy::quantizedConstants,plausible[1],samples);
    }
    print("PASS valid-TEV random materials, exact policy",plausible[0]);
    print("PASS valid-TEV random materials, quantized policy",plausible[1]);
    assert(plausible[0].referencePlans>3000&&plausible[1].referencePlans>10000&&plausible[0].added>500&&plausible[1].addedQuantized>1000);
    for(int file=1;file<argc;++file){
        std::ifstream in(argv[file]);
        if(!in){std::fprintf(stderr,"cannot read %s\n",argv[file]);return 1;}
        Tally survey[2];unsigned malformed=0;std::string line;
        while(std::getline(in,line)){
            std::size_t at=0;
            for(unsigned tab=0;tab<4&&at!=std::string::npos;++tab){at=line.find('\t',at);if(at!=std::string::npos)++at;}
            MaterialState s;
            if(at==std::string::npos||!parseSurveyMaterial(line.c_str()+at,s)){++malformed;continue;}
            check(s,MaterialPolicy::exactConstants,survey[0],samples);
            check(s,MaterialPolicy::quantizedConstants,survey[1],samples);
            checkCarriers(s);
        }
        std::printf("survey %s (%u malformed lines skipped)\n",argv[file],malformed);
        print("PASS survey, exact policy",survey[0]);
        print("PASS survey, quantized policy",survey[1]);
    }
    std::printf("{\"random_materials\":150000,\"valid_tev_materials\":300000,\"exact_reference_plans\":%u,\"exact_added\":%u,\"quantized_reference_plans\":%u,\"quantized_added\":%u,\"fixed_point_gpu_verified\":false}\n",
        random[0].referencePlans+plausible[0].referencePlans,random[0].added+plausible[0].added,
        random[1].referencePlans+plausible[1].referencePlans,random[1].added+plausible[1].added);
}
