#include "native_game_parameters_fixture.hpp"
#include <cstdio>
using namespace dusk::pica;
using namespace nativeParametersFixture;
namespace {
J3DColorChan channel(bool enabled,unsigned material,unsigned ambient,unsigned mask,unsigned diffuse,unsigned attenuation){
    return J3DColorChan(J3DColorChanInfo{u8(enabled),u8(material),u8(mask),u8(diffuse),u8(attenuation),u8(ambient),{}});
}
void close(float actual,float expected){assert(std::abs(actual-expected)<1e-6f);}
}
int main(){
    J3DMaterial material;
    material.color.material[0]={11,22,33,44};material.color.material[1]={55,66,77,88};
    material.color.ambient[0]={9,8,7,6};material.color.ambient[1]={5,4,3,2};
    // Deliberately distinct RGB/alpha declarations in source getter order.
    material.color.channels[0]=channel(true,GX_SRC_REG,GX_SRC_REG,0x92,GX_DF_CLAMP,GX_AF_SPOT);
    material.color.channels[1]=channel(true,GX_SRC_REG,GX_SRC_REG,0x10,GX_DF_SIGN,GX_AF_NONE);
    material.color.channels[2]=channel(false,GX_SRC_VTX,GX_SRC_REG,0xff,GX_DF_NONE,GX_AF_NONE);
    material.color.channels[3]=channel(true,GX_SRC_REG,GX_SRC_VTX,0x02,GX_DF_CLAMP,GX_AF_SPEC);
    J3DLightObj local;
    local.info.mLightPosition={21,22,23};local.info.mLightDirection={0,0,-1};
    local.info.mColor={10,20,30,40};local.info.mCosAtten={1,.5f,.25f};local.info.mDistAtten={2,.125f,.0625f};
    material.color.lights[4]=&local;
    availableLights=0x82;globalReads=0;
    globalLights[1].mLightPosition={-2,3,-4};globalLights[1].mLightDirection={1,0,0};
    globalLights[1].mColor={80,90,100,110};globalLights[1].mCosAtten={1,0,0};globalLights[1].mDistAtten={1,0,0};
    globalLights[7].mColor={0,0,0,255}; // Alpha-only light, used only by RGB0: must not occupy a compact slot.
    for(unsigned i=0;i<3;++i)material.registers[i]={s16(10+i),s16(20+i),s16(30+i),s16(40+i)};
    for(unsigned i=0;i<4;++i)material.konst[i]={u8(50+i),u8(60+i),u8(70+i),u8(80+i)};
    material.fog.mType=2;material.fog.mAdjEnable=1;material.fog.mCenter=287;
    material.fog.mStartZ=10;material.fog.mEndZ=110;material.fog.mNearZ=1;material.fog.mFarZ=1000;
    material.fog.mColor={7,17,27,37};
    for(unsigned i=0;i<10;++i)material.fog.mFogAdjTable.r[i]=256+16*i;
    UniformBinding binding;binding.sourceAttributes=(1u<<GX_VA_NRM)|(1u<<GX_VA_CLR1);
    binding.parameterReadMasks.fill(15);
    DrawUniforms out;
    assert(J3DModel::copyNativeMaterial(material,binding,out));
    assert(out.lightCount==2&&globalReads==0x82); // Slot4 comes from the material, not an unavailable global.
    assert(out.channels[0].lightMask==3&&out.channels[1].lightMask==0&&out.channels[2].lightMask==2&&out.channels[3].lightMask==1);
    assert(out.channels[0].attenuation==Attenuation::spot&&out.channels[2].attenuation==Attenuation::none&&out.channels[3].attenuation==Attenuation::specular);
    assert(out.channels[2].diffuse==Diffuse::signedDot&&out.channels[3].diffuse==Diffuse::none);
    assert(out.channels[1].materialVertex&&out.channels[3].ambientVertex);
    assert((out.lights[0].position==std::array<float,3>{-2,3,-4}));
    assert((out.lights[1].position==std::array<float,3>{21,22,23}));
    assert((out.lights[1].direction==std::array<float,3>{0,0,-1}));
    assert((out.lights[1].cosine==std::array<float,3>{1,.5f,.25f}));
    assert((out.lights[1].distance==std::array<float,3>{2,.125f,.0625f}));
    assert(out.lights[1].kind==LightKind::point);close(out.lights[1].color[3],40/255.f);
    close(out.materialColors[0][2],33/255.f);close(out.ambientColors[0][3],6/255.f);
    assert(out.material.suppliedMask==255&&out.material.colors[0]==0x281e140a&&out.material.colors[6]==0x53493f35&&out.material.colors[7]==0x251b1107);
    assert(out.fog.provided&&out.fog.enabled&&out.fog.rangeEnabled&&!out.fog.orthographic);
    close(out.fog.a,1000.f/(999*100));close(out.fog.b,1000.f/999);close(out.fog.c,.1f);close(out.fog.range[9],400/256.f);
    cameraRegion={608,448,152,112,304,224,.25f,.75f};
    DrawUniforms cameraOut=out;
    assert(applyCamera(cameraOut,material.fog.mCenter));
    close(cameraOut.fog.rangeScale,-2);close(cameraOut.fog.rangeBias,34/304.f);
    close(cameraOut.fog.a,out.fog.a*.5f);close(cameraOut.fog.b,out.fog.b*.5f+.25f);
    // The viewport center maps to source X=304, not a guessed full-screen center.
    close(cameraOut.fog.rangeScale*0+cameraOut.fog.rangeBias,2*(304-287)/304.f);
    material.fog.mType=10;assert(J3DModel::copyNativeMaterial(material,binding,cameraOut));
    close(cameraOut.fog.a,9.99f);close(cameraOut.fog.c,.09f);
    assert(applyCamera(cameraOut,287));close(cameraOut.fog.a,19.98f);close(cameraOut.fog.c,5.085f);
    material.fog.mType=2;
    // Mutating live declarations must change exports without an asset rebuild.
    material.registers[0].r=201;local.info.mColor.a=200;
    assert(J3DModel::copyNativeMaterial(material,binding,out));
    assert((out.material.colors[0]&255)==201);close(out.lights[1].color[3],200/255.f);
    material.registers[0].r=-1;assert(!J3DModel::copyNativeMaterial(material,binding,out));material.registers[0].r=201;
    // Unread S10 components do not constrain admission; A/B/C wrap, D never does.
    binding.parameterReadMasks[0]=1;material.registers[0]={-257,-1024,1023,-1};
    binding.parameterWrapMasks[0]=1;
    assert(J3DModel::copyNativeMaterial(material,binding,out));assert(out.material.colors[0]==255);
    binding.parameterSignedMasks[0]=1;
    assert(!J3DModel::copyNativeMaterial(material,binding,out));
    material.registers[0].r=127;
    assert(J3DModel::copyNativeMaterial(material,binding,out));assert(out.material.colors[0]==127);
    binding.parameterReadMasks[0]=0;binding.parameterWrapMasks[0]=binding.parameterSignedMasks[0]=0;
    assert(J3DModel::copyNativeMaterial(material,binding,out));assert(!(out.material.suppliedMask&1));
    binding.parameterReadMasks[0]=15;material.registers[0]={201,20,30,40};
    const auto allChannels=binding.channelReadMask;
    binding.channelReadMask=4;binding.sourceAttributes=1u<<GX_VA_NRM;globalReads=0;
    assert(J3DModel::copyNativeMaterial(material,binding,out));
    assert(out.lightCount==1&&globalReads==0); // Only alpha0's local slot4 is read.
    assert(out.materialColors[0][0]==0);close(out.materialColors[0][3],44/255.f);
    binding.channelReadMask=2;material.color.count=0;
    assert(!J3DModel::copyNativeMaterial(material,binding,out)); // Disabled channel still selects CLR1.
    binding.sourceAttributes=1u<<GX_VA_CLR1;
    assert(J3DModel::copyNativeMaterial(material,binding,out));
    binding.channelReadMask=allChannels;material.color.count=2;
    binding.sourceAttributes=(1u<<GX_VA_NRM)|(1u<<GX_VA_CLR1);
    material.color.channels[0]=channel(true,GX_SRC_VTX,GX_SRC_REG,0x92,GX_DF_CLAMP,GX_AF_SPOT);
    assert(!J3DModel::copyNativeMaterial(material,binding,out)); // No CLR0: white is not a provider.
    binding.sourceAttributes|=1u<<GX_VA_CLR0;
    assert(J3DModel::copyNativeMaterial(material,binding,out));
    availableLights=0x80;assert(!J3DModel::copyNativeMaterial(material,binding,out));availableLights=0x82;
    material.hasFog=false;assert(J3DModel::copyNativeMaterial(material,binding,out));
    assert(!out.fog.provided&&!(out.material.suppliedMask&128)); // No invented disabled declaration/color.

    J3DTexCoord& coord=material.coords[0];coord.mTexGenType=GX_TG_MTX2x4;coord.mTexGenSrc=GX_TG_TEX1;
    coord.mTexGenMtx=GX_TEXMTX0;coord.setTexMtxReg(GX_IDENTITY);
    C_MTXIdentity(material.matrices[0].matrix);
    material.matrices[0].matrix[0][2]=.25f;material.matrices[0].matrix[0][3]=.5f;
    material.matrices[0].matrix[1][2]=-.125f;material.matrices[0].matrix[1][3]=.75f;
    binding.sourceAttributes|=1u<<GX_VA_TEX1;out={};
    assert(nativeTexGen(material,nullptr,false,0,2,binding,out));
    assert(out.uvTransforms[2].source==1);close(out.uvTransforms[2].rows[0][2],.75f);close(out.uvTransforms[2].rows[1][2],.625f);
    coord.mTexGenType=GX_TG_MTX3x4;coord.mTexGenSrc=GX_TG_POS;binding.sourceAttributes|=1u<<GX_VA_POS;
    material.matrices[0].info.mInfo=J3DTexMtxMode_ViewProjmap;
    C_MTXIdentity(material.matrices[0].matrix);material.matrices[0].matrix[0][3]=10;
    out={};assert(nativeTexGen(material,nullptr,false,0,0,binding,out));
    assert(out.projective.enabled&&out.projective.source==PositionSource::object);close(out.projective.rows[0][3],10);
    Mtx snapshot[1];C_MTXIdentity(snapshot[0]);snapshot[0][0][3]=5;
    J3DTexMtxObj deferred{snapshot,nullptr,1};out={};
    assert(nativeTexGen(material,&deferred,false,0,0,binding,out));
    assert(out.projective.source==PositionSource::posed);close(out.projective.rows[0][3],5); // No second joint concatenation.
    C_MTXIdentity(j3dSys.view);j3dSys.view[0][3]=40;
    material.matrices[0].info.mInfo=J3DTexMtxMode_Projmap;coord.setTexMtxReg(0);out={};
    assert(nativeTexGen(material,&deferred,true,0,0,binding,out));
    assert(out.projective.source==PositionSource::posed);close(out.projective.rows[0][3],-35);
    coord.mTexGenSrc=GX_TG_NRM;out={};assert(!nativeTexGen(material,&deferred,true,0,0,binding,out));
    // Post normal matrix30 must use inverse-transpose normals, not linear draw
    // normals or the lighting path's normalized value. Bias survives final W1.
    coord.setTexMtxReg(GX_TEXMTX0);
    material.matrices[0].info.mInfo=J3DTexMtxMode_EnvmapOld;
    C_MTXIdentity(snapshot[0]);snapshot[0][0][0]=.5f;snapshot[0][1][1]=-.5f;
    snapshot[0][0][3]=snapshot[0][1][3]=.5f;out={};
    assert(nativeTexGen(material,&deferred,true,0,0,binding,out));
    assert(out.projective.source==PositionSource::posedNormal&&!out.projective.normalize);
    close(out.projective.rows[0][3],.5f);close(out.projective.rows[1][3],.5f);
    for(unsigned mode:{1u,6u,7u}){
        material.matrices[0].info.mInfo=mode;out={};
        assert(nativeTexGen(material,&deferred,false,0,0,binding,out));
        assert(out.projective.source==PositionSource::posedLinearNormal&&!out.projective.normalize);
        close(out.projective.rows[0][3],.5f);
    }
    coord.mTexGenType=GX_TG_MTX2x4;out={};
    assert(nativeTexGen(material,&deferred,false,0,2,binding,out));
    assert(out.uvTransforms[2].source==7);close(out.uvTransforms[2].translation[0],.5f);
    // A post2x4 base replaces Z with1 before the complete matrix.
    snapshot[0][0][2]=.25f;out={};
    assert(nativeTexGen(material,&deferred,true,0,1,binding,out));
    assert(out.uvTransforms[1].source==6);
    close(out.uvTransforms[1].rows[0][2],0);close(out.uvTransforms[1].translation[0],.75f);
    // Actual effect recipes, with translated/nonuniform view and authored SRT.
    Mtx44 effects[1]{};for(unsigned i=0;i<4;++i)effects[0][i][i]=1;
    deferred.mpEffectMtx=effects;
    auto& info=material.matrices[0].info;
    info.mSRT.mScaleX=info.mSRT.mScaleY=1;info.mSRT.mRotation=0;
    info.mSRT.mTranslationX=.25f;info.mSRT.mTranslationY=-.125f;info.mCenter={0,0,0};
    C_MTXIdentity(j3dSys.view);j3dSys.view[0][0]=2;j3dSys.view[1][1]=3;j3dSys.view[2][2]=4;j3dSys.view[0][3]=40;
    coord.mTexGenType=GX_TG_MTX3x4;
    for(unsigned mode:{5u,10u,11u}){
        info.mInfo=mode;out={};
        assert(nativeTexGen(material,&deferred,false,0,0,binding,out));
        assert(out.projective.source==PositionSource::posedLinearNormal);
        close(out.projective.rows[0][0],mode==5?.5f:.25f);
        close(out.projective.rows[0][2],mode==5?0:mode==10?.0625f:.1875f);
        close(out.projective.rows[0][3],0);
        out={};assert(nativeTexGen(material,&deferred,true,0,0,binding,out));
        assert(out.projective.source==PositionSource::posedNormal);close(out.projective.rows[0][3],0);
    }
    info.mInfo=1;coord.mTexGenMtx=GX_IDENTITY;coord.setTexMtxReg(GX_IDENTITY);out={};
    assert(nativeTexGen(material,nullptr,false,0,0,binding,out));
    assert(out.projective.source==PositionSource::objectNormal);
    binding.sourceAttributes&=~(1u<<GX_VA_NRM);
    assert(!nativeTexGen(material,nullptr,false,0,0,binding,out));

    Palette billboard;
    Mtx full={{0,-3,0,11},{2,0,0,22},{0,0,4,33}};
    assert(nativeBillboard(full,1,false,billboard,0));
    close(billboard.position[0][0],2);close(billboard.position[1][1],3);close(billboard.position[2][2],4);
    close(billboard.position[0][3],11);close(billboard.position[1][3],22);close(billboard.position[2][3],33);
    close(billboard.normal[0][0],.5f);close(billboard.normal[1][1],1/3.f);close(billboard.normal[2][2],.25f);
    Mtx axial={{0,0,4,11},{2,3,0,22},{0,4,0,33}};
    assert(nativeBillboard(axial,2,false,billboard,1));
    close(billboard.position[3][0],2);close(billboard.position[4][1],3);close(billboard.position[5][1],4);
    close(billboard.position[4][2],-3.2f);close(billboard.position[5][2],2.4f);
    close(billboard.normal[3][0],.5f);close(billboard.normal[4][1],.12f);close(billboard.normal[5][1],.16f);
    close(billboard.normal[4][2],-.2f);close(billboard.normal[5][2],.15f);
    Mtx degenerate{};assert(!nativeBillboard(degenerate,1,false,billboard,0));
    assert(!nativeBillboard(degenerate,2,false,billboard,0));

    ResTIMG image{};image.wrapS=2;image.wrapT=1;image.minFilter=5;image.magFilter=0;
    image.mipmapCount=3;image.mipmapEnabled=1;image.minLOD=8;image.maxLOD=16;image.LODBias=-125;
    Sampler sampler;assert(nativeSampler(image,sampler));
    assert(sampler.wrapS==Wrap::mirror&&sampler.wrapT==Wrap::repeat&&sampler.minify==Filter::linear&&sampler.magnify==Filter::nearest&&sampler.mipmap==Filter::linear);
    assert(sampler.firstLevel==1&&sampler.lastLevel==2&&sampler.lodBias==-320);
    image.minLOD=7;assert(!nativeSampler(image,sampler));image.minLOD=8;image.doEdgeLOD=1;assert(!nativeSampler(image,sampler));
    std::puts("PASS native consumed channels/components, A/B/C wrap vs signed D, live lighting/fog, normal/deferred texgen bias/spaces, original billboard palettes and source samplers (host only)");
}
