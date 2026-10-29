#pragma once
#include "native_camera_fixture.hpp"

namespace {
void nativeCameraContracts(){
    unsigned samples=0;
    // Project points through the authored matrix, then map through a continuous
    // logical viewport independently of the matrix-composition implementation.
    for(unsigned kind=0;kind<3;++kind){
        nativeCameraOracle::Mtx44 source{};
        if(kind==0)nativeCameraOracle::C_MTXPerspective(source,65,608.0f/448,1,1000);
        if(kind==1)nativeCameraOracle::C_MTXOrtho(source,5,-3,-4,7,1,1000);
        if(kind==2)nativeCameraOracle::C_MTXFrustum(source,2,-1,-2,3,1,1000);
        Matrix projection;for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)projection[r][c]=source[r][c];
        for(const CameraRegion region:{CameraRegion{608,448,0,0,608,448,0,1},
                CameraRegion{608,448,152,112,304,224,.25f,.75f},
                CameraRegion{608,448,-20,-30,400,300,1,0}}){
            CameraProjection camera;assert(prepareCamera(projection,region,240,400,camera));
            for(float x:{-2.0f,0.0f,3.0f})for(float y:{-1.0f,0.0f,2.0f})for(float z:{-1.0f,-20.0f,-999.0f}){
                const double v[]{x,y,z,1};double clip[4]{},native[4]{};
                for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c){clip[r]+=projection[r][c]*v[c];native[r]+=camera.rows[r][c]*v[c];}
                const double logicalX=region.x+(clip[0]/clip[3]+1)*region.width/2;
                const double logicalY=region.y+(1-clip[1]/clip[3])*region.height/2;
                const double expected[]{1-2*logicalY/region.canvasHeight,1-2*logicalX/region.canvasWidth,
                    (region.farDepth-region.nearDepth)*clip[2]/clip[3]+region.farDepth-1,1};
                for(unsigned r=0;r<4;++r)assert(std::abs(native[r]/native[3]-expected[r])<2e-5*(1+std::abs(expected[r])));
                ++samples;
            }
        }
    }
    Matrix projection{};for(unsigned i=0;i<4;++i)projection[i][i]=1;
    CameraRegion region{608,448,152,112,304,224,0,1};CameraProjection camera;
    assert(prepareCamera(projection,region,240,400,camera));
    assert(camera.left==60&&camera.right==180&&camera.top==100&&camera.bottom==300);
    const auto reject=[&](const Matrix& p,const CameraRegion& r,unsigned w=240,unsigned h=400){
        camera.left=999;assert(!prepareCamera(p,r,w,h,camera)&&camera.left==0&&camera.rows==Matrix{});
    };
    reject(projection,region,0);reject(projection,region,1025);reject(Matrix{},region);
    for(unsigned field=0;field<8;++field)for(unsigned bits:{1u,0x7f800000u,0x7fc00000u}){
        auto bad=region;float* fields[]{&bad.canvasWidth,&bad.canvasHeight,&bad.x,&bad.y,&bad.width,&bad.height,&bad.nearDepth,&bad.farDepth};
        *fields[field]=std::bit_cast<float>(bits);reject(projection,bad);
    }
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c){auto bad=projection;bad[r][c]=std::numeric_limits<float>::infinity();reject(bad,region);}
    for(unsigned field=0;field<4;++field){auto bad=region;if(field==0)bad.width=0;if(field==1)bad.height=-1;if(field==2)bad.nearDepth=-1;if(field==3)bad.farDepth=2;reject(projection,bad);}
    auto outside=region;outside.x=609;reject(projection,outside);
    std::printf("PASS native camera: %u independent point/viewport/depth samples, exact partial scissor and invalid-input reset controls (host arithmetic only)\n",samples);
}

void nativeCameraFrame(C3D_RenderTarget& surface){
    Matrix projection{};for(unsigned i=0;i<4;++i)projection[i][i]=1;
    const CameraRegion region{608,448,152,112,304,224,0,1};DrawUniforms out;
    assert(!game::applyCamera(out));
    assert(game::publishCamera(&surface,projection,region)&&game::target(&surface)&&game::applyCamera(out));
    assert(nativeMock::scissor[1]==60&&nativeMock::scissor[2]==100&&nativeMock::scissor[3]==180&&nativeMock::scissor[4]==300);
    assert(out.viewProjection[0][1]==.5f&&out.viewProjection[1][0]==-.5f);
    assert(!game::publishCamera(nullptr,projection,region)&&!game::applyCamera(out));
    C3D_RenderTarget offscreen{{reinterpret_cast<void*>(3),64,64,reinterpret_cast<void*>(4)}};
    assert(game::publishCamera(&surface,projection,region)&&game::target(&offscreen)&&!game::applyCamera(out));
    assert(!game::publishCamera(&offscreen,projection,region));
    // A fresh main publication following an offscreen pass survives main bind.
    assert(game::publishCamera(&surface,projection,region)&&game::target(&surface)&&game::applyCamera(out));
    namespace f=nativeCameraGameFixture;f::active=&surface;f::Mtx44 authored{};
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)authored[r][c]=projection[r][c];
    f::view_port_class viewport{0,0,608,448,0,1};
    f::publishNativeSceneCamera(authored,&viewport);assert(game::target(&surface)&&game::applyCamera(out));
    assert(out.viewProjection[0][1]==1&&out.viewProjection[1][0]==-1);
    f::publishNativeSceneCamera(authored,nullptr);assert(!game::applyCamera(out));
    f::publishNativeSceneCamera(authored,&viewport);assert(game::target(&surface));
    // Exercise the extracted actual game producer, not the packet test's proxy.
    namespace s=nativeSourceFixture;s::Data data;s::Buffer buffer;
    float positions[3][3][4]{},normals[3][3][3]{};
    for(unsigned m=0;m<3;++m)for(unsigned r=0;r<3;++r){positions[m][r][r]=1;normals[m][r][r]=1;}
    buffer.p[0]=positions;buffer.n[0]=normals;s::J3DModel model{&data,&buffer};UniformBinding binding{23,1,3,{1,2,3}};
    assert(s::nativeGameParameters(&model,binding,out,nullptr)&&out.palette.count==3);
    game::invalidateCamera();assert(!s::nativeGameParameters(&model,binding,out,nullptr));
    f::publishNativeSceneCamera(authored,&viewport);assert(game::target(&surface));
}
}
