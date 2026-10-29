#pragma once
#include "../runtime/gx_rigid_vertex.hpp"
#include <cassert>
#include <cstring>
// Independent host execution of the shader's uniform/attribute contract;
// compared to the existing original GX transform/lighting/fog CPU functions.
// Real PICA float24 execution remains a separate Azahar check.
inline dusk::native::gx::GpuVertex rigidReference(const dusk::native::gx::RigidVertexPlan& p,const dusk::native::gx::GpuVertex& v){
    using namespace dusk::native::gx;
    const auto& u=p.uniforms;GpuVertex out;auto dot=[](const auto& a,const auto& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    std::array<float,4> pos{v.position[0],v.position[1],v.position[2],1},view{};
    std::array<float,3> inputNormal{v.texcoord[1][0],v.texcoord[1][1],v.position[3]},normal{};
    const unsigned matrix=unsigned(v.texcoord[2][0]),normalMatrix=unsigned(v.texcoord[2][1]);
    for(unsigned r=0;r<3;++r){view[r]=dot(u[matrix+r],pos)+u[matrix+r][3];normal[r]=dot(u[normalMatrix+r],inputNormal);}view[3]=1;
    const auto length=dot(normal,normal);if(length>1e-10f)for(auto& n:normal)n/=std::sqrt(length);
    for(unsigned r=0;r<4;++r)out.position[r]=dot(u[6+r],view)+u[6+r][3];
    auto illumination=u[10];
    if(u[17][3])for(unsigned c=0;c<3;++c)illumination[c]=v.color[c];
    for(unsigned l=0;l<p.lights;++l){
        const unsigned base=19+4*l;
        std::array<float,3> d;for(unsigned c=0;c<3;++c)d[c]=u[base][c]-view[c];
        const float d2=dot(d,d);if(d2==0)continue;
        const float distance=std::sqrt(d2);for(auto& c:d)c/=distance;
        const float cosine=std::max(0.f,dot(u[base+2],d));
        const float ca=u[base][3]+u[base+2][3]*cosine+u[base+3][3]*cosine*cosine;
        const float da=u[base+3][0]+u[base+3][1]*distance+u[base+3][2]*d2;
        const float weight=std::max(0.f,ca/da)*std::max(0.f,dot(d,normal));
        for(unsigned c=0;c<3;++c)illumination[c]+=weight*u[base+1][c];
    }
    out.color=v.color;
    if(u[17][3]&&!u[14][3])for(unsigned c=0;c<3;++c)out.color[c]=u[93][c];
    for(unsigned c=0;c<3;++c)out.color[c]*=std::clamp(illumination[c],0.f,1.f);
    out.texcoord[0]=v.texcoord[0];
    if(p.projectedTexture){
        const unsigned base=p.projectionBase();std::array<float,3> tex{};
        for(unsigned r=0;r<3;++r)tex[r]=dot(u[base+r],pos)+u[base+r][3];
        out.texcoord[1]=out.texcoord[0];
        out.texcoord[0]={tex[0]*u[base+3][0],tex[2]-tex[1]*u[base+3][1]};out.textureQ=tex[2];
    }
    if(u[12][0]){
        const float depth=out.position[3]>0?std::clamp(1+out.position[2]/out.position[3],0.f,1.f):0;
        float f=u[11][0]/(u[11][1]-depth);
        if(u[12][1]&&out.position[3]>0){
            const float offset=u[13][0]*out.position[1]/out.position[3]+u[13][1];
            const float index=std::clamp(9-std::abs(offset)*9,0.f,9.f);const unsigned low=unsigned(index);
            const auto& range=u[14+low/2];const unsigned part=low%2;
            const float k=std::max(range[part]+(range[part+1]-range[part])*(index-low),1e-6f);
            f*=std::sqrt(1+offset*offset/(k*k));
        }
        f=std::clamp(f-u[11][2],0.f,1.f);
        if(u[12][2])out.color[3]=f;else out.texcoord[p.projectedTexture?2:1]={(f*255+.5f)/256,.5f};
    }
    return out;
}


// Interpret the compact attribute layout as the PICA shader does, using host
// floats for the CPU differential. Actual float24 rounding is checked in Azahar.
inline dusk::native::gx::GpuVertex expandCompactRigid(const dusk::native::gx::RigidVertexPlan& p,
    std::span<const std::byte> bytes,unsigned index){
    using namespace dusk::native::gx;
    CompactRigidVertex v;std::memcpy(&v,bytes.data()+index*sizeof(v),sizeof(v));
    GpuVertex out;out.position={v.position[0],v.position[1],v.position[2],v.normal[2]*p.uniforms[11][3]};
    out.color=p.uniforms[93];
    if(p.uniforms[14][3]>0||p.uniforms[17][3]>0)for(unsigned c=0;c<3;++c)out.color[c]=v.color[c]*p.uniforms[16][3];
    if(p.uniforms[15][3]>=0)out.color[3]=v.color[3]*p.uniforms[16][3]*p.uniforms[15][3];
    out.texcoord[0]={v.texcoord[0]*p.uniforms[13][2],1-v.texcoord[1]*p.uniforms[13][3]};
    out.texcoord[1]={v.normal[0]*p.uniforms[11][3],v.normal[1]*p.uniforms[11][3]};
    out.texcoord[2]={float(v.matrix[0]),float(v.matrix[1])};
    return out;
}
