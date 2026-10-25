#include "gx_geometry_pack.hpp"
#include <algorithm>
#include <cmath>
namespace dusk::native::gx {
namespace {
bool overlap(std::span<const std::byte> a,std::span<const std::byte> b){
    if(a.empty()||b.empty())return false;
    const auto x=reinterpret_cast<std::uintptr_t>(a.data()),y=reinterpret_cast<std::uintptr_t>(b.data());
    return x<=y?y-x<a.size():x-y<b.size();
}
}
bool packGpuGeometry(const PreparedGeometry& input,std::span<const std::array<float,4>> colors,
                     const PicaMaterialPlan& material,const std::array<std::array<float,2>,3>& scales,
                     const GpuGeometryBatch& output,GeometryPackError* error){
    if(error)*error=GeometryPackError::none;
    auto fail=[&](GeometryPackError e){if(error)*error=e;return false;};
    const auto count=input.vertices.size();
    if(count<3||count>65535||input.indices.empty()||input.indices.size()%3||
       input.indices.size()>196605||colors.size()!=count||output.vertices.size()!=count||
       output.indices.size()!=input.indices.size())return fail(GeometryPackError::capacity);
    const auto vertexBytes=std::as_bytes(output.vertices),indexBytes=std::as_bytes(output.indices);
    if(overlap(vertexBytes,indexBytes))return fail(GeometryPackError::alias);
    auto aliases=[&](auto bytes){return overlap(vertexBytes,bytes)||overlap(indexBytes,bytes);};
    if(aliases(std::as_bytes(input.vertices))||aliases(std::as_bytes(input.indices))||aliases(std::as_bytes(colors))||
       aliases(std::as_bytes(std::span(&material,1)))||aliases(std::as_bytes(std::span(&scales,1))))return fail(GeometryPackError::alias);
    if(material.textureCount>3)return fail(GeometryPackError::binding);
    if(!validGeometryQBinding(output,material))return fail(GeometryPackError::projective);
    for(unsigned unit=0;unit<material.textureCount;++unit){
        const auto coord=material.textures[unit].coordinate;
        if(coord>=8||!(input.coordinateMask&(1u<<coord)))return fail(GeometryPackError::binding);
        const bool projected=input.projectiveMask&(1u<<coord);
        if(projected!=(unit==0&&material.projectiveTexture0))return fail(GeometryPackError::projective);
        for(float scale:scales[unit])if(!std::isfinite(scale)||scale<=0||scale>1)return fail(GeometryPackError::binding);
    }
    for(auto index:input.indices)if(index>=count)return fail(GeometryPackError::index);
    // Validate before touching the reservation. This also avoids partial output
    // if an unsupported UV/lighting value occurs near the end of a draw.
    for(std::size_t i=0;i<count;++i){
        for(float x:input.vertices[i].clip)if(!std::isfinite(x))return fail(GeometryPackError::position);
        for(float x:colors[i])if(!std::isfinite(x)||x<0||x>1)return fail(GeometryPackError::color);
        for(unsigned unit=0;unit<material.textureCount;++unit){
            const auto& uv=input.vertices[i].texcoord[material.textures[unit].coordinate];
            const float q=unit==0&&material.projectiveTexture0?uv[2]:1;
            if(!std::isfinite(q)||q==0||(q<0&&output.qPolicy!=GeometryQPolicy::directTexture0Diagnostic))return fail(GeometryPackError::projective);
            for(unsigned c=0;c<2;++c)if(!std::isfinite(uv[c])||
                (q>0?(uv[c]<0||uv[c]>q):(uv[c]<q||uv[c]>0)))return fail(GeometryPackError::coordinate);
        }
    }
    for(std::size_t i=0;i<count;++i){
        auto& target=output.vertices[i];target={};target.position=input.vertices[i].clip;target.color=colors[i];
        for(unsigned unit=0;unit<material.textureCount;++unit){
            const auto& uv=input.vertices[i].texcoord[material.textures[unit].coordinate];
            const float q=unit==0&&material.projectiveTexture0?uv[2]:1;
            if(unit==0)target.textureQ=q;
            target.texcoord[unit]={uv[0]*scales[unit][0],q-uv[1]*scales[unit][1]};
        }
    }
    std::copy(input.indices.begin(),input.indices.end(),output.indices.begin());
    return true;
}
}
