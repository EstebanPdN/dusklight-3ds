#include "material_equivalence.hpp"
#include "../runtime/gx_material_liveness.hpp"
#include "../runtime/gx_fog.hpp"
#include <cstdio>
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
int main(){
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
    auto unboundedHalf=baseline();unboundedHalf.stages[0].color={8,15,15,10};unboundedHalf.stages[0].colorOp.scale=3;
    assert(!planPicaMaterial(unboundedHalf,p,nullptr,MaterialPolicy::quantizedConstants));
    auto biasedHalf=baseline();biasedHalf.stages[0].colorOp.scale=3;biasedHalf.stages[0].colorOp.bias=2;
    assert(!planPicaMaterial(biasedHalf,p,nullptr,MaterialPolicy::quantizedConstants));
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
    s=baseline();s.stages[0].alphaOp.bias=2;s.stages[0].alphaOp.clamp=0;
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
    s=baseline();s.stageCount=6;for(unsigned t=0;t<6;++t)s.stages[t].texture=t%3;assert(planPicaMaterial(s,p)&&p.textureCount==3);compare(s,p);
    s.stages[3].texture=4;reject(s,MaterialPlanError::textureUnits);
    s=baseline();s.stageCount=0;reject(s,MaterialPlanError::stages);s.stageCount=7;reject(s,MaterialPlanError::stages);
    s=baseline();s.indirectCount=1;reject(s,MaterialPlanError::indirect);s.indirectCount=0;s.stages[0].indirect=624;reject(s,MaterialPlanError::indirect);
    for(unsigned which=0;which<5;++which){s=baseline();auto& op=s.stages[0].colorOp;
        if(which==0)op.operation=2;if(which==1)op.bias=1;if(which==2)op.scale=3;if(which==3)op.output=4;if(which==4)op.clamp=2;
        reject(s,MaterialPlanError::operation);}
    s=baseline();s.stages[0].color={4,15,15,15};s.colors[2][0]=-1;
    assert(planPicaMaterial(s,p));compare(s,p);
    s.stages[0].color={15,15,15,4};reject(s,MaterialPlanError::constantRange);
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
    s.colors[2][3]=-256;reject(s,MaterialPlanError::constantRange);
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
    s.stageCount=6;reject(s,MaterialPlanError::stages);
    s=baseline();s.stages[0].color={8,10,9,4};assert(planPicaMaterial(s,p)&&p.stageCount==2);compare(s,p,256);
    s.stages[0].alpha={4,5,4,2};s.stages[0].alphaOp.scale=2;
    assert(planPicaMaterial(s,p)&&p.stageCount==2);compare(s,p,256);
    s.stageCount=2;s.stages[1].color={8,10,9,0};reject(s,MaterialPlanError::equation);
    s=baseline();s.stageCount=6;s.stages[5].color={8,10,9,4};reject(s,MaterialPlanError::stages);
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
    reject(exterior,MaterialPlanError::operation);
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
    s=baseline();s.stages[0].colorOp.clamp=0;s.stages[0].color={8,15,15,10};reject(s,MaterialPlanError::operation);
    s.stages[0].color={15,8,10,15};s.stages[0].colorOp.scale=1;reject(s,MaterialPlanError::operation);
    s=baseline();s.stages[0].colorOp.clamp=0;s.stages[0].colorOp.scale=1;s.colors[2]={127,127,127,255};s.stages[0].color={4,15,15,15};
    assert(planPicaMaterial(s,p));compare(s,p,4096);
    s.colors[2][1]=128;reject(s,MaterialPlanError::operation);
    // The bounded buffer allocation also handles PREV copies and products.
    // Scaled non-PREV outputs still reject until their lifetime is supported.
    s=baseline();s.stageCount=3;s.stages[1].color={0,15,15,15};s.stages[1].colorOp.output=3;assert(planPicaMaterial(s,p));compare(s,p,1024);
    s.stages[1].color={15,8,10,15};assert(planPicaMaterial(s,p));compare(s,p,1024);
    s.stages[1].color={8,15,15,15};s.stages[1].colorOp.scale=1;reject(s,MaterialPlanError::operation);
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
    printf("{\"random_equations\":30000,\"admitted_equations\":%u,\"mixed_clamp_equations\":30000,\"admitted_mixed_clamp_equations\":%u,\"register_equations\":30000,\"admitted_register_equations\":%u,\"bias_equations\":30000,\"admitted_bias_equations\":%u,\"plan_bytes\":%zu,\"fixed_point_gpu_verified\":false}\n",accepted,unclampedAccepted,registerAccepted,biasAccepted,sizeof(p));
}
