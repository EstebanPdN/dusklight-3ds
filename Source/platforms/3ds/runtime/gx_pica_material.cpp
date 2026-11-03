#include "gx_pica_material.hpp"
#include <algorithm>
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
struct Builder {
    const MaterialState& state;PicaMaterialPlan& plan;MaterialPolicy policy;MaterialPlanError error=MaterialPlanError::none;
    RasterCarrier carrier{};
    unsigned index=0,output=0;std::array<bool,2> split{};std::array<Value,2> additions{};bool alpha=false,constantSet=false;std::array<u8,3> assigned{};
    // Copy propagation for a bounded, unscaled direct register assignment.
    // Texture/raster/literal sources are stable for a fragment. PREVIOUS is not.
    std::array<std::array<Value,4>,2> aliases{};
    std::array<std::array<bool,4>,2> aliasValid{};
    std::array<Value,2> pendingAliases{};
    std::array<bool,2> pendingValid{};
    bool aliasMode=false;
    bool finishingStage=false;
    std::array<bool,2> finishInverse{};
    std::array<bool,2> bufferSet{};
    unsigned temporaryReg=0,temporaryIndex=0;
    unsigned bufferedReg=0;
    std::array<MaterialFunction,2> finishFunctions{MaterialFunction::add,MaterialFunction::add};
    bool fail(MaterialPlanError e){error=e;return false;}
    bool color(unsigned reg,bool componentAlpha,Value& v,bool wrap=false){
        if(!componentAlpha&&bufferedReg&&reg==bufferedReg&&index>=2){
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
            if(!output||(!alpha&&plan.bufferUpdateRgb))return fail(MaterialPlanError::constantConflict);
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
    bool emit(MaterialFunction f,const Value& a,const Value& b=constant(0),const Value& c=constant(0)){
        const auto& op=alpha?state.stages[index].alphaOp:state.stages[index].colorOp;
        if(op.scale==3&&!finishingStage){
            // PICA has no half output scale. A bounded sampled replace can
            // use nearest UNORM8 half (128/255), with <=0.5/255 real error.
            // Larger sums/products must not be clamped before halving.
            if(policy==MaterialPolicy::exactConstants||op.operation||op.bias||f!=MaterialFunction::replace||a.literal||aliasMode)
                return fail(MaterialPlanError::operation);
            auto& out=alpha?plan.stages[output].alpha:plan.stages[output].rgb;
            out.function=MaterialFunction::modulate;out.scale=0;
            if(!bind(a,out.inputs[0])||!bind(constant(128),out.inputs[1]))return false;
            plan.quantizedConstants=true;return true;
        }
        if(!op.clamp) {
            // PICA clamps every result. That clamp is harmless only when the
            // complete unscaled GX equation, including its scale, stays in
            // [0,1]. All admitted inputs are nonnegative and bounded: literals
            // were checked on read, samples are UNORM, and PREV was produced by
            // a clamped or independently bounded preceding stage. Use integer
            // numerator bounds to avoid floating-point acceptance at the edge.
            auto upper=[](const Value& v)->unsigned {
                return v.literal?*std::max_element(v.bytes.begin(),v.bytes.end()):255;
            };
            const unsigned x=upper(a),y=upper(b),z=upper(c);
            unsigned bound=x*255;
            switch(f) {
            case MaterialFunction::replace:break;
            case MaterialFunction::modulate:bound=x*y;break;
            case MaterialFunction::add:bound=(x+y)*255;break;
            case MaterialFunction::interpolate:bound=std::max(x,y)*255;break;
            case MaterialFunction::multiplyAdd:bound=x*y+z*255;break;
            case MaterialFunction::addSigned:case MaterialFunction::subtract:return fail(MaterialPlanError::operation);
            }
            if(bound*(1u<<op.scale)>255u*255u)return fail(MaterialPlanError::operation);
        }
        if(aliasMode) {
            if(f!=MaterialFunction::replace || (!a.literal && a.input.source==MaterialSource::previous))
                return fail(MaterialPlanError::operation);
            pendingAliases[alpha]=a;pendingValid[alpha]=true;return true;
        }
        auto& out=alpha?plan.stages[output].alpha:plan.stages[output].rgb;out.function=f;
        if(!bind(a,out.inputs[0]))return false;
        if(f!=MaterialFunction::replace&&!bind(b,out.inputs[1]))return false;
        if((f==MaterialFunction::interpolate||f==MaterialFunction::multiplyAdd)&&!bind(c,out.inputs[2]))return false;
        return true;
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
            if(aliasMode)return fail(MaterialPlanError::operation);
            if(output+1>=plan.stages.size())return fail(MaterialPlanError::stages);
            if(!emit(MaterialFunction::modulate,a,b))return false;
            (alpha?plan.stages[output].alpha:plan.stages[output].rgb).scale=0;
            split[alpha]=true;additions[alpha]=d;return true;
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
        if(aliasMode)return fail(MaterialPlanError::operation);
        if(output+1>=plan.stages.size())return fail(MaterialPlanError::stages);
        const Value& low=increasing?a:b;
        const Value& high=increasing?b:a;
        Value difference=constant(0);
        for(unsigned c=0;c<3;++c)difference.bytes[c]=high.bytes[c]-low.bytes[c];
        if(!emit(MaterialFunction::modulate,increasing?factor:inverse(factor),difference))return false;
        (alpha?plan.stages[output].alpha:plan.stages[output].rgb).scale=0;
        split[alpha]=true;additions[alpha]=low;return true;
    }
    bool channelExpression(bool isAlpha){
        alpha=isAlpha;constantSet=false;const auto& stage=state.stages[index];
        const auto& args=alpha?stage.alpha:stage.color;const auto& op=alpha?stage.alphaOp:stage.colorOp;
        if(op.operation>1||(op.bias!=0&&op.bias!=2)||op.scale>3||op.clamp>1||op.output>3||
           (op.output!=0&&op.scale!=0&&index+1<state.stageCount))return fail(MaterialPlanError::operation);
        (alpha?plan.stages[output].alpha:plan.stages[output].rgb).scale=op.scale;
        const unsigned zero=alpha?7:15;Value a,b,c,d;
        // Initial signed D in [-255,0] is an exact subtract from the bounded
        // interpolation. Preserve the intermediate before the final clamp and
        // scale; mixed-sign offsets and computed register lifetimes still reject.
        const unsigned dReg=alpha?args[3]:args[3]/2;
        const bool dAlpha=alpha||(args[3]&1);
        if(!index&&args[3]<(alpha?4:8)&&(state.colorValid&(1u<<dReg))){
            bool negative=false,nonpositive=true;Value magnitude=constant(0);
            for(unsigned component=0;component<3;++component){
                const int n=state.colors[dReg][dAlpha?3:component];
                negative|=n<0;nonpositive&=n>=-255&&n<=0;
                magnitude.bytes[component]=u8(-n);
            }
            if(negative&&nonpositive){
                if(op.operation||op.bias||!op.clamp||op.scale==3||aliasMode)
                    return fail(MaterialPlanError::operation);
                if(output+1>=plan.stages.size())return fail(MaterialPlanError::stages);
                if(!input(args[0],a)||!input(args[1],b)||!input(args[2],c))return false;
                bool okay;
                if(a==b||is(c,0))okay=emit(MaterialFunction::replace,a);
                else if(is(c,255))okay=emit(MaterialFunction::replace,b);
                else if(is(a,0))okay=emit(MaterialFunction::modulate,b,c);
                else if(is(b,0))okay=emit(MaterialFunction::modulate,a,inverse(c));
                else okay=emit(MaterialFunction::interpolate,b,a,c);
                if(!okay)return false;
                (alpha?plan.stages[output].alpha:plan.stages[output].rgb).scale=0;
                split[alpha]=true;additions[alpha]=magnitude;finishFunctions[alpha]=MaterialFunction::subtract;
                return true;
            }
        }
        if(op.operation==1){
            // clamp(D-B*C) = 1-clamp(B*C + (1-D)). The complement
            // form keeps D available in PREVIOUS while forming the product,
            // including when the RGB buffer already holds another register.
            if(op.bias||!op.clamp||aliasMode)return fail(MaterialPlanError::operation);
            if(output+1>=plan.stages.size())return fail(MaterialPlanError::stages);
            if(!input(args[0],a)||!input(args[1],b)||!input(args[2],c)||!input(args[3],d,false))return false;
            bool okay=false;
            if(is(a,0))okay=emit(MaterialFunction::multiplyAdd,b,c,inverse(d));
            else if(is(b,0))okay=emit(MaterialFunction::multiplyAdd,a,inverse(c),inverse(d));
            else return fail(MaterialPlanError::equation);
            if(!okay)return false;
            (alpha?plan.stages[output].alpha:plan.stages[output].rgb).scale=0;
            split[alpha]=true;finishFunctions[alpha]=MaterialFunction::replace;finishInverse[alpha]=true;
            return true;
        }
        if(op.bias==2) {
            // First form the bounded interpolation/product, then add D and
            // subtract half in PICA ADD_SIGNED. Never clamp A*(1-C)+B*C+D
            // before subtracting: that would lose bright grass/foliage values.
            // The UNORM8 half-bias path is explicitly part of native Tier D;
            // strict mode retains its refusal pending fixed-point GPU proof.
            if(policy==MaterialPolicy::exactConstants||!op.clamp||aliasMode)return fail(MaterialPlanError::operation);
            if(output+1>=plan.stages.size())return fail(MaterialPlanError::stages);
            if(!input(args[0],a)||!input(args[1],b)||!input(args[2],c)||!input(args[3],d,false))return false;
            if(!d.literal&&d.input.source==MaterialSource::previous)return fail(MaterialPlanError::equation);
            bool okay;
            if(a==b||is(c,0))okay=emit(MaterialFunction::replace,a);
            else if(is(c,255))okay=emit(MaterialFunction::replace,b);
            else if(is(a,0))okay=emit(MaterialFunction::modulate,b,c);
            else if(is(b,0))okay=emit(MaterialFunction::modulate,a,inverse(c));
            else okay=emit(MaterialFunction::interpolate,b,a,c);
            if(!okay)return false;
            (alpha?plan.stages[output].alpha:plan.stages[output].rgb).scale=0;
            split[alpha]=true;additions[alpha]=d;finishFunctions[alpha]=MaterialFunction::addSigned;
            plan.quantizedConstants=true;return true;
        }
        // Eliminate mathematically unused inputs before checking their resource
        // dependencies; a missing unused PREV/register must not reject a draw.
        if(args[0]==args[1])return input(args[0],a)&&input(args[3],d,false)&&add(a,d);
        if(args[2]==zero)return input(args[0],a)&&input(args[3],d,false)&&add(a,d);
        if(!input(args[2],c)||!input(args[3],d,false))return false;
        if(is(c,0))return input(args[0],a)&&add(a,d);
        if(is(c,255))return input(args[1],b)&&add(b,d);
        if(!input(args[0],a)||!input(args[1],b))return false;
        if(a==b)return add(a,d);
        if(is(a,0))return multiplyAdd(b,c,d);
        if(is(b,0))return multiplyAdd(a,inverse(c),d);
        if(is(d,0))return interpolate(a,b,c);
        // A convex interpolation stays in [0,1], so an intermediate clamp
        // preserves the equation. Defer scaling until the addition. PREV in D
        // would be overwritten by that intermediate and needs a separate buffer.
        if(!d.literal&&d.input.source==MaterialSource::previous)return fail(MaterialPlanError::equation);
        if(output+1>=plan.stages.size())return fail(MaterialPlanError::stages);
        if(!emit(MaterialFunction::interpolate,b,a,c))return false;
        (alpha?plan.stages[output].alpha:plan.stages[output].rgb).scale=0;
        split[alpha]=true;additions[alpha]=d;return true;
    }
    bool channel(bool isAlpha) {
        const auto& op=isAlpha?state.stages[index].alphaOp:state.stages[index].colorOp;
        const bool canRoute=!isAlpha&&op.output&&!temporaryReg&&!bufferSet[0]&&!plan.bufferUpdateRgb&&
            index+2==state.stageCount&&output&&output<=4;
        PicaMaterialPlan before;
        if(canRoute)before=plan;
        const bool finalStage=index+1==state.stageCount;
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
            plan=before;error=MaterialPlanError::none;pendingValid[0]=false;
            plan.bufferUpdateRgb=1u<<(output-1);
            if(!channelExpression(false))return false;
            temporaryReg=op.output;temporaryIndex=index;
            return true;
        }
        if(op.output!=0&&!finalStage) {
            if(!pendingValid[isAlpha])return fail(MaterialPlanError::operation);
            // Preserve GX PREV, while the independently tracked register keeps
            // its stable source. RGB and alpha assignments commit simultaneously.
            Value previous;
            if(!color(0,isAlpha,previous))return false;
            constantSet=false;
            if(!emit(MaterialFunction::replace,previous))return false;
            (isAlpha?plan.stages[output].alpha:plan.stages[output].rgb).scale=0;
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
        if(!split[0]&&!split[1]){++output;return true;}
        ++output;finishingStage=true;
        for(unsigned channel=0;channel<2;++channel){
            alpha=channel;constantSet=false;
            const Value previous{false,{}, {MaterialSource::previous,alpha?MaterialOperand::alpha:MaterialOperand::color,false}};
            if(split[channel]){
                if(!emit(finishFunctions[channel],finishInverse[channel]?inverse(previous):previous,additions[channel]))return false;
                (alpha?plan.stages[output].alpha:plan.stages[output].rgb).scale=
                    (alpha?state.stages[index].alphaOp:state.stages[index].colorOp).scale;
            }else if(!emit(MaterialFunction::replace,previous))return false;
        }
        ++output;finishingStage=false;return true;
    }
};
}
static bool buildPicaMaterial(const MaterialState& state,PicaMaterialPlan& out,MaterialPlanError* error,MaterialPolicy policy,unsigned bufferedReg=0,RasterCarrier carrier=RasterCarrier::none){
    out={};if(error)*error=MaterialPlanError::none;
    auto reject=[&](MaterialPlanError e){out={};if(error)*error=e;return false;};
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
        Builder b{state,out,policy};b.carrier=carrier;b.output=prologue;b.bufferedReg=bufferedReg;bool retry=false;
        for(b.index=0;b.index<state.stageCount;++b.index){
            if(state.stages[b.index].indirect)return reject(MaterialPlanError::indirect);
            if(b.output>=out.stages.size())return reject(MaterialPlanError::stages);
            b.split={};b.pendingValid={};b.finishInverse={};
            b.finishFunctions={MaterialFunction::add,MaterialFunction::add};
            if(!b.channel(false)||!b.channel(true)||!b.finishStage()) {
                if(!prologue&&!b.output&&b.error==MaterialPlanError::constantConflict){retry=true;break;}
                return reject(b.error);
            }
            b.commitAliases();
            if(bufferedReg&&!b.index){
                if(b.bufferSet[0]||!b.output||b.output>4)return reject(MaterialPlanError::operation);
                out.bufferUpdateRgb=1u<<(b.output-1);
            }
        }
        if(retry)continue;
        out.stageCount=b.output;return true;
    }
    return reject(MaterialPlanError::constantConflict);
}
bool planPicaMaterial(const MaterialState& state,PicaMaterialPlan& out,MaterialPlanError* error,MaterialPolicy policy){
    MaterialPlanError initial;
    if(buildPicaMaterial(state,out,&initial,policy)){if(error)*error=MaterialPlanError::none;return true;}
    // Hoist an independent second-stage RGB register calculation before the
    // first PREV calculation. The delayed buffer then retains that register
    // from the third GX stage onward while PREVIOUS continues to evolve.
    // This is a dependency proof, not a material/asset-specific substitution.
    auto refuse=[&](){if(error)*error=initial;return false;};
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
            for(auto& input:lowered.stages[i].color)if(i&&input<8&&!(input&1)){
                const unsigned reg=input/2;
                if(reg==last)input=0;
                else if(reg==savedReg&&i>=2){} // captured first result
                else if(written[reg]||reg==0){valid=false;break;}
            }
            lowered.stages[i].colorOp.output=0;written[destination]=true;last=destination;
        }
        if(valid)return buildPicaMaterial(lowered,out,error,policy,savedReg);
        return refuse();
    }
    const auto& first=state.stages[0];const auto& saved=state.stages[1];
    const auto& op=saved.colorOp;const auto& a=saved.alphaOp;
    if(!op.output||op.operation||op.bias||op.scale||!op.clamp||first.colorOp.output||first.alphaOp.output||
       a.operation||a.bias||a.scale||a.output||saved.alpha[0]!=7||saved.alpha[1]!=7||saved.alpha[3]!=0)return refuse();
    for(auto input:saved.color)if(input==0||input==1)return refuse();
    for(auto input:first.color)if(input==0)return refuse();
    for(unsigned i=2;i+1<state.stageCount;++i)if(state.stages[i].colorOp.output==op.output)return refuse();
    MaterialState reordered=state;std::swap(reordered.stages[0],reordered.stages[1]);
    reordered.stages[0].colorOp.output=0;
    return buildPicaMaterial(reordered,out,error,policy,op.output);
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
