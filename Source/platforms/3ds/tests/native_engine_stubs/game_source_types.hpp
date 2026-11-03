#pragma once
#include "native/game_source.hpp"
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>

// Test-only declaration storage. Getters expose authored fields; production
// conversion bodies are extracted unchanged by native_game_source_fixture.py.
namespace nativeSourceFixture {
using u8=std::uint8_t;using u16=std::uint16_t;using u32=std::uint32_t;using s16=std::int16_t;using f32=float;
using Mtx=float[3][4];using Mtx44=float[4][4];using MtxP=float (*)[4];using MtxPtr=MtxP;
constexpr unsigned GX_SRC_VTX=1,GX_VA_POS=9,GX_VA_NRM=10,GX_VA_CLR0=11,GX_VA_TEX0=13;
constexpr unsigned GX_TG_POS=0,GX_TG_NRM=1,GX_TG_TEX0=4,GX_TG_TEX2=6,GX_TG_MTX2x4=1;
constexpr unsigned GX_TEXMTX0=30,GX_TEXMTX9=57,GX_IDENTITY=60;
constexpr unsigned J3DTexMtxMode_None=0,J3DTexMtxMode_ProjmapBasic=2,J3DTexMtxMode_ViewProjmapBasic=3,
    J3DTexMtxMode_Unknown4=4,J3DTexMtxMode_Projmap=8,J3DTexMtxMode_ViewProjmap=9;
void MTXIdentity(Mtx);void MTXCopy(const Mtx,Mtx);
u32 MTXInverse(const Mtx,Mtx);void MTXConcat(const Mtx,const Mtx,Mtx);
struct Vec {float x{},y{},z{};};
struct J3DTextureSRTInfo {float mScaleX=1,mScaleY=1;s16 mRotation{};float mTranslationX{},mTranslationY{};};
struct J3DGXColor {unsigned char r{},g{},b{},a{};};
struct J3DGXColorS10 {short r{},g{},b{},a{};};
struct J3DLightInfo {Vec mLightPosition{},mLightDirection{};J3DGXColor mColor{};Vec mCosAtten{1,0,0},mDistAtten{1,0,0};};
struct J3DLightObj {J3DLightInfo value;J3DLightInfo* getLightInfo(){return &value;}};
inline std::array<J3DLightInfo,8> globalLights{};
inline std::array<bool,8> globalLightProvided{};
inline bool dKy_getNativeGlobalLight(unsigned slot,J3DLightInfo& out){assert(slot<8);if(!globalLightProvided[slot])return false;out=globalLights[slot];return true;}
struct J3DColorChan {
    unsigned enabled{},materialVertex{},ambientVertex{},diffuse=2,attenuation=2,mask{};
    unsigned getEnable()const{return enabled;}unsigned getMatSrc()const{return materialVertex;}
    unsigned getAmbSrc()const{return ambientVertex;}unsigned getDiffuseFn()const{return diffuse;}
    unsigned getAttnFn()const{return attenuation;}unsigned getLightMask()const{return mask;}
};
struct J3DColorBlock {
    unsigned count=1;std::array<J3DColorChan,4> channels{};
    std::array<J3DGXColor,2> material{{{255,255,255,255},{255,255,255,255}}},ambient{};
    std::array<J3DLightObj*,8> lights{};
    unsigned getColorChanNum()const{return count;}J3DColorChan* getColorChan(unsigned i){return &channels.at(i);}
    J3DGXColor* getMatColor(unsigned i){return &material.at(i);}J3DGXColor* getAmbColor(unsigned i){return &ambient.at(i);}
    J3DLightObj* getLight(unsigned i){return lights.at(i);}
};
struct J3DFog {
    unsigned mType{},mAdjEnable{},mCenter{};J3DGXColor mColor{};
    float mStartZ{},mEndZ=100,mNearZ=1,mFarZ=1000;
    struct {std::array<unsigned,10> r{256,256,256,256,256,256,256,256,256,256};} mFogAdjTable;
};
struct ResTIMG {
    unsigned wrapS{},wrapT{},minFilter{},magFilter{},mipmapEnabled{},doEdgeLOD{},biasClamp{},maxAnisotropy{},mipmapCount=1;
    int minLOD{},maxLOD{},LODBias{};
};
struct J3DTexture {
    std::array<ResTIMG,8> images{};unsigned count=1;
    unsigned getNum()const{return count;}ResTIMG* getResTIMG(unsigned n){return &images.at(n);}
};
struct J3DTexCoord {
    unsigned type=GX_TG_MTX2x4,source=GX_TG_TEX0,matrix=GX_IDENTITY,reg=GX_IDENTITY;
    unsigned getTexGenType()const{return type;}unsigned getTexGenSrc()const{return source;}
    unsigned getTexGenMtx()const{return matrix;}unsigned getTexMtxReg()const{return reg;}
};
struct J3DTexMtx {
    Mtx value{};struct Info{unsigned mInfo{};J3DTextureSRTInfo mSRT;Vec mCenter{};}info;
    J3DTexMtx(){MTXIdentity(value);}Mtx& getMtx(){return value;}Info& getTexMtxInfo(){return info;}
};
struct J3DTexMtxObj {
    Mtx* mpTexMtx{};Mtx44* mpEffectMtx{};unsigned count{};
    unsigned getNumTexMtx()const{return count;}Mtx& getMtx(unsigned i){assert(i<count&&mpTexMtx);return mpTexMtx[i];}
    Mtx44& getEffectMtx(unsigned i){assert(i<count&&mpEffectMtx);return mpEffectMtx[i];}
};
struct J3DShapeMtx {
    u32 type=0x534d5458;unsigned useMtxIndex{},useMtxNum=1;std::array<unsigned,10> indices{};
    u32 getType()const{return type;}unsigned getUseMtxNum()const{return useMtxNum;}
    unsigned getUseMtxIndex(unsigned i)const{assert(i<useMtxNum);return useMtxNum==1?useMtxIndex:indices.at(i);}
};
struct J3DShape {
    unsigned index{},nbt{},groupCount{};std::array<J3DShapeMtx,3> groups{};
    unsigned getIndex()const{return index;}unsigned getNBTFlag()const{return nbt;}
    unsigned getMtxGroupNum()const{return groupCount;}
    J3DShapeMtx* getShapeMtx(unsigned i){return i<groupCount?&groups.at(i):nullptr;}
};
struct J3DShapePacket {J3DTexMtxObj* matrices{};Mtx* base{};J3DTexMtxObj* getTexMtxObj(){return matrices;}Mtx* getBaseMtxPtr()const{return base;}};
struct J3DTevOrder {
    unsigned texMap=255;
    unsigned getTexMap()const{return texMap;}
};
struct J3DTevBlock {
    u8 stageCount=1;std::array<J3DTevOrder,16> orders{};
    u8 getTevStageNum()const{return stageCount;}
    void setTevStageNum(u8 count){stageCount=count;}
    J3DTevOrder* getTevOrder(unsigned i){return &orders.at(i);}
};
struct J3DTexGenBlock {
    unsigned count=1;
    unsigned getTexGenNum()const{return count;}
    void setTexGenNum(unsigned value){count=value;}
};
struct J3DMaterial {
    J3DColorBlock color;J3DFog fog;J3DTexture texture;J3DTevBlock tev;J3DTexGenBlock texGen;
    bool hasColor=true,hasTev=true,hasPE=true,hasFog=true,hasTexGen=true;
    std::array<unsigned,8> textureNumbers{};
    std::array<J3DTexCoord,8> coordinates;std::array<J3DTexMtx,8> matrices;
    std::array<J3DGXColorS10,3> registers{};std::array<J3DGXColor,4> konst{};
    J3DShape* shape{};
    J3DColorBlock* getColorBlock(){return hasColor?&color:nullptr;}
    J3DTevBlock* getTevBlock(){return hasTev?&tev:nullptr;}void* getPEBlock(){return hasPE?this:nullptr;}
    J3DFog* getFog(){return hasFog?&fog:nullptr;}
    J3DTexGenBlock* getTexGenBlock(){return hasTexGen?&texGen:nullptr;}
    unsigned getTexGenNum()const{return texGen.getTexGenNum();}
    unsigned getTevStageNum()const{return tev.getTevStageNum();}
    J3DTexCoord* getTexCoord(unsigned i){return &coordinates.at(i);}J3DTexMtx* getTexMtx(unsigned i){return &matrices.at(i);}
    unsigned getTexNo(unsigned i)const{return textureNumbers.at(i);}
    J3DGXColorS10* getTevColor(unsigned i){return &registers.at(i);}J3DGXColor* getTevKColor(unsigned i){return &konst.at(i);}
    J3DShape* getShape(){return shape;}
};
struct SourceState {Mtx view{};Mtx& getViewMtx(){return view;}};
inline SourceState j3dSys;
}
