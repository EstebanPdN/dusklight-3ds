#include "../runtime/gx_gpu_geometry.hpp"
#include "../runtime/gx_geometry_pack.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <limits>
using namespace dusk::native::gx;

static void signedQPackCases(){
    std::array<TransformedVertex,3> v{};std::array<std::array<float,4>,3> colors{};
    std::array<GpuVertex,3> gpu{};std::array<std::uint16_t,3> ix{};const std::uint16_t indices[]{0,1,2};
    const float q[]{-2,4,-8};const float uq[]{-1,1,-2},vq[]{-.5f,2,-6};
    for(unsigned i=0;i<3;++i){v[i].clip={float(i),0,-1,2};v[i].texcoord[0]={uq[i],vq[i],q[i]};colors[i]={.25f,.5f,.75f,1};}
    PreparedGeometry input{v,indices,1,1};PicaMaterialPlan m;m.textureCount=1;m.projectiveTexture0=true;
    m.textures[0]={0,0};const std::array<std::array<float,2>,3> scale{{{.5f,.25f},{1,1},{1,1}}};
    GpuGeometryBatch b{gpu,ix};GeometryPackError error;
    const auto unchanged=gpu;assert(!packGpuGeometry(input,colors,m,scale,b,&error)&&error==GeometryPackError::projective);
    assert(!std::memcmp(gpu.data(),unchanged.data(),sizeof(gpu)));
    b.qPolicy=GeometryQPolicy::directTexture0Diagnostic;
    assert(packGpuGeometry(input,colors,m,scale,b)&&validateGpuGeometry(b));
    const float expectedU[]{-.5f,.5f,-1},expectedV[]{-1.875f,3.5f,-6.5f};
    for(unsigned i=0;i<3;++i){GpuVertex expected{};expected.position=v[i].clip;expected.color=colors[i];
        expected.textureQ=q[i];expected.texcoord[0]={expectedU[i],expectedV[i]};assert(!std::memcmp(&gpu[i],&expected,sizeof(expected)));}
    assert(!std::memcmp(ix.data(),indices,sizeof(indices)));
    const auto packed=gpu;auto reject=[&](){assert(!packGpuGeometry(input,colors,m,scale,b));assert(!std::memcmp(gpu.data(),packed.data(),sizeof(gpu)));};
    for(float bad:{0.f,-0.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){v[2].texcoord[0][2]=bad;reject();}
    v[2].texcoord[0][2]=-8;v[2].texcoord[0][0]=-9;reject(); // Homogeneous padded-clamp bounds retained.
    v[2].texcoord[0][0]=-2;v[2].clip[3]=std::numeric_limits<float>::infinity();reject();v[2].clip[3]=2;
    m.projectiveTexture0=false;reject();m.projectiveTexture0=true;
    m.textureCount=2;m.textures[1]={1,1};input.projectiveMask=3;input.coordinateMask=3;reject();m.textureCount=1;input.projectiveMask=input.coordinateMask=1;
    auto strict=b;strict.qPolicy=GeometryQPolicy::positiveOnly;assert(!validateGpuGeometry(strict)&&!validateCompactRigidGeometry(b));
    for(float bad:{0.f,-0.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}){gpu[2].textureQ=bad;assert(!validateGpuGeometry(b));}gpu=packed;
    gpu[2].texcoord[0][0]=std::numeric_limits<float>::infinity();assert(!validateGpuGeometry(b));gpu=packed;
    b.qPolicy=static_cast<GeometryQPolicy>(77);assert(!validateGpuGeometry(b)&&!packGpuGeometry(input,colors,m,scale,b));
    std::puts("PASS signed-q geometry/packer: explicit token, exact mixed vertex/index bytes, signed homogeneous bounds, strict/unavailable policy, zero/nonfinite rejection before writes");
}

int main(){
    signedQPackCases();
    alignas(16) std::array<std::byte,512> bytes{};
    GpuGeometryArena arena;GpuGeometryBatch a,b,invalid;
    assert(!arena.begin()&&!arena.end());
    assert(!arena.attach(std::span(bytes).subspan(1)));
    assert(arena.attach(bytes)&&!arena.reserve(4,6,a)&&arena.begin());
    assert(!arena.begin()&&!arena.attach(bytes));
    for(auto pair:{std::array<std::size_t,2>{2,3},{65536,3},{4,2},{4,0},{4,196608},
                   {std::numeric_limits<std::size_t>::max(),3}})
        assert(!arena.reserve(pair[0],pair[1],a)&&a.vertices.empty()&&arena.used()==0);
    assert(arena.reserve(4,6,a)&&arena.owns(a)&&!arena.end());
    assert(reinterpret_cast<std::uintptr_t>(a.vertices.data())%16==0);
    assert(reinterpret_cast<std::uintptr_t>(a.indices.data())%16==0);
    assert(!arena.reserve(4,6,b));
    for(unsigned i=0;i<4;++i){a.vertices[i]={};a.vertices[i].position={float(i),1,2,3};a.vertices[i].color={.1,.2,.3,.4};}
    const std::uint16_t indices[]{0,1,2,0,2,3};std::memcpy(a.indices.data(),indices,sizeof(indices));
    assert(validateGpuGeometry(a));a.indices[5]=4;assert(!validateGpuGeometry(a));a.indices[5]=3;
    a.vertices[0].position[0]=std::numeric_limits<float>::infinity();assert(!validateGpuGeometry(a));a.vertices[0].position[0]=0;
    a.vertices[0].color[0]=-1;assert(!validateGpuGeometry(a));a.vertices[0].color[0]=0;
    a.vertices[0].texcoord[2][1]=std::numeric_limits<float>::quiet_NaN();assert(!validateGpuGeometry(a));a.vertices[0].texcoord[2][1]=0;
    invalid=a;invalid.indices=invalid.indices.first(3);assert(!arena.commit(invalid)&&!arena.cancel(invalid));
    const auto saved=bytes;
    assert(arena.commit(a)&&!arena.owns(a)&&!arena.cancel(a)&&!arena.commit(a)&&arena.used()==252);
    assert(arena.reserve(4,6,b)&&b.ticket!=a.ticket);
    for(auto& v:b.vertices)v={};for(auto& i:b.indices)i=0;
    assert(std::memcmp(bytes.data(),saved.data(),252)==0);
    assert(arena.cancel(b)&&arena.used()==252&&!arena.owns(b));
    assert(arena.reserve(4,6,b)&&arena.commit(b)&&arena.used()==508);
    assert(!arena.reserve(4,6,invalid)&&arena.used()==508);
    assert(arena.end()&&!arena.end()&&!arena.owns(b));
    // begin's caller contract is a completed GPU fence, not mere VBlank.
    assert(arena.begin()&&arena.used()==0&&!arena.owns(a));
    assert(arena.reserve(4,6,b)&&b.vertices.data()==a.vertices.data()&&b.ticket!=a.ticket);
    assert(!arena.commit(a)&&arena.cancel(b)&&arena.end());
    std::array<TransformedVertex,4> transformed{};
    std::array<std::array<float,4>,4> colors{};
    for(unsigned i=0;i<4;++i){
        transformed[i].clip={float(i),float(i+1),-.5f,float(i+2)};
        colors[i]={.25f,.5f,.75f,1};
        for(unsigned coord=0;coord<8;++coord)transformed[i].texcoord[coord]={coord*.125f,i*.25f,1};
    }
    PreparedGeometry prepared{transformed,indices,0xff,0};
    PicaMaterialPlan material;material.textureCount=3;
    material.textures[0]={4,7};material.textures[1]={2,1};material.textures[2]={5,3};
    const std::array<std::array<float,2>,3> scales{{{.5f,1},{1,.5f},{.75f,.25f}}};
    assert(arena.begin()&&arena.reserve(4,6,b));
    GeometryPackError error;
    assert(packGpuGeometry(prepared,colors,material,scales,b,&error)&&validateGpuGeometry(b));
    for(unsigned i=0;i<4;++i){
        assert(b.vertices[i].position==transformed[i].clip&&b.vertices[i].color==colors[i]);
        for(unsigned unit=0;unit<3;++unit){
            auto coord=material.textures[unit].coordinate;
            assert(b.vertices[i].texcoord[unit][0]==coord*.125f*scales[unit][0]);
            assert(b.vertices[i].texcoord[unit][1]==1-i*.25f*scales[unit][1]);
        }
    }
    assert(std::memcmp(b.indices.data(),indices,sizeof(indices))==0);
    const auto packed=bytes;
    prepared.projectiveMask=1u<<7;
    assert(!packGpuGeometry(prepared,colors,material,scales,b,&error)&&error==GeometryPackError::projective&&bytes==packed);
    prepared.projectiveMask=1; // An unused projective coordinate does not block the draw.
    assert(packGpuGeometry(prepared,colors,material,scales,b));
    transformed[3].texcoord[7][0]=2;
    assert(!packGpuGeometry(prepared,colors,material,scales,b,&error)&&error==GeometryPackError::coordinate&&bytes==packed);
    transformed[3].texcoord[7][0]=.875f;colors[3][0]=-1;
    assert(!packGpuGeometry(prepared,colors,material,scales,b,&error)&&error==GeometryPackError::color&&bytes==packed);
    colors[3][0]=.25f;prepared.coordinateMask=0;
    assert(!packGpuGeometry(prepared,colors,material,scales,b,&error)&&error==GeometryPackError::binding&&bytes==packed);
    prepared.coordinateMask=0xff;auto invalidScale=scales;invalidScale[1][0]=0;
    assert(!packGpuGeometry(prepared,colors,material,invalidScale,b,&error)&&error==GeometryPackError::binding&&bytes==packed);
    const std::uint16_t badIndices[]{0,1,2,0,2,4};prepared.indices=badIndices;
    assert(!packGpuGeometry(prepared,colors,material,scales,b,&error)&&error==GeometryPackError::index&&bytes==packed);
    // Preserve distinct q at every vertex. At a fragment with arbitrary
    // perspective-correct weights, homogeneous t inversion commutes with
    // interpolation/division; dividing t/q at each vertex would not.
    prepared.indices=indices;prepared.projectiveMask=1u<<7;material.projectiveTexture0=true;
    for(unsigned i=0;i<4;++i) {
        const float q=2+i*3;
        transformed[i].texcoord[7]={.2f*q,.1f*(i+1)*q,q};
    }
    assert(packGpuGeometry(prepared,colors,material,scales,b)&&validateGpuGeometry(b));
    double s=0,t=0,q=0,packedS=0,packedT=0,packedQ=0;
    const double weights[]{.11,.23,.37,.29};
    for(unsigned i=0;i<4;++i) {
        const auto& uv=transformed[i].texcoord[7];const auto& gpu=b.vertices[i];
        assert(gpu.textureQ==uv[2]);
        s+=weights[i]*uv[0];t+=weights[i]*uv[1];q+=weights[i]*uv[2];
        packedS+=weights[i]*gpu.texcoord[0][0];packedT+=weights[i]*gpu.texcoord[0][1];packedQ+=weights[i]*gpu.textureQ;
    }
    assert(std::abs(packedS/packedQ-s/q*scales[0][0])<1e-6);
    assert(std::abs(packedT/packedQ-(1-t/q*scales[0][1]))<1e-6);
    const auto projectedBytes=bytes;
    transformed[3].texcoord[7][2]=0;
    assert(!packGpuGeometry(prepared,colors,material,scales,b,&error)&&error==GeometryPackError::projective&&bytes==projectedBytes);
    transformed[3].texcoord[7][2]=11;prepared.projectiveMask|=1u<<1;
    assert(!packGpuGeometry(prepared,colors,material,scales,b,&error)&&error==GeometryPackError::projective&&bytes==projectedBytes);
    assert(arena.cancel(b)&&arena.end());
    std::puts("PASS bounded indexed arena: alignment, capacity, cancellation, immutable submitted bytes, stale tickets, frame lifecycle, index/attribute validation; vertex stride 60 B");
    std::puts("PASS prepared GPU packing: clip w/color, independent UVs, homogeneous projective s/t/q and fragment interpolation, unsupported multiple/invalid divisors rejected before writes");
}
