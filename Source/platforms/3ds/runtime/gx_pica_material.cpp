#include "gx_pica_material.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
namespace dusk::native::gx {
namespace {
using u8=std::uint8_t;
struct Value {
    bool literal=true;
    std::array<u8,3> bytes{};
    MaterialInput input{};
    bool operator==(const Value&) const = default;
};
Value constant(u8 n){return {true,{n,n,n},{}};}
bool is(const Value& v,u8 n){return v.literal&&v.bytes==std::array<u8,3>{n,n,n};}
Value inverse(Value v){if(v.literal)for(auto& b:v.bytes)b=255-b;else v.input.inverse=!v.input.inverse;return v;}
// Halved literals for the divide-by-2 scale. Rounding down offsets the x/510
// excess of a following x*128/255 term, so each sum stays within 0.5/255.
Value halfDown(Value v){for(auto& b:v.bytes)b=u8(b>>1);return v;}
Value halfUp(Value v){for(auto& b:v.bytes)b=u8((b+1)>>1);return v;}
unsigned upper(const Value& v){return v.literal?*std::max_element(v.bytes.begin(),v.bytes.end()):255;}
// GX A*(1-C)+B*C after dead-input elimination: zero, one value (p), a
// product (p*q, a literal factor in q) or a blend (p*(1-r)+q*r).
struct Term {enum Kind:u8 {zero,single,product,blend} kind=zero;Value p{},q{},r{};};
struct Builder {
    const MaterialState& state;PicaMaterialPlan& plan;MaterialPolicy policy;MaterialPlanError error=MaterialPlanError::none;
    RasterCarrier carrier{};
    unsigned index=0,output=0;bool alpha=false,constantSet=false;std::array<u8,3> assigned{};
    // Copy propagation for a bounded, unscaled direct register assignment.
    // Texture/raster/literal sources are stable for a fragment. PREV is not.
    std::array<std::array<Value,4>,2> aliases{};
    std::array<std::array<bool,4>,2> aliasValid{};
    std::array<Value,2> pendingAliases{};
    std::array<bool,2> pendingValid{};
    bool aliasMode=false;
    std::array<bool,2> bufferSet{};
    unsigned temporaryReg=0,temporaryIndex=0;
    // A hoisted register lives in the combiner buffer from GX stage bufferedFrom.
    unsigned bufferedReg=0,bufferedFrom=2;
    // PICA stages after a GX stage's first one, per channel (RGB, alpha). A
    // channel with fewer steps passes PREVIOUS through the remaining stages.
    // Checked steps repeat the clamp-off bound proof of emit(); the others
    // were proved in range where they were planned.
    struct Step {MaterialFunction function=MaterialFunction::replace;Value a{},b{},c{};u8 scale=0;bool checked=false;};
    std::array<std::array<Step,4>,2> tail{};
    std::array<u8,2> tailCount{};
    // Scale applied after the stage's sum (0 for a halved sum) and the shift
    // the clamp-off bound proof of emit() uses.
    u8 finishScale=0;unsigned boundShift=0;
    std::uint16_t lowering=0;
    // Second planner pass: the sum rules (MaterialLowering::sumRules) replace
    // refusals of the first. Off, every path is the wave 1 planner's.
    bool sumRules=false;
    // Third planner pass: the operation rules (MaterialLowering::operationRules)
    // replace refusals of the first two. Off, every path is the Alpha 11 one.
    bool operationRules=false;
    MaterialOperationReason reason=MaterialOperationReason::none;
    bool fail(MaterialPlanError e,MaterialOperationReason why=MaterialOperationReason::none){error=e;reason=why;return false;}
    // The last stage's result is the output, saturated by the 8-bit target
    // and by the alpha test on the clamped value (aurora clamps it the same
    // way): clamp off there is clamp on, which PICA always applies. Set only
    // on a second build, after the plain one failed (see buildPicaMaterial).
    bool saturateLast=false;
    MaterialOperation operation(bool isAlpha)const{
        auto op=isAlpha?state.stages[index].alphaOp:state.stages[index].colorOp;
        if(saturateLast&&index+1==state.stageCount&&!op.clamp)op.clamp=1;
        return op;
    }
    PicaMaterialChannel& current(){return alpha?plan.stages[output].alpha:plan.stages[output].rgb;}
    Value previous()const{return {false,{}, {MaterialSource::previous,alpha?MaterialOperand::alpha:MaterialOperand::color,false}};}
    bool push(MaterialFunction f,const Value& a,const Value& b,const Value& c,u8 scale,bool checked=false){
        if(tailCount[alpha]>=tail[alpha].size())return fail(MaterialPlanError::stages);
        tail[alpha][tailCount[alpha]++]={f,a,b,c,scale,checked};return true;
    }
    bool color(unsigned reg,bool componentAlpha,Value& v,bool wrap=false){
        if(!componentAlpha&&bufferedReg&&reg==bufferedReg&&index>=bufferedFrom){
            v={false,{}, {MaterialSource::buffer,MaterialOperand::color,false}};return true;
        }
        if(!componentAlpha&&temporaryReg&&index>temporaryIndex&&(reg==0||reg==temporaryReg)) {
            v={false,{}, {reg==0?MaterialSource::buffer:MaterialSource::previous,MaterialOperand::color,false}};return true;
        }
        if(reg!=0 && aliasValid[componentAlpha][reg]) {v=aliases[componentAlpha][reg];return true;}
        if(reg==0&&index){v={false,{}, {MaterialSource::previous,componentAlpha?MaterialOperand::alpha:MaterialOperand::color,false}};return true;}
        if(!(state.colorValid&(1u<<reg)))return fail(MaterialPlanError::uninitialized);
        v=constant(0);
        for(unsigned c=0;c<3;++c){
            const auto n=state.colors[reg][componentAlpha?3:c];
            // GX truncates register inputs A/B/C to their low eight bits.
            // D retains signed precision and needs a separate lowering.
            if(wrap?(n < -1024 || n > 1023):(n < 0 || n > 255))return fail(MaterialPlanError::constantRange);
            v.bytes[c]=u8(n);
        }
        return true;
    }
    // True when color() would not return the initial register literal.
    bool routed(unsigned reg,bool componentAlpha)const{
        if(!componentAlpha&&bufferedReg&&reg==bufferedReg&&index>=bufferedFrom)return true;
        if(!componentAlpha&&temporaryReg&&index>temporaryIndex&&(reg==0||reg==temporaryReg))return true;
        return (reg!=0&&aliasValid[componentAlpha][reg])||(reg==0&&index);
    }
    bool konst(unsigned selector,Value& v){
        if(selector==0){v=constant(255);return true;}
        if(selector<8){
            if(policy==MaterialPolicy::exactConstants)return fail(MaterialPlanError::konstFraction);
            // Nearest UNORM8 to the nominal GX fraction. Also agrees with the
            // hardware konst byte table documented by Dolphin. TEV/PICA
            // interpolation rounding differs, so mark this bounded Tier D path.
            constexpr u8 fractions[]{255,223,191,159,128,96,64,32};
            v=constant(fractions[selector]);plan.quantizedConstants=true;return true;
        }
        unsigned reg,component=4;
        if(!alpha&&selector>=12&&selector<=15)reg=selector-12;
        else if(selector>=16&&selector<=31){reg=(selector-16)%4;component=(selector-16)/4;}
        else return fail(MaterialPlanError::source);
        if(!(state.konstValid&(1u<<reg)))return fail(MaterialPlanError::uninitialized);
        v=constant(0);for(unsigned c=0;c<3;++c)v.bytes[c]=state.konst[reg][component==4?c:component];return true;
    }
    bool sampled(bool texture,bool componentAlpha,Value& v){
        const auto& stage=state.stages[index];
        if(!texture&&stage.raster==7){v=constant(0);return true;}
        const unsigned swap=texture?stage.textureSwap:stage.rasterSwap;
        if(swap>=4||!(state.swapValid&(1u<<swap)))return fail(MaterialPlanError::uninitialized);
        const auto& swizzle=state.swaps[swap];MaterialOperand operand=MaterialOperand::color;
        if(componentAlpha){
            constexpr MaterialOperand operands[]{MaterialOperand::red,MaterialOperand::green,MaterialOperand::blue,MaterialOperand::alpha};
            if(swizzle[3]>3)return fail(MaterialPlanError::swap);
            operand=operands[swizzle[3]];
        }else if(swizzle[0]==swizzle[1]&&swizzle[1]==swizzle[2]&&swizzle[0]<4){
            // PICA can broadcast one sampled channel to RGB directly. Other
            // nonidentity permutations still need an unsupported lowering.
            constexpr MaterialOperand operands[]{MaterialOperand::red,MaterialOperand::green,MaterialOperand::blue,MaterialOperand::alpha};
            operand=operands[swizzle[0]];
        }else if(swizzle[0]!=0||swizzle[1]!=1||swizzle[2]!=2)return fail(MaterialPlanError::swap);
        MaterialSource source=MaterialSource::primary;
        if(texture){
            if(!stage.textureEnabled||stage.texture>=8||stage.coordinate>=state.texgenCount)return fail(MaterialPlanError::source);
            const MaterialTextureBinding binding{stage.texture,stage.coordinate};unsigned slot=0;
            while(slot<plan.textureCount&&plan.textures[slot]!=binding)++slot;
            if(slot==plan.textureCount){if(slot==3)return fail(MaterialPlanError::textureUnits);plan.textures[slot]=binding;++plan.textureCount;}
            source=static_cast<MaterialSource>(unsigned(MaterialSource::texture0)+slot);
        }else{
            if(carrier==RasterCarrier::raster0RgbaRaster1Lookup){
                if(stage.raster>1||state.channelCount!=2)return fail(MaterialPlanError::source);
                plan.rasterReadMask[stage.raster]|=operand==MaterialOperand::alpha?2:1;
                source=stage.raster?MaterialSource::texture2:MaterialSource::primary;
            }else if(carrier==RasterCarrier::raster1RgbRaster0GrayAlpha){
                if(stage.raster>1||state.channelCount!=2||componentAlpha||operand!=MaterialOperand::color)
                    return fail(MaterialPlanError::source);
                operand=stage.raster==0?MaterialOperand::alpha:MaterialOperand::color;
            }else if(stage.raster!=0||state.channelCount<1)return fail(MaterialPlanError::source);
        }
        v={false,{}, {source,operand,false}};return true;
    }
    bool input(unsigned id,Value& v,bool wrap=true){
        if(alpha){
            if(id<4)return color(id,true,v,wrap);
            if(id==4)return sampled(true,true,v);
            if(id==5)return sampled(false,true,v);
            if(id==6)return konst(state.stages[index].konstAlpha,v);
            if(id==7){v=constant(0);return true;}
        }else{
            if(id<8)return color(id/2,id&1,v,wrap);
            if(id==8||id==9)return sampled(true,id==9,v);
            if(id==10||id==11)return sampled(false,id==11,v);
            if(id==12){v=constant(255);return true;}
            if(id==13){
                if(policy==MaterialPolicy::exactConstants)return fail(MaterialPlanError::konstFraction);
                v=constant(128);plan.quantizedConstants=true;return true;
            }
            if(id==14)return konst(state.stages[index].konstColor,v);
            if(id==15){v=constant(0);return true;}
        }
        return fail(MaterialPlanError::source);
    }
    bool bind(const Value& v,MaterialInput& out){
        if(!v.literal){out=v.input;return true;}
        if(constantSet&&assigned!=v.bytes){
            // PICA exposes the programmed initial combiner buffer starting at
            // stage1. With all buffer-update masks disabled it is a second,
            // immutable RGB/alpha constant for the remaining material.
            if(!output||(alpha?plan.bufferUpdateAlpha:plan.bufferUpdateRgb))return fail(MaterialPlanError::constantConflict);
            auto& buffer=plan.bufferConstant;
            for(unsigned c=0;c<(alpha?1u:3u);++c)
                if(bufferSet[alpha]&&buffer[alpha?3:c]!=v.bytes[c])return fail(MaterialPlanError::constantConflict);
            for(unsigned c=0;c<(alpha?1u:3u);++c)buffer[alpha?3:c]=v.bytes[c];
            bufferSet[alpha]=true;
            out={MaterialSource::buffer,alpha?MaterialOperand::alpha:MaterialOperand::color,false};return true;
        }
        constantSet=true;assigned=v.bytes;auto& c=plan.stages[output].constant;
        if(alpha)c[3]=v.bytes[0];else for(unsigned n=0;n<3;++n)c[n]=v.bytes[n];
        out={MaterialSource::constant,alpha?MaterialOperand::alpha:MaterialOperand::color,false};return true;
    }
    bool place(MaterialFunction f,const Value& a,const Value& b=constant(0),const Value& c=constant(0)){
        auto& out=current();out.function=f;
        if(!bind(a,out.inputs[0]))return false;
        if(f!=MaterialFunction::replace&&!bind(b,out.inputs[1]))return false;
        if((f==MaterialFunction::interpolate||f==MaterialFunction::multiplyAdd)&&!bind(c,out.inputs[2]))return false;
        return true;
    }
    bool emit(MaterialFunction f,const Value& a,const Value& b=constant(0),const Value& c=constant(0)){
        if(!operation(alpha).clamp) {
            // PICA clamps every result. That clamp is harmless only when the
            // complete unscaled GX equation, including its scale, stays in
            // [0,1]. All admitted inputs are nonnegative and bounded: literals
            // were checked on read, samples are UNORM, and PREV was produced by
            // a clamped or independently bounded preceding stage. Use integer
            // numerator bounds to avoid floating-point acceptance at the edge.
            const unsigned x=upper(a),y=upper(b),z=upper(c);
            unsigned bound=x*255;
            switch(f) {
            case MaterialFunction::replace:break;
            case MaterialFunction::modulate:bound=x*y;break;
            case MaterialFunction::add:bound=(x+y)*255;break;
            case MaterialFunction::interpolate:bound=std::max(x,y)*255;break;
            case MaterialFunction::multiplyAdd:bound=x*y+z*255;break;
            case MaterialFunction::addSigned:case MaterialFunction::subtract:return fail(MaterialPlanError::operation,MaterialOperationReason::clampOffSigned);
            }
            if(bound*(1u<<boundShift)>255u*255u)return fail(MaterialPlanError::operation,MaterialOperationReason::clampOffBound);
        }
        if(aliasMode) {
            if(f!=MaterialFunction::replace || (!a.literal && a.input.source==MaterialSource::previous))
                return fail(MaterialPlanError::operation,MaterialOperationReason::registerValue);
            pendingAliases[alpha]=a;pendingValid[alpha]=true;return true;
        }
        return place(f,a,b,c);
    }
    bool add(const Value& a,const Value& d){
        if(is(a,0))return emit(MaterialFunction::replace,d);
        if(is(d,0))return emit(MaterialFunction::replace,a);
        return emit(MaterialFunction::add,a,d);
    }
    bool multiplyAdd(const Value& a,const Value& b,const Value& d){
        if(is(a,0)||is(b,0))return emit(MaterialFunction::replace,d);
        if(is(a,255))return add(b,d);
        if(is(b,255))return add(a,d);
        if(is(d,0))return emit(MaterialFunction::modulate,a,b);
        if(d.literal&&((a.literal&&a.bytes!=d.bytes)||(b.literal&&b.bytes!=d.bytes))) {
            // PICA has one RGB constant per stage. The unscaled bounded
            // product can be clamped without changing it, then added to D.
            if(aliasMode)return fail(MaterialPlanError::operation,MaterialOperationReason::registerSplit);
            if(output+1>=plan.stages.size())return fail(MaterialPlanError::stages);
            if(!emit(MaterialFunction::modulate,a,b))return false;
            current().scale=0;
            return push(MaterialFunction::add,previous(),d,constant(0),finishScale,true);
        }
        return emit(MaterialFunction::multiplyAdd,a,b,d);
    }
    bool interpolate(const Value& a,const Value& b,const Value& factor) {
        // GX A*(1-C)+B*C can use two distinct literal endpoints. PICA has
        // one RGB constant per stage. For componentwise ordered endpoints,
        // use abs(B-A)*C (or 1-C), then add the smaller endpoint. The product
        // is bounded, so its intermediate clamp cannot change the equation.
        if(!a.literal||!b.literal||a==b)
            return emit(MaterialFunction::interpolate,b,a,factor);
        bool increasing=true,decreasing=true;
        for(unsigned c=0;c<3;++c) {
            increasing&=b.bytes[c]>=a.bytes[c];
            decreasing&=a.bytes[c]>=b.bytes[c];
        }
        if(!increasing&&!decreasing)return emit(MaterialFunction::interpolate,b,a,factor);
        if(aliasMode)return fail(MaterialPlanError::operation,MaterialOperationReason::registerSplit);
        if(output+1>=plan.stages.size())return fail(MaterialPlanError::stages);
        const Value& low=increasing?a:b;
        const Value& high=increasing?b:a;
        Value difference=constant(0);
        for(unsigned c=0;c<3;++c)difference.bytes[c]=high.bytes[c]-low.bytes[c];
        if(!emit(MaterialFunction::modulate,increasing?factor:inverse(factor),difference))return false;
        current().scale=0;
        return push(MaterialFunction::add,previous(),low,constant(0),finishScale,true);
    }
    // The convex interpolation alone, for lowerings that add D afterwards.
    bool lerp(const Value& a,const Value& b,const Value& c){
        if(a==b||is(c,0))return emit(MaterialFunction::replace,a);
        if(is(c,255))return emit(MaterialFunction::replace,b);
        if(is(a,0))return emit(MaterialFunction::modulate,b,c);
        if(is(b,0))return emit(MaterialFunction::modulate,a,inverse(c));
        return emit(MaterialFunction::interpolate,b,a,c);
    }
    bool term(const std::array<u8,4>& args,Term& x){
        const unsigned zero=alpha?7:15;Value a,b,c;x={};
        if(args[0]==args[1]||args[2]==zero){if(!input(args[0],a))return false;x={Term::single,a};}
        else{
            if(!input(args[2],c))return false;
            if(is(c,0)){if(!input(args[0],a))return false;x={Term::single,a};}
            else if(is(c,255)){if(!input(args[1],b))return false;x={Term::single,b};}
            else{
                if(!input(args[0],a)||!input(args[1],b))return false;
                if(a==b)x={Term::single,a};
                else if(is(a,0))x={Term::product,b,c};
                else if(is(b,0))x={Term::product,a,inverse(c)};
                else x={Term::blend,a,b,c};
            }
        }
        if(x.kind==Term::single&&is(x.p,0))x={};
        if(x.kind==Term::product){
            if(is(x.p,0)||is(x.q,0))x={};
            else if(is(x.p,255))x={Term::single,x.q};
            else if(is(x.q,255))x={Term::single,x.p};
            else if(x.p.literal&&!x.q.literal)std::swap(x.p,x.q);
        }
        return true;
    }
    bool emitTerm(const Term& x){
        switch(x.kind){
        case Term::zero:return emit(MaterialFunction::replace,constant(0));
        case Term::single:return emit(MaterialFunction::replace,x.p);
        case Term::product:return emit(MaterialFunction::modulate,x.p,x.q);
        case Term::blend:return emit(MaterialFunction::interpolate,x.q,x.p,x.r);
        }
        return fail(MaterialPlanError::equation);
    }
    // clamp(D-B*C) = 1-clamp(B*C + (1-D)). The complement form keeps D
    // available in PREVIOUS while forming the product, including when the
    // RGB buffer already holds another register. The caller checks clamp on.
    bool subtract(const std::array<u8,4>& args){
        if(aliasMode)return fail(MaterialPlanError::operation,MaterialOperationReason::registerSubtract);
        if(output+1>=plan.stages.size())return fail(MaterialPlanError::stages);
        Value a,b,c,d;
        if(!input(args[0],a)||!input(args[1],b)||!input(args[2],c)||!input(args[3],d,false))return false;
        bool okay=false;
        if(is(a,0))okay=emit(MaterialFunction::multiplyAdd,b,c,inverse(d));
        else if(is(b,0))okay=emit(MaterialFunction::multiplyAdd,a,inverse(c),inverse(d));
        else if(sumRules)return subtractTerms(a,b,c,d);
        else return fail(MaterialPlanError::equation);
        if(!okay)return false;
        current().scale=0;
        return push(MaterialFunction::replace,inverse(previous()),constant(0),constant(0),finishScale,true);
    }
    static bool readsPrevious(const Value& v){return !v.literal&&v.input.source==MaterialSource::previous;}
    // Sum rule: clamp(D-X) with A and B both nonzero (clamp on). One value X
    // (A=B, C=0 or C=1) is one SUBTRACT, or a folded byte difference for
    // literals. A blend X (in [0,1]) is formed first and then subtracted
    // from D, which must outlive it: D not PREV as is, PREV as D in the
    // combiner buffer. Without a free buffer, A*(1-C) and then B*C add to
    // 1-D before the complement: the partial sums are nonnegative, so the
    // clamp between them saturates only when the whole sum does, and
    // 1-clamp(1-D+X) is clamp(D-X); C and the second product must not read
    // PREV, which the first replaces.
    bool subtractTerms(const Value& a,const Value& b,const Value& c,const Value& d){
        lowering|=MaterialLowering::subtractTerms;
        if(a==b||is(c,0)||is(c,255)){
            const Value& x=is(c,255)?b:a;
            if(x.literal&&d.literal){
                Value difference=constant(0);
                for(unsigned n=0;n<3;++n)difference.bytes[n]=u8(std::max(0,int(d.bytes[n])-int(x.bytes[n])));
                return emit(MaterialFunction::replace,difference);
            }
            return emit(MaterialFunction::subtract,d,x);
        }
        Value p=a,q=b,first=inverse(c),second=c;
        const bool complement=!readsPrevious(c)&&!(readsPrevious(p)&&readsPrevious(q));
        if(!readsPrevious(d)||!complement){
            Value kept=d;
            if(readsPrevious(d)&&!capturePrevious(kept))return false;
            if(!emit(MaterialFunction::interpolate,b,a,c))return false;
            current().scale=0;
            return push(MaterialFunction::subtract,kept,previous(),constant(0),finishScale,true);
        }
        if(readsPrevious(q)){std::swap(p,q);std::swap(first,second);}
        if(output+2>=plan.stages.size())return fail(MaterialPlanError::stages);
        if(!emit(MaterialFunction::multiplyAdd,p,first,inverse(d)))return false;
        current().scale=0;
        return push(MaterialFunction::multiplyAdd,q,second,previous(),0,true)&&
            push(MaterialFunction::replace,inverse(previous()),constant(0),constant(0),finishScale,true);
    }
    // Sum rule: GX D + A*(1-C) + B*C with PREV as D. Two multiply-adds,
    // D + A*(1-C) first (it reads PREV), then B*C + PREVIOUS with the scale.
    // All terms are nonnegative and the clamp is on (or the stage is last),
    // so the clamp between them saturates only when the whole sum does, and
    // a 2x/4x of a saturated sum saturates too. C and the second product
    // must not read PREV; otherwise PREV waits in the combiner buffer while
    // the blend replaces it (capturePrevious), then one ADD.
    bool runningSum(const Value& a,const Value& b,const Value& c,Value d){
        if(aliasMode)return fail(MaterialPlanError::equation);
        if(output+1>=plan.stages.size())return fail(MaterialPlanError::stages);
        if(!readsPrevious(c)&&!(readsPrevious(a)&&readsPrevious(b))){
            // The product that reads PREV goes first.
            const bool swap=readsPrevious(b);
            if(!emit(MaterialFunction::multiplyAdd,swap?b:a,swap?c:inverse(c),d))return false;
            current().scale=0;lowering|=MaterialLowering::runningSum;
            return push(MaterialFunction::multiplyAdd,swap?a:b,swap?inverse(c):c,previous(),finishScale,true);
        }
        if(!capturePrevious(d)||!emit(MaterialFunction::interpolate,b,a,c))return false;
        current().scale=0;
        return push(MaterialFunction::add,previous(),d,constant(0),finishScale,true);
    }
    // Sum rule: PREV as D moves to the combiner buffer, captured at the
    // stage before this one and visible from the next, where the finishing
    // step reads it while PREVIOUS holds the term. The channel's buffer must
    // hold nothing else: no capture yet (later buffer constants then refuse
    // in bind), no hoisted or temporary register (RGB), and D must be the
    // channel's own PREV component (APREV in RGB would need the alpha buffer).
    bool capturePrevious(Value& d){
        auto& update=alpha?plan.bufferUpdateAlpha:plan.bufferUpdateRgb;
        const auto own=alpha?MaterialOperand::alpha:MaterialOperand::color;
        if(aliasMode||!readsPrevious(d)||d.input.operand!=own||d.input.inverse||!output||output>4||update||
           (!alpha&&(bufferedReg||temporaryReg)))return fail(MaterialPlanError::equation);
        update=u8(1u<<(output-1));lowering|=MaterialLowering::previousCapture;
        d.input.source=MaterialSource::buffer;return true;
    }
    // Sum rule: PREV + X - 1/2 (clamp on). One value X is one ADD_SIGNED;
    // a product or blend X is formed after PREV moved to the buffer, then
    // ADD_SIGNED adds the two (both in [0,1], so nothing clamps between).
    bool biasPrevious(const Value& a,const Value& b,const Value& c,Value d){
        if(aliasMode)return fail(MaterialPlanError::equation);
        plan.quantizedConstants=true;
        if(a==b||is(c,0)||is(c,255)){
            lowering|=MaterialLowering::biasFold;
            return emit(MaterialFunction::addSigned,d,is(c,255)?b:a);
        }
        if(!capturePrevious(d)||!lerp(a,b,c))return false;
        current().scale=0;
        return push(MaterialFunction::addSigned,previous(),d,constant(0),finishScale,true);
    }
    // A signed or >255 initial register as D. With clamping each component
    // gets one nonzero operation, so clamp(clamp(X+max(D,0))-max(-D,0)) is
    // clamp(X+D) exactly; a later 2x/4x of a clamped value is still exact.
    bool signedD(const std::array<u8,4>& args,const MaterialOperation& op,const std::array<int,3>& raw){
        if(aliasMode)return fail(MaterialPlanError::operation,MaterialOperationReason::registerSignedD);
        if(op.operation||op.bias||!op.clamp||op.scale==3)return fail(MaterialPlanError::operation,MaterialOperationReason::signedDForm);
        Value positive=constant(0),negative=constant(0),a,b,c;
        for(unsigned n=0;n<3;++n){positive.bytes[n]=u8(std::clamp(raw[n],0,255));negative.bytes[n]=u8(std::clamp(-raw[n],0,255));}
        if(!input(args[0],a)||!input(args[1],b)||!input(args[2],c)||!lerp(a,b,c))return false;
        current().scale=0;
        // Alpha 6 already subtracted a D in [-255,0] on the first stage.
        bool earlier=!index;
        for(unsigned n=0;n<3;++n)earlier&=raw[n]>=-255&&raw[n]<=0;
        if(!earlier)lowering|=MaterialLowering::signedD;
        const bool subtracts=!is(negative,0);
        if(!is(positive,0)&&!push(MaterialFunction::add,previous(),positive,constant(0),subtracts?0:op.scale))return false;
        return !subtracts||push(MaterialFunction::subtract,previous(),negative,constant(0),op.scale);
    }
    // GX divide-by-2 scale. PICA scales only by 1, 2 or 4 and clamps every
    // result, so a sum above one is halved term by term: 128/255 for a
    // computed term, halved bytes for literals. Each form stays within
    // 0.5/255 of the real GX value and every step stays at or below one, so
    // a clamp-off stage keeps its value (the water shine base (7/8+T*V)/2).
    bool halve(const std::array<u8,4>& args,const MaterialOperation& op){
        if(aliasMode)return fail(MaterialPlanError::operation,MaterialOperationReason::registerHalve);
        if(policy==MaterialPolicy::exactConstants)return fail(MaterialPlanError::operation,MaterialOperationReason::halveExact);
        const auto before=lowering;
        plan.quantizedConstants=true;lowering|=MaterialLowering::halve;
        const Value half=constant(128);
        if(op.operation==1){
            // clamp(D-X) is in [0,1]: halve the final complement instead.
            if(op.bias||!op.clamp)return fail(MaterialPlanError::operation,MaterialOperationReason::halveSubtract);
            if(!subtract(args))return false;
            // The complement step takes the half. The other differences of
            // the sum rules (one SUBTRACT, a captured D) are halved after.
            auto* step=tailCount[alpha]?&tail[alpha][tailCount[alpha]-1]:nullptr;
            if(!step||step->function!=MaterialFunction::replace)return push(MaterialFunction::modulate,previous(),half,constant(0),0);
            *step={MaterialFunction::modulate,step->a,half,constant(0),0,false};
            return true;
        }
        Term x;Value d;
        if(!term(args,x)||!input(args[3],d,false))return false;
        if(op.bias==2){
            // clamp(X-1/2) stays at or below 1/2 only without D.
            if(!op.clamp||!is(d,0))return fail(MaterialPlanError::operation,MaterialOperationReason::halveBias);
            if(x.kind==Term::zero)return emit(MaterialFunction::replace,constant(0));
            if(output+2>=plan.stages.size())return fail(MaterialPlanError::stages);
            return emitTerm(x)&&push(MaterialFunction::addSigned,previous(),constant(0),constant(0),0)&&
                push(MaterialFunction::modulate,previous(),half,constant(0),0);
        }
        switch(x.kind){
        case Term::zero:
            if(d.literal)return emit(MaterialFunction::replace,halfDown(d));
            lowering=before; // a halved sampled replace, as for the single term below
            return emit(MaterialFunction::modulate,d,half);
        case Term::single:{
            const Value& v=x.p;
            if(v.literal&&d.literal){
                Value sum=constant(0);
                for(unsigned n=0;n<3;++n)sum.bytes[n]=u8((v.bytes[n]+d.bytes[n])>>1);
                return emit(MaterialFunction::replace,sum);
            }
            if(is(d,0)){
                if(v.literal)return emit(MaterialFunction::replace,halfDown(v));
                // A halved sampled value was planned before these rules.
                lowering=before;
                return emit(MaterialFunction::modulate,v,half);
            }
            if(d.literal)return emit(MaterialFunction::multiplyAdd,v,half,halfDown(d));
            if(v.literal)return emit(MaterialFunction::multiplyAdd,d,half,halfDown(v));
            return emit(MaterialFunction::interpolate,v,d,half);
        }
        case Term::product:
            if(x.q.literal&&is(d,0))return emit(MaterialFunction::modulate,x.p,halfDown(x.q));
            if(x.q.literal&&d.literal)return emit(MaterialFunction::multiplyAdd,x.p,halfUp(x.q),halfDown(d));
            break;
        case Term::blend:
            // Literal endpoints absorb a literal D: (p+d)/2 and (q+d)/2.
            if(x.p.literal&&x.q.literal&&d.literal){
                Value low=constant(0),high=constant(0);
                for(unsigned n=0;n<3;++n){low.bytes[n]=u8((x.p.bytes[n]+d.bytes[n])>>1);high.bytes[n]=u8((x.q.bytes[n]+d.bytes[n])>>1);}
                return interpolate(low,high,x.r);
            }
            break;
        }
        // X first, then (X+D)/2 from PREVIOUS. D must outlive the X stage
        // (sum rule: PREV waits in the combiner buffer).
        if(!is(d,0)&&!d.literal&&d.input.source==MaterialSource::previous){
            if(!sumRules)return fail(MaterialPlanError::equation);
            if(!capturePrevious(d))return false;
        }
        if(!emitTerm(x))return false;
        if(is(d,0))return push(MaterialFunction::modulate,previous(),half,constant(0),0);
        if(d.literal)return push(MaterialFunction::multiplyAdd,previous(),half,halfDown(d),0);
        return push(MaterialFunction::interpolate,previous(),d,half,0);
    }
    // GX compare: D + (A > B ? C : 0). PICA has no comparison: (A-B)x4 and
    // twice (PREV+PREV)x4 multiply the difference by 256, so an 8-bit
    // difference of at least 1/255 saturates to one and anything else is
    // zero. A and B are read in the first stage, C and D after the mask: a
    // PREV operand there is captured in the combiner buffer one stage
    // earlier. EQ, GR16 and BGR24 forms are refused. A saturating literal
    // C and D may drop one gain stage (see soft below).
    bool compare(const std::array<u8,4>& args,const MaterialOperation& op){
        const unsigned mode=(op.operation-8)/2; // R8, GR16, BGR24, RGB8 (A8 for alpha)
        if(aliasMode)return fail(MaterialPlanError::operation,MaterialOperationReason::registerCompare);
        if((op.operation&1)||op.operation>15||op.clamp>1||op.output>3||
           (alpha?mode!=3:(mode!=0&&mode!=3)))return fail(MaterialPlanError::operation,MaterialOperationReason::compareForm);
        current().scale=0;
        Value a,b,c,d;
        if(!input(args[2],c)||!input(args[3],d,false))return false;
        if(!op.clamp&&upper(c)+upper(d)>255)return fail(MaterialPlanError::operation,MaterialOperationReason::compareClampOff);
        if(is(c,0))return emit(MaterialFunction::replace,d);
        if(!input(args[0],a)||!input(args[1],b))return false;
        auto red=[&](Value& v){
            if(alpha||mode!=0)return;
            if(v.literal)v=constant(v.bytes[0]);
            else if(v.input.operand==MaterialOperand::color)v.input.operand=MaterialOperand::red;
        };
        red(a);red(b);
        if(a.literal&&b.literal){
            Value mask=constant(0);
            for(unsigned n=0;n<3;++n)mask.bytes[n]=a.bytes[n]>b.bytes[n]?255:0;
            return multiplyAdd(mask,c,d);
        }
        if(a==b)return emit(MaterialFunction::replace,d);
        bool captured=false;
        auto keep=[&](Value& v){
            if(v.literal||v.input.source!=MaterialSource::previous)return true;
            // The RGB capture holds PREV's RGB only; APREV in a colour
            // input would need the alpha buffer too.
            if(!alpha&&v.input.operand!=MaterialOperand::color)return fail(MaterialPlanError::equation);
            if(!captured){
                auto& update=alpha?plan.bufferUpdateAlpha:plan.bufferUpdateRgb;
                if(!output||output>4||update||(!alpha&&(bufferedReg||temporaryReg)))return fail(MaterialPlanError::equation);
                update=u8(1u<<(output-1));captured=true;
            }
            v.input.source=MaterialSource::buffer;return true;
        };
        if(!keep(c)||!keep(d)||!place(MaterialFunction::subtract,a,b))return false;
        current().scale=2;lowering|=MaterialLowering::compare;
        // Gain 32 is enough when literal C and D saturate the true result
        // from two levels above the threshold: only a one-level difference
        // gives D+C/8 instead of white. Used only for a last-stage compare
        // whose full form would fill all six stages, so a stage stays free
        // for the vertex fog stage (water shine MA09: K1 >= 200 plus ONE).
        bool soft=policy==MaterialPolicy::quantizedConstants&&index+1==state.stageCount&&
            output+4>=plan.stages.size()&&c.literal&&d.literal;
        for(unsigned n=0;soft&&n<3;++n)soft=255u*d.bytes[n]+64u*c.bytes[n]>=255u*255u;
        if(soft){lowering|=MaterialLowering::softCompare;plan.quantizedConstants=true;}
        if(!push(MaterialFunction::add,previous(),previous(),constant(0),2)||
           (!soft&&!push(MaterialFunction::add,previous(),previous(),constant(0),2)))return false;
        if(is(d,0))return is(c,255)||push(MaterialFunction::modulate,previous(),c,constant(0),0);
        if(is(c,255))return push(MaterialFunction::add,previous(),d,constant(0),0);
        return push(MaterialFunction::multiplyAdd,previous(),c,d,0);
    }
    // Operation rule: GX ADDHALF, clamp(k*(X+D+128/255)) with X the lerp of
    // A, B, C (in [0,1]) and k=1,2,4 (clamp on; a saturated last stage
    // counts). Exact under the reference's 128/255 half, as SUBHALF's
    // ADD_SIGNED. A literal D takes the half: d' = min(d+128,255) per
    // component; where d+128 >= 255 the sum is at least one, so the result
    // is one either way. An initial register D in [-128,127] is a literal
    // after the fold; below -128 it is the signed-D split. Any other D: the
    // sum is formed with scale 1 and ADD constant 128 takes the scale. The
    // terms are nonnegative, so clamp(clamp(S)+h) = clamp(S+h), and with
    // k >= 1 a saturated clamp(S) stays saturated after the scale.
    bool addHalf(const std::array<u8,4>& args,const MaterialOperation& op){
        if(aliasMode||op.operation||!op.clamp||op.scale==3)
            return fail(MaterialPlanError::operation,MaterialOperationReason::addHalf);
        lowering|=MaterialLowering::addHalf;
        auto folded=[](int v){return u8(std::min(v+128,255));};
        const unsigned dReg=alpha?args[3]:args[3]/2;
        const bool dAlpha=alpha||(args[3]&1);
        if(args[3]<(alpha?4u:8u)&&!routed(dReg,dAlpha)){
            if(!(state.colorValid&(1u<<dReg)))return fail(MaterialPlanError::uninitialized);
            std::array<int,3> raw{};bool negative=false;
            for(unsigned n=0;n<3;++n){raw[n]=state.colors[dReg][dAlpha?3:n]+128;negative|=raw[n]<0;}
            if(negative){
                MaterialOperation plain=op;plain.bias=0;
                return signedD(args,plain,raw);
            }
            Value d=constant(0);
            for(unsigned n=0;n<3;++n)d.bytes[n]=u8(std::min(raw[n],255));
            return sum(args,&d);
        }
        Value d;
        if(!input(args[3],d,false))return false;
        if(d.literal){
            for(auto& b:d.bytes)b=folded(b);
            return sum(args,&d);
        }
        if(output+1>=plan.stages.size())return fail(MaterialPlanError::stages);
        finishScale=0;boundShift=0;current().scale=0;
        if(!sum(args,&d))return false;
        // The sum's own steps end with scale 1; the half goes after them.
        return push(MaterialFunction::add,previous(),constant(128),constant(0),u8(op.scale),true);
    }
    bool channelExpression(bool isAlpha){
        alpha=isAlpha;constantSet=false;const auto& stage=state.stages[index];
        const auto& args=alpha?stage.alpha:stage.color;const auto op=operation(alpha);
        finishScale=op.scale==3?0:op.scale;boundShift=finishScale;
        if(op.operation>=8)return compare(args,op);
        if(op.operation>1||op.bias>2||op.scale>3||op.clamp>1||op.output>3)
            return fail(MaterialPlanError::operation,MaterialOperationReason::invalidOperation);
        if(op.output!=0&&op.scale!=0&&index+1<state.stageCount){
            // Alias mode records the register as a copy and cannot carry a
            // scale. Outside it (the register route of channel(), which keeps
            // the register in PREVIOUS) the scaled, clamped value is exact.
            if(aliasMode||!operationRules)return fail(MaterialPlanError::operation,MaterialOperationReason::scaledRegister);
            lowering|=MaterialLowering::scaledRegister;
        }
        current().scale=finishScale;
        if(op.bias==1)return operationRules?addHalf(args,op):fail(MaterialPlanError::operation,MaterialOperationReason::addHalf);
        if(op.scale==3)return halve(args,op);
        Value a,b,c,d;
        // An initial register D outside [0,255] in any component.
        const unsigned dReg=alpha?args[3]:args[3]/2;
        const bool dAlpha=alpha||(args[3]&1);
        if(args[3]<(alpha?4u:8u)&&!routed(dReg,dAlpha)&&(state.colorValid&(1u<<dReg))){
            std::array<int,3> raw{};bool outside=false;
            for(unsigned n=0;n<3;++n){raw[n]=state.colors[dReg][dAlpha?3:n];outside|=raw[n]<0||raw[n]>255;}
            if(outside)return signedD(args,op,raw);
        }
        if(op.operation==1){
            if(op.bias||!op.clamp)return fail(MaterialPlanError::operation,MaterialOperationReason::subtractForm);
            return subtract(args);
        }
        if(op.bias==2) {
            // First form the bounded interpolation/product, then add D and
            // subtract half in PICA ADD_SIGNED. Never clamp A*(1-C)+B*C+D
            // before subtracting: that would lose bright grass/foliage values.
            // The UNORM8 half-bias path is explicitly part of native Tier D;
            // strict mode retains its refusal pending fixed-point GPU proof.
            if(aliasMode)return fail(MaterialPlanError::operation,MaterialOperationReason::registerBias);
            if(policy==MaterialPolicy::exactConstants)return fail(MaterialPlanError::operation,MaterialOperationReason::subHalfExact);
            if(!op.clamp)return fail(MaterialPlanError::operation,MaterialOperationReason::subHalfClampOff);
            if(output+1>=plan.stages.size())return fail(MaterialPlanError::stages);
            if(!input(args[0],a)||!input(args[1],b)||!input(args[2],c)||!input(args[3],d,false))return false;
            if(!d.literal&&d.input.source==MaterialSource::previous)
                return sumRules?biasPrevious(a,b,c,d):fail(MaterialPlanError::equation);
            if(!lerp(a,b,c))return false;
            current().scale=0;plan.quantizedConstants=true;
            return push(MaterialFunction::addSigned,previous(),d,constant(0),finishScale,true);
        }
        return sum(args,nullptr);
    }
    // GX D + A*(1-C) + B*C (bias 0, add) with finishScale. forcedD replaces
    // the D input (operation rules: a literal D with the half folded in);
    // otherwise the inputs are read in the Alpha 11 order.
    bool sum(const std::array<u8,4>& args,const Value* forcedD){
        const unsigned zero=alpha?7:15;Value a,b,c,d;
        auto readD=[&]{if(forcedD){d=*forcedD;return true;}return input(args[3],d,false);};
        // Eliminate mathematically unused inputs before checking their resource
        // dependencies; a missing unused PREV/register must not reject a draw.
        if(args[0]==args[1])return input(args[0],a)&&readD()&&add(a,d);
        if(args[2]==zero)return input(args[0],a)&&readD()&&add(a,d);
        if(!input(args[2],c)||!readD())return false;
        if(is(c,0))return input(args[0],a)&&add(a,d);
        if(is(c,255))return input(args[1],b)&&add(b,d);
        if(!input(args[0],a)||!input(args[1],b))return false;
        if(a==b)return add(a,d);
        if(is(a,0))return multiplyAdd(b,c,d);
        if(is(b,0))return multiplyAdd(a,inverse(c),d);
        if(is(d,0))return interpolate(a,b,c);
        // A convex interpolation stays in [0,1], so an intermediate clamp
        // preserves the equation. Defer scaling until the addition. PREV in D
        // would be overwritten by that intermediate (sum rule: runningSum).
        if(!d.literal&&d.input.source==MaterialSource::previous)
            return sumRules?runningSum(a,b,c,d):fail(MaterialPlanError::equation);
        if(output+1>=plan.stages.size())return fail(MaterialPlanError::stages);
        if(!emit(MaterialFunction::interpolate,b,a,c))return false;
        current().scale=0;
        return push(MaterialFunction::add,previous(),d,constant(0),finishScale,true);
    }
    // Operation rule (registerChain): every GX stage from this one to the one
    // before the last writes its RGB to reg, so no stage before the last
    // writes PREV's RGB once the chain starts.
    bool chainTo(unsigned reg)const{
        if(!reg||reg>3)return false;
        for(unsigned j=index;j+1<state.stageCount;++j)if(state.stages[j].colorOp.output!=reg)return false;
        return true;
    }
    bool channel(bool isAlpha) {
        const auto op=operation(isAlpha);
        const bool finalStage=index+1==state.stageCount;
        if(!isAlpha&&temporaryReg&&!finalStage) {
            // A later step of a register chain: PREVIOUS holds the register
            // (temporaryReg), the buffer GX PREV's RGB (see color()). The
            // step is computed into PREVIOUS like any PREV write; no other
            // register write can reach here (chainTo held at the start).
            if(op.output!=temporaryReg)return fail(MaterialPlanError::operation,MaterialOperationReason::registerValue);
            if(!channelExpression(false))return false;
            temporaryIndex=index;
            return true;
        }
        // Third pass only: a chain that starts before the stage preceding the
        // last one (the first two passes keep their Alpha 12 plans).
        const bool chain=operationRules&&index+2<state.stageCount&&chainTo(op.output);
        const bool canRoute=!isAlpha&&op.output&&!temporaryReg&&!bufferSet[0]&&!plan.bufferUpdateRgb&&
            (index+2==state.stageCount||chain)&&output&&output<=4;
        PicaMaterialPlan before;
        const auto loweringBefore=lowering;
        if(canRoute)before=plan;
        aliasMode=op.output!=0&&!finalStage;
        const bool okay=channelExpression(isAlpha);
        aliasMode=false;
        if(!okay) {
            // One computed RGB register immediately before the final GX stage:
            // capture the preceding PREV in PICA's delayed combiner buffer,
            // calculate the temporary in PREVIOUS, then route both separately.
            // Earlier/later register lifetimes and an occupied RGB buffer remain
            // unsupported rather than silently overwriting a live value.
            if(error!=MaterialPlanError::operation||!canRoute)return false;
            plan=before;error=MaterialPlanError::none;reason=MaterialOperationReason::none;pendingValid[0]=false;tailCount[0]=0;lowering=loweringBefore;
            plan.bufferUpdateRgb=1u<<(output-1);
            if(!channelExpression(false))return false;
            temporaryReg=op.output;temporaryIndex=index;
            if(index+2<state.stageCount)lowering|=MaterialLowering::registerChain;
            return true;
        }
        if(op.output!=0&&!finalStage) {
            if(!pendingValid[isAlpha])return fail(MaterialPlanError::operation,MaterialOperationReason::registerPending);
            // Preserve GX PREV, while the independently tracked register keeps
            // its stable source. RGB and alpha assignments commit simultaneously.
            Value previous;
            if(!color(0,isAlpha,previous))return false;
            constantSet=false;
            if(!emit(MaterialFunction::replace,previous))return false;
            current().scale=0;
        }
        return true;
    }
    void commitAliases() {
        for(unsigned channel=0;channel<2;++channel)if(pendingValid[channel]) {
            const auto reg=(channel?state.stages[index].alphaOp:state.stages[index].colorOp).output;
            aliases[channel][reg]=pendingAliases[channel];aliasValid[channel][reg]=true;
        }
    }
    bool finishStage(){
        const unsigned extra=std::max(tailCount[0],tailCount[1]);
        ++output;
        if(!extra)return true;
        if(output+extra>plan.stages.size())return fail(MaterialPlanError::stages);
        for(unsigned n=0;n<extra;++n,++output)for(unsigned channel=0;channel<2;++channel){
            alpha=channel;constantSet=false;
            // Exact pass-through: PREVIOUS already holds the proved GX value.
            // Alpha 6 re-ran the clamp-off bound proof here with the scale.
            if(n>=tailCount[channel]){
                const auto& raw=channel?state.stages[index].alphaOp:state.stages[index].colorOp;
                if(!raw.clamp&&raw.scale)lowering|=MaterialLowering::passThrough;
                if(!place(MaterialFunction::replace,previous()))return false;
                current().scale=0;continue;
            }
            const auto& step=tail[channel][n];
            if(step.checked){boundShift=step.scale;if(!emit(step.function,step.a,step.b,step.c))return false;}
            else if(!place(step.function,step.a,step.b,step.c))return false;
            current().scale=step.scale;
        }
        return true;
    }
};
}
static bool buildPass(const MaterialState& state,PicaMaterialPlan& out,MaterialPlanError* error,MaterialPolicy policy,unsigned bufferedReg,RasterCarrier carrier,unsigned bufferedFrom,bool saturateLast,bool sumRules,
                      bool operationRules,MaterialOperationReason* reason){
    out={};if(error)*error=MaterialPlanError::none;if(reason)*reason=MaterialOperationReason::none;
    auto reject=[&](MaterialPlanError e,MaterialOperationReason why=MaterialOperationReason::none){
        out={};if(error)*error=e;if(reason)*reason=e==MaterialPlanError::operation?why:MaterialOperationReason::none;return false;
    };
    if(!state.stageCount||state.stageCount>6)return reject(MaterialPlanError::stages);
    if(state.texgenCount>8||state.channelCount>2)return reject(MaterialPlanError::source);
    if(state.indirectCount)return reject(MaterialPlanError::indirect);
    // GX displays the last stage's RGB/alpha results regardless of destination
    // register. Only intermediate register writes need lifetime preservation.
    for(unsigned prologue=0;prologue<2;++prologue) {
        out={};
        if(prologue) {
            // PICA cannot read the programmed buffer constant in stage0.
            // A dead zero stage advances that pipeline for a first GX stage
            // needing two constants. Original initial registers remain literals.
            out.stages[0].rgb.inputs[0]={MaterialSource::constant,MaterialOperand::color,false};
            out.stages[0].alpha.inputs[0]={MaterialSource::constant,MaterialOperand::alpha,false};
        }
        out.rasterCarrier=carrier;
        Builder b{state,out,policy};b.carrier=carrier;b.output=prologue;b.bufferedReg=bufferedReg;b.bufferedFrom=bufferedFrom;
        b.saturateLast=saturateLast;b.sumRules=sumRules;b.operationRules=operationRules;bool retry=false;
        for(b.index=0;b.index<state.stageCount;++b.index){
            if(state.stages[b.index].indirect)return reject(MaterialPlanError::indirect);
            if(b.output>=out.stages.size())return reject(MaterialPlanError::stages);
            b.pendingValid={};b.tailCount={};
            if(!b.channel(false)||!b.channel(true)||!b.finishStage()) {
                if(!prologue&&!b.output&&b.error==MaterialPlanError::constantConflict){retry=true;break;}
                return reject(b.error,b.reason);
            }
            b.commitAliases();
            if(bufferedReg&&!b.index){
                if(b.bufferSet[0]||!b.output||b.output>4)return reject(MaterialPlanError::operation,MaterialOperationReason::hoistBuffer);
                out.bufferUpdateRgb=1u<<(b.output-1);
            }
        }
        if(retry)continue;
        out.stageCount=b.output;out.lowering=b.lowering|(saturateLast?MaterialLowering::finalClamp:0);return true;
    }
    return reject(MaterialPlanError::constantConflict);
}
// A clamp-off last stage first keeps the Alpha 6 bound proof (same plan when
// it passes, the clamp only gates refusals); the saturated reading is a second
// build, so finalClamp marks exactly the plans that need it. Its error is the
// one reported when both fail.
static bool buildPicaMaterial(const MaterialState& state,PicaMaterialPlan& out,MaterialPlanError* error,MaterialPolicy policy,unsigned bufferedReg=0,RasterCarrier carrier=RasterCarrier::none,unsigned bufferedFrom=2,bool sumRules=false,
                              bool operationRules=false,MaterialOperationReason* reason=nullptr){
    if(buildPass(state,out,error,policy,bufferedReg,carrier,bufferedFrom,false,sumRules,operationRules,reason))return true;
    if(!state.stageCount||state.stageCount>6)return false;
    const auto& last=state.stages[state.stageCount-1];
    if(last.colorOp.clamp&&last.alphaOp.clamp)return false;
    return buildPass(state,out,error,policy,bufferedReg,carrier,bufferedFrom,true,sumRules,operationRules,reason);
}
// One planner pass: the direct build, then the register routes. sumRules
// selects the second pass (see planPicaMaterial).
static bool planPass(const MaterialState& state,PicaMaterialPlan& out,MaterialPlanError* error,MaterialPolicy policy,bool sumRules,
                     bool operationRules=false,MaterialOperationReason* reason=nullptr){
    MaterialPlanError initial;MaterialOperationReason initialReason;
    if(buildPicaMaterial(state,out,&initial,policy,0,RasterCarrier::none,2,sumRules,operationRules,&initialReason)){
        if(error)*error=MaterialPlanError::none;
        if(reason)*reason=MaterialOperationReason::none;
        return true;
    }
    auto refuse=[&](){if(error)*error=initial;if(reason)*reason=initialReason;return false;};
    if(initial!=MaterialPlanError::operation||state.stageCount<3||state.stageCount>6)return refuse();
    // Keep a computed first RGB destination in the buffer while subsequent
    // destinations use PREVIOUS. Remap only proved last-writer dependencies;
    // initial PREV or a third live computed register is explicitly refused.
    if(state.stages[0].colorOp.output){
        const unsigned savedReg=state.stages[0].colorOp.output;
        MaterialState lowered=state;std::array<bool,4> written{};
        unsigned last=0;bool valid=savedReg<4;
        for(unsigned i=0;valid&&i<state.stageCount;++i){
            const unsigned destination=state.stages[i].colorOp.output;
            if(destination>3||(i&&i+1<state.stageCount&&destination==savedReg)){valid=false;break;}
            // Operation rules: an RGB write of ZERO to another register (a
            // write the liveness rule found dead) keeps its register, an
            // alias of zero that leaves PREVIOUS and the chain alone.
            if(operationRules&&i&&i+1<state.stageCount&&destination&&
               state.stages[i].color==std::array<std::uint8_t,4>{15,15,15,15})continue;
            for(auto& input:lowered.stages[i].color)if(i&&input<8&&!(input&1)){
                const unsigned reg=input/2;
                if(reg==last)input=0;
                else if(reg==savedReg&&i>=2){} // captured first result
                else if(written[reg]||reg==0){valid=false;break;}
            }
            lowered.stages[i].colorOp.output=0;written[destination]=true;last=destination;
        }
        if(valid)return buildPicaMaterial(lowered,out,error,policy,savedReg,RasterCarrier::none,2,sumRules,operationRules,reason);
        return refuse();
    }
    // Hoist one independent RGB register calculation (GX stage 1 up to the
    // stage before the last) ahead of the first PREV calculation. The delayed
    // buffer then holds that register for every later GX stage while
    // PREVIOUS continues to evolve; earlier stages still read its initial
    // value. A dependency proof, not a material/asset-specific substitution.
    const auto& first=state.stages[0];
    if(first.colorOp.output||first.alphaOp.output)return refuse();
    for(auto input:first.color)if(input==0)return refuse(); // would see the hoisted value
    bool attempted=false;MaterialPlanError attempt=initial;MaterialOperationReason attemptReason=initialReason;
    for(unsigned at=1;at+1<state.stageCount;++at){
        const auto& saved=state.stages[at];const auto& op=saved.colorOp;const auto& a=saved.alphaOp;
        const unsigned reg=op.output;
        if(!reg||reg>3||a.operation||a.bias||a.scale||a.output||saved.alpha[0]!=7||saved.alpha[1]!=7||saved.alpha[3]!=0)continue;
        bool valid=true;
        // It may read nothing an earlier stage wrote (PREV included).
        for(auto input:saved.color)if(input<8){
            if(input<2){valid=false;break;}
            for(unsigned j=0;j<at;++j)
                if(((input&1)?state.stages[j].alphaOp.output:state.stages[j].colorOp.output)==input/2)valid=false;
        }
        for(unsigned j=1;j<at;++j)if(state.stages[j].colorOp.output==reg)valid=false;
        for(unsigned j=at+1;j+1<state.stageCount;++j)if(state.stages[j].colorOp.output==reg)valid=false;
        if(!valid)continue;
        MaterialState reordered=state;
        reordered.stages[0]=saved;
        for(unsigned j=0;j<at;++j)reordered.stages[j+1]=state.stages[j];
        reordered.stages[0].colorOp.output=0;
        MaterialPlanError result;MaterialOperationReason resultReason;
        if(buildPicaMaterial(reordered,out,&result,policy,reg,RasterCarrier::none,at+1,sumRules,operationRules,&resultReason)){
            // GX stage 1 with a plain operation was the earlier, narrower rule.
            if(at>1||op.operation||op.bias||op.scale||!op.clamp)out.lowering|=MaterialLowering::hoist;
            if(error)*error=MaterialPlanError::none;
            if(reason)*reason=MaterialOperationReason::none;
            return true;
        }
        if(!attempted){attempt=result;attemptReason=resultReason;attempted=true;}
    }
    if(error)*error=attempt;
    if(reason)*reason=attemptReason;
    return false;
}
// Operation rules on the GX program (Alpha 12): rewrites of the TEV stages
// that keep every observed value (the last stage's RGB and alpha) equal, so
// that any exact plan of the rewritten program is an exact plan of the
// original. A register component is a bit (channel*4 + register; channel 0
// RGB, 1 alpha; register 0 PREV, 1..3 REG0..REG2).
namespace program {
constexpr unsigned bit(unsigned channel,unsigned reg){return 1u<<(channel*4+reg);}
const MaterialOperation& op(const MaterialStage& s,unsigned channel){return channel?s.alphaOp:s.colorOp;}
MaterialOperation& op(MaterialStage& s,unsigned channel){return channel?s.alphaOp:s.colorOp;}
// Register components one channel of a stage reads. Inputs the equation
// provably ignores do not count: B and C with C = ZERO, C with A = B, A
// with C = ONE (RGB). Compares read all four.
unsigned reads(const MaterialStage& s,unsigned channel){
    const auto& args=channel?s.alpha:s.color;
    std::array<bool,4> used{true,true,true,true};
    if(op(s,channel).operation<8){
        const unsigned zero=channel?7:15;
        if(args[2]==zero)used[1]=used[2]=false;
        else if(args[0]==args[1])used[2]=false;
        else if(!channel&&args[2]==12)used[0]=used[2]=false;
    }
    unsigned mask=0;
    for(unsigned n=0;n<4;++n)if(used[n]){
        if(channel){if(args[n]<4)mask|=bit(1,args[n]);}
        else if(args[n]<8)mask|=bit(args[n]&1,args[n]/2);
    }
    return mask;
}
unsigned reads(const MaterialStage& s){return reads(s,0)|reads(s,1);}
unsigned writes(const MaterialStage& s){return bit(0,s.colorOp.output&3)|bit(1,s.alphaOp.output&3);}
// live[j]: components whose value after stage j is read later. Nothing is
// live after the last stage (its result is the output, whatever register).
void liveness(const MaterialState& s,std::array<unsigned,16>& live){
    unsigned now=0;
    for(unsigned j=s.stageCount;j-->0;){live[j]=now;now=(now&~writes(s.stages[j]))|reads(s.stages[j]);}
}
// Dead writes: a stage before the last whose two writes nothing reads is
// removed (its writes are its only effect). One dead channel becomes ZERO
// (it reads nothing, so earlier writes may die too). A dead PREV write moves
// to a register that is dead there, so that PREV can carry a renamed value
// across it; without one it stays on PREV.
bool dropDeadWrites(MaterialState& s){
    bool changed=false;
    for(bool again=true;again;){
        again=false;std::array<unsigned,16> live{};liveness(s,live);
        for(unsigned j=0;j+1<s.stageCount&&!again;++j)if(!(live[j]&writes(s.stages[j]))){
            for(unsigned k=j;k+1<s.stageCount;++k)s.stages[k]=s.stages[k+1];
            s.stages[--s.stageCount]=MaterialStage{};again=changed=true;
        }
        if(again)continue;
        for(unsigned j=0;j+1<s.stageCount;++j)for(unsigned channel=0;channel<2;++channel){
            auto& st=s.stages[j];auto& o=op(st,channel);
            if(o.output>3||(live[j]&bit(channel,o.output)))continue;
            auto& args=channel?st.alpha:st.color;
            const std::uint8_t zero=channel?7:15;
            unsigned target=o.output;
            if(!target)for(unsigned r=1;r<4&&!target;++r)if(!(live[j]&bit(channel,r)))target=r;
            const MaterialOperation dead{0,0,0,1,std::uint8_t(target)};
            if(args==std::array<std::uint8_t,4>{zero,zero,zero,zero}&&o.operation==dead.operation&&o.bias==0&&o.scale==0&&
               o.clamp==1&&o.output==target)continue;
            args={zero,zero,zero,zero};o=dead;again=changed=true;
        }
    }
    return changed;
}
// Renaming: a value one stage writes to REGn (one channel) and later stages
// read moves to PREV when PREV's own value is not read after that stage and
// no stage before the value's last read writes that PREV channel. Reads of
// it switch to PREV; REGn keeps its earlier value, which nothing reads in
// that range (the next write of REGn, if any, ends it).
// latestFirst tries the last written values first: a value of stage 0 then
// tends to stay in its register, where the first-register route keeps it in
// the combiner buffer while PREV carries the others.
bool renameToPrevious(MaterialState& s,bool latestFirst){
    bool changed=false;
    for(bool again=true;again;){
        again=false;std::array<unsigned,16> live{};liveness(s,live);
        for(unsigned n=0;n+1<s.stageCount&&!again;++n)for(unsigned channel=0;channel<2&&!again;++channel){
            const unsigned i=latestFirst?s.stageCount-2-n:n;
            const unsigned r=op(s.stages[i],channel).output;
            if(!r||r>3||!(live[i]&bit(channel,r))||(live[i]&bit(channel,0)))continue;
            unsigned last=i;bool blocked=false;
            for(unsigned k=i+1;k<s.stageCount;++k){
                if(reads(s.stages[k])&bit(channel,r))last=k;
                if(writes(s.stages[k])&bit(channel,r))break;
            }
            for(unsigned k=i+1;k<last;++k)blocked|=(writes(s.stages[k])&bit(channel,0))!=0;
            if(blocked||last==i)continue;
            op(s.stages[i],channel).output=0;
            for(unsigned k=i+1;k<=last;++k){
                auto& st=s.stages[k];
                for(auto& id:st.color)if(id<8&&(id&1)==channel&&id/2==r)id=std::uint8_t(channel);
                if(channel)for(auto& id:st.alpha)if(id==r)id=0;
            }
            again=changed=true;
        }
    }
    return changed;
}
// Saturation: GX keeps a clamp-off result unclamped. PICA clamps. Clamping
// value X to [0,1] early is exact when X >= 0 and every read of it (until
// its register is written again) is as D of an add or compare whose result
// is clamped (or is the saturated output) and scaled by 1, 2 or 4 with a
// nonnegative bias: with L >= 0, clamp(k*(L+X+b)) = clamp(k*(L+min(X,1)+b)),
// both one when X >= 1. A/B/C reads wrap in GX instead, so they refuse. X
// >= 0: an add without SUBHALF or a compare, whose D is nonnegative (a
// clamped value, a nonnegative initial register, a stage source). Stages
// are visited from the last, so a reader saturated here counts as clamped.
// Compare mask to alpha: an RGB R8_GT compare with C = ONE and D = ZERO
// writes the same 0 or 1 to all three components, read later only by RGB
// inputs. The same mask in the alpha channel (A8_GT on the red components:
// a texture or raster swap whose alpha is that red, the konst alpha
// selector of the red konst) frees the RGB channel for another value; its
// readers take the register's alpha, which is the same number. C = 1 comes
// from a register whose alpha nothing reads or writes, given 255 as its
// initial alpha. The stage's own alpha write must be dead and the target
// alpha free over the mask's lifetime (as for renaming).
bool maskToAlpha(MaterialState& s){
    std::array<unsigned,16> live{};liveness(s,live);
    unsigned alphaUse=0;
    for(unsigned k=0;k<s.stageCount;++k)alphaUse|=(reads(s.stages[k])|writes(s.stages[k]))&0xf0;
    for(unsigned m=0;m+1<s.stageCount;++m){
        auto& st=s.stages[m];const auto& o=st.colorOp;const unsigned r=o.output;
        if(o.operation!=8||r>3||st.color[2]!=12||st.color[3]!=15||(live[m]&bit(1,st.alphaOp.output&3))||
           (live[m]&bit(1,r))||!(live[m]&bit(0,r)))continue;
        unsigned last=m;bool blocked=false;
        for(unsigned k=m+1;k<s.stageCount;++k){
            if(reads(s.stages[k],0)&bit(0,r))last=k;
            if(writes(s.stages[k])&bit(0,r))break;
        }
        for(unsigned k=m+1;k<last;++k)blocked|=(writes(s.stages[k])&bit(1,r))!=0;
        if(blocked||last==m)continue;
        // The alpha inputs for A and B (red components).
        std::array<std::uint8_t,2> in{};std::uint8_t konstAlpha=st.konstAlpha,textureSwap=st.textureSwap,rasterSwap=st.rasterSwap;
        bool okay=true,swapTexture=false,swapRaster=false;
        for(unsigned n=0;n<2&&okay;++n)switch(st.color[n]){
        case 8:in[n]=4;swapTexture=true;break;
        case 10:in[n]=5;swapRaster=true;break;
        case 14:{
            const unsigned k=st.konstColor;
            konstAlpha=std::uint8_t(k<8||k>=16?k:16+(k-12));in[n]=6;
            okay=k<8||(k>=12&&k<=31);break;}
        case 15:in[n]=7;break;
        default:okay=false;
        }
        if(!okay)continue;
        // A swap table whose alpha is the red of the stage's current one.
        auto swapFor=[&](std::uint8_t current,std::uint8_t& chosen){
            if(current>3||!(s.swapValid&(1u<<current)))return false;
            auto want=s.swaps[current];want[3]=want[0];
            for(unsigned t=0;t<4;++t)if((s.swapValid&(1u<<t))&&s.swaps[t]==want){chosen=std::uint8_t(t);return true;}
            for(unsigned t=0;t<4;++t){
                bool used=false;
                for(unsigned k=0;k<s.stageCount;++k)used|=s.stages[k].textureSwap==t||s.stages[k].rasterSwap==t;
                if(!used){s.swaps[t]=want;s.swapValid|=std::uint8_t(1u<<t);chosen=std::uint8_t(t);return true;}
            }
            return false;
        };
        // An initialized register, or one whose RGB nothing reads either
        // (validity covers all four components).
        unsigned one=0,rgbRead=0;
        for(unsigned k=0;k<s.stageCount;++k)rgbRead|=reads(s.stages[k])&0x0f;
        for(unsigned f=1;f<4&&!one;++f)
            if(!(alphaUse&bit(1,f))&&f!=r&&((s.colorValid&(1u<<f))||!(rgbRead&bit(0,f))))one=f;
        if(!one)continue;
        const auto savedSwaps=s.swaps;const auto savedValid=s.swapValid;
        if((swapTexture&&!swapFor(st.textureSwap,textureSwap))||(swapRaster&&!swapFor(st.rasterSwap,rasterSwap))){
            s.swaps=savedSwaps;s.swapValid=savedValid;continue;
        }
        s.colors[one][3]=255;s.colorValid|=std::uint8_t(1u<<one);
        st.textureSwap=textureSwap;st.rasterSwap=rasterSwap;st.konstAlpha=konstAlpha;
        st.alpha={in[0],in[1],std::uint8_t(one),7};st.alphaOp={14,0,0,1,std::uint8_t(r)};
        st.color={15,15,15,15};st.colorOp={0,0,0,1,std::uint8_t(r)};
        for(unsigned k=m+1;k<=last;++k)for(auto& id:s.stages[k].color)if(id==2*r)id=std::uint8_t(2*r+1);
        return true;
    }
    return false;
}
bool saturateIntermediate(MaterialState& s){
    std::array<unsigned,17> nonnegative{};
    unsigned now=0;
    for(unsigned r=0;r<4;++r)if(s.colorValid&(1u<<r)){
        if(s.colors[r][0]>=0&&s.colors[r][1]>=0&&s.colors[r][2]>=0)now|=bit(0,r);
        if(s.colors[r][3]>=0)now|=bit(1,r);
    }
    auto readsNonnegative=[&](const MaterialStage& st,unsigned channel,unsigned state){
        const unsigned d=channel?st.alpha[3]:st.color[3];
        if(channel)return d>=4||(state&bit(1,d));
        return d>=8||(state&bit(d&1,d/2));
    };
    auto producesNonnegative=[&](const MaterialStage& st,unsigned channel,unsigned state){
        const auto& o=op(st,channel);
        if(o.clamp)return true;
        if(o.operation>=8)return readsNonnegative(st,channel,state);
        return o.operation==0&&o.bias!=2&&readsNonnegative(st,channel,state);
    };
    for(unsigned j=0;j<s.stageCount;++j){
        nonnegative[j]=now;
        unsigned next=now&~writes(s.stages[j]);
        for(unsigned channel=0;channel<2;++channel)
            if(producesNonnegative(s.stages[j],channel,now))next|=bit(channel,op(s.stages[j],channel).output&3);
        now=next;
    }
    bool changed=false;
    for(unsigned j=s.stageCount-1;j-->0;)for(unsigned channel=0;channel<2;++channel){
        auto& o=op(s.stages[j],channel);
        if(o.clamp||o.output>3||!producesNonnegative(s.stages[j],channel,nonnegative[j]))continue;
        const unsigned value=bit(channel,o.output);bool safe=true;
        for(unsigned k=j+1;k<s.stageCount&&safe;++k){
            const auto& st=s.stages[k];
            for(unsigned reader=0;reader<2&&safe;++reader){
                if(!(reads(st,reader)&value))continue;
                const auto& args=reader?st.alpha:st.color;
                auto component=[&](unsigned id){return reader?(id<4?bit(1,id):0u):(id<8?bit(id&1,id/2):0u);};
                for(unsigned n=0;n<3;++n)safe&=!(component(args[n])&value);
                const auto& ro=op(st,reader);
                const bool saturated=ro.clamp||k+1==s.stageCount;
                safe&=saturated&&(ro.operation>=8||(ro.operation==0&&ro.bias!=2&&ro.scale<3));
            }
            if(writes(st)&value)break;
        }
        if(!safe)continue;
        o.clamp=1;changed=true;
    }
    return changed;
}
}
void lowerMaterialProgram(const MaterialState& state,MaterialState& lowered,std::uint16_t* rules,unsigned variant){
    lowered=state;std::uint16_t used=0;
    if(state.stageCount&&state.stageCount<=16)for(unsigned round=0;round<8;++round){
        bool changed=false;
        if(program::dropDeadWrites(lowered)){used|=MaterialLowering::registerLiveness;changed=true;}
        if(program::renameToPrevious(lowered,variant==1)){used|=MaterialLowering::registerLiveness;changed=true;}
        if(program::saturateIntermediate(lowered)){used|=MaterialLowering::saturateIntermediate;changed=true;}
        if(program::maskToAlpha(lowered)){used|=MaterialLowering::registerLiveness;changed=true;}
        if(!changed)break;
    }
    if(rules)*rules=used;
}
// The third pass: the GX rewrites above (to a fixed point), then the
// planner with the sum and operation rules. The plan must use a rule.
static bool planOperationRules(const MaterialState& state,PicaMaterialPlan& out,MaterialPolicy policy,
                               MaterialPlanError* error=nullptr,MaterialOperationReason* reason=nullptr){
    if(error)*error=MaterialPlanError::stages;
    if(reason)*reason=MaterialOperationReason::none;
    if(!state.stageCount||state.stageCount>6)return false;
    // Two renaming orders (earliest and latest value first); the first
    // that plans with an operation rule is kept, else variant 0's refusal.
    for(unsigned variant=0;variant<lowerMaterialVariants;++variant){
        MaterialState lowered;std::uint16_t rules=0;
        lowerMaterialProgram(state,lowered,&rules,variant);
        if(variant&&!rules)break;
        MaterialPlanError e;MaterialOperationReason r;
        if(planPass(lowered,out,&e,policy,true,true,&r)){
            out.lowering|=rules;
            if(out.lowering&MaterialLowering::operationRules){
                if(error)*error=MaterialPlanError::none;
                if(reason)*reason=MaterialOperationReason::none;
                return true;
            }
            e=MaterialPlanError::none;r=MaterialOperationReason::none;
        }
        out={};
        if(!variant){if(error)*error=e;if(reason)*reason=r;}
    }
    return false;
}
bool planMaterialOperationRules(const MaterialState& state,PicaMaterialPlan& out,MaterialPlanError* error,MaterialPolicy policy,MaterialOperationReason* reason){
    return planOperationRules(state,out,policy,error,reason);
}
namespace {
std::atomic<bool> sumRulesEnabled{true};
std::atomic<bool> operationRulesEnabled{true};
constexpr unsigned operationReasons=unsigned(MaterialOperationReason::count);
std::array<std::atomic<std::uint32_t>,operationReasons> operationRefusals{};
}
void setMaterialSumRules(bool enabled){sumRulesEnabled.store(enabled,std::memory_order_relaxed);}
bool materialSumRules(){return sumRulesEnabled.load(std::memory_order_relaxed);}
void setMaterialOperationRules(bool enabled){operationRulesEnabled.store(enabled,std::memory_order_relaxed);}
bool materialOperationRules(){return operationRulesEnabled.load(std::memory_order_relaxed);}
// The second pass runs only where the first refused, and keeps only plans
// that used a sum rule: a route the first pass skipped after a different
// error (the register routes follow operation errors only) is not new
// coverage to take silently. The third pass (Alpha 12) runs where both
// refused, with the sum rules too, and keeps only plans that used an
// operation rule. Refusals keep the first pass's error and reason, so the
// stand-in and its counters see what they saw before.
bool planPicaMaterial(const MaterialState& state,PicaMaterialPlan& out,MaterialPlanError* error,MaterialPolicy policy,MaterialOperationReason* reason){
    MaterialPlanError first;MaterialOperationReason firstReason;
    if(reason)*reason=MaterialOperationReason::none;
    if(planPass(state,out,&first,policy,false,false,&firstReason)){if(error)*error=MaterialPlanError::none;return true;}
    const bool sums=sumRulesEnabled.load(std::memory_order_relaxed);
    if(sums&&planPass(state,out,nullptr,policy,true)&&(out.lowering&MaterialLowering::sumRules)){
        if(error)*error=MaterialPlanError::none;
        return true;
    }
    if(sums&&operationRulesEnabled.load(std::memory_order_relaxed)&&planOperationRules(state,out,policy)){
        if(error)*error=MaterialPlanError::none;
        return true;
    }
    out={};if(error)*error=first;
    if(first==MaterialPlanError::operation){
        if(reason)*reason=firstReason;
        const unsigned at=std::min(unsigned(firstReason),operationReasons-1);
        operationRefusals[at].fetch_add(1,std::memory_order_relaxed);
    }
    return false;
}
bool planPicaMaterial(const MaterialState& state,PicaMaterialPlan& out,MaterialPlanError* error,MaterialPolicy policy){
    return planPicaMaterial(state,out,error,policy,nullptr);
}
bool planAdditiveTextureSplit(const MaterialState& state,PicaMaterialPlan& base,
                              PicaMaterialPlan& contribution,MaterialPolicy policy){
    if(state.stageCount<2||state.stageCount>state.stages.size())return false;
    const auto& tail=state.stages[state.stageCount-1];
    const auto& prev=state.stages[state.stageCount-2];
    const auto& op=tail.colorOp;
    const auto& alpha=tail.alphaOp;
    const unsigned reg=tail.color[1]/2;
    if(!tail.textureEnabled||!reg||reg>3||(tail.color[1]&1)||
       tail.color!=std::array<std::uint8_t,4>{15,tail.color[1],8,0}||
       op.operation||op.bias||op.scale||!op.clamp||op.output||
       !prev.colorOp.clamp||prev.colorOp.output||!prev.alphaOp.clamp||prev.alphaOp.output||
       alpha.operation||alpha.bias||alpha.scale||!alpha.clamp||alpha.output||
       (tail.alpha!=std::array<std::uint8_t,4>{0,7,7,7}&&
        tail.alpha!=std::array<std::uint8_t,4>{7,7,7,0}))return false;
    auto prefix=state;
    --prefix.stageCount;
    for(unsigned i=0;i<prefix.stageCount;++i)if(prefix.stages[i].colorOp.output==reg)return false;
    PicaMaterialPlan basePlan,addition;
    if(!planPicaMaterial(prefix,basePlan,nullptr,policy)||basePlan.textureCount!=3)return false;
    for(const auto& binding:basePlan.textures)if(binding.map==tail.texture)return false;
    auto final=state;
    final.stageCount=1;
    final.stages[0]=tail;
    final.stages[0].color[3]=15; // PREV is the destination in the additive pass.
    final.stages[0].alpha={7,7,7,7};
    if(!planPicaMaterial(final,addition,nullptr,policy))return false;
    base=basePlan;
    contribution=addition;
    return true;
}
void materialOperationRefusals(std::array<std::uint32_t,unsigned(MaterialOperationReason::count)>& counts){
    for(unsigned n=0;n<operationReasons;++n)counts[n]=operationRefusals[n].load(std::memory_order_relaxed);
}
// Lines of at most ~200 characters (OSReport keeps 255), nonzero rules only.
void reportMaterialOperationRefusals(void(*sink)(const char*)){
    char line[256];int used=0;bool any=false;
    auto flush=[&](){if(used>0){line[used++]='\n';line[used]=0;sink(line);}used=0;};
    for(unsigned n=1;n<operationReasons;++n){
        const auto count=operationRefusals[n].load(std::memory_order_relaxed);
        if(!count)continue;
        if(used>200)flush();
        if(!used)used=std::snprintf(line,sizeof line,"Native TEV operation refusals by rule (planner calls):");
        used+=std::snprintf(line+used,sizeof line-unsigned(used)-2,"%s %s=%lu",any&&used>60?",":"",
                            materialOperationReasonText(MaterialOperationReason(n)),(unsigned long)count);
        any=true;
    }
    flush();
}
const char* materialOperationReasonText(MaterialOperationReason r){
    switch(r){
    case MaterialOperationReason::none:return "none";
    case MaterialOperationReason::clampOffSigned:return "clamp-off-signed";
    case MaterialOperationReason::clampOffBound:return "clamp-off-bound";
    case MaterialOperationReason::registerValue:return "register-value";
    case MaterialOperationReason::registerSplit:return "register-split";
    case MaterialOperationReason::registerSubtract:return "register-subtract";
    case MaterialOperationReason::registerHalve:return "register-halve";
    case MaterialOperationReason::registerBias:return "register-bias";
    case MaterialOperationReason::registerCompare:return "register-compare";
    case MaterialOperationReason::registerSignedD:return "register-signed-d";
    case MaterialOperationReason::registerPending:return "register-pending";
    case MaterialOperationReason::halveExact:return "halve-exact";
    case MaterialOperationReason::halveSubtract:return "halve-subtract";
    case MaterialOperationReason::halveBias:return "halve-bias";
    case MaterialOperationReason::compareForm:return "compare-form";
    case MaterialOperationReason::compareClampOff:return "compare-clamp-off";
    case MaterialOperationReason::signedDForm:return "signed-d-form";
    case MaterialOperationReason::addHalf:return "add-half";
    case MaterialOperationReason::scaledRegister:return "scaled-register";
    case MaterialOperationReason::invalidOperation:return "invalid-operation";
    case MaterialOperationReason::subtractForm:return "subtract-form";
    case MaterialOperationReason::subHalfExact:return "sub-half-exact";
    case MaterialOperationReason::subHalfClampOff:return "sub-half-clamp-off";
    case MaterialOperationReason::hoistBuffer:return "hoist-buffer";
    case MaterialOperationReason::count:break;
    }
    return "unknown";
}
bool planRasterCarrierMaterial(const MaterialState& state,PicaMaterialPlan& out,MaterialPlanError* error){
    out={};if(error)*error=MaterialPlanError::none;
    auto reject=[&](MaterialPlanError e){if(error)*error=e;return false;};
    if(!state.stageCount||state.stageCount>6)return reject(MaterialPlanError::stages);
    if(state.channelCount!=2)return reject(MaterialPlanError::source);
    if(state.indirectCount)return reject(MaterialPlanError::indirect);
    unsigned rgbMask=0;
    // Conservative over-approximation of reads: even algebraically dead raster
    // operands must satisfy this rule. No discarded alpha can free our carrier.
    for(unsigned i=0;i<state.stageCount;++i){
        const auto& s=state.stages[i];
        if(s.indirect)return reject(MaterialPlanError::indirect);
        bool rgb=false;
        for(auto id:s.color){if(id==11)return reject(MaterialPlanError::source);rgb|=id==10;}
        for(auto id:s.alpha)if(id==5)return reject(MaterialPlanError::source);
        if(!rgb)continue;
        if(s.raster>1)return reject(MaterialPlanError::source);
        if(s.rasterSwap>=4||!(state.swapValid&(1u<<s.rasterSwap)))return reject(MaterialPlanError::uninitialized);
        if(state.swaps[s.rasterSwap]!=std::array<std::uint8_t,4>{0,1,2,3})return reject(MaterialPlanError::swap);
        rgbMask|=1u<<s.raster;
    }
    if(rgbMask!=3)return reject(MaterialPlanError::source);
    if(!buildPicaMaterial(state,out,error,MaterialPolicy::exactConstants,0,RasterCarrier::raster1RgbRaster0GrayAlpha))return false;
    // Demand actual emitted consumption of both carriers, not only dead GX
    // syntax. Unknown operand/permutation cases are refused.
    bool rgb=false,scalar=false;
    auto arity=[](MaterialFunction f){switch(f){case MaterialFunction::replace:return 1u;
        case MaterialFunction::interpolate:case MaterialFunction::multiplyAdd:return 3u;default:return 2u;}};
    for(unsigned i=0;i<out.stageCount;++i)for(unsigned a=0;a<2;++a){
        const auto& ch=a?out.stages[i].alpha:out.stages[i].rgb;
        for(unsigned n=0;n<arity(ch.function);++n){const auto& in=ch.inputs[n];
            if(in.source!=MaterialSource::primary)continue;
            rgb|=in.operand==MaterialOperand::color;scalar|=in.operand==MaterialOperand::alpha;
        }
    }
    if(!rgb||!scalar){out={};return reject(MaterialPlanError::source);}
    return true;
}
bool planRasterLookupMaterial(const MaterialState& state,PicaMaterialPlan& out,MaterialPlanError* error,MaterialPolicy policy){
    out={};if(error)*error=MaterialPlanError::none;
    auto reject=[&](MaterialPlanError e){if(error)*error=e;return false;};
    if(state.channelCount!=2)return reject(MaterialPlanError::source);
    PicaMaterialPlan candidate;
    if(!buildPicaMaterial(state,candidate,error,policy,0,RasterCarrier::raster0RgbaRaster1Lookup))return false;
    if(!candidate.rasterReadMask[1])return reject(MaterialPlanError::source);
    if(candidate.textureCount>1)return reject(MaterialPlanError::textureUnits);
    if(candidate.stageCount>4)return reject(MaterialPlanError::stages);
    if(candidate.bufferUpdateRgb||candidate.bufferUpdateAlpha)return reject(MaterialPlanError::equation);
    for(unsigned n=0;n<candidate.stageCount;++n)for(auto* channel:{&candidate.stages[n].rgb,&candidate.stages[n].alpha}){
        for(auto& input:channel->inputs){
            if(input.source==MaterialSource::buffer)return reject(MaterialPlanError::constantConflict);
            if(input.source==MaterialSource::texture2)input.source=MaterialSource::buffer;
        }
    }
    const unsigned base=candidate.textureCount;
    for(unsigned n=candidate.stageCount;n>0;--n)candidate.stages[n+1]=candidate.stages[n-1];
    candidate.stages[0]={};candidate.stages[1]={};
    const auto rg=MaterialSource(unsigned(MaterialSource::texture0)+base),ba=MaterialSource(unsigned(rg)+1);
    candidate.stages[0].rgb={MaterialFunction::add,{{{rg,MaterialOperand::color,false},{ba,MaterialOperand::color,false},{}}}};
    candidate.stages[0].alpha.inputs[0]={ba,MaterialOperand::alpha,false};
    candidate.stages[1].rgb.inputs[0]={MaterialSource::previous,MaterialOperand::color,false};
    candidate.stages[1].alpha.inputs[0]={MaterialSource::previous,MaterialOperand::alpha,false};
    candidate.bufferUpdateRgb=candidate.bufferUpdateAlpha=1;
    candidate.stageCount+=2;
    candidate.textures[base]=candidate.textures[base+1]={255,255};
    candidate.textureCount+=2;
    out=candidate;return true;
}
bool appendAlphaBand(PicaMaterialPlan& plan,std::uint8_t upper){
    const unsigned n=plan.stageCount;
    if(!n||n>3||upper==255||plan.rasterCarrier!=RasterCarrier::none)return false;
    const MaterialInput previous{MaterialSource::previous,MaterialOperand::alpha,false};
    for(unsigned i=n;i<n+3;++i){
        plan.stages[i]={};
        plan.stages[i].rgb.inputs[0]={MaterialSource::previous,MaterialOperand::color,false};
    }
    // t = 4*(upper+1 - alpha): zero above the band, positive inside it.
    plan.stages[n].constant={0,0,0,std::uint8_t(upper+1)};
    plan.stages[n].alpha={MaterialFunction::subtract,{{{MaterialSource::constant,MaterialOperand::alpha,false},previous,{}}},2};
    // 8*t: one from eight levels below the upper bound downwards.
    plan.stages[n+1].alpha={MaterialFunction::add,{{previous,previous,{}}},2};
    // The original alpha, captured after its last stage, times that factor.
    plan.stages[n+2].alpha={MaterialFunction::modulate,{{previous,{MaterialSource::buffer,MaterialOperand::alpha,false},{}}},0};
    plan.bufferUpdateAlpha|=std::uint8_t(1u<<(n-1));
    plan.stageCount=std::uint8_t(n+3);
    return true;
}
bool routeProjectiveTexture(PicaMaterialPlan& plan,std::uint8_t projectiveCoordinates) {
    if(plan.textureCount>3 || !plan.stageCount || plan.stageCount>6)return false;
    unsigned projected=3;
    for(unsigned unit=0;unit<plan.sourceTextures();++unit) {
        if(plan.textures[unit].coordinate>=8)return false;
        if(projectiveCoordinates&(1u<<plan.textures[unit].coordinate)) {
            if(projected!=3)return false;
            projected=unit;
        }
    }
    if(projected==3){plan.projectiveTexture0=false;return true;}
    if(projected) {
        std::swap(plan.textures[0],plan.textures[projected]);
        const auto old=static_cast<MaterialSource>(unsigned(MaterialSource::texture0)+projected);
        for(unsigned i=0;i<plan.stageCount;++i)for(auto* channel:{&plan.stages[i].rgb,&plan.stages[i].alpha})
            for(auto& input:channel->inputs) {
                if(input.source==MaterialSource::texture0)input.source=old;
                else if(input.source==old)input.source=MaterialSource::texture0;
            }
    }
    plan.projectiveTexture0=true;return true;
}
const char* materialPlanErrorText(MaterialPlanError e){
    switch(e){
    case MaterialPlanError::none:return "none";
    case MaterialPlanError::stages:return "PICA stage budget";
    case MaterialPlanError::operation:return "TEV operation/scale/output unsupported";
    case MaterialPlanError::indirect:return "indirect texture effect pending";
    case MaterialPlanError::source:return "TEV source/order unsupported";
    case MaterialPlanError::uninitialized:return "TEV color/swap not initialized";
    case MaterialPlanError::constantRange:return "signed TEV constant outside PICA range";
    case MaterialPlanError::konstFraction:return "TEV fraction needs exact translation";
    case MaterialPlanError::constantConflict:return "multiple constants need another PICA stage";
    case MaterialPlanError::swap:return "TEV channel permutation unsupported";
    case MaterialPlanError::textureUnits:return "PICA texture unit budget";
    case MaterialPlanError::equation:return "TEV equation needs multiple stages";
    }
    return "unknown material plan error";
}
}
