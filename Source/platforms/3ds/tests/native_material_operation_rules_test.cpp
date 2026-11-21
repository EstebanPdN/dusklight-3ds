// Alpha 12 operation rules against the Alpha 11 planner (the reference, see
// Tools/3ds/test_native_material_operation_rules.py). With the operation rules
// off the planner must equal the reference on every material: acceptance,
// error and every plan field. With them on, every reference plan stays
// byte-identical; a reference refusal either stays the same refusal (cleared
// output, same error) or becomes a plan with an operation-rule bit, which
// must match the GX reference evaluator: exactly (1e-12) where no quantized
// constant is involved, exactly with the planner's own quantized inputs
// otherwise (no divide-by-2 scale), within the divide-by-2 bound with it.
// Every operation refusal names its rule; no other refusal does.
// Materials: the whole decoded state space, valid TEV, programs built to
// reach the new rules (register lifetimes, clamp-off intermediates, ADDHALF),
// and optional survey files (one material per line, numbers after the
// fourth tab).
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
    unsigned materials=0,referencePlans=0,added=0,addedExact=0,addedQuantized=0,addedHalf=0,unevaluated=0,compares=0,stillRefused=0;
    std::array<unsigned,MaterialLowering::count> bits{};
    std::array<unsigned,unsigned(MaterialOperationReason::count)> reasons{};
    double worstHalf=0;
};
// The reference evaluator reads dead inputs too: it accepts only konst
// selectors the hardware defines and alpha compares in the A8 form (a dead
// stage the rules drop may hold others).
bool evaluable(const MaterialState& s){return referenceEvaluable(s);}
bool halfScale(const MaterialState& s){
    for(unsigned i=0;i<s.stageCount;++i)if(s.stages[i].colorOp.scale==3||s.stages[i].alphaOp.scale==3)return true;
    return false;
}
double quantizedError(const MaterialState& s,const PicaMaterialPlan& p,std::uint32_t& seed){
    double worst=0;
    for(unsigned k=0;k<24;++k){
        Samples in{};
        for(auto& v:in.raster)v=double(randomWord(seed)&255)/255;
        for(auto& v:in.secondary)v=double(randomWord(seed)&255)/255;
        for(auto& m:in.texture)for(auto& uv:m)for(auto& v:uv)v=double(randomWord(seed)&255)/255;
        const auto a=gxEvaluate(s,in,true),b=picaEvaluate(p,in);
        for(unsigned c=0;c<4;++c)worst=std::max(worst,std::abs(a[c]-b[c]));
    }
    return worst;
}
// The GX rewrites alone, in exact real arithmetic: the rewritten program's
// output equals the original's (RGB and alpha) on random samples.
unsigned programsChecked=0;
void checkProgram(const MaterialState& s,std::uint32_t& seed){
    if(!s.stageCount||s.stageCount>16||!referenceEvaluable(s))return;
    for(unsigned i=0;i<s.stageCount;++i)if(s.stages[i].indirect)return;
    for(unsigned variant=0;variant<lowerMaterialVariants;++variant){
    MaterialState lowered;std::uint16_t rules=0;lowerMaterialProgram(s,lowered,&rules,variant);
    if(!rules)return;
    assert(lowered.stageCount>=1&&lowered.stageCount<=s.stageCount);
    ++programsChecked;
    for(unsigned k=0;k<8;++k){
        Samples in{};
        auto v=[&]{return k==0?0.0:k==1?1.0:double(randomWord(seed)&255)/255;};
        for(auto& x:in.raster)x=v();
        for(auto& x:in.secondary)x=v();
        for(auto& m:in.texture)for(auto& uv:m)for(auto& x:uv)x=v();
        const auto a=gxEvaluate(s,in),b=gxEvaluate(lowered,in);
        for(unsigned c=0;c<4;++c)if(std::abs(a[c]-b[c])>=1e-12){
            std::fprintf(stderr,"Program rewrite changed the output: component %u %.17g -> %.17g rules %u\n",c,a[c],b[c],unsigned(rules));
            reportMismatch(s,PicaMaterialPlan{},k,c,a[c],b[c]);assert(false);
        }
    }
    }
}
void check(const MaterialState& s,MaterialPolicy policy,Tally& t,std::uint32_t& seed){
    ++t.materials;
    if(policy==MaterialPolicy::exactConstants)checkProgram(s,seed);
    PicaMaterialPlan reference,off,on;MaterialPlanError re,oe,ne;MaterialOperationReason oreason,nreason;
    const bool r=identity_test::referencePlan(s,reference,&re,policy);
    setMaterialOperationRules(false);
    const bool o=planPicaMaterial(s,off,&oe,policy,&oreason);
    setMaterialOperationRules(true);
    const bool n=planPicaMaterial(s,on,&ne,policy,&nreason);
    auto fail=[&](const char* what){
        std::fprintf(stderr,"Operation-rule identity failure (%s): reference=%d/%u off=%d/%u on=%d/%u lowering=%u/%u policy=%u reason=%s\n",what,r,unsigned(re),o,unsigned(oe),n,unsigned(ne),
            unsigned(reference.lowering),unsigned(on.lowering),unsigned(policy),materialOperationReasonText(nreason));
        reportMismatch(s,on,0,0,0,0);assert(false);
    };
    if(o!=r||oe!=re||!samePlan(off,reference))fail("operation rules off differ from the reference");
    // Reasons: only operation refusals carry one, and they always do; the
    // rules do not change the first pass's reason.
    if(o||oe!=MaterialPlanError::operation){if(oreason!=MaterialOperationReason::none)fail("reason without an operation refusal");}
    else if(oreason==MaterialOperationReason::none||oreason>=MaterialOperationReason::count)fail("operation refusal without a reason");
    if(!n&&nreason!=oreason)fail("reason changed by the rules");
    if(r){
        ++t.referencePlans;
        if(!n||ne!=MaterialPlanError::none||!samePlan(on,reference)||(on.lowering&MaterialLowering::operationRules))fail("reference plan changed");
        return;
    }
    if(!n){
        if(ne!=re||!samePlan(on,PicaMaterialPlan{}))fail("refusal changed");
        if(ne==MaterialPlanError::operation)++t.reasons[unsigned(nreason)];
        ++t.stillRefused;return;
    }
    if(ne!=MaterialPlanError::none||!(on.lowering&MaterialLowering::operationRules))fail("added plan without an operation-rule bit");
    if(on.stageCount>on.stages.size()||on.textureCount>3)fail("added plan outside the PICA budget");
    ++t.added;
    for(unsigned b=0;b<t.bits.size();++b)if(on.lowering&(1u<<b))++t.bits[b];
    if(!evaluable(s)){++t.unevaluated;return;}
    // The compare gain mask is exact for 8-bit operands only (compareCases).
    if(on.lowering&MaterialLowering::compare){++t.compares;return;}
    if(!on.quantizedConstants){compare(s,on,24);sweep(s,on,17);++t.addedExact;}
    else if(!halfScale(s)){
        if(quantizedError(s,on,seed)>=1e-12)fail("added quantized plan differs with quantized inputs");
        ++t.addedQuantized;
    }else{
        // The divide-by-2 approximation of the first passes (half a byte
        // per halving, amplified by later gains and factor uses; wave 1
        // plans reach about four bytes): recorded, with a coarse sanity
        // limit. The rewrite itself is checked exactly in checkProgram.
        const double e=quantizedError(s,on,seed);
        t.worstHalf=std::max(t.worstHalf,e);
        if(e*255>16)fail("added divide-by-2 plan beyond sixteen bytes");
        ++t.addedHalf;
    }
}
unsigned pick(std::uint32_t& seed,unsigned modulo){return (randomWord(seed)>>16)%modulo;}
// The whole decoded state space (as the wave 1 identity test).
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
            op->bias=std::uint8_t(op->operation>=8?0:pick(seed,6)?0:pick(seed,3));
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
// Valid TEV that reaches the new rules: register writes (often dead or one
// at a time), clamp-off intermediates read as D, ADDHALF, scaled registers.
MaterialState ruleMaterial(std::uint32_t& seed){
    MaterialState s{};
    s.stageCount=std::uint8_t(2+pick(seed,5));s.texgenCount=8;s.channelCount=1;
    s.colorValid=s.konstValid=s.swapValid=15;
    for(auto& r:s.colors)for(auto& c:r)c=std::int16_t(pick(seed,6)?int(randomWord(seed)&255):pick(seed,2)?0:int(randomWord(seed)%512)-256);
    for(auto& r:s.konst)for(auto& c:r)c=std::uint8_t(randomWord(seed)&255);
    for(unsigned r=0;r<4;++r)s.swaps[r]={0,1,2,3};
    for(unsigned i=0;i<s.stageCount;++i){
        auto& st=s.stages[i];
        // Inputs: registers often, stable sources otherwise.
        for(auto& v:st.color)v=std::uint8_t(pick(seed,3)?pick(seed,8):8+pick(seed,8));
        for(auto& v:st.alpha)v=std::uint8_t(pick(seed,3)?pick(seed,4):4+pick(seed,4));
        if(pick(seed,3)==0){st.color[2]=15;}
        if(pick(seed,2)==0)st.color[3]=std::uint8_t(pick(seed,2)?0:2*(1+pick(seed,3)));
        if(pick(seed,2)==0)st.alpha[3]=std::uint8_t(pick(seed,2)?0:1+pick(seed,3));
        for(auto* op:{&st.colorOp,&st.alphaOp}){
            const unsigned kind=pick(seed,20);
            op->operation=std::uint8_t(kind<17?0:kind<19?1:pick(seed,2)?8:14);
            op->bias=std::uint8_t(op->operation>=8?0:pick(seed,3)?0:1+pick(seed,2));
            op->scale=std::uint8_t(op->operation>=8?0:pick(seed,3)?pick(seed,3):3);
            op->clamp=std::uint8_t(pick(seed,3)!=0);
            op->output=std::uint8_t(pick(seed,2)?0:1+pick(seed,3));
        }
        st.coordinate=std::uint8_t(pick(seed,3));st.texture=std::uint8_t(pick(seed,3));
        st.textureEnabled=pick(seed,8)!=0;st.raster=std::uint8_t(pick(seed,8)?0:7);
        st.konstColor=std::uint8_t(pick(seed,8)?12+pick(seed,20):pick(seed,2)*1);
        st.konstAlpha=std::uint8_t(pick(seed,8)?16+pick(seed,16):0);
    }
    return s;
}
void print(const char* what,const Tally& t){
    std::printf("%s: %u materials, %u reference plans unchanged, %u refusals unchanged, %u plans added (%u checked exact, %u exact with quantized inputs, "
        "%u with a divide-by-2 within %.3f bytes, %u with a compare and %u with undefined konst selectors unevaluated); bits",
        what,t.materials,t.referencePlans,t.stillRefused,t.added,t.addedExact,t.addedQuantized,t.addedHalf,t.worstHalf*255,t.compares,t.unevaluated);
    for(unsigned b=0;b<t.bits.size();++b)if(t.bits[b])std::printf(" %u:%u",1u<<b,t.bits[b]);
    std::printf("; operation refusals by rule");
    for(unsigned n=1;n<t.reasons.size();++n)if(t.reasons[n])std::printf(" %s=%u",materialOperationReasonText(MaterialOperationReason(n)),t.reasons[n]);
    std::printf("\n");
}
}
int main(int argc,char** argv){
    std::uint32_t seed=0xa12e,samples=0x5a12;
    Tally random[2];
    for(unsigned n=0;n<100000;++n){
        const auto s=randomMaterial(seed);
        check(s,MaterialPolicy::exactConstants,random[0],samples);
        check(s,MaterialPolicy::quantizedConstants,random[1],samples);
    }
    print("PASS random materials, exact policy",random[0]);
    print("PASS random materials, quantized policy",random[1]);
    Tally rules[2];
    for(unsigned n=0;n<300000;++n){
        const auto s=ruleMaterial(seed);
        check(s,MaterialPolicy::exactConstants,rules[0],samples);
        check(s,MaterialPolicy::quantizedConstants,rules[1],samples);
    }
    print("PASS register/clamp/bias materials, exact policy",rules[0]);
    print("PASS register/clamp/bias materials, quantized policy",rules[1]);
    // Each rule must actually be reached by the generated materials.
    for(auto bit:{MaterialLowering::addHalf,MaterialLowering::saturateIntermediate,MaterialLowering::registerLiveness,MaterialLowering::scaledRegister}){
        unsigned index=0;while((1u<<index)!=bit)++index;
        assert(rules[1].bits[index]>0);
    }
    assert(rules[0].addedExact>1000&&rules[1].added>rules[0].added);
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
        }
        std::printf("survey %s (%u malformed lines skipped)\n",argv[file],malformed);
        print("PASS survey, exact policy",survey[0]);
        print("PASS survey, quantized policy",survey[1]);
    }
    std::printf("GX program rewrites checked exact (real arithmetic, RGB and alpha): %u materials\n",programsChecked);
    assert(programsChecked>10000);
    std::printf("{\"random_materials\":100000,\"rule_materials\":300000,\"exact_added\":%u,\"quantized_added\":%u,\"fixed_point_gpu_verified\":false}\n",
        random[0].added+rules[0].added,random[1].added+rules[1].added);
}
