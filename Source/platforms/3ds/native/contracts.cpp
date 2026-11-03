#include "contracts.hpp"
#include <algorithm>
#include <cmath>
#include <bit>
#include <cstring>
namespace dusk::pica {
namespace {
bool bounded(float x,float bound){
    // Bit classification is independent of ARM's FZ/DN floating-point mode,
    // including signaling NaNs and subnormal input words.
    const auto magnitude=std::bit_cast<std::uint32_t>(x)&0x7fffffffu;
    return magnitude<=std::bit_cast<std::uint32_t>(bound)&&(!magnitude||magnitude>=std::bit_cast<std::uint32_t>(0x1p-62f));
}
template<class E> bool atMost(E x,E max){return unsigned(x)<=unsigned(max);}
bool vertexValid(const Vertex& v,unsigned& highest){
    for(float x:v.position)if(!bounded(x,0x1p20f))return false;
    for(float x:v.normal)if(!bounded(x,2))return false;
    for(const auto& uv:v.uv)for(float x:uv)if(!bounded(x,32768))return false;
    unsigned sum=0;
    for(unsigned i=0;i<4;++i){
        if(v.boneRows[i]%3||v.boneRows[i]>=paletteSize*3)return false;
        sum+=v.weights[i];if(v.weights[i])highest=std::max(highest,unsigned(v.boneRows[i]/3));
    }
    return sum==255;
}
}
Palette Palette::identity() noexcept {Palette p;for(unsigned i=0;i<paletteSize;++i)for(unsigned r=0;r<3;++r)p.position[3*i+r][r]=p.normal[3*i+r][r]=1;return p;}
Material Material::vertexColor(){Material m;m.stages[0].rgb[0]=m.stages[0].alpha[0]=Source::primary;return m;}
Material Material::textured(unsigned unit){
    auto m=vertexColor();if(unit>2){m.stageCount=0;return m;}
    m.textureMask=1u<<unit;m.stages[0].rgbFunction=m.stages[0].alphaFunction=Combine::modulate;
    m.stages[0].rgb[1]=m.stages[0].alpha[1]=Source(unsigned(Source::texture0)+unit);return m;
}
bool inspectGeometry(std::span<const Vertex> vertices,std::span<const std::uint16_t> indices,GeometryInfo& out){
    out={};
    if(vertices.empty()||vertices.size()>65536||indices.empty()||indices.size()%3||indices.size()>65535)return false;
    unsigned highest=0;bool rigid=true;
    for(const auto& v:vertices){if(!vertexValid(v,highest))return false;rigid&=v.weights==std::array<std::uint8_t,4>{255,0,0,0};}
    for(auto i:indices)if(i>=vertices.size())return false;
    out={highest,unsigned(vertices.size()),unsigned(indices.size()),align16(vertices.size_bytes())+align16(indices.size_bytes()),rigid};return true;
}
bool inspectPackedGeometry(std::span<const std::byte> vertices,std::span<const std::byte> indices,GeometryInfo& out){
    static_assert(std::endian::native==std::endian::little);
    out={};
    if(vertices.empty()||vertices.size()%sizeof(Vertex)||vertices.size()/sizeof(Vertex)>65536||indices.empty()||indices.size()%6||indices.size()/2>65535)return false;
    unsigned highest=0;bool rigid=true;const unsigned count=unsigned(vertices.size()/sizeof(Vertex));
    // Decode one fixed native record at a time into a real C++ object. File
    // bytes need not be aligned or contain pre-existing Vertex objects.
    for(unsigned i=0;i<count;++i){Vertex v;std::memcpy(&v,vertices.data()+i*sizeof(Vertex),sizeof(v));if(!vertexValid(v,highest))return false;rigid&=v.weights==std::array<std::uint8_t,4>{255,0,0,0};}
    for(std::size_t i=0;i<indices.size();i+=2){std::uint16_t index;std::memcpy(&index,indices.data()+i,2);if(index>=count)return false;}
    out={highest,count,unsigned(indices.size()/2),align16(vertices.size())+align16(indices.size()),rigid};return true;
}
bool validateSampler(const Sampler& s,unsigned levels){
    return levels&&levels<=8&&atMost(s.wrapS,Wrap::mirror)&&atMost(s.wrapT,Wrap::mirror)&&
        atMost(s.magnify,Filter::linear)&&atMost(s.minify,Filter::linear)&&atMost(s.mipmap,Filter::linear)&&
        s.firstLevel<=s.lastLevel&&s.lastLevel<levels&&s.lodBias>=-4096&&s.lodBias<=4095;
}
bool validateUniforms(const DrawUniforms& u,unsigned requiredBone){
    if(u.samplerMask>7||u.imageOverrideMask>7||u.lightCount>u.lights.size()||
       !atMost(u.cullOverride,CullOverride::back))return false;
    for(unsigned i=0;i<3;++i)if(bool(u.imageOverrideMask&(1u<<i))!=bool(u.imageOverrides[i]))return false;
    for(const auto& sampler:u.samplers)if(!validateSampler(sampler,8))return false;
    if(!u.palette.count||u.palette.count>paletteSize||requiredBone>=u.palette.count)return false;
    for(unsigned i=0;i<3*u.palette.count;++i){
        for(unsigned c=0;c<4;++c)if(!bounded(u.palette.position[i][c],c==3?0x1p20f:32768.f))return false;
        for(float x:u.palette.normal[i])if(!bounded(x,256))return false;
    }
    for(const auto& row:u.viewProjection)for(unsigned c=0;c<4;++c)if(!bounded(row[c],c==3?0x1p20f:32768.f))return false;
    if(u.viewProjection[3]==Row{})return false; // Every vertex would have W=0.
    for(const auto& transform:u.uvTransforms){
        if(transform.source>7)return false;
        for(const auto& row:transform.rows)for(float x:row)if(!bounded(x,32768))return false;
        for(float x:transform.translation)if(!bounded(x,0x1p20f))return false;
    }
    if(!atMost(u.projective.source,PositionSource::posedLinearNormal))return false;
    for(const auto& row:u.projective.rows)for(unsigned c=0;c<4;++c)if(!bounded(row[c],c==3?0x1p20f:32768.f))return false;
    if(u.projective.enabled&&u.projective.rows[2]==Row{})return false;
    for(const auto* colors:{&u.materialColors,&u.ambientColors})for(const auto& row:*colors)for(float x:row)if(!bounded(x,1)||x<0)return false;
    for(const auto& channel:u.channels){
        if(!atMost(channel.diffuse,Diffuse::clamped)||!atMost(channel.attenuation,Attenuation::none)||
           (channel.enabled&&(unsigned(channel.lightMask)>>u.lightCount)))return false;
    }
    double viewBound=0;
    for(unsigned row=0;row<3*u.palette.count;++row){
        const auto& p=u.palette.position[row];
        viewBound=std::max(viewBound,(std::abs(double(p[0]))+std::abs(double(p[1]))+std::abs(double(p[2])))*0x1p20+std::abs(double(p[3])));
    }
    for(unsigned i=0;i<u.lightCount;++i){const auto& light=u.lights[i];
        if(!atMost(light.kind,LightKind::directional))return false;
        for(float x:light.position)if(!bounded(x,0x1p20f))return false;
        float n=0;for(float x:light.direction){if(!bounded(x,32768))return false;n+=x*x;}
        if(light.kind==LightKind::directional&&std::abs(n-1)>0.001f)return false;
        for(float x:light.color)if(!bounded(x,1)||x<0)return false;
        for(const auto* coefficients:{&light.cosine,&light.distance})for(float x:*coefficients)if(!bounded(x,32768))return false;
        bool vectorNeeded=false,spotNeeded=false;
        for(const auto& channel:u.channels)if(channel.enabled&&(channel.lightMask&(1u<<i))){
            vectorNeeded|=channel.diffuse!=Diffuse::none||channel.attenuation!=Attenuation::none;
            spotNeeded|=channel.attenuation==Attenuation::spot;
        }
        if(light.kind==LightKind::point&&vectorNeeded){
            double positionBound=viewBound;
            for(float x:light.position)positionBound=std::max(positionBound,viewBound+std::abs(double(x)));
            // Whole-asset coordinate bound, not a CPU vertex transform. Leave
            // twofold arithmetic headroom below the PICA float24 exponent limit.
            const double squared=12*positionBound*positionBound;
            if(squared>=0x1p60)return false;
            if(spotNeeded&&std::abs(double(light.distance[0]))+std::abs(double(light.distance[1]))*std::sqrt(squared)+std::abs(double(light.distance[2]))*squared>=0x1p60)return false;
        }
        for(const auto& channel:u.channels)if(channel.enabled&&(channel.lightMask&(1u<<i))&&channel.attenuation!=Attenuation::none){
            // A positive denominator over the complete evaluated domain avoids
            // GPU reciprocal infinities without CPU vertex evaluation.
            const auto& d=light.distance;
            if(channel.attenuation==Attenuation::spot){
                if(d[0]<=0||d[2]<0||(d[1]<0&&(d[2]==0||double(d[0])-double(d[1])*d[1]/(4*double(d[2]))<=0)))return false;
            }else{
                if(d[0]<=0||double(d[0])+d[1]+d[2]<=0)return false;
                if(d[2]>0&&d[1]<0&&-double(d[1])<2*double(d[2])&&double(d[0])-double(d[1])*d[1]/(4*double(d[2]))<=0)return false;
            }
        }
    }
    const auto& fog=u.fog;
    if((fog.enabled||fog.rangeEnabled)&&!fog.provided)return false;
    if(!atMost(fog.function,FogFunction::reverseExpSquared))return false;
    for(float x:{fog.a,fog.b,fog.c,fog.rangeScale,fog.rangeBias})if(!bounded(x,0x1p20f))return false;
    for(float x:fog.range)if(!bounded(x,64)||x<0)return false;
    return true;
}
bool validateMaterial(const Material& m){
    if(!m.stageCount||m.stageCount>6||m.textureMask>7||!atMost(m.blend,Blend::custom)||!atMost(m.cull,Cull::back)||
       !atMost(m.depth,Compare::greaterEqual)||!atMost(m.alpha,Compare::greaterEqual)||m.bufferRgbMask>15||m.bufferAlphaMask>15||
       m.lookupBase>1||!atMost(m.fogCarrier,FogCarrier::texture2))return false;
    if(!m.rasterLookup&&m.lookupBase)return false;
    const bool textureFog=m.fogCarrier>=FogCarrier::texture0;
    const unsigned fogUnit=textureFog?unsigned(m.fogCarrier)-unsigned(FogCarrier::texture0):0;
    if(m.rasterLookup&&m.textureMask!=(((1u<<(m.lookupBase+2))-1)|(textureFog?1u<<fogUnit:0)))return false;
    if(textureFog&&(!(m.textureMask&(1u<<fogUnit))||
       (m.rasterLookup&&((3u<<m.lookupBase)&(1u<<fogUnit)))))return false;
    if((m.fogCarrier==FogCarrier::secondaryAlpha||m.fogCarrier==FogCarrier::secondaryRed)&&!m.rasterLookup)return false;
    if(m.fog&&m.fogCarrier!=FogCarrier::none)return false;
    const auto& blend=m.customBlend;
    if(!atMost(blend.color,BlendEquation::maximum)||!atMost(blend.alpha,BlendEquation::maximum))return false;
    for(auto factor:{blend.colorSource,blend.colorDestination,blend.alphaSource,blend.alphaDestination})if(!atMost(factor,BlendFactor::oneMinusDestinationAlpha))return false;
    for(unsigned i=0;i<m.stageCount;++i){const auto& s=m.stages[i];
        for(auto binding:s.constantBindings)if(binding>32)return false;
        if(!atMost(s.rgbFunction,Combine::addMultiply)||!atMost(s.alphaFunction,Combine::addMultiply)||s.alphaFunction==Combine::dot3||s.rgbScale>2||s.alphaScale>2)return false;
        for(unsigned c=0;c<3;++c){
            if(!atMost(s.rgb[c],Source::buffer)||!atMost(s.alpha[c],Source::buffer)||!atMost(s.rgbOperand[c],ColorOperand::oneMinusBlue)||!atMost(s.alphaOperand[c],AlphaOperand::oneMinusBlue))return false;
        }
        const auto arity=[](Combine f){return f==Combine::replace?1u:(f==Combine::interpolate||f==Combine::multiplyAdd||f==Combine::addMultiply?3u:2u);};
        for(unsigned a=0;a<2;++a)for(unsigned c=0;c<arity(a?s.alphaFunction:s.rgbFunction);++c){
            auto source=a?s.alpha[c]:s.rgb[c];
            if(i==0&&source==Source::previous)return false;
            if(source>=Source::texture0&&source<=Source::texture2&&!(m.textureMask&(1u<<(unsigned(source)-unsigned(Source::texture0)))))return false;
        }
    }
    if(m.fogCarrier!=FogCarrier::none){
        const auto& fog=m.stages[m.stageCount-1];
        const bool secondary=m.fogCarrier==FogCarrier::secondaryAlpha||m.fogCarrier==FogCarrier::secondaryRed;
        const bool red=m.fogCarrier==FogCarrier::primaryRed||m.fogCarrier==FogCarrier::secondaryRed;
        const auto carrier=textureFog?Source(unsigned(Source::texture0)+fogUnit):
            secondary?Source(unsigned(Source::texture0)+m.lookupBase+(red?0:1)):Source::primary;
        if(m.stageCount<2||fog.rgbFunction!=Combine::interpolate||fog.alphaFunction!=Combine::replace||
           fog.rgb!=std::array<Source,3>{Source::constant,Source::previous,carrier}||
           fog.rgbOperand!=std::array<ColorOperand,3>{ColorOperand::color,ColorOperand::color,red?ColorOperand::red:ColorOperand::alpha}||
           fog.alpha[0]!=Source::previous||fog.alphaOperand[0]!=AlphaOperand::alpha||fog.rgbScale||fog.alphaScale||
           fog.constantBindings!=std::array<std::uint8_t,4>{29,30,31,32})return false;
        const auto reads=[&](Source source,unsigned component){
            if(source!=carrier)return false;
            return red?component==0:component==3;
        };
        // No earlier stage may consume the component overwritten by vertex fog.
        for(unsigned i=0;i+1<m.stageCount;++i){const auto& stage=m.stages[i];
            const auto arity=[](Combine f){return f==Combine::replace?1u:(f==Combine::interpolate||f==Combine::multiplyAdd||f==Combine::addMultiply?3u:2u);};
            // A synthetic fog unit is reserved in its entirety, not just alpha.
            if(textureFog){
                for(unsigned j=0;j<arity(stage.rgbFunction);++j)if(stage.rgb[j]==carrier)return false;
                for(unsigned j=0;j<arity(stage.alphaFunction);++j)if(stage.alpha[j]==carrier)return false;
            }
            for(unsigned j=0;j<arity(stage.rgbFunction);++j){
                const unsigned operand=unsigned(stage.rgbOperand[j]);
                if(operand<2){if(reads(stage.rgb[j],0)||reads(stage.rgb[j],1)||reads(stage.rgb[j],2))return false;}
                else if(reads(stage.rgb[j],operand<4?3:(operand-4)/2))return false;
            }
            for(unsigned j=0;j<arity(stage.alphaFunction);++j){
                const unsigned operand=unsigned(stage.alphaOperand[j]);
                if(reads(stage.alpha[j],operand<2?3:(operand-2)/2))return false;
            }
        }
    }
    if(m.fog)for(float x:m.fogValues)if(!bounded(x,1)||x<0)return false;
    return true;
}
std::size_t textureBytes(const TextureInfo& info){
    auto pow2=[](unsigned n){return n>=8&&n<=1024&&!(n&(n-1));};
    if(!pow2(info.width)||!pow2(info.height)||!info.mipLevels||info.mipLevels>8||!atMost(info.format,TextureFormat::rgba5551)||unsigned(info.format)==4||!atMost(info.wrapS,Wrap::mirror)||!atMost(info.wrapT,Wrap::mirror))return 0;
    unsigned w=info.width,h=info.height;std::size_t total=0;
    for(unsigned i=0;i<info.mipLevels;++i){
        if(w<8||h<8)return 0;
        const std::size_t pixels=w*h;
        total+=info.format==TextureFormat::rgba8?pixels*4:(info.format==TextureFormat::rgb565||info.format==TextureFormat::rgba5551)?pixels*2:info.format==TextureFormat::etc1?pixels/2:pixels;
        w>>=1;h>>=1;
    }
    return total;
}
}
