#pragma once
// Independent normalized-real reference. This checks translation algebra, not
// GX/PICA integer rounding, texture sampling, lighting, or GPU execution.
#include "../runtime/gx_pica_material.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
namespace material_test {
using namespace dusk::native::gx;
using Color=std::array<double,4>;
struct Samples {Color raster;std::array<std::array<Color,8>,8> texture;Color secondary;};
inline double operand(const Color& c,MaterialOperand op,unsigned component){
    switch(op){case MaterialOperand::color:return c[component];case MaterialOperand::alpha:return c[3];
    case MaterialOperand::red:return c[0];case MaterialOperand::green:return c[1];case MaterialOperand::blue:return c[2];}
    assert(false);return 0;
}
// quantizedInputs: HALF and the konst fractions take the bytes the
// quantized policy plans them with (128/255, nearest UNORM8), so that a
// quantized plan without the half scale can be checked for exactness.
inline Color gxEvaluate(const MaterialState& s,const Samples& samples,bool quantizedInputs=false){
    std::array<Color,4> regs{};
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)regs[r][c]=s.colors[r][c]/255.0;
    for(unsigned t=0;t<s.stageCount;++t){
        const auto& stage=s.stages[t];Color next{};
        for(unsigned c=0;c<4;++c){
            const bool alpha=c==3;const auto& args=alpha?stage.alpha:stage.color;
            auto value=[&](unsigned arg,unsigned component)->double{
                if((alpha&&arg<4)||(!alpha&&arg<8))return regs[alpha?arg:arg/2][alpha||(arg&1)?3:component];
                if((alpha&&arg==4)||(!alpha&&(arg==8||arg==9)))return samples.texture[stage.texture][stage.coordinate][s.swaps[stage.textureSwap][alpha||arg==9?3:component]];
                if((alpha&&arg==5)||(!alpha&&(arg==10||arg==11)))return stage.raster==7?0:(stage.raster==1?samples.secondary:samples.raster)[s.swaps[stage.rasterSwap][alpha||arg==11?3:component]];
                if((alpha&&arg==6)||(!alpha&&arg==14)){
                    const unsigned k=alpha?stage.konstAlpha:stage.konstColor;
                    constexpr double fractionBytes[]{255,223,191,159,128,96,64,32};
                    if(k<8)return quantizedInputs?fractionBytes[k]/255.0:(8-k)/8.0;
                    if(k>=12&&k<=15&&!alpha)return s.konst[k-12][component]/255.0;
                    assert(k>=16&&k<=31);return s.konst[(k-16)%4][(k-16)/4]/255.0;
                }
                if(!alpha&&arg==12)return 1;
                if(!alpha&&arg==13)return quantizedInputs?128/255.0:.5;
                assert(arg==(alpha?7:15));return 0;
            };
            // Initial signed registers are reduced modulo256 in A/B/C;
            // D keeps the signed value. Bounded normalized intermediates are
            // unchanged. This remains a real-arithmetic, not rounding model.
            auto wrap=[](double v){return v>=0&&v<=1?v:(v*255-std::floor(v*255/256)*256)/255;};
            const double a=wrap(value(args[0],c)),b=wrap(value(args[1],c)),k=wrap(value(args[2],c)),d=value(args[3],c);
            const auto& op=alpha?stage.alphaOp:stage.colorOp;
            assert(op.output<4&&op.clamp<=1&&op.scale<=3);
            if(op.operation>=8){
                // GX compare: D + (A op B ? C : 0) on the 8-bit operands (as
                // aurora rounds them). R8/GR16/BGR24 read the RGB inputs.
                const unsigned mode=(op.operation-8)/2;
                auto byte=[&](unsigned arg,unsigned component){return std::lround(wrap(value(arg,component))*255);};
                auto packed=[&](unsigned arg){
                    long v=0;for(unsigned n=0;n<=mode;++n)v+=byte(arg,n)<<(8*n);return v;
                };
                long x,y;
                if(mode==3||alpha){assert(mode==3);x=std::lround(a*255);y=std::lround(b*255);}
                else{x=packed(args[0]);y=packed(args[1]);}
                next[c]=d+(((op.operation&1)?x==y:x>y)?k:0);
            }else{
                // Bias: ADDHALF +128/255, SUBHALF -128/255 (the byte the
                // hardware adds; PICA ADD_SIGNED subtracts the same).
                assert(op.operation<=1&&op.bias<=2);
                const double bias=op.bias==2?-128/255.0:op.bias==1?128/255.0:0;
                next[c]=((op.operation?-1:1)*(a*(1-k)+b*k)+d+bias)*(op.scale==3?.5:double(1u<<op.scale));
            }
            if(op.clamp)next[c]=std::clamp(next[c],0.0,1.0);
        }
        for(unsigned c=0;c<4;++c)regs[(c==3?stage.alphaOp:stage.colorOp).output][c]=next[c];
    }
    // The output is saturated: 8-bit target, alpha test on the clamped value.
    Color result{};const auto& last=s.stages[s.stageCount-1];
    for(unsigned c=0;c<4;++c)result[c]=std::clamp(regs[(c==3?last.alphaOp:last.colorOp).output][c],0.0,1.0);
    return result;
}
// Forms gxEvaluate defines: in-range texture, coordinate and swap indices,
// konst selectors the hardware defines, add or
// subtract with bias 0..2 and scale 0..3 (any compare in RGB), A8 compares
// in alpha, registers 0..3, clamp 0/1. Others (only ever in dead stages of
// plans) are not evaluated.
inline bool referenceEvaluable(const MaterialState& s){
    for(const auto& swap:s.swaps)for(auto c:swap)if(c>3)return false;
    for(unsigned i=0;i<s.stageCount;++i){
        const auto& st=s.stages[i];
        if(st.texture>7||st.coordinate>7||st.textureSwap>3||st.rasterSwap>3)return false;
        if((st.konstColor>=8&&st.konstColor<12)||(st.konstAlpha>=8&&st.konstAlpha<16)||st.konstColor>31||st.konstAlpha>31)return false;
        for(unsigned channel=0;channel<2;++channel){
            const auto& op=channel?st.alphaOp:st.colorOp;
            if(op.output>3||op.clamp>1)return false;
            if(op.operation>=8){if(op.operation>15||(channel&&(op.operation-8)/2!=3))return false;}
            else if(op.operation>1||op.bias>2||op.scale>3)return false;
        }
    }
    return true;
}
inline Color picaEvaluate(const PicaMaterialPlan& p,const Samples& samples){
    Color prev{},buffer{},nextBuffer{};
    for(unsigned c=0;c<4;++c)nextBuffer[c]=p.bufferConstant[c]/255.0;
    for(unsigned t=0;t<p.stageCount;++t){
        const auto& stage=p.stages[t];Color next{};
        for(unsigned c=0;c<4;++c){
            const auto& ch=c==3?stage.alpha:stage.rgb;
            auto value=[&](unsigned n){
                const auto& in=ch.inputs[n];Color source{};
                switch(in.source){
                case MaterialSource::primary:source=samples.raster;break;
                case MaterialSource::previous:source=prev;break;
                case MaterialSource::constant:for(unsigned k=0;k<4;++k)source[k]=stage.constant[k]/255.0;break;
                case MaterialSource::buffer:source=buffer;break;
                default:{const unsigned unit=unsigned(in.source)-unsigned(MaterialSource::texture0);assert(unit<p.textureCount);
                    const auto& binding=p.textures[unit];
                    if(binding.map==255&&p.rasterCarrier==RasterCarrier::raster0RgbaRaster1Lookup){
                        source=unit==p.sourceTextures()?Color{samples.secondary[0],samples.secondary[1],0,0}:Color{0,0,samples.secondary[2],samples.secondary[3]};
                    }else source=samples.texture[binding.map][binding.coordinate];break;}
                }
                const double v=operand(source,in.operand,c);return in.inverse?1-v:v;
            };
            double result=value(0);
            switch(ch.function){
            case MaterialFunction::replace:break;
            case MaterialFunction::modulate:result*=value(1);break;
            case MaterialFunction::add:result+=value(1);break;
            case MaterialFunction::interpolate:result=result*value(2)+value(1)*(1-value(2));break;
            case MaterialFunction::multiplyAdd:result=result*value(1)+value(2);break;
            case MaterialFunction::addSigned:result=result+value(1)-128/255.0;break;
            case MaterialFunction::subtract:result-=value(1);break;
            }
            next[c]=std::clamp(result*(1u<<ch.scale),0.0,1.0);
        }
        buffer=nextBuffer;
        if(p.bufferUpdateRgb&(1u<<t))for(unsigned c=0;c<3;++c)nextBuffer[c]=next[c];
        if(p.bufferUpdateAlpha&(1u<<t))nextBuffer[3]=next[3];
        prev=next;
    }
    return prev;
}
// A material in the disc survey's number format: stageCount texgenCount
// channelCount indirect blend alphaCompare(5), the four registers (PREV,
// REG0-2, signed), konst, swaps, then 26 numbers per stage (colour inputs,
// colour op, alpha inputs, alpha op, coordinate texture enabled raster
// rasterSwap textureSwap konstColor konstAlpha). TEV state only; the
// alpha compare and blend fields are skipped. False for a malformed line.
inline bool parseSurveyMaterial(const char* numbers,MaterialState& s){
    std::array<int,16+48+26*16+1> n{};unsigned count=0;
    for(const char* p=numbers;*p&&count<n.size();){
        char* end;const long v=std::strtol(p,&end,10);if(end==p){++p;continue;}
        n[count++]=int(v);p=end;
    }
    s={};unsigned k=0;
    if(count<10+48||n[0]<0||n[0]>16||count!=10+48+26u*unsigned(n[0]))return false;
    s.stageCount=std::uint8_t(n[k++]);s.texgenCount=std::uint8_t(n[k++]);s.channelCount=std::uint8_t(n[k++]);k+=7;
    for(auto& r:s.colors)for(auto& c:r)c=std::int16_t(n[k++]);
    for(auto& r:s.konst)for(auto& c:r)c=std::uint8_t(n[k++]);
    for(auto& r:s.swaps)for(auto& c:r)c=std::uint8_t(n[k++]);
    s.colorValid=s.konstValid=s.swapValid=15;
    for(unsigned i=0;i<s.stageCount;++i){
        auto& st=s.stages[i];auto u=[&]{return std::uint8_t(n[k++]);};
        for(auto& v:st.color)v=u();
        st.colorOp.operation=u();st.colorOp.bias=u();st.colorOp.scale=u();st.colorOp.clamp=u();st.colorOp.output=u();
        for(auto& v:st.alpha)v=u();
        st.alphaOp.operation=u();st.alphaOp.bias=u();st.alphaOp.scale=u();st.alphaOp.clamp=u();st.alphaOp.output=u();
        st.coordinate=u();st.texture=u();st.textureEnabled=u();st.raster=u();st.rasterSwap=u();st.textureSwap=u();
        st.konstColor=u();st.konstAlpha=u();
        // As decodeMaterial: a compare operation carries no bias or scale.
        for(auto* op:{&st.colorOp,&st.alphaOp})if(op->operation>=8)op->bias=op->scale=0;
    }
    return true;
}
inline MaterialState surveyMaterial(const char* numbers){
    MaterialState s;const bool okay=parseSurveyMaterial(numbers,s);assert(okay);(void)okay;return s;
}
inline bool samePlan(const PicaMaterialPlan& a,const PicaMaterialPlan& b){
    for(unsigned i=0;i<a.stages.size();++i){
        const auto& x=a.stages[i];const auto& y=b.stages[i];
        if(x.constant!=y.constant)return false;
        for(auto [p,q]:{std::pair{&x.rgb,&y.rgb},std::pair{&x.alpha,&y.alpha}})
            if(p->function!=q->function||p->inputs!=q->inputs||p->scale!=q->scale)return false;
    }
    return a.textures==b.textures&&a.stageCount==b.stageCount&&a.textureCount==b.textureCount&&
        a.bufferConstant==b.bufferConstant&&a.bufferUpdateRgb==b.bufferUpdateRgb&&a.bufferUpdateAlpha==b.bufferUpdateAlpha&&
        a.rasterReadMask==b.rasterReadMask&&a.projectiveTexture0==b.projectiveTexture0&&
        a.quantizedConstants==b.quantizedConstants&&a.rasterCarrier==b.rasterCarrier&&a.lowering==b.lowering;
}
inline std::uint32_t randomWord(std::uint32_t& seed){seed=1664525*seed+1013904223;return seed;}
inline void reportMismatch(const MaterialState& state,const PicaMaterialPlan& plan,unsigned sample,unsigned component,double gx,double pica){
    std::fprintf(stderr,"Material mismatch sample=%u component=%u gx=%.17g pica=%.17g stages=%u/%u lowering=%u\n",sample,component,gx,pica,state.stageCount,plan.stageCount,unsigned(plan.lowering));
    for(unsigned t=0;t<state.stageCount;++t){const auto& st=state.stages[t];std::fprintf(stderr,"stage%u RGB=%u,%u,%u,%u alpha=%u,%u,%u,%u op=%u/%u bias=%u/%u scale=%u/%u clamp=%u/%u out=%u/%u\n",t,st.color[0],st.color[1],st.color[2],st.color[3],st.alpha[0],st.alpha[1],st.alpha[2],st.alpha[3],st.colorOp.operation,st.alphaOp.operation,st.colorOp.bias,st.alphaOp.bias,st.colorOp.scale,st.alphaOp.scale,st.colorOp.clamp,st.alphaOp.clamp,st.colorOp.output,st.alphaOp.output);}
}
// Integer sweep: every sampled input is a byte. Two free bytes run over a
// grid of [0,255] and are spread over the raster and texture channels with
// fixed per-map offsets, so sums, products and complements meet 0 and 255
// and a texture never equals the raster colour. Asserts the reference GX
// value within tolerance; returns the largest error seen.
inline double sweep(const MaterialState& state,const PicaMaterialPlan& plan,unsigned stride=3,double tolerance=1e-12){
    double worst=0;unsigned sample=0;
    auto byte=[](unsigned v){return double(v&255)/255.0;};
    for(unsigned x=0;x<256;x+=stride)for(unsigned y=0;y<256;y+=stride,++sample){
        Samples in{};
        in.raster={byte(x),byte(y),byte(255-x),byte(y^0x5a)};
        in.secondary={byte(y),byte(x+17),byte(x^y),byte(255-y)};
        for(unsigned m=0;m<8;++m)for(unsigned c=0;c<8;++c){
            const unsigned k=37*m+11*c;
            in.texture[m][c]={byte(y+k),byte(x+3*k),byte(x+y+k),byte(255-x+k)};
        }
        const auto a=gxEvaluate(state,in),b=picaEvaluate(plan,in);
        for(unsigned c=0;c<4;++c){
            const double e=std::abs(a[c]-b[c]);worst=std::max(worst,e);
            if(e>tolerance){reportMismatch(state,plan,sample,c,a[c],b[c]);assert(false);}
        }
    }
    return worst;
}
inline void compare(const MaterialState& state,const PicaMaterialPlan& plan,unsigned count=32){
    std::uint32_t seed=0x764a219b;
    for(unsigned n=0;n<count;++n){
        Samples samples{};
        auto sample=[&](){return n==0?0.0:n==1?1.0:double(randomWord(seed)&255)/255.0;};
        for(auto& v:samples.raster)v=sample();
        for(auto& v:samples.secondary)v=sample();
        for(auto& map:samples.texture)for(auto& uv:map)for(auto& v:uv)v=sample();
        const auto a=gxEvaluate(state,samples),b=picaEvaluate(plan,samples);
        for(unsigned c=0;c<4;++c)if(std::abs(a[c]-b[c])>=1e-12){
            std::fprintf(stderr,"Material mismatch sample=%u component=%u gx=%.17g pica=%.17g stages=%u/%u\n",n,c,a[c],b[c],state.stageCount,plan.stageCount);
            for(unsigned t=0;t<state.stageCount;++t){const auto& st=state.stages[t];std::fprintf(stderr,"stage%u RGB=%u,%u,%u,%u alpha=%u,%u,%u,%u bias=%u/%u scale=%u/%u out=%u/%u\n",t,st.color[0],st.color[1],st.color[2],st.color[3],st.alpha[0],st.alpha[1],st.alpha[2],st.alpha[3],st.colorOp.bias,st.alphaOp.bias,st.colorOp.scale,st.alphaOp.scale,st.colorOp.output,st.alphaOp.output);}
            assert(false);
        }
    }
}
}
