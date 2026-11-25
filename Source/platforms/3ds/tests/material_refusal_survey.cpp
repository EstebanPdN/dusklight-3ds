// Host survey of the PICA material planner over material lists (one material
// per line: four tab-separated header fields, then the numbers of
// material_equivalence.hpp's survey format). For every material the planner
// runs with the Alpha 12 operation rules off and on; refusals are counted by
// planner error and, for MaterialPlanError::operation, by rule. Materials
// count once each (no draw weighting). With --group, counts are also given
// per group: the stage directory of a /res/Stage/<stage>/ archive, else the
// first header field. TEV state only; no image or game data is printed.
#include "material_equivalence.hpp"
#include <cstring>
#include <fstream>
#include <map>
#include <string>
using namespace material_test;
namespace {
struct Counts {
    unsigned materials=0,planned=0,added=0;
    std::array<unsigned,12> errors{};
    std::array<unsigned,unsigned(MaterialOperationReason::count)> reasons{};
    std::array<unsigned,MaterialLowering::count> addedBits{};
};
// As the draw path's liveness retry (gx::discardUnobservedTevAlpha) for an
// opaque material whose alpha test always passes: alpha unread by RGB is dead.
bool alwaysPasses(int c0,int r0,int op,int c1,int r1){
    auto t=[](int c,int r,int a){switch(c){case 0:return false;case 1:return a<r;case 2:return a==r;case 3:return a<=r;
        case 4:return a>r;case 5:return a!=r;case 6:return a>=r;default:return true;}};
    for(int a=0;a<256;++a){
        const bool x=t(c0,r0,a),y=t(c1,r1,a);bool v;
        switch(op){case 0:v=x&&y;break;case 1:v=x||y;break;case 2:v=x!=y;break;default:v=x==y;}
        if(!v)return false;
    }
    return true;
}
bool discardAlpha(MaterialState& s){
    for(unsigned i=0;i<s.stageCount;++i)for(auto in:s.stages[i].color)if(in<8&&(in&1))return false;
    for(unsigned i=0;i<s.stageCount;++i){s.stages[i].alpha={7,7,7,std::uint8_t(i?0:7)};s.stages[i].alphaOp={0,0,0,1,0};}
    return true;
}
bool plan(const MaterialState& s,bool liveAlpha,MaterialPolicy policy,PicaMaterialPlan& p,MaterialPlanError& e,MaterialOperationReason& r){
    if(planPicaMaterial(s,p,&e,policy,&r))return true;
    MaterialState d=s;
    if(!liveAlpha&&discardAlpha(d)){
        MaterialPlanError e2;MaterialOperationReason r2;
        if(planPicaMaterial(d,p,&e2,policy,&r2)){e=e2;r=r2;return true;}
        e=e2;r=r2;
    }
    return false;
}
void tally(Counts& c,bool okay,MaterialPlanError e,MaterialOperationReason r){
    ++c.materials;
    if(okay){++c.planned;return;}
    ++c.errors[std::min(unsigned(e),11u)];
    if(e==MaterialPlanError::operation)++c.reasons[unsigned(r)];
}
void print(const char* label,const Counts& c){
    std::printf("%s: %u materials, %u planned, %u refused",label,c.materials,c.planned,c.materials-c.planned);
    for(unsigned n=1;n<c.errors.size();++n)if(c.errors[n])std::printf(" | %s=%u",materialPlanErrorText(MaterialPlanError(n)),c.errors[n]);
    std::printf("\n  operation by rule:");
    for(unsigned n=1;n<c.reasons.size();++n)if(c.reasons[n])std::printf(" %s=%u",materialOperationReasonText(MaterialOperationReason(n)),c.reasons[n]);
    if(c.added){
        std::printf("\n  added by the operation rules: %u (lowering bits",c.added);
        for(unsigned b=0;b<c.addedBits.size();++b)if(c.addedBits[b])std::printf(" %u:%u",1u<<b,c.addedBits[b]);
        std::printf(")");
    }
    std::printf("\n");
}
std::string groupOf(const std::string& first){
    const auto at=first.find("/Stage/");
    if(at==std::string::npos)return first;
    const auto end=first.find('/',at+7);
    return first.substr(at+7,end==std::string::npos?std::string::npos:end-at-7);
}
}
int main(int argc,char** argv){
    bool group=false,exact=false,list=false;std::uint32_t seed=0x5a17;
    unsigned checked=0,wholeInstead=0,wholeLonger=0;std::array<unsigned,2> wholeStages{};
    for(int file=1;file<argc;++file){
        if(!std::strcmp(argv[file],"--group")){group=true;continue;}
        if(!std::strcmp(argv[file],"--exact")){exact=true;continue;}
        if(!std::strcmp(argv[file],"--list")){list=true;continue;}
        std::ifstream in(argv[file]);
        if(!in){std::fprintf(stderr,"cannot read %s\n",argv[file]);return 1;}
        const auto policy=exact?MaterialPolicy::exactConstants:MaterialPolicy::quantizedConstants;
        Counts before,after;std::map<std::string,std::pair<Counts,Counts>> groups;std::string line;unsigned malformed=0;
        while(std::getline(in,line)){
            std::size_t at=0;
            for(unsigned tab=0;tab<4&&at!=std::string::npos;++tab){at=line.find('\t',at);if(at!=std::string::npos)++at;}
            MaterialState s;
            if(at==std::string::npos||!parseSurveyMaterial(line.c_str()+at,s)){++malformed;continue;}
            // Blend mode and the alpha test (fields 5..10) decide whether alpha is observed.
            int n[10]{};{const char* p=line.c_str()+at;for(int k=0;k<10;++k){char* end;n[k]=int(std::strtol(p,&end,10));p=end;}}
            const bool liveAlpha=n[4]!=0||!alwaysPasses(n[5],n[6],n[7],n[8],n[9]);
            PicaMaterialPlan p,q;MaterialPlanError e,f;MaterialOperationReason r,t;
            setMaterialOperationRules(false);
            const bool okay=plan(s,liveAlpha,policy,p,e,r);
            setMaterialOperationRules(true);
            const bool now=plan(s,liveAlpha,policy,q,f,t);
            // Rules on never lose a plan. A plan may change only where the
            // rules-off plan needed the alpha liveness retry and the rules now
            // plan the material whole (the draw path tries the whole material
            // first): counted, with the stage counts of both.
            if(okay&&!now){std::fprintf(stderr,"plan lost: %s\n",line.substr(0,at).c_str());return 1;}
            if(okay&&!samePlan(p,q)){
                PicaMaterialPlan whole;MaterialPlanError x;MaterialOperationReason y;
                setMaterialOperationRules(false);
                const bool before=planPicaMaterial(s,whole,&x,policy,&y);
                setMaterialOperationRules(true);
                if(before){std::fprintf(stderr,"plan changed: %s\n",line.substr(0,at).c_str());return 1;}
                ++wholeInstead;wholeStages[0]+=p.stageCount;wholeStages[1]+=q.stageCount;
                if(q.stageCount>p.stageCount||q.textureCount>p.textureCount)++wholeLonger;
            }
            const auto key=group?groupOf(line.substr(0,line.find('\t'))):std::string();
            for(auto* c:{&after,group?&groups[key].second:nullptr})if(c){
                tally(*c,now,f,t);
                if(now&&!okay){++c->added;for(unsigned b=0;b<c->addedBits.size();++b)if(q.lowering&(1u<<b))++c->addedBits[b];}
            }
            tally(before,okay,e,r);
            // --list: each material still refused for an operation, with its TEV stages.
            if(list&&!now&&f==MaterialPlanError::operation){
                PicaMaterialPlan third;MaterialPlanError te;MaterialOperationReason tr;
                planMaterialOperationRules(s,third,&te,policy,&tr);
                std::printf("refused %s [%s; third pass: %s %s]",line.substr(0,at-1).c_str(),materialOperationReasonText(t),
                            materialPlanErrorText(te),materialOperationReasonText(tr));
                for(unsigned i=0;i<s.stageCount;++i){
                    const auto& st=s.stages[i];const auto& c=st.colorOp;const auto& a=st.alphaOp;
                    std::printf(" | c%u,%u,%u,%u op%u b%u s%u k%u o%u a%u,%u,%u,%u op%u b%u s%u k%u o%u kc%u ka%u",
                        st.color[0],st.color[1],st.color[2],st.color[3],c.operation,c.bias,c.scale,c.clamp,c.output,
                        st.alpha[0],st.alpha[1],st.alpha[2],st.alpha[3],a.operation,a.bias,a.scale,a.clamp,a.output,st.konstColor,st.konstAlpha);
                }
                std::printf("\n");
            }
            if(group)tally(groups[key].first,okay,e,r);
            // Every added plan is checked against the GX reference evaluator
            // (real arithmetic; the planner's quantized inputs where marked).
            if(now&&!okay&&(q.lowering&MaterialLowering::operationRules)&&!(q.lowering&MaterialLowering::compare)){
                bool evaluable=true;
                for(unsigned i=0;i<s.stageCount;++i){const auto c=s.stages[i].konstColor,a=s.stages[i].konstAlpha;evaluable&=!((c>=8&&c<12)||(a>=8&&a<16));}
                bool half=false;
                for(unsigned i=0;i<s.stageCount;++i)half|=s.stages[i].colorOp.scale==3||s.stages[i].alphaOp.scale==3;
                MaterialState checkedState=s;
                if(!liveAlpha){MaterialPlanError x;MaterialOperationReason y;PicaMaterialPlan z;
                    if(!planPicaMaterial(s,z,&x,policy,&y))discardAlpha(checkedState);}
                if(evaluable&&!half){
                    for(unsigned k=0;k<24;++k){
                        Samples in{};
                        for(auto& v:in.raster)v=double(randomWord(seed)&255)/255;
                        for(auto& v:in.secondary)v=double(randomWord(seed)&255)/255;
                        for(auto& m:in.texture)for(auto& uv:m)for(auto& v:uv)v=double(randomWord(seed)&255)/255;
                        const auto a=gxEvaluate(checkedState,in,q.quantizedConstants),b=picaEvaluate(q,in);
                        for(unsigned c=0;c<4;++c)if((liveAlpha||c<3)&&std::abs(a[c]-b[c])>=1e-12){
                            reportMismatch(checkedState,q,k,c,a[c],b[c]);return 1;
                        }
                    }
                    ++checked;
                }
            }
        }
        std::printf("survey %s (%s policy, %u malformed lines skipped)\n",argv[file],exact?"exact":"quantized",malformed);
        print(" operation rules off",before);
        print(" operation rules on ",after);
        if(group)for(const auto& [name,c]:groups)if(c.first.planned!=c.first.materials){
            std::printf("  group %s: %u materials, refused %u -> %u",name.c_str(),c.first.materials,
                        c.first.materials-c.first.planned,c.second.materials-c.second.planned);
            std::printf(" (operation %u -> %u)",c.first.errors[2],c.second.errors[2]);
            for(unsigned n=1;n<c.first.reasons.size();++n)if(c.first.reasons[n]||c.second.reasons[n])
                std::printf(" %s=%u->%u",materialOperationReasonText(MaterialOperationReason(n)),c.first.reasons[n],c.second.reasons[n]);
            std::printf("\n");
        }
    }
    std::printf("added plans checked against the GX reference evaluator: %u\n",checked);
    std::printf("planned whole instead of after the alpha liveness retry: %u (%u with more PICA stages or textures; stages %u -> %u)\n",
                wholeInstead,wholeLonger,wholeStages[0],wholeStages[1]);
}
