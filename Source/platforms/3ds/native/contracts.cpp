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
Palette Palette::identity(){Palette p;for(unsigned i=0;i<paletteSize;++i)for(unsigned r=0;r<3;++r)p.position[3*i+r][r]=p.normal[3*i+r][r]=1;return p;}
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
    if(u.samplerMask>7)return false;
    for(const auto& sampler:u.samplers)if(!validateSampler(sampler,8))return false;
    if(!u.palette.count||u.palette.count>paletteSize||requiredBone>=u.palette.count)return false;
    for(unsigned i=0;i<3*u.palette.count;++i){
        for(unsigned c=0;c<4;++c)if(!bounded(u.palette.position[i][c],c==3?0x1p20f:32768.f))return false;
        for(float x:u.palette.normal[i])if(!bounded(x,256))return false;
    }
    for(const auto& row:u.viewProjection)for(unsigned c=0;c<4;++c)if(!bounded(row[c],c==3?0x1p20f:32768.f))return false;
    if(u.viewProjection[3]==Row{})return false; // Every vertex would have W=0.
    for(const auto& transform:u.uvTransforms){
        if(transform.source>2)return false;
        for(const auto& row:transform.rows)for(float x:row)if(!bounded(x,32768))return false;
    }
    if(!atMost(u.projective.source,PositionSource::posed))return false;
    for(const auto& row:u.projective.rows)for(unsigned c=0;c<4;++c)if(!bounded(row[c],c==3?0x1p20f:32768.f))return false;
    if(u.projective.enabled&&u.projective.rows[2]==Row{})return false;
    for(float x:u.ambient)if(!bounded(x,1)||x<0)return false;
    for(const auto& light:u.lights){
        float n=0;for(float x:light.toward){if(!bounded(x,1))return false;n+=x*x;}
        if(n!=0&&std::abs(n-1)>0.001f)return false;
        for(float x:light.color)if(!bounded(x,1)||x<0)return false;
    }
    return true;
}
bool validateMaterial(const Material& m){
    if(!m.stageCount||m.stageCount>6||m.textureMask>7||!atMost(m.blend,Blend::custom)||!atMost(m.cull,Cull::back)||
       !atMost(m.depth,Compare::greaterEqual)||!atMost(m.alpha,Compare::greaterEqual)||m.bufferRgbMask>15||m.bufferAlphaMask>15)return false;
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
    if(m.fog)for(float x:m.fogValues)if(!bounded(x,1)||x<0)return false;
    return true;
}
std::size_t textureBytes(const TextureInfo& info){
    auto pow2=[](unsigned n){return n>=8&&n<=1024&&!(n&(n-1));};
    if(!pow2(info.width)||!pow2(info.height)||!info.mipLevels||info.mipLevels>8||!atMost(info.format,TextureFormat::etc1a4)||!atMost(info.wrapS,Wrap::mirror)||!atMost(info.wrapT,Wrap::mirror))return 0;
    unsigned w=info.width,h=info.height;std::size_t total=0;
    for(unsigned i=0;i<info.mipLevels;++i){
        if(w<8||h<8)return 0;
        const std::size_t pixels=w*h;
        total+=info.format==TextureFormat::rgba8?pixels*4:info.format==TextureFormat::rgb565?pixels*2:info.format==TextureFormat::etc1?pixels/2:pixels;
        w>>=1;h>>=1;
    }
    return total;
}
}
