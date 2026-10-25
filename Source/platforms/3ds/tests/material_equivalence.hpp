#pragma once
// Independent normalized-real reference. This checks translation algebra, not
// GX/PICA integer rounding, texture sampling, lighting, or GPU execution.
#include "../runtime/gx_pica_material.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
namespace material_test {
using namespace dusk::native::gx;
using Color=std::array<double,4>;
struct Samples {Color raster;std::array<std::array<Color,8>,8> texture;};
inline double operand(const Color& c,MaterialOperand op,unsigned component){
    switch(op){case MaterialOperand::color:return c[component];case MaterialOperand::alpha:return c[3];
    case MaterialOperand::red:return c[0];case MaterialOperand::green:return c[1];case MaterialOperand::blue:return c[2];}
    assert(false);return 0;
}
inline Color gxEvaluate(const MaterialState& s,const Samples& samples){
    std::array<Color,4> regs{};
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)regs[r][c]=s.colors[r][c]/255.0;
    for(unsigned t=0;t<s.stageCount;++t){
        const auto& stage=s.stages[t];Color next{};
        for(unsigned c=0;c<4;++c){
            const bool alpha=c==3;const auto& args=alpha?stage.alpha:stage.color;
            auto value=[&](unsigned arg)->double{
                if((alpha&&arg<4)||(!alpha&&arg<8))return regs[alpha?arg:arg/2][alpha||(arg&1)?3:c];
                if((alpha&&arg==4)||(!alpha&&(arg==8||arg==9)))return samples.texture[stage.texture][stage.coordinate][s.swaps[stage.textureSwap][alpha||arg==9?3:c]];
                if((alpha&&arg==5)||(!alpha&&(arg==10||arg==11)))return stage.raster==7?0:samples.raster[s.swaps[stage.rasterSwap][alpha||arg==11?3:c]];
                if((alpha&&arg==6)||(!alpha&&arg==14)){
                    const unsigned k=alpha?stage.konstAlpha:stage.konstColor;
                    if(k<8)return (8-k)/8.0;
                    if(k>=12&&k<=15&&!alpha)return s.konst[k-12][c]/255.0;
                    assert(k>=16&&k<=31);return s.konst[(k-16)%4][(k-16)/4]/255.0;
                }
                if(!alpha&&arg==12)return 1;
                if(!alpha&&arg==13)return .5;
                assert(arg==(alpha?7:15));return 0;
            };
            // Initial signed registers are reduced modulo256 in A/B/C;
            // D keeps the signed value. Bounded normalized intermediates are
            // unchanged. This remains a real-arithmetic, not rounding model.
            auto wrap=[](double v){return v>=0&&v<=1?v:(v*255-std::floor(v*255/256)*256)/255;};
            const double a=wrap(value(args[0])),b=wrap(value(args[1])),k=wrap(value(args[2])),d=value(args[3]);
            const auto& op=alpha?stage.alphaOp:stage.colorOp;
            assert(op.operation<=1&&(op.bias==0||op.bias==2)&&op.output<4&&op.clamp<=1&&op.scale<=3);
            next[c]=((op.operation?-1:1)*(a*(1-k)+b*k)+d-(op.bias==2?128/255.0:0))*(op.scale==3?.5:double(1u<<op.scale));
            if(op.clamp)next[c]=std::clamp(next[c],0.0,1.0);
        }
        for(unsigned c=0;c<4;++c)regs[(c==3?stage.alphaOp:stage.colorOp).output][c]=next[c];
    }
    Color result{};const auto& last=s.stages[s.stageCount-1];
    for(unsigned c=0;c<4;++c)result[c]=regs[(c==3?last.alphaOp:last.colorOp).output][c];
    return result;
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
                    const auto& binding=p.textures[unit];source=samples.texture[binding.map][binding.coordinate];break;}
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
        prev=next;
    }
    return prev;
}
inline std::uint32_t randomWord(std::uint32_t& seed){seed=1664525*seed+1013904223;return seed;}
inline void compare(const MaterialState& state,const PicaMaterialPlan& plan,unsigned count=32){
    std::uint32_t seed=0x764a219b;
    for(unsigned n=0;n<count;++n){
        Samples samples{};
        auto sample=[&](){return n==0?0.0:n==1?1.0:double(randomWord(seed)&255)/255.0;};
        for(auto& v:samples.raster)v=sample();
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
