#include "material_equivalence.hpp"
#include "../runtime/gx_material_liveness.hpp"
#include "../runtime/gx_fog.hpp"
#include "raster_carrier_fixture.hpp"
#include "color_lookup_test.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <utility>
using namespace material_test;
static MaterialState baseline(){
    MaterialState s{};s.stageCount=1;s.texgenCount=3;s.channelCount=1;s.colorValid=15;s.konstValid=15;s.swapValid=15;
    for(unsigned r=0;r<4;++r){s.swaps[r]={0,1,2,3};for(unsigned c=0;c<4;++c){s.colors[r][c]=(37+41*r+29*c)%256;s.konst[r][c]=(53+31*r+23*c)%256;}}
    for(auto& stage:s.stages){stage.color={8,15,15,15};stage.alpha={4,7,7,7};stage.colorOp.clamp=stage.alphaOp.clamp=1;stage.textureEnabled=true;stage.konstColor=12;stage.konstAlpha=28;}
    return s;
}
static void reject(MaterialState s,MaterialPlanError expected){
    PicaMaterialPlan p{};p.stageCount=6;p.textureCount=3;MaterialPlanError e{};
    const bool okay=planPicaMaterial(s,p,&e);
    if(okay||e!=expected){
        std::fprintf(stderr,"Unexpected rejection result accepted=%d error=%u expected=%u stages=%u\n",okay,unsigned(e),unsigned(expected),s.stageCount);
        for(unsigned i=0;i<s.stageCount;++i){const auto& st=s.stages[i];std::fprintf(stderr,"stage%u color=%u,%u,%u,%u output=%u alpha=%u,%u,%u,%u output=%u\n",i,st.color[0],st.color[1],st.color[2],st.color[3],st.colorOp.output,st.alpha[0],st.alpha[1],st.alpha[2],st.alpha[3],st.alphaOp.output);}
    }
    assert(!okay&&e==expected&&p.stageCount==0&&p.textureCount==0);
    for(const auto& stage:p.stages)assert((stage.constant==std::array<std::uint8_t,4>{}));
}
// Refused by the Alpha 11 rules (same error and an operation rule named),
// planned exactly by the Alpha 12 operation rules.
static void plannedSinceAlpha12(const MaterialState& s,MaterialPlanError before,MaterialPolicy policy=MaterialPolicy::exactConstants){
    setMaterialOperationRules(false);
    PicaMaterialPlan p;MaterialPlanError e;MaterialOperationReason r;
    assert(!planPicaMaterial(s,p,&e,policy,&r)&&e==before);
    assert((e==MaterialPlanError::operation)==(r!=MaterialOperationReason::none));
    setMaterialOperationRules(true);
    assert(planPicaMaterial(s,p,&e,policy,&r)&&e==MaterialPlanError::none&&r==MaterialOperationReason::none);
    assert(p.lowering&MaterialLowering::operationRules);
    if(p.quantizedConstants)compare(s,p,0);else{compare(s,p,1024);sweep(s,p,5);}
}
static void fullRasterCases(){
    colorLookupInterpolationCases();
    auto state=carrier_test::capturedMaterial();
    PicaMaterialPlan plan;MaterialPlanError error;
    assert(planRasterLookupMaterial(state,plan,&error));
    compare(state,plan,32768);
    // Independently varying RGB and alpha, both raster selectors, component
    // broadcasts and texture alpha must survive the delayed RGBA buffer.
    for(unsigned selected=0;selected<2;++selected)for(unsigned component=0;component<4;++component){
        auto sample=state;sample.stages[0].raster=selected;sample.stages[1].raster=1-selected;
        sample.swaps[0]={std::uint8_t(component),std::uint8_t(component),std::uint8_t(component),std::uint8_t(3-component)};
        sample.stages[0].alpha={7,7,7,5};sample.stages[1].alpha={7,0,5,7};
        sample.stages[2].alpha={7,0,4,7};
        assert(planRasterLookupMaterial(sample,plan,&error));compare(sample,plan,8192);
    }
    auto untextured=state;untextured.stages[2].textureEnabled=false;
    untextured.stages[2].color={15,12,0,2};untextured.stages[2].alpha={7,7,7,0};
    assert(planRasterLookupMaterial(untextured,plan,&error));compare(untextured,plan,8192);
    auto tooMany=state;tooMany.stages[1].textureEnabled=true;tooMany.stages[1].texture=1;tooMany.stages[1].coordinate=0;tooMany.stages[1].color={8,0,10,15};
    assert(!planRasterLookupMaterial(tooMany,plan,&error)&&error==MaterialPlanError::textureUnits&&!plan.stageCount);
    auto indirect=state;indirect.indirectCount=1;
    assert(!planRasterLookupMaterial(indirect,plan,&error)&&error==MaterialPlanError::indirect&&!plan.stageCount);
    std::puts("PASS two-raster RGBA fragment equations, independent interpolation, alpha/swaps and bounded refusal");
}
static Samples randomSamples(std::uint32_t& seed,unsigned n){
    Samples in{};
    auto v=[&]{return n==0?0.0:n==1?1.0:double(randomWord(seed)&255)/255;};
    for(auto& c:in.raster)c=v();
    for(auto& c:in.secondary)c=v();
    for(auto& map:in.texture)for(auto& uv:map)for(auto& c:uv)c=v();
    return in;
}
static void additiveTextureSplitCases(){
    // The four-texture GX draw uses a normal-generated highlight in the
    // final, additive stage. Supply its already-sampled texels to the GX oracle.
    auto state=baseline();
    state.stageCount=5;state.texgenCount=5;
    state.colors[1]={0,0,0,255};state.colors[2]={0,145,125,255};state.colors[3]={246,105,0,255};
    state.konst[0]={0,0,0,255};state.konst[1]={21,0,0,255};state.konst[2]={199,255,160,255};
    state.stages[0].color={14,12,8,2};state.stages[0].colorOp.scale=1;
    state.stages[0].alpha={7,6,4,7};state.stages[0].konstAlpha=31;state.konst[3][3]=255;
    for(unsigned i=1;i<5;++i)state.stages[i].alpha={0,7,7,7};
    auto& shine=state.stages[1];
    shine.color={15,8,0,15};shine.colorOp.scale=2;shine.texture=shine.coordinate=2;
    auto& tint=state.stages[2];
    tint.color={4,14,0,15};tint.textureEnabled=false;tint.konstColor=13;
    auto& lines=state.stages[3];
    lines.color={15,14,8,0};lines.texture=lines.coordinate=1;lines.konstColor=14;
    auto& tail=state.stages[4];
    tail.color={15,6,8,0};tail.texture=tail.coordinate=3;

    PicaMaterialPlan base,contribution;
    MaterialPlanError error;
    const auto policy=MaterialPolicy::quantizedConstants;
    assert(!planPicaMaterial(state,base,&error,policy)&&error==MaterialPlanError::textureUnits);
    assert(planAdditiveTextureSplit(state,base,contribution,policy));
    assert(base.textureCount==3&&contribution.textureCount==1&&contribution.textures[0].map==3);
    std::uint32_t seed=719;
    for(unsigned n=0;n<8192;++n){
        const auto inputs=randomSamples(seed,n);
        const auto expected=gxEvaluate(state,inputs);
        const auto under=picaEvaluate(base,inputs),over=picaEvaluate(contribution,inputs);
        for(unsigned c=0;c<3;++c)assert(std::abs(expected[c]-std::min(1.0,under[c]+over[c]))<1e-9);
        assert(std::abs(expected[3]-under[3])<1e-9);
    }
    auto invalid=state;invalid.stages[4].colorOp.operation=1;
    assert(!planAdditiveTextureSplit(invalid,base,contribution,policy));
    invalid=state;invalid.stages[4].alpha={7,7,7,4};
    assert(!planAdditiveTextureSplit(invalid,base,contribution,policy));
    invalid=state;invalid.stages[4].texture=0;
    assert(!planAdditiveTextureSplit(invalid,base,contribution,policy));
    invalid=state;invalid.stages[2].colorOp.output=3;
    assert(!planAdditiveTextureSplit(invalid,base,contribution,policy));
    std::puts("PASS four-texture additive split: saturation, alpha, live register and unique map");
}
// The quantized half scale keeps every component within half an output byte
// of GX (real arithmetic). Returns the largest error seen.
static double halfWithin(const MaterialState& s,const PicaMaterialPlan& p,std::uint32_t& seed,unsigned count=4096){
    double worst=0;
    for(unsigned n=0;n<count;++n){
        const auto in=randomSamples(seed,n);
        const auto a=gxEvaluate(s,in),b=picaEvaluate(p,in);
        for(unsigned c=0;c<4;++c){
            const double e=std::abs(a[c]-b[c]);worst=std::max(worst,e);
            if(e>.500001/255){
                std::fprintf(stderr,"Half-scale error sample=%u component=%u gx=%.6f pica=%.6f (bytes) stages=%u/%u\n",n,c,a[c]*255,b[c]*255,s.stageCount,p.stageCount);
                for(unsigned t=0;t<s.stageCount;++t){const auto& st=s.stages[t];std::fprintf(stderr,"stage%u RGB=%u,%u,%u,%u alpha=%u,%u,%u,%u op=%u/%u bias=%u/%u scale=%u/%u clamp=%u/%u\n",t,st.color[0],st.color[1],st.color[2],st.color[3],st.alpha[0],st.alpha[1],st.alpha[2],st.alpha[3],st.colorOp.operation,st.alphaOp.operation,st.colorOp.bias,st.alphaOp.bias,st.colorOp.scale,st.alphaOp.scale,st.colorOp.clamp,st.alphaOp.clamp);}
                assert(false);
            }
        }
    }
    return worst;
}
static unsigned randomOp(std::uint32_t& seed,unsigned modulo){return (randomWord(seed)>>16)%modulo;}
// Divide-by-2 scale on the last stage (where its error is not amplified):
// sums, products, blends, register/PREV D, subtract and subtract-half forms.
static void halveCases(std::uint32_t& seed){
    unsigned accepted=0,lowered=0,attempts=0;double worst=0;
    for(unsigned n=0;n<60000;++n){
        auto s=baseline();s.stageCount=1+randomOp(seed,2);
        for(unsigned t=0;t<s.stageCount;++t){
            auto& st=s.stages[t];st.texture=t;st.coordinate=t;
            for(unsigned i=0;i<4;++i){st.color[i]=randomOp(seed,16);st.alpha[i]=randomOp(seed,8);}
            // HALF has its own bounded suite; keep every other constant exact.
            for(auto& input:st.color)if(input==13)input=12;
            const bool last=t+1==s.stageCount;
            st.colorOp.scale=last&&randomOp(seed,4)?3:randomOp(seed,3);
            st.alphaOp.scale=last&&randomOp(seed,4)?3:randomOp(seed,3);
            st.colorOp.clamp=randomOp(seed,2);st.alphaOp.clamp=randomOp(seed,2);
            st.colorOp.bias=randomOp(seed,5)?0:2;st.alphaOp.bias=randomOp(seed,5)?0:2;
            st.colorOp.operation=!randomOp(seed,5);st.alphaOp.operation=!randomOp(seed,5);
        }
        const auto& last=s.stages[s.stageCount-1];
        if(last.colorOp.scale!=3&&last.alphaOp.scale!=3)continue;
        ++attempts;
        PicaMaterialPlan p;MaterialPlanError e;
        assert(!planPicaMaterial(s,p,&e)); // strict policy never halves
        if(!planPicaMaterial(s,p,&e,MaterialPolicy::quantizedConstants))continue;
        ++accepted;lowered+=bool(p.lowering&MaterialLowering::halve);
        worst=std::max(worst,halfWithin(s,p,seed,16));
    }
    assert(accepted>attempts/10&&lowered>accepted/2);
    // Water shine base (MA09 stage 0): (K + T*V)/2, clamp off. K0 holds
    // the 7/8 konst as the byte 223 so that only the halving errs here.
    auto water=baseline();auto& w=water.stages[0];water.konst[0]={223,223,223,255};
    w.color={15,8,10,14};w.konstColor=12;w.colorOp.scale=3;w.colorOp.clamp=0;
    w.alpha={7,6,5,7};w.konstAlpha=31;
    PicaMaterialPlan p;
    assert(planPicaMaterial(water,p,nullptr,MaterialPolicy::quantizedConstants)&&p.stageCount==2&&(p.lowering&MaterialLowering::halve));
    worst=std::max(worst,halfWithin(water,p,seed,65536));
    // A blend with PREV as D: the blend overwrites PREV, which the wave 1
    // rules refuse; the sum rules keep PREV in the combiner buffer.
    auto prevD=baseline();prevD.stageCount=2;prevD.stages[1].color={8,10,9,0};prevD.stages[1].colorOp.scale=3;
    reject(prevD,MaterialPlanError::operation);
    MaterialPlanError e;
    setMaterialSumRules(false);
    assert(!planPicaMaterial(prevD,p,&e,MaterialPolicy::quantizedConstants)&&e==MaterialPlanError::equation);
    setMaterialSumRules(true);
    assert(planPicaMaterial(prevD,p,&e,MaterialPolicy::quantizedConstants)&&e==MaterialPlanError::none);
    assert((p.lowering&MaterialLowering::previousCapture)&&p.bufferUpdateRgb==1&&p.stageCount==3);
    worst=std::max(worst,halfWithin(prevD,p,seed,8192));
    std::printf("PASS halve scale: %u of %u random last-stage halvings planned (%u new lowerings), MA09 shine base; worst real error %.4f bytes\n",
        accepted,attempts,lowered,worst*255);
}
// Integer PICA combiner model (Citra/Azahar software formulas: truncating
// division, saturating scale). Used where the real-arithmetic model cannot
// form a clean 0/1 compare mask from a computed operand.
using Bytes=std::array<int,4>;
static Bytes toBytes(const Color& c){Bytes b;for(unsigned i=0;i<4;++i)b[i]=int(std::lround(c[i]*255));return b;}
static Bytes picaBytes(const PicaMaterialPlan& p,const Samples& samples){
    Bytes prev{},buffer{},nextBuffer{};
    for(unsigned c=0;c<4;++c)nextBuffer[c]=p.bufferConstant[c];
    for(unsigned t=0;t<p.stageCount;++t){
        const auto& stage=p.stages[t];Bytes next{};
        for(unsigned c=0;c<4;++c){
            const auto& ch=c==3?stage.alpha:stage.rgb;
            auto value=[&](unsigned n){
                const auto& in=ch.inputs[n];Bytes source{};
                switch(in.source){
                case MaterialSource::primary:source=toBytes(samples.raster);break;
                case MaterialSource::previous:source=prev;break;
                case MaterialSource::constant:for(unsigned k=0;k<4;++k)source[k]=stage.constant[k];break;
                case MaterialSource::buffer:source=buffer;break;
                default:{const auto& binding=p.textures[unsigned(in.source)-unsigned(MaterialSource::texture0)];
                    source=toBytes(samples.texture[binding.map][binding.coordinate]);break;}
                }
                int v=0;
                switch(in.operand){case MaterialOperand::color:v=source[c];break;case MaterialOperand::alpha:v=source[3];break;
                case MaterialOperand::red:v=source[0];break;case MaterialOperand::green:v=source[1];break;case MaterialOperand::blue:v=source[2];break;}
                return in.inverse?255-v:v;
            };
            int r=value(0);
            switch(ch.function){
            case MaterialFunction::replace:break;
            case MaterialFunction::modulate:r=r*value(1)/255;break;
            case MaterialFunction::add:r=std::min(255,r+value(1));break;
            case MaterialFunction::interpolate:r=(r*value(2)+value(1)*(255-value(2)))/255;break;
            case MaterialFunction::multiplyAdd:r=std::min(255,(r*value(1)+255*value(2))/255);break;
            case MaterialFunction::addSigned:r=std::clamp(r+value(1)-128,0,255);break;
            case MaterialFunction::subtract:r=std::max(0,r-value(1));break;
            }
            next[c]=std::min(255,r<<ch.scale);
        }
        buffer=nextBuffer;
        if(p.bufferUpdateRgb&(1u<<t))for(unsigned c=0;c<3;++c)nextBuffer[c]=next[c];
        if(p.bufferUpdateAlpha&(1u<<t))nextBuffer[3]=next[3];
        prev=next;
    }
    return prev;
}
// A soft compare (MaterialLowering::softCompare) equals GX except one level
// above its threshold, where a saturated GX result reads D+C/8 (at least
// 223/255). Returns how many components took that value.
static unsigned compareSoft(const MaterialState& state,const PicaMaterialPlan& plan,unsigned count){
    std::uint32_t seed=0x764a219b;unsigned edge=0;
    for(unsigned n=0;n<count;++n){
        Samples samples{};
        auto sample=[&](){return n==0?0.0:n==1?1.0:double(randomWord(seed)&255)/255.0;};
        for(auto& v:samples.raster)v=sample();
        for(auto& v:samples.secondary)v=sample();
        for(auto& map:samples.texture)for(auto& uv:map)for(auto& v:uv)v=sample();
        const auto a=gxEvaluate(state,samples),b=picaEvaluate(plan,samples);
        for(unsigned c=0;c<4;++c){
            if(std::abs(a[c]-b[c])<1e-12)continue;
            assert(a[c]==1.0&&b[c]<1.0&&b[c]>=223/255.0-1e-12);++edge;
        }
    }
    return edge;
}
// GX compare operations. Integral operands (bytes, sums, differences and
// 2x/4x) make the 256x gain mask exact in real arithmetic too.
static void compareCases(std::uint32_t& seed){
    unsigned accepted=0,lowered=0,attempts=0,soft=0,edges=0,deadForms=0;
    for(unsigned n=0;n<80000;++n){
        auto s=baseline();s.stageCount=1+randomOp(seed,3);
        bool any=false;
        for(unsigned t=0;t<s.stageCount;++t){
            auto& st=s.stages[t];st.texture=t;st.coordinate=t;
            for(unsigned i=0;i<4;++i){st.color[i]=randomOp(seed,16);st.alpha[i]=randomOp(seed,8);}
            for(auto& input:st.color)if(input==13)input=12;
            st.colorOp.clamp=randomOp(seed,2);st.alphaOp.clamp=randomOp(seed,2);
            if(randomOp(seed,2)){
                const unsigned mode=randomOp(seed,4);
                st.colorOp.operation=std::uint8_t(8+2*mode+!randomOp(seed,6));any=true;
            }else{
                // Integral non-compare stages: interpolation factor 0 or 1.
                st.color[2]=randomOp(seed,2)?15:12;st.colorOp.scale=randomOp(seed,3);
                st.colorOp.bias=randomOp(seed,4)?0:2;st.colorOp.operation=!randomOp(seed,4);
            }
            if(randomOp(seed,2)){st.alphaOp.operation=std::uint8_t(randomOp(seed,4)?14:8+2*randomOp(seed,4)+randomOp(seed,2));any=true;}
            else{st.alpha[2]=7;st.alphaOp.scale=randomOp(seed,3);}
        }
        if(!any)continue;
        ++attempts;
        PicaMaterialPlan p;MaterialPlanError e;
        if(!planPicaMaterial(s,p,&e,MaterialPolicy::quantizedConstants)){
            // Refusals stay classified: EQ, GR16 and BGR24 forms are operations.
            assert(e==MaterialPlanError::operation||e==MaterialPlanError::stages||e==MaterialPlanError::equation||
                   e==MaterialPlanError::constantConflict||e==MaterialPlanError::constantRange);
            continue;
        }
        bool defined=true;
        for(unsigned t=0;t<s.stageCount;++t){
            const auto& st=s.stages[t];
            for(auto* op:{&st.colorOp,&st.alphaOp})if(op->operation>=8)defined&=!(op->operation&1)&&(op->operation==8||op->operation==14);
            defined&=st.alphaOp.operation<8||st.alphaOp.operation==14;
        }
        // An unsupported compare may only sit in a dead stage channel the
        // Alpha 12 liveness rule dropped (the GX reference cannot evaluate
        // every such form, so these plans are not evaluated here).
        if(!defined){assert(p.lowering&MaterialLowering::registerLiveness);++deadForms;continue;}
        ++accepted;lowered+=bool(p.lowering&MaterialLowering::compare);
        if(p.lowering&MaterialLowering::softCompare){++soft;edges+=compareSoft(s,p,48);}
        else compare(s,p,48);
    }
    assert(accepted>attempts/8&&lowered>accepted/4);
    std::printf("PASS compare operations: %u of %u random materials planned, %u through the gain mask, exact against GX "
                "(%u soft: %u components one level above the threshold; %u more with an unsupported form in a dead stage)\n",accepted,attempts,lowered,soft,edges,deadForms);
}
// The soft compare is used only where it frees the sixth stage, for literal
// C and D that saturate from two levels up, under the quantized policy.
static void softCompareCases(std::uint32_t& seed){
    // Single-stage compare: the full form fits in four stages, stays exact.
    auto s=baseline();s.stages[0].color={8,9,12,14};s.stages[0].colorOp.operation=8;s.konst[0]={230,230,230,255};
    PicaMaterialPlan p;
    assert(planPicaMaterial(s,p,nullptr,MaterialPolicy::quantizedConstants)&&p.stageCount==4&&!(p.lowering&MaterialLowering::softCompare));
    compare(s,p,4096);
    // Integer model over every operand pair: one-level band only.
    unsigned checked=0,banded=0;
    for(unsigned k1:{191u,200u,223u,245u}){
        auto w=baseline();w.stageCount=3;w.texgenCount=2;w.konst[1]={std::uint8_t(k1),std::uint8_t(k1),std::uint8_t(k1),255};
        // Two copy stages put the compare at PICA stage 2: 2+4 fills six.
        w.stages[0].color={8,15,15,15};w.stages[1].color={0,15,15,15};w.stages[1].alpha={7,7,7,0};
        w.stages[1].textureEnabled=false;w.stages[1].raster=7;
        auto& last=w.stages[2];last.texture=1;last.coordinate=1;last.color={8,0,12,14};last.konstColor=13;last.colorOp.operation=8;last.alpha={7,7,7,0};
        assert(planPicaMaterial(w,p,nullptr,MaterialPolicy::quantizedConstants)&&(p.lowering&MaterialLowering::softCompare)&&p.stageCount<=5);
        PicaMaterialPlan strict;assert(planPicaMaterial(w,strict)&&!(strict.lowering&MaterialLowering::softCompare)&&strict.stageCount==6);
        for(unsigned a=0;a<256;a+=3)for(unsigned b=0;b<256;b+=5){
            Samples in{};for(auto& map:in.texture)for(auto& uv:map)uv={b/255.0,b/255.0,b/255.0,1};
            for(auto& uv:in.texture[1])uv={a/255.0,0,0,1};
            const auto bytes=picaBytes(p,in);const auto gx=gxEvaluate(w,in);++checked;
            for(unsigned c=0;c<3;++c){
                const int expected=int(std::lround(gx[c]*255));
                if(int(a)-int(b)==1&&k1<223){++banded;assert(bytes[c]==int(k1)+32&&expected==255);}
                else assert(bytes[c]==expected);
            }
        }
        // Below 191 the band would span two levels: the full gain is kept.
        w.konst[1]={190,190,190,255};
        assert(planPicaMaterial(w,p,nullptr,MaterialPolicy::quantizedConstants)&&!(p.lowering&MaterialLowering::softCompare)&&p.stageCount==6);
    }
    (void)seed;
    std::printf("PASS soft compare: only at the stage limit and for saturating literals; %u integer cases, %u one-level band values\n",checked,banded);
}
// Lowering bits mark exactly the plans Alpha 6 refused.
static void loweringBitCases(std::uint32_t& seed){
    PicaMaterialPlan p;
    // Halved sampled D alone, and a first-stage D in [-255,0]: Alpha 6 forms.
    auto s=baseline();s.stages[0].color={15,15,15,8};s.stages[0].colorOp.scale=3;
    assert(planPicaMaterial(s,p,nullptr,MaterialPolicy::quantizedConstants)&&!p.lowering);halfWithin(s,p,seed,1024);
    s=baseline();s.colors[2]={-10,-200,0,255};s.stages[0].color={8,15,15,4};
    assert(planPicaMaterial(s,p)&&!p.lowering);compare(s,p,1024);
    s.stageCount=2;s.stages[1]=s.stages[0];s.stages[0].color={8,15,15,15};s.stages[1].color={0,15,15,4};s.stages[1].texture=1;s.stages[1].coordinate=1;
    assert(planPicaMaterial(s,p)&&p.lowering==MaterialLowering::signedD);compare(s,p,1024);
    // Clamp off on a bounded last stage (T*K): Alpha 6 proved it, no bit.
    s=baseline();s.stages[0].color={15,8,14,15};s.stages[0].colorOp.clamp=0;
    assert(planPicaMaterial(s,p)&&!p.lowering);compare(s,p,1024);
    // A split RGB stage passing a clamp-off x2 alpha through: Alpha 6 refused.
    s=baseline();s.stages[0].color={10,15,15,15};s.stages[0].colorOp.bias=2;s.stages[0].colorOp.scale=1;
    s.stages[0].alpha={7,6,4,7};s.stages[0].alphaOp.clamp=0;s.stages[0].alphaOp.scale=1;
    assert(planPicaMaterial(s,p,nullptr,MaterialPolicy::quantizedConstants)&&p.lowering==MaterialLowering::passThrough);compare(s,p,1024);
    std::puts("PASS lowering bits: Alpha 6 forms (halved sampled D, first-stage nonpositive D, bounded clamp off) carry none; pass-through marked");
}
// Dungeon minimap icons (d_map_path.cpp setTevSettingIntensityTextureToCI):
// palette index P = C1 + TEX/4, then COMP_R8_GT against C2 keeps P or 0.
static void minimapIconCases(std::uint32_t& seed){
    auto s=baseline();s.stageCount=2;s.texgenCount=1;
    for(unsigned t=0;t<2;++t){auto& st=s.stages[t];st.texture=0;st.coordinate=0;}
    s.stages[0].color={15,14,8,4};s.stages[0].konstColor=6;s.stages[0].alpha={7,7,7,7};
    s.stages[1].color={0,6,0,15};s.stages[1].colorOp.operation=8;s.stages[1].alpha={7,7,7,4};
    PicaMaterialPlan p;
    reject(s,MaterialPlanError::konstFraction);
    unsigned mismatched=0,checked=0;
    for(unsigned variant=0;variant<64;++variant){
        const int c1=int(randomWord(seed)%200),c2=variant&1?c1:c1+4; // doors C1==C2, treasures C2=C1+4
        for(unsigned c=0;c<4;++c){s.colors[2][c]=std::int16_t(c1+c);s.colors[3][c]=std::int16_t(c2+c);} // GX C1, C2
        assert(planPicaMaterial(s,p,nullptr,MaterialPolicy::quantizedConstants));
        assert(p.stageCount==6&&p.textureCount==1&&(p.lowering&MaterialLowering::compare)&&p.bufferUpdateRgb==2);
        for(unsigned texel=0;texel<256;++texel){
            Samples in{};
            for(auto& map:in.texture)for(auto& uv:map)uv={texel/255.0,(255-texel)/255.0,double(texel^0x5a)/255.0,double(texel*7&255)/255.0};
            const auto gx=gxEvaluate(s,in);const auto pica=picaBytes(p,in);
            const double index=std::min(255.0,c1+texel/4.0);
            const bool gxGreater=std::lround(index)>c2;
            const bool picaGreater=pica[0]>0;
            ++checked;
            if(gxGreater!=picaGreater){++mismatched;assert(std::abs(index-c2)<=1.0);continue;}
            for(unsigned c=0;c<3;++c)assert(std::abs(gx[c]*255-pica[c])<=1.0);
            assert(pica[3]==int(std::lround(in.texture[0][0][3]*255)));
        }
    }
    std::printf("PASS minimap icon compare: 6 PICA stages, %u texel/constant cases, %u differ only at the threshold\n",checked,mismatched);
}
// Water shine layer MA09: P = (7/8 + T0*V)/2 (clamp off), then
// K1 + (T1.r > P.r ? 1 : 0); the glint edge may move by the rounding of P.
// The game sets fog type 6 on every MA09 material (d_kankyo.cpp), so the
// plan must leave a stage and a texture unit for the vertex fog stage.
static void waterShineCases(std::uint32_t& seed){
    auto s=baseline();s.stageCount=2;s.texgenCount=2;
    s.konst[1]={200,200,210,255};
    auto& s0=s.stages[0];s0.texture=0;s0.coordinate=0;s0.color={15,8,10,14};s0.konstColor=1;
    s0.colorOp.scale=3;s0.colorOp.clamp=0;s0.alpha={7,6,5,7};s0.konstAlpha=31;
    auto& s1=s.stages[1];s1.texture=1;s1.coordinate=1;s1.color={8,0,12,14};s1.konstColor=13;
    s1.colorOp.operation=8;s1.alpha={7,7,7,0};
    PicaMaterialPlan p;
    reject(s,MaterialPlanError::operation);
    assert(planPicaMaterial(s,p,nullptr,MaterialPolicy::quantizedConstants));
    assert(p.stageCount==5&&p.textureCount==2&&p.lowering==(MaterialLowering::halve|MaterialLowering::compare|MaterialLowering::softCompare));
    {
        VertexFogPlan fog{};fog.enabled=true;fog.function=6;fog.color={255,255,255,255};
        auto fogged=p;
        assert(appendVertexFog(fogged,fog)&&fogged.stageCount==6&&fogged.textureCount==3);
        assert(fogged.stages[5].rgb.function==MaterialFunction::interpolate&&fogged.stages[5].rgb.inputs[1].source==MaterialSource::previous);
    }
    unsigned edge=0,band=0;
    for(unsigned n=0;n<65536;++n){
        const auto in=randomSamples(seed,n);
        const auto gx=gxEvaluate(s,in),pica=picaEvaluate(p,in);const auto bytes=picaBytes(p,in);
        const double t0=in.texture[0][0][0]*255,v=in.raster[0]*255,t1=in.texture[1][1][0]*255;
        const double base=(7.0/8*255+t0*v/255)/2; // GX P.r in bytes
        const bool near=std::abs(t1-std::lround(base))<=2;
        edge+=near;
        // Real arithmetic: alpha exact, RGB exact away from the threshold.
        assert(std::abs(gx[3]-pica[3])<1e-12);
        for(unsigned c=0;c<3;++c){
            assert(pica[c]>=s.konst[1][c]/255.0-1e-12);
            if(!near)assert(std::abs(gx[c]-pica[c])<1e-12);
        }
        // Integer model: K1 or white, never darker; K1+32 one level above
        // the threshold (soft compare); same choice away from it.
        assert(std::abs(bytes[3]-gx[3]*255)<1.0);
        const bool white=bytes[0]==255,dim=bytes[0]==s.konst[1][0];
        if(!white&&!dim){assert(near);++band;}
        for(unsigned c=0;c<3;++c)assert(bytes[c]==(white?255:dim?s.konst[1][c]:s.konst[1][c]+32));
        if(!near)for(unsigned c=0;c<3;++c)assert(std::abs(gx[c]*255-bytes[c])<1e-9);
    }
    // Deterministic integer sweep (the sampled loop above rarely lands one
    // level above the threshold): K1 at or below it, K1+32 one level above,
    // white from two levels; GX agrees away from the threshold.
    for(unsigned t0=0;t0<256;t0+=5)for(unsigned v=0;v<256;v+=15)for(unsigned t1=0;t1<256;++t1){
        Samples in{};in.raster={v/255.0,v/255.0,v/255.0,1};
        for(auto& uv:in.texture[0])uv={t0/255.0,t0/255.0,t0/255.0,1};
        for(auto& uv:in.texture[1])uv={t1/255.0,0,0,1};
        const auto bytes=picaBytes(p,in);const auto gx=gxEvaluate(s,in);
        const int level=int(t1)-int((t0*v/255*128+255*111)/255);
        const bool near=std::abs(int(t1)-int(std::lround((7.0/8*255+t0*v/255.0)/2)))<=2;
        band+=level==1;
        for(unsigned c=0;c<3;++c){
            assert(bytes[c]==(level<=0?s.konst[1][c]:level==1?s.konst[1][c]+32:255));
            if(!near)assert(std::lround(gx[c]*255)==bytes[c]);
        }
    }
    assert(band>0);
    // The full gain form below K1 191 (soft band would span two levels).
    s.konst[1]={180,180,180,255};
    assert(planPicaMaterial(s,p,nullptr,MaterialPolicy::quantizedConstants)&&p.stageCount==6&&!(p.lowering&MaterialLowering::softCompare));
    std::printf("PASS water shine MA09: 5 stages + fog stage, 3 textures with the fog ramp; 65536 fragments, %u within 2 levels of the glint threshold, %u at K1+32\n",edge,band);
}
// Signed or >255 initial registers as D, at any stage, split into one add
// and one subtract per component sign; exact under clamping.
static void signedDCases(std::uint32_t& seed){
    unsigned accepted=0,lowered=0;
    for(unsigned n=0;n<40000;++n){
        auto s=baseline();s.stageCount=1+randomOp(seed,2);
        for(unsigned r=1;r<4;++r)for(unsigned c=0;c<4;++c)s.colors[r][c]=std::int16_t(int(randomWord(seed)%2048)-1024);
        for(unsigned t=0;t<s.stageCount;++t){
            auto& st=s.stages[t];st.texture=t;st.coordinate=t;
            for(unsigned i=0;i<3;++i){st.color[i]=std::uint8_t(8+randomOp(seed,8));st.alpha[i]=std::uint8_t(4+randomOp(seed,4));}
            for(auto& input:st.color)if(input==13)input=12;
            st.color[3]=std::uint8_t(2+randomOp(seed,6));st.alpha[3]=std::uint8_t(1+randomOp(seed,3));
            st.colorOp.scale=randomOp(seed,3);st.alphaOp.scale=randomOp(seed,3);
        }
        PicaMaterialPlan p;
        if(!planPicaMaterial(s,p))continue;
        ++accepted;lowered+=bool(p.lowering&MaterialLowering::signedD);
        compare(s,p,32);
    }
    assert(accepted>1000&&lowered>accepted/2);
    // Freezard eye constants (416,-251,-255) in a register read as D at the
    // second stage.
    auto eye=baseline();eye.stageCount=2;eye.colors[1]={416,-251,-255,255};
    eye.stages[1].texture=1;eye.stages[1].coordinate=1;eye.stages[1].color={0,8,10,2};
    PicaMaterialPlan p;assert(planPicaMaterial(eye,p)&&(p.lowering&MaterialLowering::signedD));compare(eye,p,4096);
    // Clamp off cannot use the clamped split before the last stage (the
    // third stage reads PREV, or the Alpha 12 liveness rule drops stage 1;
    // a possibly negative value cannot be saturated early either).
    eye.stageCount=3;eye.stages[1].colorOp.clamp=0;eye.stages[2].color={8,15,15,0};reject(eye,MaterialPlanError::operation);
    eye.stages[2].color={8,15,15,15};plannedSinceAlpha12(eye,MaterialPlanError::operation);
    std::printf("PASS signed D: %u random materials planned (%u split), freezard eye constants exact\n",accepted,lowered);
}
// Clamp off on the last stage equals the saturated output.
static void finalClampCases(std::uint32_t& seed){
    unsigned accepted=0,saturated=0;
    for(unsigned n=0;n<30000;++n){
        auto s=baseline();s.stageCount=1+randomOp(seed,3);
        for(unsigned t=0;t<s.stageCount;++t){
            auto& st=s.stages[t];st.texture=t;st.coordinate=t;
            for(unsigned i=0;i<4;++i){st.color[i]=randomOp(seed,16);st.alpha[i]=randomOp(seed,8);}
            for(auto& input:st.color)if(input==13)input=12;
            st.colorOp.scale=randomOp(seed,3);st.alphaOp.scale=randomOp(seed,3);
        }
        auto& last=s.stages[s.stageCount-1];last.colorOp.clamp=last.alphaOp.clamp=0;
        PicaMaterialPlan p;
        if(!planPicaMaterial(s,p))continue;
        ++accepted;saturated+=bool(p.lowering&MaterialLowering::finalClamp);
        compare(s,p,32);
    }
    // The bit marks only the stages the Alpha 6 bound proof refused.
    assert(accepted>1000&&saturated>0&&saturated<accepted);
    // Lakebed waterfall pattern: (T + K) x2 with clamp off as the only stage.
    auto fall=baseline();fall.stages[0].color={8,15,15,14};fall.stages[0].colorOp={0,0,1,0,0};
    PicaMaterialPlan p;assert(planPicaMaterial(fall,p)&&p.stageCount==1&&(p.lowering&MaterialLowering::finalClamp));compare(fall,p,4096);
    std::printf("PASS clamp off on the last stage: %u random materials planned against the saturated output (%u needed it)\n",accepted,saturated);
}
// A computed register that reads no earlier result moves to the first
// stage; the combiner buffer holds it for every later reader.
static void hoistCases(std::uint32_t& seed){
    // Terrain (F_SP111 tsuchi) shape: REG2 = T1 + K1.R written at stage 2
    // with a runtime konst, read by stage 3 after PREV kept evolving.
    auto s=baseline();s.stageCount=5;s.texgenCount=3;
    s.konst[1]={170,0,0,0};
    for(unsigned t=0;t<5;++t){auto& st=s.stages[t];st.texture=t<3?t:0;st.coordinate=st.texture;st.textureEnabled=t<3;st.alpha={7,7,7,0};}
    s.stages[0].texture=0;s.stages[0].color={8,15,15,15};s.stages[0].alpha={7,7,7,4};
    s.stages[1].texture=s.stages[1].coordinate=2;s.stages[1].color={0,8,11,15};
    s.stages[2].texture=s.stages[2].coordinate=1;s.stages[2].konstColor=17;
    s.stages[2].color={14,15,15,8};s.stages[2].colorOp.output=2;
    s.stages[3].color={15,0,6,15};s.stages[3].textureEnabled=false;s.stages[3].raster=7;
    s.stages[4].color={15,0,10,15};s.stages[4].colorOp.scale=2;s.stages[4].textureEnabled=false;
    PicaMaterialPlan p;
    assert(planPicaMaterial(s,p)&&(p.lowering&MaterialLowering::hoist));compare(s,p,8192);
    unsigned accepted=0,hoisted=0;
    for(unsigned n=0;n<40000;++n){
        auto m=baseline();m.stageCount=4+randomOp(seed,3);
        for(unsigned t=0;t<m.stageCount;++t){
            auto& st=m.stages[t];st.texture=t%3;st.coordinate=t%3;
            for(unsigned i=0;i<4;++i){st.color[i]=randomOp(seed,16);st.alpha[i]=randomOp(seed,8);}
            for(auto& input:st.color)if(input==13)input=12;
            st.colorOp.scale=randomOp(seed,3);
        }
        const unsigned at=1+randomOp(seed,m.stageCount-2);
        auto& h=m.stages[at];h.colorOp.output=std::uint8_t(1+randomOp(seed,3));h.colorOp.scale=0;h.alpha={7,7,7,0};
        PicaMaterialPlan q;
        if(!planPicaMaterial(m,q))continue;
        ++accepted;hoisted+=bool(q.lowering&MaterialLowering::hoist);
        compare(m,q,32);
    }
    assert(accepted>0&&hoisted>0);
    std::printf("PASS register hoist: terrain shape exact; %u random register materials planned, %u hoisted past stage 1\n",accepted,hoisted);
}
// The second pass changes nothing the first one plans and keeps only plans
// that used a sum rule; a refusal keeps the first pass's error and clears
// the output. Returns 1 when the sum rules added a plan.
// The Alpha 12 operation rules are off here (operationRuleCases and the
// operation-rule identity test cover the third pass).
static unsigned sumRulesOnly(const MaterialState& s,MaterialPolicy policy,PicaMaterialPlan& plan){
    PicaMaterialPlan first;MaterialPlanError e1,e2;
    setMaterialOperationRules(false);
    setMaterialSumRules(false);const bool a=planPicaMaterial(s,first,&e1,policy);
    setMaterialSumRules(true);const bool b=planPicaMaterial(s,plan,&e2,policy);
    setMaterialOperationRules(true);
    if(a){assert(b&&e2==MaterialPlanError::none&&samePlan(first,plan)&&!(plan.lowering&MaterialLowering::sumRules));return 0;}
    if(!b){assert(e1==e2&&samePlan(plan,PicaMaterialPlan{}));return 0;}
    assert(e2==MaterialPlanError::none&&(plan.lowering&MaterialLowering::sumRules));
    return 1;
}
// Measured sum-rule materials (disc TEV state; Alpha 6 tour dumps and the
// wave 1 Azahar run name them), each refused by the wave 1 rules.
static void sumRuleFixtures(std::uint32_t& seed){
    // tolerance (output bytes): the sum rules are exact; the quantized
    // policy's 128/255 for HALF, the 1/2 konst and the half scale (half a
    // byte each) remain, doubled by a later x2 or x4 where one follows.
    struct Fixture {const char* what;const char* numbers;MaterialPolicy policy;std::uint16_t bits;MaterialPlanError first;double tolerance;};
    const Fixture fixtures[]{
        // Freezard eye (E_fz fz_zeye): PREV + lerp(C2, K2, TEX); C1/C2 signed.
        {"freezard eye","2 2 1 0 1 7 0 1 7 0 0 0 0 0 255 255 255 255 416 -251 -255 255 -239 -494 -417 255 193 57 1 255 255 255 255 255 255 85 85 255 255 255 255 255 0 1 2 3 0 1 2 3 0 1 2 3 0 1 2 3 "
            "14 4 8 15 0 0 0 1 0 7 7 7 4 0 0 0 1 0 0 0 1 0 0 0 12 31 6 14 8 0 0 0 0 1 0 7 7 4 0 0 0 0 0 0 1 1 1 0 0 0 14 28",
            MaterialPolicy::exactConstants,MaterialLowering::runningSum,MaterialPlanError::equation,0},
        // Lakebed shoreline (cc_MA06_mizugiwa): halved raster alpha, then
        // alpha (APREV + lerp(TEXA, A0, TEXA)) x2 with clamp off on the last stage.
        {"Lakebed shoreline","2 2 1 0 1 3 50 1 3 180 0 0 0 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 0 1 2 3 0 1 2 3 0 1 2 3 0 1 2 3 "
            "11 15 15 15 0 0 3 1 0 5 4 1 7 0 0 3 1 0 0 0 1 0 0 0 12 28 15 15 8 0 0 0 0 0 0 4 1 4 0 0 0 1 0 0 1 1 1 0 0 0 12 28",
            MaterialPolicy::quantizedConstants,MaterialLowering::runningSum|MaterialLowering::finalClamp,MaterialPlanError::equation,1.000001},
        // Water pillar (ef_mizubashira mat_hashira_soto): alpha APREV + A1*TEXA - 1/2.
        {"water pillar","2 2 1 0 1 6 40 0 3 150 0 0 0 0 255 255 255 255 255 255 255 223 100 100 100 255 255 255 255 255 255 255 255 221 255 255 255 255 255 255 255 255 0 1 2 3 0 1 2 3 0 1 2 3 0 1 2 3 "
            "15 13 10 14 0 0 2 1 0 7 4 6 7 0 0 0 1 0 0 0 1 0 0 0 7 29 15 0 6 15 0 0 0 0 0 7 2 4 0 0 2 0 1 0 1 1 1 0 0 0 12 29",
            MaterialPolicy::quantizedConstants,MaterialLowering::previousCapture,MaterialPlanError::equation,2.000001},
        // Palace of Twilight door pattern (cc_door_moyou_kage): RGB
        // (PREV + lerp(HALF, K, 0) - 1/2) x2, the half folded into ADD_SIGNED.
        {"door pattern","2 2 1 0 1 7 0 1 7 0 0 0 0 0 255 255 255 0 255 255 255 255 255 255 663 97 255 255 255 255 3 255 134 255 255 255 255 255 255 255 255 255 0 1 2 3 0 1 2 3 0 1 2 3 0 1 2 3 "
            "12 14 11 15 0 0 0 1 0 7 5 4 7 0 2 2 1 0 0 0 1 0 0 0 13 28 13 14 15 0 0 2 1 1 0 7 4 0 0 1 0 0 1 0 1 1 1 0 0 0 12 28",
            MaterialPolicy::quantizedConstants,MaterialLowering::biasFold,MaterialPlanError::equation,1.000001},
        // Lv6 warp base (aa_warp00_v): (PREV + TEX*HALF)/2 with clamp off.
        {"Lv6 warp","3 3 1 0 0 7 0 1 7 0 0 0 0 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 0 0 0 255 255 255 255 255 255 255 255 255 0 1 2 3 0 1 2 3 0 1 2 3 0 1 2 3 "
            "15 8 10 15 0 0 0 1 0 7 4 5 7 0 0 0 1 0 0 0 1 0 0 0 12 28 15 8 14 0 0 0 2 1 0 5 7 7 7 0 0 0 0 0 1 1 1 0 0 0 13 28 15 8 13 0 0 0 3 0 0 5 7 7 7 0 0 0 0 0 2 2 1 0 0 0 12 28",
            MaterialPolicy::quantizedConstants,MaterialLowering::halve|MaterialLowering::previousCapture,MaterialPlanError::equation,1.000001},
        // Hyrule Castle barrier base (ganonwall/@bg003e mat01): (PREV + K*TEX)/2, clamp off.
        {"castle barrier","4 3 1 1 3 7 0 1 7 0 0 0 0 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 0 20 20 20 0 255 255 255 0 255 255 255 255 0 1 2 3 0 1 2 3 0 1 2 3 0 1 2 3 "
            "14 15 15 15 0 0 1 1 0 7 7 7 7 0 0 0 1 0 0 0 0 0 0 0 13 28 15 14 8 0 0 0 3 0 0 7 7 7 7 0 0 0 0 0 1 1 1 0 0 0 3 28 "
            "15 14 8 0 0 0 0 1 0 7 7 7 7 0 0 0 0 0 2 2 1 0 0 0 4 28 14 15 15 0 1 0 0 1 0 7 7 7 7 0 0 0 0 0 0 0 0 0 0 0 30 28",
            MaterialPolicy::quantizedConstants,MaterialLowering::halve|MaterialLowering::previousCapture,MaterialPlanError::equation,1.000001},
        // Twilight hole pattern (hh_monyou02): alpha APREV - lerp(RASA, TEXA, APREV),
        // the blend factor is PREV, so PREV as D waits in the buffer.
        {"twilight hole pattern","3 2 1 0 1 7 0 1 7 0 0 0 0 0 255 255 255 7 255 255 255 255 255 255 663 97 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 0 1 2 3 0 1 2 3 0 1 2 3 0 1 2 3 "
            "3 8 10 15 0 0 2 1 0 7 5 4 7 0 0 0 1 0 0 0 1 0 0 0 12 28 12 2 12 0 0 0 2 1 0 5 4 0 0 1 0 0 1 0 1 1 1 0 0 0 12 28 15 15 15 6 0 0 0 1 0 7 1 1 0 1 0 0 1 0 0 0 1 0 0 0 12 28",
            MaterialPolicy::exactConstants,MaterialLowering::subtractTerms|MaterialLowering::previousCapture|MaterialLowering::signedD,MaterialPlanError::equation,0},
        // Cutscene light (aa_light04): alpha APREV - lerp(TEXA, TEXA, A0) = APREV - TEXA.
        {"cutscene light","2 2 1 0 1 7 0 1 7 0 0 0 0 0 0 0 0 0 255 255 255 255 255 255 255 255 0 0 0 255 255 255 255 255 255 255 255 255 255 255 255 255 0 1 2 3 0 1 2 3 0 1 2 3 0 1 2 3 "
            "3 8 10 15 0 0 2 1 0 7 5 4 7 0 0 0 1 0 0 0 1 0 0 0 12 31 15 15 12 0 0 0 2 1 0 4 4 1 0 1 0 0 1 0 1 1 1 0 0 0 12 28",
            MaterialPolicy::exactConstants,MaterialLowering::subtractTerms,MaterialPlanError::equation,0},
    };
    for(const auto& f:fixtures){
        const auto s=surveyMaterial(f.numbers);
        PicaMaterialPlan p;MaterialPlanError e;
        setMaterialSumRules(false);
        const bool before=planPicaMaterial(s,p,&e,f.policy);
        setMaterialSumRules(true);
        if(before||e!=f.first){std::fprintf(stderr,"%s: wave 1 rules accepted=%d error=%u\n",f.what,before,unsigned(e));assert(false);}
        assert(sumRulesOnly(s,f.policy,p)==1);
        if(p.lowering!=f.bits){std::fprintf(stderr,"%s: lowering %u expected %u\n",f.what,unsigned(p.lowering),unsigned(f.bits));assert(false);}
        double worst;
        if(f.tolerance){
            worst=sweep(s,p,3,f.tolerance/255);
            for(unsigned n=0;n<16384;++n){
                const auto in=randomSamples(seed,n);const auto a=gxEvaluate(s,in),b=picaEvaluate(p,in);
                for(unsigned c=0;c<4;++c){const double d=std::abs(a[c]-b[c]);worst=std::max(worst,d);if(d>f.tolerance/255){reportMismatch(s,p,n,c,a[c],b[c]);assert(false);}}
            }
        }else{compare(s,p,16384);worst=sweep(s,p,3);}
        std::printf("PASS sum rule fixture %s: %u PICA stages, lowering %u, integer sweep worst %.4f bytes\n",f.what,p.stageCount,unsigned(p.lowering),worst*255);
    }
}
// Random materials for each sum rule: a GX stage after the first takes PREV
// as D (running sum, half bias, halve) or subtracts (A and B nonzero).
// Plans are exact against the reference evaluator (halve: half a byte on the
// last stage) over random bytes and the integer sweep.
static void sumRuleRandom(std::uint32_t& seed){
    enum Kind {running,bias,halved,difference,kinds};
    const char* names[kinds]{"running sum","half bias","halve","subtract"};
    for(unsigned kind=0;kind<kinds;++kind){
        unsigned tried=0,planned=0,bits=0;double worst=0;
        const auto policy=kind==running||kind==difference?MaterialPolicy::exactConstants:MaterialPolicy::quantizedConstants;
        for(unsigned n=0;n<20000;++n){
            auto s=baseline();s.stageCount=2+randomOp(seed,3);s.texgenCount=3;
            for(unsigned r=1;r<4;++r)for(unsigned c=0;c<4;++c)s.colors[r][c]=std::int16_t(randomWord(seed)&255);
            for(unsigned t=0;t<s.stageCount;++t){
                auto& st=s.stages[t];st.texture=t%3;st.coordinate=(t+1)%3;
                for(unsigned i=0;i<4;++i){st.color[i]=randomOp(seed,16);st.alpha[i]=randomOp(seed,8);}
                for(auto& input:st.color)if(input==13)input=12;
                st.konstColor=std::uint8_t(12+randomOp(seed,20));st.konstAlpha=std::uint8_t(16+randomOp(seed,16));
                st.colorOp.scale=randomOp(seed,3);st.alphaOp.scale=randomOp(seed,3);
            }
            const bool last=randomOp(seed,2);
            const unsigned at=last?s.stageCount-1:1+randomOp(seed,s.stageCount-1);
            auto& st=s.stages[at];const bool rgb=randomOp(seed,4)!=0;
            auto& op=rgb?st.colorOp:st.alphaOp;auto& args=rgb?st.color:st.alpha;
            if(kind!=difference)args[3]=0; // PREV (CPREV or APREV)
            if(kind==bias)op.bias=2;
            if(kind==halved){op.scale=3;if(!last)continue;}
            if(kind==difference){op.operation=1;if(randomOp(seed,2))args[1]=args[0];}
            if(at+1==s.stageCount&&randomOp(seed,4)==0)op.clamp=0;
            ++tried;PicaMaterialPlan p;
            if(!sumRulesOnly(s,policy,p))continue;
            ++planned;bits+=bool(p.lowering&(kind==running?MaterialLowering::runningSum|MaterialLowering::previousCapture:
                kind==bias?MaterialLowering::biasFold|MaterialLowering::previousCapture:
                kind==halved?MaterialLowering::previousCapture:MaterialLowering::subtractTerms));
            if(kind==halved)worst=std::max({worst,halfWithin(s,p,seed,64),sweep(s,p,51,.500001/255)});
            else{compare(s,p,64);worst=std::max(worst,sweep(s,p,51));}
        }
        // Other stages may take another sum rule; most plans use this one.
        assert(planned>tried/50&&bits>planned/2);
        std::printf("PASS sum rule %s: %u of %u random materials added by the sum rules (%u with its bit), worst %.4f bytes\n",names[kind],planned,tried,bits,worst*255);
    }
    // The capture needs a free combiner buffer and a stage before; the two
    // multiply-adds need C and one product free of PREV. A blend whose
    // factor is PREV takes the capture instead.
    auto s=baseline();s.stageCount=2;s.stages[1].color={8,10,0,0}; // PREV + lerp(TEX, RAS, PREV)
    PicaMaterialPlan p;assert(sumRulesOnly(s,MaterialPolicy::exactConstants,p)&&p.lowering==MaterialLowering::previousCapture&&p.bufferUpdateRgb==1);
    compare(s,p,4096);sweep(s,p);
    s.stages[1].color={0,10,8,0}; // PREV + lerp(PREV, RAS, TEX): the PREV product goes first
    assert(sumRulesOnly(s,MaterialPolicy::exactConstants,p)&&p.lowering==MaterialLowering::runningSum&&!p.bufferUpdateRgb);
    compare(s,p,4096);sweep(s,p);
    s.stages[1].color={10,0,8,0}; // PREV + lerp(RAS, PREV, TEX): swapped products
    assert(sumRulesOnly(s,MaterialPolicy::exactConstants,p)&&p.lowering==MaterialLowering::runningSum);
    compare(s,p,4096);sweep(s,p);
    // Subtract of a blend: X first, then D - X (two stages); PREV as D takes
    // the complement form (three stages, no buffer), or with a PREV factor
    // the captured D (two stages).
    s=baseline();s.stageCount=2;s.stages[1].color={8,10,9,0};s.stages[1].colorOp.operation=1;
    assert(sumRulesOnly(s,MaterialPolicy::exactConstants,p)&&p.lowering==MaterialLowering::subtractTerms&&p.stageCount==4&&!p.bufferUpdateRgb);
    compare(s,p,4096);sweep(s,p);
    s.stages[1].color={8,10,0,0};
    assert(sumRulesOnly(s,MaterialPolicy::exactConstants,p)&&p.lowering==(MaterialLowering::subtractTerms|MaterialLowering::previousCapture));
    assert(p.stageCount==3&&p.bufferUpdateRgb==1);compare(s,p,4096);sweep(s,p);
    s=baseline();s.stages[0].color={8,10,9,4};s.stages[0].colorOp.operation=1;
    assert(sumRulesOnly(s,MaterialPolicy::exactConstants,p)&&p.lowering==MaterialLowering::subtractTerms&&p.stageCount==2&&!p.bufferUpdateRgb);
    compare(s,p,4096);sweep(s,p);
    // A literal difference folds.
    s.stages[0].color={2,2,10,4};s.colors[1]={10,200,30,0};s.colors[2]={50,100,250,0};
    assert(sumRulesOnly(s,MaterialPolicy::exactConstants,p)&&p.stageCount==1&&p.stages[0].rgb.function==MaterialFunction::replace);
    assert((p.stages[0].constant==std::array<std::uint8_t,4>{40,0,220,p.stages[0].constant[3]}));compare(s,p,1024);
    // Halved subtract of one value: SUBTRACT, then x128/255.
    s.stages[0].color={8,8,10,4};s.stages[0].colorOp.scale=3;
    assert(sumRulesOnly(s,MaterialPolicy::quantizedConstants,p)&&(p.lowering&MaterialLowering::subtractTerms)&&p.stageCount==2);
    halfWithin(s,p,seed,8192);sweep(s,p,3,.500001/255);
    // The buffer holds one capture per channel: a second PREV factor keeps
    // the wave 1 refusal, as does APREV as the RGB D of a capture (it would
    // need the alpha buffer).
    s=baseline();s.stageCount=3;s.stages[1].color={8,10,0,0};s.stages[2].color={8,10,0,0};
    MaterialPlanError e;
    assert(!planPicaMaterial(s,p,&e)&&e==MaterialPlanError::equation&&!p.stageCount);
    s.stageCount=2;s.stages[1].color={8,10,1,1};
    assert(!planPicaMaterial(s,p,&e)&&e==MaterialPlanError::equation&&!p.stageCount);
    // Without a stage before it (GX stage 0 reads the initial PREV as a
    // literal) nothing is captured; the switch off restores every refusal.
    s.stages[1].color={8,10,9,0};assert(planPicaMaterial(s,p)&&p.lowering==MaterialLowering::runningSum);
    setMaterialSumRules(false);assert(!planPicaMaterial(s,p,&e)&&e==MaterialPlanError::equation);setMaterialSumRules(true);
    std::puts("PASS sum rule routes: capture for a PREV factor, PREV product first, blend differences (direct, complement, captured), folded literal difference, halved difference, one capture per channel");
}
// Generic random materials: the sum rules never change a plan the wave 1
// rules make, and every added plan matches the reference evaluator.
static void sumRuleIdentity(std::uint32_t& seed){
    unsigned planned=0,added=0;
    for(unsigned n=0;n<60000;++n){
        auto s=baseline();s.stageCount=1+randomOp(seed,5);s.texgenCount=3;
        for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)s.colors[r][c]=std::int16_t(randomOp(seed,8)?int(randomWord(seed)&255):int(randomWord(seed)%2048)-1024);
        for(unsigned t=0;t<s.stageCount;++t){
            auto& st=s.stages[t];st.texture=randomOp(seed,3);st.coordinate=randomOp(seed,3);
            for(unsigned i=0;i<4;++i){st.color[i]=randomOp(seed,16);st.alpha[i]=randomOp(seed,8);}
            for(auto& input:st.color)if(input==13)input=12;
            if(randomOp(seed,3)==0)st.color[3]=0;
            if(randomOp(seed,3)==0)st.alpha[3]=0;
            st.konstColor=std::uint8_t(12+randomOp(seed,20));st.konstAlpha=std::uint8_t(16+randomOp(seed,16));
            for(auto* op:{&st.colorOp,&st.alphaOp}){
                op->operation=std::uint8_t(randomOp(seed,6)==0);op->bias=std::uint8_t(randomOp(seed,5)?0:2);
                op->scale=std::uint8_t(randomOp(seed,4));op->clamp=std::uint8_t(randomOp(seed,5)!=0);
                op->output=std::uint8_t(randomOp(seed,6)?0:1+randomOp(seed,3));
            }
        }
        for(auto policy:{MaterialPolicy::exactConstants,MaterialPolicy::quantizedConstants}){
            PicaMaterialPlan p;
            if(!sumRulesOnly(s,policy,p))continue;
            ++added;
            if(!p.quantizedConstants){compare(s,p,16);++planned;}
        }
    }
    assert(added>0&&planned>0);
    std::printf("PASS sum rules over generic random materials: wave 1 plans unchanged, %u plans added (%u exact ones checked)\n",added,planned);
}
// Alpha 12 operation rules (third pass). Returns 1 when the rules added a
// plan; the first two passes' plans and refusals (error and rule) stay.
static unsigned operationRulesOnly(const MaterialState& s,MaterialPolicy policy,PicaMaterialPlan& plan,MaterialOperationReason* offReason=nullptr){
    PicaMaterialPlan first;MaterialPlanError e1,e2;MaterialOperationReason r1,r2;
    setMaterialOperationRules(false);const bool a=planPicaMaterial(s,first,&e1,policy,&r1);
    setMaterialOperationRules(true);const bool b=planPicaMaterial(s,plan,&e2,policy,&r2);
    if(offReason)*offReason=r1;
    if(a){assert(b&&samePlan(first,plan)&&!(plan.lowering&MaterialLowering::operationRules));return 0;}
    if(!b){assert(e1==e2&&r1==r2&&samePlan(plan,PicaMaterialPlan{}));return 0;}
    assert(e2==MaterialPlanError::none&&r2==MaterialOperationReason::none&&(plan.lowering&MaterialLowering::operationRules));
    return 1;
}
// Random samples through the GX reference with the planner's quantized
// inputs (HALF and konst fractions as planned): exact for plans without the
// divide-by-2 scale.
static void quantizedExact(const MaterialState& s,const PicaMaterialPlan& p,std::uint32_t& seed,unsigned count=2048){
    for(unsigned n=0;n<count;++n){
        const auto in=randomSamples(seed,n);
        const auto a=gxEvaluate(s,in,true),b=picaEvaluate(p,in);
        for(unsigned c=0;c<4;++c)if(std::abs(a[c]-b[c])>=1e-12){reportMismatch(s,p,n,c,a[c],b[c]);assert(false);}
    }
}
static void operationRuleCases(std::uint32_t& seed){
    PicaMaterialPlan p;MaterialOperationReason why;
    // ADDHALF, one stage: a literal D (konst) takes the half, clamped at
    // one; the exact policy admits it (+128 is the hardware byte).
    for(unsigned scale=0;scale<3;++scale)for(unsigned k:{0u,100u,127u,128u,200u}){
        auto s=baseline();s.stages[0].color={8,10,9,14};s.stages[0].colorOp={0,1,std::uint8_t(scale),1,0};
        s.stages[0].konstColor=12;s.konst[0]={std::uint8_t(k),std::uint8_t(255-k),std::uint8_t(k/2),255};
        assert(operationRulesOnly(s,MaterialPolicy::exactConstants,p,&why)==1&&why==MaterialOperationReason::addHalf);
        // The folded half costs nothing over the same equation without it.
        auto plain=s;plain.stages[0].colorOp.bias=0;PicaMaterialPlan q;assert(planPicaMaterial(plain,q,nullptr,MaterialPolicy::exactConstants));
        assert((p.lowering&MaterialLowering::addHalf)&&p.stageCount==q.stageCount&&!p.quantizedConstants);
        sweep(s,p,5);
    }
    // ADDHALF with an initial register D: [-128,127] folds to a literal,
    // below that the signed-D split (an add and a subtract).
    for(int d:{-1024,-300,-129,-128,-100,0,127,128,300}){
        auto s=baseline();s.stages[0].color={8,10,9,2};s.stages[0].colorOp={0,1,1,1,0};
        s.colors[1]={std::int16_t(d),std::int16_t(d/2),std::int16_t(-d/3),255};
        assert(operationRulesOnly(s,MaterialPolicy::exactConstants,p)==1&&(p.lowering&MaterialLowering::addHalf));
        sweep(s,p,5);
    }
    // ADDHALF with a sampled D (RGB and alpha): the sum, then ADD 128 with
    // the scale; and after another stage (PREV as D, a sum rule too).
    for(unsigned scale=0;scale<3;++scale){
        auto s=baseline();s.stages[0].color={8,14,9,10};s.stages[0].colorOp={0,1,std::uint8_t(scale),1,0};
        s.stages[0].alpha={4,6,5,4};s.stages[0].alphaOp={0,1,std::uint8_t(scale),1,0};
        // One ADD of constant 128 after the sum the bias-0 equation needs.
        auto plain=s;plain.stages[0].colorOp.bias=plain.stages[0].alphaOp.bias=0;PicaMaterialPlan q;
        assert(planPicaMaterial(plain,q,nullptr,MaterialPolicy::exactConstants));
        assert(operationRulesOnly(s,MaterialPolicy::exactConstants,p)==1&&p.stageCount==q.stageCount+1);
        sweep(s,p,5);
        s.stageCount=2;s.stages[1].texture=1;s.stages[1].coordinate=1;
        s.stages[1].color={8,10,9,0};s.stages[1].colorOp={0,1,std::uint8_t(scale),1,0};
        assert(operationRulesOnly(s,MaterialPolicy::exactConstants,p)==1);
        sweep(s,p,5);
    }
    // Still refused: ADDHALF with the divide-by-2 scale. Into an
    // intermediate register: renamed onto PREV when PREV is free, else the
    // register route of the stage before the last.
    {
        auto s=baseline();s.stages[0].color={8,10,9,14};s.stages[0].colorOp={0,1,3,1,0};
        assert(operationRulesOnly(s,MaterialPolicy::quantizedConstants,p,&why)==0&&why==MaterialOperationReason::addHalf);
        s.stageCount=2;s.stages[0].colorOp={0,1,0,1,2};s.stages[1].texture=1;s.stages[1].color={4,8,9,15};
        assert(operationRulesOnly(s,MaterialPolicy::exactConstants,p,&why)==1&&why==MaterialOperationReason::addHalf);
        assert(p.lowering&MaterialLowering::registerLiveness);sweep(s,p,5);
        s.stageCount=3;s.stages[1].color={8,15,15,15};s.stages[2].texture=2;s.stages[2].color={4,8,9,0};
        s.stages[1].colorOp={0,0,0,1,0};s.stages[0].colorOp={0,1,0,1,2};std::swap(s.stages[0],s.stages[1]);
        assert(operationRulesOnly(s,MaterialPolicy::exactConstants,p,&why)==1&&why==MaterialOperationReason::addHalf);
        assert(p.bufferUpdateRgb);sweep(s,p,5);
    }
    // Clamp-off intermediate read only as a saturating D (in every Alpha 11
    // tour zone, F_SP104 aside where its D register is signed): stage 0
    // TEX*RAS x4 clamp off, stage 1 RAS*(1-TEX)+HALF*TEX+PREV clamped.
    const char* clampOff="2 2 1 0 0 4 128 0 3 255 0 0 0 0 0 0 0 0 255 255 255 255 255 255 255 255 0 0 0 0 255 255 255 255 255 255 255 255 255 255 255 255 "
        "0 1 2 3 0 1 2 3 0 1 2 3 0 1 2 3 14 8 10 2 0 0 2 0 0 7 6 4 7 0 0 0 1 0 0 0 1 0 0 0 12 31 10 13 8 0 0 0 0 1 0 7 7 7 0 0 0 0 0 0 1 1 1 0 0 0 12 28";
    {
        auto s=surveyMaterial(clampOff);
        assert(operationRulesOnly(s,MaterialPolicy::quantizedConstants,p,&why)==1&&why==MaterialOperationReason::clampOffBound);
        assert(p.lowering&MaterialLowering::saturateIntermediate);
        quantizedExact(s,p,seed);
        // With ONE for HALF the plan is exact under the exact policy too.
        s.stages[1].color[1]=12;
        assert(operationRulesOnly(s,MaterialPolicy::exactConstants,p)==1);
        sweep(s,p,3);
        // A signed D in stage 0 can make the unclamped value negative:
        // clamping it early would change the sum, so it stays refused.
        s.colors[1]={-40,10,10,255};
        assert(operationRulesOnly(s,MaterialPolicy::exactConstants,p,&why)==0&&why==MaterialOperationReason::signedDForm);
        // Read as A/B/C (GX wraps there) or by a halving stage: refused.
        s=surveyMaterial(clampOff);s.stages[1].color={0,13,8,15};
        assert(operationRulesOnly(s,MaterialPolicy::quantizedConstants,p)==0);
        s=surveyMaterial(clampOff);s.stages[1].colorOp.scale=3;
        assert(operationRulesOnly(s,MaterialPolicy::quantizedConstants,p)==0);
    }
    // Register liveness (R_SP161 r7, the worst Alpha 11 zone): REG2 is
    // TEX0*TEX1 and its alpha a running sum, PREV is free while they live,
    // stage 2's RGB is dead. Renamed onto PREV, the program plans exactly.
    const char* star="5 4 1 0 1 7 0 1 7 0 0 0 0 0 0 0 0 0 0 150 255 255 0 0 0 255 0 0 0 0 0 20 80 255 255 255 255 224 255 255 255 255 0 1 2 3 0 1 2 3 0 1 2 3 0 1 2 3 "
        "8 15 15 15 0 0 0 1 0 4 7 7 7 0 0 0 1 0 0 0 1 0 0 0 12 0 15 8 0 15 0 0 0 1 3 4 7 7 0 0 0 0 1 3 2 2 1 0 0 0 12 28 "
        "15 15 15 15 0 0 0 1 0 4 7 7 3 0 0 0 1 0 3 3 1 0 0 0 12 0 14 4 6 15 0 0 0 1 0 7 5 0 7 0 0 0 1 0 0 0 0 0 0 0 13 28 "
        "15 0 14 15 0 0 0 0 0 7 2 0 7 0 0 0 0 0 0 0 0 0 0 0 30 31";
    {
        const auto s=surveyMaterial(star);
        assert(operationRulesOnly(s,MaterialPolicy::exactConstants,p,&why)==1&&why==MaterialOperationReason::registerValue);
        assert((p.lowering&MaterialLowering::registerLiveness)&&p.stageCount<=6&&p.textureCount<=3);
        sweep(s,p,3);
    }
    // Dead writes and renaming on generated programs: a register written
    // from stable sources, PREV unused while it lives, read later.
    unsigned renamed=0;
    for(unsigned n=0;n<4000;++n){
        auto s=baseline();s.stageCount=3+randomOp(seed,3);
        for(unsigned t=0;t<s.stageCount;++t){auto& st=s.stages[t];st.texture=t%3;st.coordinate=t%3;
            st.color={std::uint8_t(8+randomOp(seed,3)),std::uint8_t(randomOp(seed,2)?14:10),std::uint8_t(randomOp(seed,2)?9:11),15};
            st.alpha={7,std::uint8_t(4+randomOp(seed,2)),std::uint8_t(randomOp(seed,2)?6:4),7};}
        const unsigned at=randomOp(seed,s.stageCount-1),reg=1+randomOp(seed,3);
        s.stages[at].colorOp.output=std::uint8_t(reg);s.stages[at].alphaOp.output=std::uint8_t(randomOp(seed,2)?reg:0);
        s.stages[at].colorOp.scale=std::uint8_t(randomOp(seed,3));
        auto& last=s.stages[s.stageCount-1];last.color={std::uint8_t(2*reg),8,9,std::uint8_t(randomOp(seed,2)?15:2*reg)};
        last.alpha={std::uint8_t(randomOp(seed,2)?reg:4),7,7,7};
        if(operationRulesOnly(s,MaterialPolicy::exactConstants,p)){sweep(s,p,15);++renamed;}
    }
    assert(renamed>100);
    // Three live values at the last stage (D_MN08D r53 room geometry, the
    // second-worst Alpha 11 zone: two scaled registers and an R8 compare
    // mask): the mask moves to the alpha channel (A8 on the red components),
    // the second register onto PREV, the first stays in the combiner buffer.
    // Byte operands keep the 256x gain mask exact on the integer sweep.
    const char* threeLive="4 2 1 0 0 7 0 1 7 0 0 0 0 0 0 0 0 255 255 255 255 255 255 255 255 255 0 0 0 255 255 255 255 255 255 255 255 255 255 255 255 255 0 1 2 3 0 1 2 3 0 1 2 3 0 1 2 3 "
        "14 8 10 2 0 0 2 1 3 7 5 4 7 0 0 0 1 3 0 0 1 0 0 0 12 31 "
        "14 8 10 2 0 0 2 1 2 7 5 4 7 0 0 0 1 0 0 1 1 0 0 0 12 28 "
        "8 14 12 15 8 0 0 1 0 0 3 6 7 0 0 0 1 0 1 2 1 0 0 0 29 29 "
        "6 4 0 15 0 0 0 0 0 5 7 7 7 0 0 0 0 0 0 0 0 0 0 0 12 28";
    {
        auto s=surveyMaterial(threeLive);
        for(auto policy:{MaterialPolicy::exactConstants,MaterialPolicy::quantizedConstants}){
            assert(operationRulesOnly(s,policy,p,&why)==1&&why==MaterialOperationReason::scaledRegister);
            assert((p.lowering&MaterialLowering::registerLiveness)&&(p.lowering&MaterialLowering::compare)&&p.stageCount==6&&p.bufferUpdateRgb);
            sweep(s,p,3);
        }
        // Konst and initial registers varied (C0 as D adds a stage each:
        // over the budget, refused with the first rule).
        for(unsigned n=0;n<64;++n){
            for(auto& k:s.konst)for(auto& c:k)c=std::uint8_t(randomWord(seed)&255);
            s.colors[1]={0,0,0,std::int16_t(randomWord(seed)&255)};
            if(operationRulesOnly(s,MaterialPolicy::exactConstants,p,&why))sweep(s,p,7);
            else assert(why==MaterialOperationReason::scaledRegister);
        }
    }
    // Still refused (F_SP127 cliffs): three live RGB values none of which
    // is a mask. Refusal counters and their report: every operation refusal
    // counted by rule, lines short enough for OSReport.
    const char* cliff="5 2 1 0 0 7 0 1 7 0 0 0 0 0 0 0 0 255 255 255 255 255 255 255 255 255 0 0 0 255 80 0 0 0 255 255 255 255 255 255 255 255 0 1 2 3 0 1 2 3 0 1 2 3 0 1 2 3 "
        "15 8 10 15 0 0 0 1 0 7 5 4 7 0 0 0 1 0 0 0 1 0 0 0 0 0 "
        "15 12 14 8 0 0 0 1 3 7 7 7 0 0 0 0 0 0 1 1 1 7 0 0 17 28 "
        "14 6 0 2 0 0 2 1 2 7 6 0 7 0 0 0 0 0 0 0 0 7 0 0 12 31 "
        "15 0 6 15 0 0 2 1 0 5 7 7 7 0 0 0 0 0 0 0 0 0 0 0 12 28 "
        "4 0 8 15 0 0 0 1 0 5 7 7 7 0 0 0 0 0 0 2 1 0 0 0 12 28";
    std::array<std::uint32_t,unsigned(MaterialOperationReason::count)> before{},after{};
    materialOperationRefusals(before);
    {auto s=surveyMaterial(cliff);PicaMaterialPlan q;MaterialPlanError e;MaterialOperationReason r;
     assert(!planPicaMaterial(s,q,&e,MaterialPolicy::quantizedConstants,&r)&&e==MaterialPlanError::operation&&r==MaterialOperationReason::scaledRegister);}
    materialOperationRefusals(after);
    assert(after[unsigned(MaterialOperationReason::scaledRegister)]==before[unsigned(MaterialOperationReason::scaledRegister)]+1);
    static unsigned lines;static bool named;lines=0;named=false;
    reportMaterialOperationRefusals(+[](const char* line){
        assert(std::strlen(line)<255&&std::strncmp(line,"Native TEV operation refusals by rule (planner calls):",54)==0);
        named|=std::strstr(line,"scaled-register=")!=nullptr;++lines;});
    assert(lines>=1&&named);
    for(unsigned r=0;r<unsigned(MaterialOperationReason::count);++r)
        assert(std::strcmp(materialOperationReasonText(MaterialOperationReason(r)),"unknown")!=0);
    std::printf("PASS operation rules: ADDHALF folds/split/extra ADD exact, clamp-off intermediate saturation, register liveness (%u generated programs renamed), "
                "D_MN08D compare mask in alpha exact, F_SP127 three-live refusal named, counters and report\n",renamed);
}
// Alpha 13 register chain (operation rule): the Ordon spring floor material
// aa_MA04_IzumiJimen_v_x of F_SP104 room 1 (disc MAT3; K1 red = the fog
// density dKy_bg_MAxx_proc writes each frame, 100 in the owner's dump 003).
// REG2 = T1 + K1R, REG2 = REG2 + RASA, out = (C0 + lerp(K0, REG2, PREV)) x4.
// Alpha 12 refused it whenever K1R was not zero (register-value) and drew the
// one-stage stand-in T0 x RAS: the floor four times too dark.
static void registerChainCases(){
    const char* spring="4 2 1 0 0 7 0 1 7 0 0 0 0 0 0 0 0 255 255 255 255 255 255 255 255 255 0 0 0 255 100 0 0 0 255 255 255 255 255 255 255 255 "
        "0 1 2 3 0 1 2 3 0 1 2 3 0 1 2 3 "
        "15 8 10 15 0 0 0 1 0 7 5 4 7 0 0 0 1 0 0 0 1 0 0 0 0 0 "
        "15 12 14 8 0 0 0 1 3 7 7 7 0 0 0 0 1 0 1 1 1 7 0 0 17 28 "
        "15 12 11 6 0 0 0 1 3 7 7 7 0 0 0 0 1 0 0 0 0 0 0 0 12 31 "
        "14 6 0 2 0 0 2 1 0 7 6 0 7 0 0 0 1 0 0 0 0 7 0 0 12 28";
    PicaMaterialPlan p;MaterialOperationReason why;
    for(auto policy:{MaterialPolicy::exactConstants,MaterialPolicy::quantizedConstants}){
        auto s=surveyMaterial(spring);
        assert(operationRulesOnly(s,policy,p,&why)==1&&why==MaterialOperationReason::registerValue);
        assert((p.lowering&MaterialLowering::registerChain)&&p.stageCount==4&&p.textureCount==2&&!p.quantizedConstants);
        sweep(s,p,3);compare(s,p,4096);
        // Fog density zero: REG2 = T1 is a copy, planned by the first pass
        // as before (no operation rule).
        auto clear=s;clear.konst[1][0]=0;
        assert(operationRulesOnly(clear,policy,p)==0&&planPicaMaterial(clear,p,nullptr,policy)&&!p.lowering);
        // Other times and the twilight: a TEV colour C0 and konst K0.
        for(int c0:{0,40,200})for(unsigned k0:{0u,30u,255u}){
            auto t=s;t.colors[1]={std::int16_t(c0),std::int16_t(c0/2),std::int16_t(c0/4),255};t.konst[0]={std::uint8_t(k0),std::uint8_t(k0/3),0,255};
            assert(operationRulesOnly(t,policy,p)==1&&(p.lowering&MaterialLowering::registerChain)&&p.stageCount<=5);
            sweep(t,p,5);
        }
    }
    // A three-step chain whose later steps read GX PREV (the combiner
    // buffer, visible from the step after the chain's first) and APREV,
    // REG2 itself, and a scaled step.
    {
        auto s=surveyMaterial(spring);s.stageCount=5;s.stages[4]=s.stages[3];
        s.stages[3]=s.stages[2];s.stages[3].color={15,0,11,6}; // REG2 + PREV*RASA
        s.stages[2].color={6,1,14,15};s.stages[2].colorOp.scale=1; // (REG2*(1-K)+APREV*K)*2
        s.stages[2].konstColor=13;s.konst[1]={100,60,20,0};
        for(auto policy:{MaterialPolicy::exactConstants,MaterialPolicy::quantizedConstants}){
            assert(operationRulesOnly(s,policy,p)==1&&(p.lowering&MaterialLowering::registerChain));
            sweep(s,p,3);compare(s,p,4096);
        }
        // Broken chains keep their refusal: a PREV write inside it, or a
        // second register.
        for(unsigned out:{0u,2u}){
            auto t=s;t.stages[2].colorOp.output=std::uint8_t(out);
            for(auto policy:{MaterialPolicy::exactConstants,MaterialPolicy::quantizedConstants})
                if(planPicaMaterial(t,p,nullptr,policy)){assert(!(p.lowering&MaterialLowering::registerChain));sweep(t,p,7);}
        }
    }
    std::puts("PASS register chain: F_SP104 spring floor (K1R fog density) exact in four stages, C0/K0 variants, three-step chain with PREV/APREV reads, broken chains refused");
}
int main(){
    additiveTextureSplitCases();
    registerChainCases();
    fullRasterCases();
    unsigned accepted=0;std::uint32_t seed=919;
    // Independent equations exercise all source IDs, unused inputs, constants,
    // complementary factors, scale, and simultaneous RGB/alpha previous values.
    for(unsigned n=0;n<30000;++n){
        auto s=baseline();s.stageCount=1+randomWord(seed)%3;
        for(unsigned t=0;t<s.stageCount;++t){auto& st=s.stages[t];st.texture=t;st.coordinate=2-t;
            for(unsigned i=0;i<4;++i){st.color[i]=(randomWord(seed)>>16)%16;st.alpha[i]=(randomWord(seed)>>16)%8;}
            st.colorOp.scale=(randomWord(seed)>>16)%3;st.alphaOp.scale=(randomWord(seed)>>16)%3;
        }
        PicaMaterialPlan p;MaterialPlanError e;
        if(planPicaMaterial(s,p,&e)){assert(e==MaterialPlanError::none);compare(s,p,8);++accepted;}
    }
    assert(accepted>0);
    unsigned unclampedAccepted=0;
    for(unsigned n=0;n<30000;++n) {
        auto candidate=baseline();candidate.stageCount=1+randomWord(seed)%3;
        for(unsigned t=0;t<candidate.stageCount;++t) {
            auto& st=candidate.stages[t];st.texture=t;st.coordinate=2-t;
            for(unsigned i=0;i<4;++i){st.color[i]=(randomWord(seed)>>16)%16;st.alpha[i]=(randomWord(seed)>>16)%8;}
            st.colorOp.scale=(randomWord(seed)>>16)%3;st.alphaOp.scale=(randomWord(seed)>>16)%3;
            st.colorOp.clamp=(randomWord(seed)>>16)%2;st.alphaOp.clamp=(randomWord(seed)>>16)%2;
        }
        PicaMaterialPlan translated;MaterialPlanError error;
        if(planPicaMaterial(candidate,translated,&error)){compare(candidate,translated,32);++unclampedAccepted;}
    }
    assert(unclampedAccepted>0);
    unsigned registerAccepted=0;
    for(unsigned n=0;n<30000;++n) {
        auto candidate=baseline();candidate.stageCount=3+(randomWord(seed)>>16)%4;
        for(unsigned t=0;t<candidate.stageCount;++t) {
            auto& st=candidate.stages[t];st.texture=t%3;st.coordinate=t%3;
            for(unsigned i=0;i<4;++i){st.color[i]=(randomWord(seed)>>16)%16;st.alpha[i]=(randomWord(seed)>>16)%8;}
        }
        candidate.stages[candidate.stageCount-2].colorOp.output=1+(randomWord(seed)>>16)%3;
        PicaMaterialPlan translated;
        if(planPicaMaterial(candidate,translated)){compare(candidate,translated,32);++registerAccepted;}
    }
    assert(registerAccepted>0);
    unsigned biasAccepted=0;
    for(unsigned n=0;n<30000;++n) {
        auto candidate=baseline();candidate.stageCount=1+(randomWord(seed)>>16)%3;
        for(unsigned t=0;t<candidate.stageCount;++t) {
            auto& st=candidate.stages[t];st.texture=t;st.coordinate=t;
            for(unsigned i=0;i<4;++i){st.color[i]=(randomWord(seed)>>16)%16;st.alpha[i]=(randomWord(seed)>>16)%8;}
            st.colorOp.bias=((randomWord(seed)>>16)&1)*2;st.alphaOp.bias=((randomWord(seed)>>16)&1)*2;
            // Fractional ONE_HALF has a separate bounded-error suite; isolate
            // byte-exact constants here while varying the half-bias lowering.
            for(auto& input:st.color)if(input==13)input=12;
            if(t+1==candidate.stageCount){st.colorOp.output=(randomWord(seed)>>16)%4;st.alphaOp.output=(randomWord(seed)>>16)%4;}
        }
        PicaMaterialPlan translated;
        if(planPicaMaterial(candidate,translated,nullptr,MaterialPolicy::quantizedConstants)){compare(candidate,translated,32);++biasAccepted;}
    }
    assert(biasAccepted>0);
    // Three-stage multi-texture multiply-add, matching the measured direct
    // title pattern with synthetic constants only.
    auto s=baseline();s.stageCount=3;s.stages[0].alpha={7,2,5,7};
    s.stages[1].texture=1;s.stages[1].coordinate=1;s.stages[1].color={15,14,8,0};s.stages[1].alpha={7,6,5,0};
    s.stages[2].texture=2;s.stages[2].coordinate=2;s.stages[2].color={15,9,8,0};s.stages[2].alpha={7,3,4,0};
    PicaMaterialPlan p;assert(planPicaMaterial(s,p));assert(p.textureCount==3);compare(s,p,256);
    // Captured normal opening F_SP102: five stages retain a computed REG2
    // while PREV evolves. Framebuffer alpha is unobserved in this draw.
    auto opening=baseline();opening.stageCount=5;opening.texgenCount=3;
    opening.colors={{{255,255,255,255},{0,0,0,255},{255,255,255,255},{255,255,255,255}}};
    opening.konst={{{0,0,0,255},{45,0,0,0},{255,255,255,255},{255,255,255,255}}};
    const std::array<std::array<std::uint8_t,4>,5> rgb{{{15,8,11,15},{15,12,14,8},{8,15,11,0},{15,0,6,15},{15,10,0,15}}};
    const std::array<std::array<std::uint8_t,4>,5> alpha{{{7,6,4,7},{7,7,7,0},{7,7,7,0},{5,7,7,0},{5,7,7,0}}};
    for(unsigned i=0;i<5;++i){auto& st=opening.stages[i];st.color=rgb[i];st.alpha=alpha[i];
        st.texture=i<3?i:7;st.coordinate=i<3?i:0;st.textureEnabled=i<3;st.raster=i==1?7:0;
        st.konstColor=i==0?0:i==1?17:12;st.konstAlpha=i==0?0:i==2?31:28;
        st.colorOp={0,0,std::uint8_t(i==4?2:0),std::uint8_t(i==3?0:1),std::uint8_t(i==1?3:0)};
        st.alphaOp={0,0,0,std::uint8_t(i==0),0};
    }
    RasterPlan opaque{};opaque.alpha.comparison=7;opaque.writeRgb=true;
    const auto originalOpening=opening;
    assert(discardUnobservedTevAlpha(opening,opaque));
    assert(planPicaMaterial(opening,p)&&p.stageCount==5&&p.textureCount==3&&p.bufferUpdateRgb==1);
    compare(opening,p,4096);
    for(unsigned sample=0;sample<4096;++sample){
        Samples inputs;for(auto& c:inputs.raster)c=double(randomWord(seed)&255)/255;
        for(auto& map:inputs.texture)for(auto& coord:map)for(auto& c:coord)c=double(randomWord(seed)&255)/255;
        const auto before=gxEvaluate(originalOpening,inputs),after=picaEvaluate(p,inputs);
        for(unsigned c=0;c<3;++c)assert(std::abs(before[c]-after[c])<1e-12);
    }
    // Every byte of draw-constant primary alpha can vacate that interpolator
    // without changing the original RGB, allowing three textures plus fog.
    for(unsigned byte=0;byte<256;++byte){
        PicaMaterialPlan constant;assert(planPicaMaterial(opening,constant));
        assert(substituteConstantPrimaryAlpha(constant,byte));
        for(unsigned sample=0;sample<32;++sample){
            Samples inputs;for(auto& c:inputs.raster)c=double(randomWord(seed)&255)/255;
            for(auto& map:inputs.texture)for(auto& coord:map)for(auto& c:coord)c=double(randomWord(seed)&255)/255;
            inputs.raster[3]=double(byte)/255;const auto before=gxEvaluate(originalOpening,inputs);
            inputs.raster[3]=double(randomWord(seed)&255)/255;const auto after=picaEvaluate(constant,inputs);
            for(unsigned c=0;c<3;++c)assert(std::abs(before[c]-after[c])<1e-12);
        }
    }
    for(unsigned mask=0;mask<16;++mask)for(unsigned inverted=0;inverted<2;++inverted){
        PicaMaterialPlan constant;constant.stageCount=1;constant.textureCount=1;auto& stage=constant.stages[0];
        stage.constant={10,20,30,40};stage.rgb.function=stage.alpha.function=MaterialFunction::multiplyAdd;
        stage.rgb.inputs[0]=stage.alpha.inputs[2]={MaterialSource::primary,MaterialOperand::alpha,bool(inverted)};
        constexpr MaterialOperand operands[]{MaterialOperand::red,MaterialOperand::green,MaterialOperand::blue,MaterialOperand::alpha};
        for(unsigned c=0;c<4;++c){auto& input=c<2?stage.rgb.inputs[c+1]:stage.alpha.inputs[c-2];
            input={mask&(1u<<c)?MaterialSource::constant:MaterialSource::texture0,operands[c],false};}
        const auto before=constant;
        assert(substituteConstantPrimaryAlpha(constant,99)==(mask!=15));
        if(mask==15)assert(constant.stages[0].constant==before.stages[0].constant&&constant.stages[0].rgb.inputs==before.stages[0].rgb.inputs);
        else for(unsigned sample=0;sample<256;++sample){
            Samples inputs;for(auto& c:inputs.raster)c=double(randomWord(seed)&255)/255;
            for(auto& map:inputs.texture)for(auto& coord:map)for(auto& c:coord)c=double(randomWord(seed)&255)/255;
            inputs.raster[3]=99./255;const auto a=picaEvaluate(before,inputs);
            inputs.raster[3]=double(randomWord(seed)&255)/255;const auto b=picaEvaluate(constant,inputs);
            for(unsigned c=0;c<4;++c)assert(std::abs(a[c]-b[c])<1e-12);
        }
    }
    // All temporary register slots, independent sampled/constant terms and
    // variable initial alpha preserve both channels after the proven hoist.
    for(unsigned reg=1;reg<=3;++reg)for(unsigned k=0;k<256;++k){
        auto material=opening;material.stages[1].colorOp.output=reg;
        material.stages[3].color[2]=2*reg;material.konst[1][0]=k;material.colors[0][3]=255-k;
        assert(planPicaMaterial(material,p));compare(material,p,64);
    }
    for(unsigned reason=0;reason<7;++reason){
        auto material=originalOpening;auto raster=opaque;
        if(reason==0)raster.writeAlpha=true;else if(reason==1)raster.blend.mode=true;
        else if(reason==2)raster.alpha.comparison=4;else material.stages[2].color[1]=1+2*(reason-3);
        assert(!discardUnobservedTevAlpha(material,raster));
        assert(material.stages[3].alpha==originalOpening.stages[3].alpha);
    }
    auto unsafe=opening;unsafe.stages[1].color[1]=0;assert(!planPicaMaterial(unsafe,p));
    unsafe=opening;unsafe.stages[0].color[3]=0;assert(!planPicaMaterial(unsafe,p));
    unsafe=opening;unsafe.stages[2].colorOp.output=3;assert(!planPicaMaterial(unsafe,p));
    std::puts("PASS opening five-stage material: RGB framebuffer equivalence, 768 buffered-register lifetimes, alpha observability and dependency rejection");
    auto openingSubtract=baseline();openingSubtract.stageCount=4;
    openingSubtract.colors={{{255,255,255,255},{0,0,0,0},{255,255,255,255},{255,255,255,255}}};
    openingSubtract.konst={{{0,0,0,0},{130,160,0,255},{255,255,255,242},{0,0,0,255}}};
    const std::array<std::array<std::uint8_t,4>,4> subtractRgb{{{15,10,8,14},{15,14,8,15},{15,14,8,6},{15,12,4,0}}};
    for(unsigned i=0;i<4;++i){auto& st=openingSubtract.stages[i];st.color=subtractRgb[i];
        st.alpha={std::uint8_t(i<2?7:5),7,7,7};st.alphaOp={0,0,0,std::uint8_t(i<2),0};
        st.colorOp={std::uint8_t(i==2),0,std::uint8_t(i==0?2:0),1,std::uint8_t(i==0?2:i==1?3:0)};
        st.konstColor=i==0?15:i==1?13:i==2?30:12;st.konstAlpha=i==3?29:16;
        st.texture=i<3?i:7;st.coordinate=i<3?i:0;st.textureEnabled=i<3;st.raster=0;
    }
    assert(planPicaMaterial(openingSubtract,p)&&p.stageCount==5&&p.bufferUpdateRgb==1&&p.textureCount==3);
    compare(openingSubtract,p,16384);
    // Vary both live register IDs, product scale and subtracted konst, keeping
    // the true lifetime graph and testing both subtraction product forms.
    for(unsigned reg=1;reg<=3;++reg)for(unsigned second=1;second<=3;++second)if(reg!=second)
    for(unsigned byte=0;byte<256;byte+=7)for(unsigned scale=0;scale<3;++scale){
        auto m=openingSubtract;m.stages[0].colorOp.output=reg;m.stages[1].colorOp.output=second;
        m.stages[2].color[3]=2*second;m.stages[3].color[2]=2*reg;
        m.stages[0].colorOp.scale=scale;m.konst[2][3]=byte;
        assert(planPicaMaterial(m,p));compare(m,p,64);
    }
    for(unsigned n=0;n<4096;++n){
        auto m=baseline();auto& st=m.stages[0];
        st.color={15,8,10,4};st.alpha={7,4,5,6};st.konstAlpha=16;
        st.colorOp.operation=st.alphaOp.operation=1;
        st.colorOp.scale=n%3;st.alphaOp.scale=(n/3)%3;
        if(n&1){std::swap(st.color[0],st.color[1]);std::swap(st.alpha[0],st.alpha[1]);}
        for(unsigned c=0;c<4;++c){m.colors[2][c]=randomWord(seed)&255;m.konst[0][c]=randomWord(seed)&255;}
        assert(planPicaMaterial(m,p)&&p.stageCount==2);compare(m,p,64);
    }
    auto deadInitial=openingSubtract;deadInitial.stages[1].color[3]=0;assert(!planPicaMaterial(deadInitial,p));
    auto overwritten=openingSubtract;overwritten.stages[1].colorOp.output=2;assert(!planPicaMaterial(overwritten,p));
    std::puts("PASS opening subtract: two computed register lifetimes, complemented clamped product subtraction and 666 variants");
    // Captured indirect opening quad after the separately tested vertex
    // indirect planner: only the first sampled RGB is halved.
    auto halfOpening=baseline();halfOpening.stageCount=3;halfOpening.texgenCount=4;
    halfOpening.colors={{{255,255,255,255},{255,255,255,255},{200,160,160,255},{117,121,149,255}}};
    halfOpening.konst={{{255,255,255,255},{255,255,255,0},{70,50,50,255},{255,255,255,255}}};
    halfOpening.stages[0].color={8,15,15,15};halfOpening.stages[0].alpha={4,7,7,7};
    halfOpening.stages[0].colorOp.scale=3;halfOpening.stages[0].texture=halfOpening.stages[0].coordinate=1;
    halfOpening.stages[1].color={0,8,14,15};halfOpening.stages[1].alpha={4,7,7,7};
    halfOpening.stages[1].konstColor=29;halfOpening.stages[1].texture=halfOpening.stages[1].coordinate=0;
    halfOpening.stages[2].color={15,4,8,0};halfOpening.stages[2].alpha={7,5,0,7};
    halfOpening.stages[2].konstColor=14;halfOpening.stages[2].konstAlpha=29;
    halfOpening.stages[2].texture=halfOpening.stages[2].coordinate=2;
    reject(halfOpening,MaterialPlanError::operation);
    assert(planPicaMaterial(halfOpening,p,nullptr,MaterialPolicy::quantizedConstants)&&p.quantizedConstants);
    double halfMaximum=0;
    for(unsigned sample=0;sample<65536;++sample){
        Samples inputs;for(auto& c:inputs.raster)c=double(randomWord(seed)&255)/255;
        for(auto& map:inputs.texture)for(auto& coord:map)for(auto& c:coord)c=double(randomWord(seed)&255)/255;
        const auto a=gxEvaluate(halfOpening,inputs),b=picaEvaluate(p,inputs);
        for(unsigned c=0;c<3;++c){const auto e=std::abs(a[c]-b[c]);halfMaximum=std::max(halfMaximum,e);assert(e<=.500001/255);}
        assert(std::abs(a[3]-b[3])<1e-12);
    }
    const auto halfNoFog=p;
    VertexFogPlan redFog;redFog.enabled=true;redFog.color={117,90,149,255};
    assert(appendVertexFog(p,redFog,false,true)&&p.textureCount==3);
    for(unsigned sample=0;sample<32768;++sample){
        Samples inputs;for(auto& c:inputs.raster)c=double(randomWord(seed)&255)/255;
        for(auto& map:inputs.texture)for(auto& coord:map)for(auto& c:coord)c=double(randomWord(seed)&255)/255;
        const auto base=picaEvaluate(halfNoFog,inputs);
        const double fog=double(randomWord(seed)&255)/255;inputs.raster[0]=fog;
        const auto result=picaEvaluate(p,inputs);
        for(unsigned c=0;c<3;++c)assert(std::abs(result[c]-(base[c]*(1-fog)+redFog.color[c]/255.0*fog))<1e-12);
        assert(std::abs(result[3]-base[3])<1e-12);
    }
    std::puts("PASS red-channel fog: three original samplers, exact material alpha and 32768 independent fragment combinations");
    // Half RGB followed by an alpha split must not apply the scale twice.
    auto halfSplit=baseline();halfSplit.stages[0].colorOp.scale=3;
    halfSplit.stages[0].alpha={4,5,4,6};halfSplit.stages[0].konstAlpha=16;
    assert(planPicaMaterial(halfSplit,p,nullptr,MaterialPolicy::quantizedConstants)&&p.stageCount==2);
    for(unsigned sample=0;sample<8192;++sample){
        Samples inputs;for(auto& c:inputs.raster)c=double(randomWord(seed)&255)/255;
        for(auto& map:inputs.texture)for(auto& coord:map)for(auto& c:coord)c=double(randomWord(seed)&255)/255;
        const auto a=gxEvaluate(halfSplit,inputs),b=picaEvaluate(p,inputs);
        for(unsigned c=0;c<3;++c)assert(std::abs(a[c]-b[c])<=.500001/255);
        assert(std::abs(a[3]-b[3])<1e-12);
    }
    // A halved sum above one is halved term by term ((T+V)/2 as T*128/255 +
    // V*127/255); a halved half-bias without D stays bounded. Both remain
    // within half an output byte; strict policy still refuses the half scale.
    auto unboundedHalf=baseline();unboundedHalf.stages[0].color={8,15,15,10};unboundedHalf.stages[0].colorOp.scale=3;
    reject(unboundedHalf,MaterialPlanError::operation);
    assert(planPicaMaterial(unboundedHalf,p,nullptr,MaterialPolicy::quantizedConstants)&&p.stageCount==1);
    assert(p.lowering&MaterialLowering::halve);halfWithin(unboundedHalf,p,seed);
    auto biasedHalf=baseline();biasedHalf.stages[0].colorOp.scale=3;biasedHalf.stages[0].colorOp.bias=2;
    assert(planPicaMaterial(biasedHalf,p,nullptr,MaterialPolicy::quantizedConstants)&&p.stageCount==3);
    halfWithin(biasedHalf,p,seed);
    biasedHalf.stages[0].color[3]=10;assert(!planPicaMaterial(biasedHalf,p,nullptr,MaterialPolicy::quantizedConstants));
    halveCases(seed);
    auto halfBoth=baseline();halfBoth.stages[0].colorOp.scale=halfBoth.stages[0].alphaOp.scale=3;
    assert(planPicaMaterial(halfBoth,p,nullptr,MaterialPolicy::quantizedConstants)&&p.stageCount==1);
    for(unsigned byte=0;byte<256;++byte){Samples input{};
        for(auto& map:input.texture)for(auto& coord:map)for(auto& c:coord)c=double(byte)/255;
        const auto a=gxEvaluate(halfBoth,input),b=picaEvaluate(p,input);
        for(unsigned c=0;c<4;++c)assert(std::abs(a[c]-b[c])<=.500001/255);
    }
    std::printf("PASS bounded sampled half-scale: maximum real RGB error %.9f; strict/unbounded refusal and alpha-split preservation\n",halfMaximum);
    // RGB and alpha may carry different constants in one stage.
    s=baseline();s.stages[0].color={15,4,8,15};s.stages[0].alpha={7,6,4,7};assert(planPicaMaterial(s,p));compare(s,p);
    // Captured outdoor sky: two texture stages, then a mixed-sign RGB tint
    // between original C0 and K0 using PREV as its interpolation factor.
    // The immutable initial PICA buffer is available after stage0 only.
    s=baseline();s.stageCount=3;s.texgenCount=2;
    s.colors[1]={255,188,55,255};s.colors[2]={255,255,255,200};s.konst[0]={89,60,60,255};
    s.stages[0].color={8,15,15,15};s.stages[0].alpha={7,2,4,7};
    s.stages[0].texture=0;s.stages[0].coordinate=0;s.stages[0].raster=7;
    s.stages[1].color={15,8,0,15};s.stages[1].alpha={7,2,4,0};
    s.stages[1].texture=1;s.stages[1].coordinate=1;s.stages[1].raster=7;
    s.stages[2].color={2,14,0,15};s.stages[2].alpha={7,1,0,7};
    s.stages[2].textureEnabled=false;s.stages[2].raster=7;
    assert(planPicaMaterial(s,p)&&p.stageCount==3&&p.textureCount==2);
    assert((p.bufferConstant==std::array<std::uint8_t,4>{255,188,55,0}));compare(s,p,4096);
    for(unsigned n=0;n<1024;++n){
        for(unsigned c=0;c<3;++c){s.colors[1][c]=(randomWord(seed)>>16)&255;s.konst[0][c]=(randomWord(seed)>>16)&255;}
        assert(planPicaMaterial(s,p));compare(s,p,64);
    }
    // Captured 27vertex exterior draw: its first and only GX stage interpolates
    // crossing RGB endpoints. A zero prologue makes both constants available.
    s=baseline();s.texgenCount=0;s.colors[1]={0,0,0,0};s.colors[2]={210,150,100,255};s.konst[0]={39,39,255,255};s.konst[3]={255,255,255,255};
    s.stages[0].textureEnabled=false;s.stages[0].color={14,4,10,2};s.stages[0].alpha={7,5,6,7};s.stages[0].konstAlpha=31;
    assert(planPicaMaterial(s,p)&&p.stageCount==2&&p.textureCount==0);compare(s,p,4096);
    for(unsigned n=0;n<1024;++n){
        for(unsigned c=0;c<3;++c){s.colors[2][c]=(randomWord(seed)>>16)&255;s.konst[0][c]=(randomWord(seed)>>16)&255;}
        assert(planPicaMaterial(s,p));compare(s,p,64);
    }
    // Scalar channel swizzles and inverses; full RGB permutation is not admitted.
    // Captured F_SP103 large automatic foliage draw: subtract-half alpha then
    // color multiply-add; final texture alpha targets REG2 but is still output.
    s=baseline();s.stageCount=2;s.texgenCount=1;s.colors[2]={131,69,45,82};
    s.stages[0].color={15,10,8,15};s.stages[0].alpha={7,2,5,5};s.stages[0].alphaOp.bias=2;
    s.stages[1].color={15,1,4,0};s.stages[1].colorOp.scale=2;s.stages[1].alpha={7,7,7,4};s.stages[1].alphaOp.output=3;
    reject(s,MaterialPlanError::operation);
    assert(planPicaMaterial(s,p,nullptr,MaterialPolicy::quantizedConstants)&&p.stageCount==3&&p.quantizedConstants);compare(s,p,4096);
    for(unsigned alpha=0;alpha<256;++alpha){s.colors[2][3]=alpha;assert(planPicaMaterial(s,p,nullptr,MaterialPolicy::quantizedConstants));compare(s,p,128);}
    for(unsigned rgb=0;rgb<4;++rgb)for(unsigned alpha=0;alpha<4;++alpha)for(unsigned scale=0;scale<3;++scale){
        s=baseline();s.stages[0].colorOp.output=rgb;s.stages[0].alphaOp.output=alpha;
        s.stages[0].colorOp.scale=s.stages[0].alphaOp.scale=scale;
        assert(planPicaMaterial(s,p));compare(s,p,256);
    }
    // Subtract-half with clamp off: the saturated output on the last stage,
    // refused before it (the negative value would reach the next stage).
    s=baseline();s.stages[0].alphaOp.bias=2;s.stages[0].alphaOp.clamp=0;
    assert(planPicaMaterial(s,p,nullptr,MaterialPolicy::quantizedConstants)&&(p.lowering&MaterialLowering::finalClamp));compare(s,p,256);
    s.stageCount=2;s.stages[1].alpha={7,7,7,0};
    assert(!planPicaMaterial(s,p,nullptr,MaterialPolicy::quantizedConstants));
    for(unsigned component=0;component<4;++component){s=baseline();s.swaps[0][3]=component;s.stages[0].color={9,15,10,15};assert(planPicaMaterial(s,p));compare(s,p);}
    // Exhaust all 256 valid RGBA swizzles for both sampled source families.
    // Independent GX/PICA evaluators compare admitted RGB broadcasts and
    // identity RGB with every independent alpha channel. Mixed RGB stays strict.
    for(unsigned raster=0;raster<2;++raster)for(unsigned bits=0;bits<256;++bits){
        s=baseline();auto& sw=s.swaps[0];
        for(unsigned c=0;c<4;++c)sw[c]=(bits>>(2*c))&3;
        s.stages[0].color={std::uint8_t(raster?10:8),15,15,15};
        s.stages[0].alpha={std::uint8_t(raster?5:4),7,7,7};
        const bool supported=(sw[0]==0&&sw[1]==1&&sw[2]==2)||(sw[0]==sw[1]&&sw[1]==sw[2]);
        if(supported){assert(planPicaMaterial(s,p));compare(s,p,256);}
        else reject(s,MaterialPlanError::swap);
    }
    // Actual monochrome equation: replicated texture R tinted by register 2,
    // with the original register alpha retained for source-alpha blending.
    s=baseline();s.swaps[1]={0,0,0,1};s.stages[0].textureSwap=1;s.stages[0].rasterSwap=1;
    s.stages[0].color={15,8,6,15};s.stages[0].alpha={7,7,7,3};
    for(unsigned a=0;a<256;++a){s.colors[3][3]=a;assert(planPicaMaterial(s,p));compare(s,p,64);}
    s=baseline();s.swaps[0]={4,4,4,3};reject(s,MaterialPlanError::swap);
    for(unsigned selector=12;selector<32;++selector){
        s=baseline();s.stages[0].color={14,15,15,15};s.stages[0].konstColor=selector;
        if(selector<16)s.stages[0].alpha={4,7,7,7};
        else{s.stages[0].alpha={6,7,7,7};s.stages[0].konstAlpha=selector;}
        assert(planPicaMaterial(s,p));compare(s,p);
    }
    // Every stage adds its sample to PREV, so each texture is observed (a
    // dead stage's texture would be dropped by the Alpha 12 liveness rule).
    s=baseline();s.stageCount=6;for(unsigned t=0;t<6;++t){s.stages[t].texture=t%3;if(t)s.stages[t].color={8,15,15,0};}
    assert(planPicaMaterial(s,p)&&p.textureCount==3);compare(s,p);
    s.stages[3].texture=4;reject(s,MaterialPlanError::textureUnits);
    s=baseline();s.stageCount=0;reject(s,MaterialPlanError::stages);s.stageCount=7;reject(s,MaterialPlanError::stages);
    s=baseline();s.indirectCount=1;reject(s,MaterialPlanError::indirect);s.indirectCount=0;s.stages[0].indirect=624;reject(s,MaterialPlanError::indirect);
    for(unsigned which=0;which<5;++which){s=baseline();auto& op=s.stages[0].colorOp;
        if(which==0)op.operation=2;if(which==1)op.bias=1;if(which==2)op.scale=3;if(which==3)op.output=4;if(which==4)op.clamp=2;
        // ADDHALF plans exactly since Alpha 12 (the operation rules).
        if(which==1){assert(planPicaMaterial(s,p)&&(p.lowering&MaterialLowering::addHalf));sweep(s,p,5);continue;}
        reject(s,MaterialPlanError::operation);}
    s=baseline();s.stages[0].color={4,15,15,15};s.colors[2][0]=-1;
    assert(planPicaMaterial(s,p));compare(s,p);
    // A signed D register splits into a clamped add and subtract.
    s.stages[0].color={15,15,15,4};assert(planPicaMaterial(s,p)&&(p.lowering&MaterialLowering::signedD));compare(s,p,1024);
    // Actual file-select cursor tint: signed initial register in A=B=C,
    // zero D, independent original texture/raster alpha product.
    s=baseline();s.stages[0].color={2,2,2,15};s.stages[0].alpha={7,4,5,7};
    s.colors[1]={-150,-190,-180,0};assert(planPicaMaterial(s,p));compare(s,p,4096);
    assert(p.stages[0].constant[0]==106&&p.stages[0].constant[1]==66&&p.stages[0].constant[2]==76);
    // All signed11-bit inputs, RGB and alpha, exercise each interpolant slot
    // and keep the signed D path separate from low-byte wrapping.
    unsigned wrappedCases=0;
    for(int value=-1024;value<=1023;++value)for(unsigned a=0;a<2;++a)for(unsigned slot=0;slot<3;++slot){
        s=baseline();s.colors[2].fill(static_cast<std::int16_t>(value));
        if(a){s.stages[0].alpha={7,7,7,7};s.stages[0].alpha[slot]=2;
            if(slot==1)s.stages[0].alpha[2]=4;if(slot==2)s.stages[0].alpha[1]=4;}
        else{s.stages[0].color={15,15,15,15};s.stages[0].color[slot]=4;
            if(slot==1)s.stages[0].color[2]=8;if(slot==2)s.stages[0].color[1]=8;}
        assert(planPicaMaterial(s,p));compare(s,p,16);++wrappedCases;
    }
    s=baseline();s.stages[0].color={4,4,4,15};s.colors[2][0]=1024;reject(s,MaterialPlanError::constantRange);
    s.colors[2][0]=-1025;reject(s,MaterialPlanError::constantRange);
    s=baseline();s.colors[2][3]=-1;s.stages[0].alpha={7,7,7,2};assert(planPicaMaterial(s,p));compare(s,p);
    s.colors[2][3]=-256;assert(planPicaMaterial(s,p)&&(p.lowering&MaterialLowering::signedD));compare(s,p,1024);
    std::printf("PASS signed11 initial register low-byte inputs: %u cases plus captured menu tint; signed D retains its value\n",wrappedCases);
    // Captured exterior camera/tree material: an initial negative D must
    // subtract after the convex interpolation, before clamping and scaling.
    s=baseline();s.stageCount=2;s.texgenCount=2;
    s.colors[2]={0,153,7,128};s.colors[3]={-77,-113,-73,255};
    s.konst[1]={230,255,190,255};s.konst[2]={88,255,48,255};
    s.stages[0].color={4,14,8,6};s.stages[0].alpha={7,7,7,7};
    s.stages[0].konstColor=13;s.stages[0].texture=s.stages[0].coordinate=0;
    s.stages[1].color={15,14,8,0};s.stages[1].alpha={7,7,6,7};
    s.stages[1].konstColor=14;s.stages[1].konstAlpha=31;
    s.stages[1].texture=s.stages[1].coordinate=1;
    assert(planPicaMaterial(s,p)&&p.stageCount==4);compare(s,p,16384);
    unsigned subtractCases=0;
    for(unsigned alpha=0;alpha<2;++alpha)for(int offset=-255;offset<0;++offset)for(unsigned scale=0;scale<3;++scale){
        auto candidate=baseline();candidate.colors[2].fill(offset);
        if(alpha){candidate.stages[0].alpha={4,5,4,2};candidate.stages[0].alphaOp.scale=scale;}
        else{candidate.stages[0].color={8,10,8,4};candidate.stages[0].colorOp.scale=scale;}
        assert(planPicaMaterial(candidate,p));compare(candidate,p,64);++subtractCases;
    }
    std::printf("PASS captured negative-D interpolation 16384 fragments and %u RGB/alpha signed subtract/scale cases\n",subtractCases);
    s=baseline();s.stages[0].color={0,15,15,15};s.colorValid=14;reject(s,MaterialPlanError::uninitialized);
    s=baseline();s.stages[0].color={14,15,15,15};s.stages[0].konstColor=4;reject(s,MaterialPlanError::konstFraction);
    // Opt-in native constants are bounded to half an output byte for these
    // one-stage equations. Strict mode still refuses nominal fractions.
    for(unsigned selector=1;selector<8;++selector) {
        s=baseline();s.stages[0].color={15,14,8,4};s.stages[0].konstColor=selector;
        s.colors[2]={184,0,0,0};s.stages[0].alpha={4,7,7,7};
        assert(planPicaMaterial(s,p,nullptr,MaterialPolicy::quantizedConstants));
        assert(p.quantizedConstants&&p.stageCount==2);
        for(unsigned v=0;v<256;++v){Samples samples{};
            for(auto& map:samples.texture)for(auto& tc:map)tc={v/255.0,v/255.0,v/255.0,v/255.0};
            const auto a=gxEvaluate(s,samples),b=picaEvaluate(p,samples);
            for(unsigned c=0;c<4;++c)assert(std::abs(a[c]-b[c])<=.5/255+1e-12);
        }
    }
    s=baseline();s.stages[0].color={13,15,15,15};
    assert(planPicaMaterial(s,p,nullptr,MaterialPolicy::quantizedConstants)&&p.stages[0].constant[0]==128);
    s=baseline();s.swaps[0][0]=1;reject(s,MaterialPlanError::swap);
    s=baseline();s.stages[0].color={4,15,15,14};assert(planPicaMaterial(s,p)&&p.stageCount==2);compare(s,p,1024);
    // Original progressive-mode J2D tint: textured PREV is interpolated
    // between independently changing black and white endpoints, followed by
    // vertex modulation. Synthetic bytes only; both endpoint orderings vary.
    for(unsigned n=0;n<256;++n)for(unsigned reverse=0;reverse<2;++reverse) {
        s=baseline();s.stageCount=3;
        s.colors[1]={std::int16_t(n/2),std::int16_t(n/3),0,0};
        s.colors[2]={255,200,0,255};
        if(reverse)std::swap(s.colors[1],s.colors[2]);
        s.stages[1].textureEnabled=false;s.stages[1].raster=7;
        s.stages[1].color={2,4,0,15};s.stages[1].alpha={1,2,0,7};
        s.stages[2].textureEnabled=false;
        s.stages[2].color={15,0,10,15};s.stages[2].alpha={7,0,5,7};
        assert(planPicaMaterial(s,p)&&p.stageCount==(n<2?3:4));compare(s,p,256);
    }
    s=baseline();s.stages[0].color={2,4,8,15};
    s.colors[1]={50,200,0,0};s.colors[2]={200,50,0,255};
    assert(planPicaMaterial(s,p)&&p.stageCount==2);compare(s,p,1024); // prologue for mixed component signs
    s.colors[1]={1,2,3,0};s.colors[2]={200,210,220,255};
    // Six stages that overflow the budget, stages 1-5 replacing PREV with
    // their sample: the first passes refuse. Stages 0-4 are dead (nothing
    // reads their PREV), so the Alpha 12 liveness rule plans the last one.
    s.stageCount=6;setMaterialOperationRules(false);reject(s,MaterialPlanError::stages);setMaterialOperationRules(true);
    assert(planPicaMaterial(s,p)&&(p.lowering&MaterialLowering::registerLiveness));compare(s,p,256);
    for(unsigned t=1;t<6;++t)s.stages[t].color={8,15,15,0}; // all live: still over budget
    reject(s,MaterialPlanError::stages);
    s=baseline();s.stages[0].color={8,10,9,4};assert(planPicaMaterial(s,p)&&p.stageCount==2);compare(s,p,256);
    s.stages[0].alpha={4,5,4,2};s.stages[0].alphaOp.scale=2;
    assert(planPicaMaterial(s,p)&&p.stageCount==2);compare(s,p,256);
    // PREV + blend: refused by the wave 1 rules, two multiply-adds with the
    // sum rules (runningSumCases covers the rule).
    s.stageCount=2;s.stages[1].color={8,10,9,0};
    setMaterialSumRules(false);reject(s,MaterialPlanError::equation);setMaterialSumRules(true);
    assert(planPicaMaterial(s,p)&&p.lowering==MaterialLowering::runningSum);compare(s,p,1024);
    s=baseline();s.stageCount=6;s.stages[5].color={8,10,9,4};
    setMaterialOperationRules(false);reject(s,MaterialPlanError::stages);setMaterialOperationRules(true);
    assert(planPicaMaterial(s,p)&&(p.lowering&MaterialLowering::registerLiveness)&&p.stageCount==2);compare(s,p,256);
    s=baseline();s.stages[0].textureEnabled=false;reject(s,MaterialPlanError::source);
    s=baseline();s.swapValid=0;s.channelCount=0;s.stages[0].raster=7;s.stages[0].color={10,15,15,15};s.stages[0].alpha={5,7,7,7};assert(planPicaMaterial(s,p)&&p.textureCount==0);compare(s,p);
    // Intermediate color registers preserve PREV and carry stable independent
    // texture/raster sources. Test with varying synthetic constants/texels.
    for(unsigned reg=1;reg<4;++reg)for(unsigned variant=0;variant<4;++variant) {
        s=baseline();s.stageCount=3;
        s.stages[0].color={15,8,10,15};s.stages[0].alpha={7,5,4,7};
        s.stages[1].texture=1;s.stages[1].coordinate=1;
        s.stages[1].color={std::uint8_t(variant==0?8:variant==1?10:variant==2?12:14),15,15,15};
        s.stages[1].colorOp.output=reg;s.stages[1].alpha={7,7,7,0};
        s.stages[2].color={15,std::uint8_t(reg*2),0,15};s.stages[2].colorOp.scale=2;
        s.stages[2].alpha={7,7,7,0};
        assert(planPicaMaterial(s,p));compare(s,p,1024);
    }
    s=baseline();s.stageCount=3;s.stages[1].alpha={4,7,7,7};s.stages[1].alphaOp.output=2;
    s.stages[2].color={5,15,15,15};s.stages[2].alpha={2,7,7,7};
    assert(planPicaMaterial(s,p));compare(s,p,1024);
    // An unclamped intermediate copy remains in range without clamping.
    // This is the room's direct texture-register algebra, with synthetic data.
    s=baseline();s.stageCount=3;s.colors[1]={0,0,0,255};s.konst[0]={0,0,0,255};s.konst[1]={0,0,0,0};s.konst[3]={255,255,255,255};
    s.stages[0].color={15,8,10,15};s.stages[0].alpha={7,5,4,7};
    s.stages[1].texture=1;s.stages[1].coordinate=1;s.stages[1].color={15,12,14,8};
    s.stages[1].konstColor=17;s.stages[1].colorOp.output=3;s.stages[1].colorOp.clamp=0;s.stages[1].alpha={7,7,7,0};
    s.stages[2].color={14,6,0,2};s.stages[2].konstColor=12;s.stages[2].colorOp.scale=2;
    s.stages[2].alpha={7,6,0,7};s.stages[2].konstAlpha=31;
    assert(planPicaMaterial(s,p));compare(s,p,4096);
    // Actual room alpha stages preserve PREV and then multiply it by K.a=1
    // without a GX clamp. PICA's compulsory clamp cannot change those values.
    s.stages[1].colorOp.clamp=1;s.stages[1].alphaOp.clamp=0;s.stages[2].alphaOp.clamp=0;
    s.stages[1].raster=s.stages[2].raster=7;s.stages[2].textureEnabled=false;
    assert(planPicaMaterial(s,p));compare(s,p,4096);
    // Captured exterior variant adds 45/255 to texture1 in REG2 while PREV
    // keeps texture0*raster. The final stage consumes both independently.
    s.konst[1][0]=45;
    assert(planPicaMaterial(s,p)&&p.stageCount==3&&p.bufferUpdateRgb==1);compare(s,p,4096);
    for(unsigned n=1;n<256;++n)for(unsigned reg=1;reg<4;++reg)for(unsigned scale=0;scale<3;++scale) {
        auto exterior=s;exterior.konst[1][0]=n;
        exterior.stages[1].colorOp.output=reg;
        exterior.stages[2].color={14,std::uint8_t(reg*2),0,15};exterior.stages[2].colorOp.scale=scale;
        assert(planPicaMaterial(exterior,p)&&p.bufferUpdateRgb==1);compare(exterior,p,64);
    }
    // A split stage before the register changes the capture index. A split
    // computed temporary must retain the captured original PREV throughout.
    auto exterior=s;
    exterior.stages[0].color={8,10,9,4};
    assert(planPicaMaterial(exterior,p)&&p.bufferUpdateRgb==2);compare(exterior,p,4096);
    exterior=s;exterior.stages[1].color={15,14,8,4};
    assert(planPicaMaterial(exterior,p)&&p.stageCount==4&&p.bufferUpdateRgb==1);compare(exterior,p,4096);
    // A hoisted register still cannot share the initial buffer with another
    // RGB constant; report the actual allocation conflict.
    exterior=s;exterior.stages[0].color={2,4,8,15};
    exterior.colors[1]={50,200,0,0};exterior.colors[2]={200,50,0,255};
    reject(exterior,MaterialPlanError::constantConflict);
    exterior=s;exterior.stageCount=4;
    assert(planPicaMaterial(exterior,p)&&p.bufferUpdateRgb==1);compare(exterior,p,8192);
    exterior=s;exterior.stageCount=4;exterior.stages[3]=s.stages[2];exterior.stages[2]=s.stages[1];
    exterior.stages[1].colorOp.output=0;exterior.stages[1].color={2,4,8,15};
    exterior.colors[1]={50,200,0,0};exterior.colors[2]={200,50,0,255};
    // The register at stage 2 is hoisted; the crossing endpoints then need
    // the buffer it occupies: the hoist's own conflict is reported.
    reject(exterior,MaterialPlanError::constantConflict);
    exterior=s;exterior.stages[0]=s.stages[1];exterior.stages[1]=s.stages[2];exterior.stageCount=2;
    reject(exterior,MaterialPlanError::operation);
    // Bounded copies, convex interpolation and products admit clamp=0. A sum
    // or scale which can exceed one must reject, even if later stages clamp it.
    for(unsigned variant=0;variant<3;++variant) {
        s=baseline();s.stages[0].colorOp.clamp=s.stages[0].alphaOp.clamp=0;
        if(variant==1){s.stages[0].color={15,8,10,15};s.stages[0].alpha={7,4,5,7};}
        if(variant==2){s.stages[0].color={8,10,9,15};s.stages[0].alpha={4,5,4,7};}
        assert(planPicaMaterial(s,p));compare(s,p,4096);
    }
    // Before the last stage (a second stage reads it); on the last stage the
    // saturated output is exact (finalClampCases).
    s=baseline();s.stageCount=2;s.stages[1].color={15,15,15,0};s.stages[1].alpha={7,7,7,0};
    // Read only as stage 1's clamped D: saturated early since Alpha 12.
    s.stages[0].colorOp.clamp=0;s.stages[0].color={8,15,15,10};plannedSinceAlpha12(s,MaterialPlanError::operation);
    s.stages[0].color={15,8,10,15};s.stages[0].colorOp.scale=1;plannedSinceAlpha12(s,MaterialPlanError::operation);
    s.stages[0].colorOp.scale=0;s.stages[0].color={8,15,15,10};s.stageCount=1;
    assert(planPicaMaterial(s,p)&&(p.lowering&MaterialLowering::finalClamp));compare(s,p,4096);
    s=baseline();s.stageCount=2;s.stages[1].color={15,15,15,0};s.stages[1].alpha={7,7,7,0};
    s.stages[0].colorOp.clamp=0;s.stages[0].colorOp.scale=1;s.colors[2]={127,127,127,255};s.stages[0].color={4,15,15,15};
    assert(planPicaMaterial(s,p));compare(s,p,4096);
    s.colors[2][1]=128;plannedSinceAlpha12(s,MaterialPlanError::operation);
    // The bounded buffer allocation also handles PREV copies and products.
    // Scaled non-PREV outputs: the register route since Alpha 12.
    s=baseline();s.stageCount=3;s.stages[1].color={0,15,15,15};s.stages[1].colorOp.output=3;assert(planPicaMaterial(s,p));compare(s,p,1024);
    s.stages[1].color={15,8,10,15};assert(planPicaMaterial(s,p));compare(s,p,1024);
    s.stages[1].color={8,15,15,15};s.stages[1].colorOp.scale=1;plannedSinceAlpha12(s,MaterialPlanError::operation);
    // Permuting bindings must preserve every material equation, independently
    // of which unit the one projected texture originally occupied.
    PicaMaterialPlan original{};original.stageCount=3;original.textureCount=3;
    original.textures={{{5,4},{2,1},{7,6}}};
    for(unsigned stage=0;stage<3;++stage)for(auto* channel:{&original.stages[stage].rgb,&original.stages[stage].alpha}) {
        channel->function=MaterialFunction::interpolate;
        for(unsigned input=0;input<3;++input)channel->inputs[input]={static_cast<MaterialSource>(unsigned(MaterialSource::texture0)+(stage+input)%3),MaterialOperand::color,bool(input&1)};
    }
    for(unsigned projected=0;projected<3;++projected) {
        auto routed=original;
        assert(routeProjectiveTexture(routed,1u<<original.textures[projected].coordinate));
        assert(routed.projectiveTexture0&&routed.textures[0]==original.textures[projected]);
        for(unsigned n=0;n<256;++n) {
            Samples samples{};
            for(auto& map:samples.texture)for(auto& uv:map)for(auto& component:uv)component=double(randomWord(seed)&255)/255;
            assert(picaEvaluate(original,samples)==picaEvaluate(routed,samples));
        }
        const auto first=routed;
        assert(routeProjectiveTexture(routed,1u<<original.textures[projected].coordinate)&&routed.textures==first.textures);
    }
    auto multiple=original;
    assert(!routeProjectiveTexture(multiple,(1u<<4)|(1u<<1))&&multiple.textures==original.textures&&!multiple.projectiveTexture0);
    assert(routeProjectiveTexture(multiple,1u<<7)&&!multiple.projectiveTexture0&&multiple.textures==original.textures);
    compareCases(seed);
    softCompareCases(seed);
    loweringBitCases(seed);
    minimapIconCases(seed);
    waterShineCases(seed);
    signedDCases(seed);
    finalClampCases(seed);
    hoistCases(seed);
    sumRuleFixtures(seed);
    sumRuleRandom(seed);
    sumRuleIdentity(seed);
    operationRuleCases(seed);
    printf("{\"random_equations\":30000,\"admitted_equations\":%u,\"mixed_clamp_equations\":30000,\"admitted_mixed_clamp_equations\":%u,\"register_equations\":30000,\"admitted_register_equations\":%u,\"bias_equations\":30000,\"admitted_bias_equations\":%u,\"plan_bytes\":%zu,\"fixed_point_gpu_verified\":false}\n",accepted,unclampedAccepted,registerAccepted,biasAccepted,sizeof(p));
}
