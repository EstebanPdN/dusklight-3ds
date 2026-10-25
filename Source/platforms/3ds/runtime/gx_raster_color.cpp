#include "gx_raster_color.hpp"
#include "gx_commands.hpp"
#include <cmath>
#include <bit>
#include <algorithm>
#include "gx_vertex_transform.hpp"
namespace dusk::native::gx {
namespace {
bool fail(RasterColorError e,RasterColorError* error){if(error)*error=e;return false;}
unsigned arity(MaterialFunction f){
    switch(f){case MaterialFunction::replace:return 1;
    case MaterialFunction::modulate:case MaterialFunction::add:return 2;
    case MaterialFunction::addSigned:case MaterialFunction::subtract:return 2;
    case MaterialFunction::interpolate:case MaterialFunction::multiplyAdd:return 3;}
    return 0;
}
}
bool planRasterColor(const CommandRegisters& r,const PicaMaterialPlan& material,RasterColorPlan& out,RasterColorError* error){
    out={};if(error)*error=RasterColorError::none;RasterColorPlan p;
    if(material.rasterCarrier!=RasterCarrier::none||!material.stageCount||material.stageCount>6)return fail(RasterColorError::material,error);
    for(unsigned i=0;i<material.stageCount;++i)for(unsigned alpha=0;alpha<2;++alpha){
        const auto& channel=alpha?material.stages[i].alpha:material.stages[i].rgb;
        const auto count=arity(channel.function);if(!count)return fail(RasterColorError::material,error);
        for(unsigned n=0;n<count;++n){
            const auto& in=channel.inputs[n];if(in.source!=MaterialSource::primary)continue;
            if(in.operand==MaterialOperand::alpha||(alpha&&in.operand==MaterialOperand::color))p.required|=2;
            else p.required|=1;
        }
    }
    return planRasterColorChannel(r,0,p.required,out,error,false);
}
bool planRasterColorChannel(const CommandRegisters& r,unsigned rasterChannel,std::uint8_t required,RasterColorPlan& out,RasterColorError* error,bool strictFinite){
    out={};if(error)*error=RasterColorError::none;RasterColorPlan p;
    if(rasterChannel>1||required>3)return fail(RasterColorError::material,error);
    p.required=required;p.rasterChannel=rasterChannel;p.strictFinite=strictFinite;
    if(p.required){
        if(!r.xfValid[0x1009]||r.xf[0x1009]<=rasterChannel||r.xf[0x1009]>2)return fail(RasterColorError::missingState,error);
        for(unsigned alpha=0;alpha<2;++alpha)if(p.required&(1u<<alpha)){
            const auto reg=(alpha?0x1010:0x100e)+rasterChannel;
            if(!r.xfValid[reg])return fail(RasterColorError::missingState,error);
            const auto control=r.xf[reg];
            if(control&2){
                p.lit|=1u<<alpha;auto& ch=p.channels[alpha];
                ch.mask=((control>>2)&15)|((control>>7)&240);ch.diffuse=(control>>7)&3;
                ch.attenuation=!(control&(1u<<10))?0:(!(control&(1u<<9))?2:1);
                if(ch.diffuse>2)return fail(RasterColorError::lighting,error);
                if(control&64)p.ambientVertex|=1u<<alpha;
                else {
                    if(!r.xfValid[0x100a+rasterChannel])return fail(RasterColorError::missingState,error);
                    for(unsigned c=alpha?3:0;c<(alpha?4:3);++c)p.ambient[c]=float((r.xf[0x100a+rasterChannel]>>(24-8*c))&255)/255;
                }
                auto vector=[&](unsigned at,std::array<float,3>& value){
                    for(unsigned c=0;c<3;++c){
                        if(!r.xfValid[at+c])return fail(RasterColorError::missingState,error);
                        value[c]=std::bit_cast<float>(r.xf[at+c]);
                        if(!std::isfinite(value[c]))return fail(RasterColorError::nonfinite,error);
                    }return true;
                };
                for(unsigned i=0;i<8;++i)if(ch.mask&(1u<<i)){
                    auto& light=p.lights[i];const unsigned at=0x600+16*i;
                    if(!r.xfValid[at+3])return fail(RasterColorError::missingState,error);
                    for(unsigned c=0;c<4;++c)light.color[c]=float((r.xf[at+3]>>(24-8*c))&255)/255;
                    // A black light contributes nothing to these consumed
                    // components. Its unused position/attenuation may be unset.
                    bool contributes=false;
                    for(unsigned c=alpha?3:0;c<(alpha?4:3);++c)contributes|=light.color[c]!=0;
                    if(!contributes){ch.mask&=~(1u<<i);continue;}
                    if(ch.diffuse||ch.attenuation!=2){p.needsPosition=true;if(!vector(at+10,light.position))return false;}
                    if(ch.diffuse||ch.attenuation==0)p.needsNormal=true;
                    if(ch.attenuation!=2&&(!vector(at+13,light.direction)||!vector(at+4,light.cosine)||!vector(at+7,light.distance)))return false;
                }
            }
            if(control&1)p.vertex|=1u<<alpha;
            else{
                if(!r.xfValid[0x100c+rasterChannel])return fail(RasterColorError::missingState,error);
                for(unsigned c=alpha?3:0;c<(alpha?4:3);++c)p.material[c]=float((r.xf[0x100c+rasterChannel]>>(24-8*c))&255)/255;
            }
        }
    }
    out=p;return true;
}
bool evaluateRasterColor(const RasterColorPlan& p,const DecodedVertex& v,std::array<float,4>& out,RasterColorError* error,const TransformedVertex* transformed){
    out={};if(error)*error=RasterColorError::none;
    if(p.rasterChannel>1)return fail(RasterColorError::material,error);
    auto color=p.material;
    if((p.vertex|p.ambientVertex)&&!(v.present&(1u<<(GX_VA_CLR0+p.rasterChannel))))return fail(RasterColorError::vertex,error);
    if((p.needsPosition||p.needsNormal)&&(!transformed||(p.needsNormal&&!transformed->normalValid)))return fail(RasterColorError::transform,error);
    if(p.needsNormal)for(float c:transformed->viewNormal)if(!std::isfinite(c))return fail(RasterColorError::nonfinite,error);
    auto dot=[](const auto& a,const auto& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    auto polynomial=[](const auto& a,float x){return a[0]+a[1]*x+a[2]*x*x;};
    for(unsigned alpha=0;alpha<2;++alpha)if(p.required&(1u<<alpha)){
        const auto& channel=p.channels[alpha];std::array<float,4> illumination=p.ambient;
        for(unsigned c=alpha?3:0;c<(alpha?4:3);++c){
            if((p.vertex|p.ambientVertex)&(1u<<alpha)){
                const auto value=v.color[p.rasterChannel][c];if(!std::isfinite(value)||value<0||value>1)return fail(RasterColorError::nonfinite,error);
                if(p.vertex&(1u<<alpha))color[c]=value;
                if(p.ambientVertex&(1u<<alpha))illumination[c]=value;
            }
        }
        if(!(p.lit&(1u<<alpha)))continue;
        for(unsigned i=0;i<8;++i)if(channel.mask&(1u<<i)){
            const auto& light=p.lights[i];std::array<float,3> direction{};float distance=0,distance2=0;
            if(channel.diffuse||channel.attenuation!=2){
                for(unsigned c=0;c<3;++c){if(!std::isfinite(transformed->viewPosition[c]))return fail(RasterColorError::nonfinite,error);direction[c]=light.position[c]-transformed->viewPosition[c];}
                distance2=dot(direction,direction);distance=std::sqrt(distance2);
                if(!std::isfinite(distance)||distance==0)return fail(RasterColorError::lighting,error);
                for(auto& c:direction)c/=distance;
            }
            float attenuation=1,diffuse=1;
            if(channel.attenuation!=2){
                float cosine;
                if(p.strictFinite){
                    float raw;
                    if(channel.attenuation==1)raw=dot(direction,light.direction);
                    else{
                        const float facing=dot(transformed->viewNormal,direction);
                        if(!std::isfinite(facing))return fail(RasterColorError::nonfinite,error);
                        raw=facing>=0?dot(transformed->viewNormal,light.direction):0;
                    }
                    if(!std::isfinite(raw))return fail(RasterColorError::nonfinite,error);
                    cosine=std::max(0.f,raw);
                }else if(channel.attenuation==1)cosine=std::max(0.f,dot(direction,light.direction));
                else cosine=dot(transformed->viewNormal,direction)>=0?std::max(0.f,dot(transformed->viewNormal,light.direction)):0;
                float denominator;
                if(channel.attenuation==1)denominator=light.distance[0]+light.distance[1]*distance+light.distance[2]*distance2;
                else {
                    auto coefficients=light.distance;
                    if(channel.diffuse){const float length=std::sqrt(dot(coefficients,coefficients));if(!std::isfinite(length)||length==0)return fail(RasterColorError::lighting,error);for(auto& c:coefficients)c/=length;}
                    const float raw=polynomial(coefficients,cosine);
                    if(p.strictFinite&&!std::isfinite(raw))return fail(RasterColorError::nonfinite,error);
                    denominator=std::max(0.f,raw);
                }
                if(!std::isfinite(denominator)||denominator==0||(p.strictFinite&&denominator<0))return fail(RasterColorError::lighting,error);
                const float raw=polynomial(light.cosine,cosine)/denominator;
                if(p.strictFinite&&!std::isfinite(raw))return fail(RasterColorError::nonfinite,error);
                attenuation=std::max(0.f,raw);
            }
            if(channel.diffuse){diffuse=dot(direction,transformed->viewNormal);if(channel.diffuse==2)diffuse=std::max(0.f,diffuse);}
            const float weight=attenuation*diffuse;if(!std::isfinite(weight))return fail(RasterColorError::nonfinite,error);
            for(unsigned c=alpha?3:0;c<(alpha?4:3);++c)illumination[c]+=weight*light.color[c];
        }
        for(unsigned c=alpha?3:0;c<(alpha?4:3);++c){if(!std::isfinite(illumination[c]))return fail(RasterColorError::nonfinite,error);color[c]*=std::clamp(illumination[c],0.f,1.f);}
    }
    out=color;return true;
}
const char* rasterColorErrorText(RasterColorError e){
    switch(e){case RasterColorError::none:return "none";
    case RasterColorError::material:return "PICA color dependency invalid";
    case RasterColorError::missingState:return "GX raster color state missing";
    case RasterColorError::lighting:return "GX lighting configuration or attenuation invalid";
    case RasterColorError::transform:return "GX lighting transform missing";
    case RasterColorError::vertex:return "GX raster vertex color missing";
    case RasterColorError::nonfinite:return "GX raster vertex color invalid";}
    return "unknown raster color error";
}
}
