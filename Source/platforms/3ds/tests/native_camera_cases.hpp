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
    binding.sourceStageCount=1;binding.sourceTexGenCount=1;
    assert(s::nativeGameParameters(&model,binding,out,nullptr)&&out.palette.count==3);
    game::invalidateCamera();assert(!s::nativeGameParameters(&model,binding,out,nullptr));
    f::publishNativeSceneCamera(authored,&viewport);assert(game::target(&surface));
}
void nativeMirrorFrame(C3D_RenderTarget& surface,MeshHandle mesh,MaterialHandle material){
    namespace camera=nativeMirrorGameFixture;
    namespace source=nativeSourceFixture;
    camera::active=&surface;
    camera::Mtx44 projection{};nativeCameraOracle::C_MTXPerspective(projection,65,608.f/448,1,1000);
    float saved[]{0,projection[0][0],projection[0][2],projection[1][1],projection[1][2],projection[2][2],projection[2][3]};
    float viewport[]{0,0,608,448,.25f,.75f};
    const std::uint32_t original[]{20,30,500,360},mirror[]{100,50,250,200};
    DrawUniforms main;
    assert(camera::publishNativeMirrorCamera(projection,saved,viewport,original));
    assert(game::target(&surface)&&game::applyCamera(main));
    assert(nativeMock::scissor[1]==31&&nativeMock::scissor[2]==57&&nativeMock::scissor[3]==224&&nativeMock::scissor[4]==387);
    // GXSetScissor invalidates the publication; the actor republishes only
    // after its exact rounded mirror rectangle has been installed.
    game::invalidateCamera();assert(!game::applyCamera(main));
    assert(camera::publishNativeMirrorCamera(projection,saved,viewport,mirror)&&game::target(&surface));
    DrawUniforms reflected;assert(game::applyCamera(reflected)&&reflected.viewProjection==main.viewProjection);
    assert(nativeMock::scissor[1]==106&&nativeMock::scissor[2]==169&&nativeMock::scissor[3]==214&&nativeMock::scissor[4]==335);
    // A newly published main camera may follow an offscreen native bind.
    // The shape-packet boundary performs the subsequent main-target bind.
    C3D_RenderTarget offscreen{{reinterpret_cast<void*>(3),64,64,reinterpret_cast<void*>(4)}};
    assert(game::target(&offscreen));
    assert(camera::publishNativeMirrorCamera(projection,saved,viewport,mirror));
    assert(game::beginMirrorDraw(&surface,0,1,{17,34,51,68})&&!game::applyCamera(reflected));
    assert(game::target(&surface)&&game::applyCamera(reflected)&&game::endMirrorDraw());

    // Exercise the actual actor's native modelDraw branch: its head packet
    // receives the reflected base, source RGBA and front-cull override, while
    // hidden shapes, depth rejection and packet ownership remain unchanged.
    namespace actor=nativeMirrorModelFixture;
    actor::J3DModelData actorData{2};actor::J3DShape actorShapes[2]{{0,false,0},{1,true,0}};
    actor::J3DShapePacket actorPackets[2];actor::J3DMatPacket actorMaterials[2];
    actor::J3DModel actorModel{&actorData,actorMaterials,actorPackets};
    source::MTXIdentity(actorModel.base);source::MTXIdentity(actor::j3dSys.view);
    for(unsigned i=0;i<2;++i){actorPackets[i]={&actorModel,&actorShapes[i],reinterpret_cast<actor::Mtx*>(actor::j3dSys.getViewMtx())};actorMaterials[i].packet=&actorPackets[i];}
    actor::environment.bg_amb_col[0]={17,34,51,68};
    actor::Mtx actorBase{};source::MTXIdentity(actorBase);
    static actor::Mtx* expectedBase;expectedBase=&actorBase;
    static unsigned actorDraws;actorDraws=0;
    actor::onDraw=+[](actor::J3DShapePacket* packet){
        assert(packet->base==expectedBase);++actorDraws;
        UniformBinding current;current.material=1;current.sourceShape=0;DrawUniforms u;
        assert(game::applyMirrorDraw(packet->model,current,u)&&u.cullOverride==CullOverride::back);
        assert((u.ambientColors[0]==Row{17/255.f,34/255.f,51/255.f,68/255.f}));
    };
    actor::dMirror_packet_c actorPacket;actorPacket.mViewScale.y=1;
    actorBase[2][3]=1;actorPacket.modelDraw(&actorModel,actorBase);assert(actorDraws==0);
    actorBase[2][3]=0;actorPacket.modelDraw(&actorModel,actorBase);
    assert(actorDraws==1&&actorShapes[0].resets==1&&actorShapes[1].resets==1);
    assert(actorPackets[0].base==reinterpret_cast<actor::Mtx*>(actor::j3dSys.getViewMtx()));
    actor::J3DShapePacket foreign{&actorModel,&actorShapes[0],&actorBase};actorMaterials[0].packet=&foreign;
    bool rejected=false;try{actorPacket.modelDraw(&actorModel,actorBase);}catch(const std::runtime_error&){rejected=true;}
    assert(rejected&&actorDraws==1);actorMaterials[0].packet=&actorPackets[0];actor::onDraw=nullptr;

    source::Data data;data.flags=0x10;data.count=data.jointCount=1;
    auto& shape=data.shapes[0];shape.groupCount=1;shape.groups[0].type=0x534d4356;
    source::Buffer buffer;source::Mtx joint[1]{},base{};
    source::MTXIdentity(joint[0]);source::MTXIdentity(base);
    joint[0][0][3]=3;joint[0][1][3]=5;base[0][0]=-1;base[0][3]=100;
    std::uint8_t scale[]{1};buffer.mpUserAnmMtx=joint;buffer.scaleFlags=scale;
    source::J3DShapePacket packets[8]{};packets[0].base=&base;
    source::J3DModel model{&data,&buffer};model.mShapePacket=packets;
    data.material.color.channels[0].enabled=1;data.material.color.channels[1].enabled=1;
    data.material.color.ambient[0]={6,7,8,9};
    UniformBinding binding{23,1,1,{1}};binding.sourceStageCount=1;binding.sourceTexGenCount=1;binding.channelReadMask=5;
    const std::array<std::uint8_t,4> ambient{17,34,51,68};
    assert(game::beginMirrorDraw(&model,0,23,ambient));
    assert(!game::beginMirrorDraw(&model,0,23,ambient));
    auto wrong=binding;wrong.material=22;assert(!game::applyMirrorDraw(&model,wrong,reflected));
    wrong=binding;wrong.sourceShape=1;assert(!game::applyMirrorDraw(&model,wrong,reflected));
    int other{};assert(!game::applyMirrorDraw(&other,binding,reflected));
    reflected={};assert(source::nativeGameParameters(&model,binding,reflected,nullptr));
    assert(reflected.palette.count==1&&reflected.palette.position[0][0]==-1&&reflected.palette.position[0][3]==97);
    assert(reflected.palette.position[1][3]==5&&reflected.palette.normal[0][0]==-1);
    assert((reflected.ambientColors[0]==Row{17/255.f,34/255.f,51/255.f,68/255.f}));
    assert(reflected.cullOverride==CullOverride::back);
    assert(game::renderer().draw(mesh,material,{},reflected)&&nativeMock::cullMode==GPU_CULL_BACK_CCW);
    assert(nativeMock::uniforms[62]==reflected.ambientColors[0]);
    auto invalid=reflected;invalid.cullOverride=CullOverride(2);
    const auto draws=nativeMock::draws;assert(!game::renderer().draw(mesh,material,{},invalid)&&nativeMock::draws==draws);
    assert(game::endMirrorDraw()&&!game::endMirrorDraw());

    // SMTX ignores the overridden packet base in the original drawFast.
    data.flags=0;shape.groups[0].type=0x534d5458;
    float positions[1][3][4]{},normals[1][3][3]{};
    for(unsigned r=0;r<3;++r){positions[0][r][r]=1;normals[0][r][r]=1;}
    positions[0][0][3]=12;buffer.p[0]=positions;buffer.n[0]=normals;
    DrawUniforms ordinary{};assert(source::nativeGameParameters(&model,binding,ordinary,nullptr));
    assert(ordinary.palette.position[0][0]==1&&ordinary.palette.position[0][3]==12);
    assert((ordinary.ambientColors[0]==Row{6/255.f,7/255.f,8/255.f,9/255.f}));
    assert(ordinary.cullOverride==CullOverride::material);
    assert(game::renderer().draw(mesh,material,{},ordinary)&&nativeMock::cullMode==GPU_CULL_FRONT_CCW);

    // mirrorZdraw restores GX scissor/projection; native publication follows it.
    game::invalidateCamera();
    assert(camera::publishNativeMirrorCamera(projection,saved,viewport,original)&&game::target(&surface));
    DrawUniforms restored;assert(game::applyCamera(restored)&&restored.viewProjection==main.viewProjection);
    assert(nativeMock::scissor[1]==31&&nativeMock::scissor[2]==57&&nativeMock::scissor[3]==224&&nativeMock::scissor[4]==387);
    saved[1]+=1;assert(!camera::publishNativeMirrorCamera(projection,saved,viewport,mirror)&&!game::applyCamera(restored));saved[1]=projection[0][0];
    projection[0][1]=1;assert(!camera::publishNativeMirrorCamera(projection,saved,viewport,mirror));projection[0][1]=0;
    const std::uint32_t empty[]{0,0,0,1},outside[]{700,0,10,10};
    assert(!camera::publishNativeMirrorCamera(projection,saved,viewport,empty));
    assert(!camera::publishNativeMirrorCamera(projection,saved,viewport,outside));
    assert(camera::publishNativeMirrorCamera(projection,saved,viewport,original)&&game::target(&surface));
    std::puts("PASS native mirror camera/scissor publication and restoration, concat-view/SMTX palettes, RGBA ambient and isolated validated front-cull override (host SDK substitutes)");
}
}
