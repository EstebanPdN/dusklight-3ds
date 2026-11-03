"""Extract real J3D parameter/palette, matrix and billboard bodies for host SDK substitutes."""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]

def body(text,signature):
    at=text.index(signature);start=text.index('{',at);depth=1;end=start+1
    while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[at:end]+'\n'

def generate():
    model=(ROOT/'Source/libs/JSystem/src/J3DGraphAnimator/J3DModel.cpp').read_text()
    resource=(ROOT/'Source/src/d/d_resorce.cpp').read_text()
    matrix=(ROOT/'Source/libs/dolphin/src/mtx/mtx.c').read_text()
    transform=(ROOT/'Source/libs/JSystem/src/J3DGraphBase/J3DTransform.cpp').read_text()
    vector=(ROOT/'Source/libs/dolphin/src/mtx/vec.c').read_text()
    math_helpers=''.join(body(matrix,s).replace('C_MTX','MTX') for s in [
        'void C_MTXIdentity(', 'void C_MTXCopy(', 'void C_MTXConcat(', 'u32 C_MTXInverse('])
    math_helpers+=body(vector,'void C_VECNormalize(')
    math_helpers+=''.join(body(transform,s) for s in [
        'void J3DCalcBBoardMtx(', 'void J3DCalcYBBoardMtx(',
        'void J3DGetTextureMtx(', 'void J3DGetTextureMtxOld(',
        'void J3DGetTextureMtxMaya(', 'void J3DGetTextureMtxMayaOld(',
        'ASM void J3DMtxProjConcat('])
    begin=resource.index('#if defined(DUSK_PICA_GAME_DRAW)',resource.index('void* res = mArchive->getIdxResource(fileIndex);'))
    end=resource.index('#if TARGET_PC && !defined(DUSK_TARGET_3DS)',begin)
    capture=resource[begin:end]
    last=resource.index('                mRes[fileIndex] = res;')
    begin=resource.rindex('#if defined(DUSK_PICA_GAME_DRAW)',0,last)
    attach=resource[begin:last]+'mRes[fileIndex] = res;'
    return '''#pragma once
#include "native/game_source.hpp"
#include "native/game_draw_api.hpp"
#include "game_source_types.hpp"
#include <span>
#define DEBUG 0
#define TARGET_PC 1
#define ASM
#define __REGISTER [[maybe_unused]]
#define ASSERTMSGLINE(line,condition,...) assert(condition)
#define __memcpy std::memcpy
#define J3D_sqrtf sqrtf
#define VECNormalize C_VECNormalize
namespace nativeSourceFixture {
namespace JMath {
inline float fastSqrt(float value){return std::sqrt(value);}
inline void gekko_ps_copy6(void* out,const void* in){std::memcpy(out,in,24);}
inline void gekko_ps_copy12(void* out,const void* in){std::memcpy(out,in,48);}
}
// Host trig substitutes; exact rotation-zero cases do not depend on SDK tables.
inline float JMASSin(s16 angle){return std::sin(float(angle)*(6.2831853071795864769f/65536));}
inline float JMASCos(s16 angle){return std::cos(float(angle)*(6.2831853071795864769f/65536));}
'''+math_helpers+'''
struct Data {
    unsigned flags{},count=3;dusk::pica::SourceIdentity source;
    J3DMaterial material;unsigned materialCount=23;J3DTexture* texture=&material.texture;
    std::array<J3DShape,8> shapes{};std::array<unsigned,3> drawIndices{0,1,2},drawFlags{};
    unsigned jointCount=3,envelopeCount=3;
    J3DShape* getShapeNodePointer(unsigned i){return i<shapes.size()?&shapes[i]:nullptr;}
    unsigned getDrawMtxIndex(unsigned i)const{return drawIndices.at(i);}
    unsigned getDrawMtxFlag(unsigned i)const{return drawFlags.at(i);}
    unsigned getJointNum()const{return jointCount;}unsigned getWEvlpMtxNum()const{return envelopeCount;}
    unsigned getMaterialNum()const{return materialCount;}
    J3DMaterial* getMaterialNodePointer(unsigned i){assert(i<materialCount);return i==22||(materialCount==1&&i==0)?&material:nullptr;}
    J3DTexture* getTexture(){return texture;}unsigned getShapeNum()const{return 8;}
    bool checkFlag(unsigned f)const{return flags&f;}
    unsigned getDrawMtxNum()const{return count;}
    void setNativeSource(const dusk::pica::SourceIdentity& s){source=s;}
};
using J3DModelData=Data;
struct dRes_info_c {
    static void onWarpMaterial(J3DModelData*);
    static void offWarpMaterial(J3DModelData*);
};
struct Buffer {
    unsigned mCurrentViewNo{},mMtxNum=1;
    float (*p[1])[3][4]{};float (*n[1])[3][3]{};
    float (**mpDrawMtxArr[2])[3][4]{nullptr,p};float (**mpNrmMtxArr[2])[3][3]{nullptr,n};
    Mtx* mpWeightEvlpMtx{};Mtx* mpUserAnmMtx{};std::uint8_t* scaleFlags{};
    MtxP getUserAnmMtx(unsigned i){assert(mpUserAnmMtx);return mpUserAnmMtx[i];}
    std::uint8_t* getScaleFlagArray(){return scaleFlags;}
    unsigned getScaleFlag(unsigned i){assert(scaleFlags);return scaleFlags[i];}
};
constexpr unsigned J3DMdlFlag_SkinPosCpu=4,J3DMdlFlag_SkinNrmCpu=8,J3DMdlFlag_UseDefaultJ3D=2,J3DMdlFlag_EnableLOD=16;
struct J3DModel {
    Data* mModelData{};Buffer* mMtxBuffer{};
    void* mDeformData{};void* mSkinDeform{};void* mVtxColorCalc{};void* mUnkCalc1{};void* mUnkCalc2{};
    unsigned flags{};unsigned accesses{};
    J3DShapePacket* mShapePacket{};
    Data* getModelData(){return mModelData;}
    J3DShapePacket* getShapePacket(unsigned shape){assert(mShapePacket&&shape<8);return mShapePacket+shape;}
    bool copyNativeMaterial(J3DMaterial&,const dusk::pica::UniformBinding&,dusk::pica::DrawUniforms&);
    bool copyNativeParameters(const dusk::pica::UniformBinding&,dusk::pica::DrawUniforms&);
    bool checkFlag(unsigned f)const{return flags&f;}
    unsigned getMtxCalcMode()const{return flags&3;}
    float (*getDrawMtxPtr())[3][4]{++accesses;return mMtxBuffer->mpDrawMtxArr[1][mMtxBuffer->mCurrentViewNo];}
    float (*getNrmMtxPtr())[3][3]{++accesses;return mMtxBuffer->mpNrmMtxArr[1][mMtxBuffer->mCurrentViewNo];}
    MtxP getWeightAnmMtx(unsigned i){assert(mMtxBuffer->mpWeightEvlpMtx);return mMtxBuffer->mpWeightEvlpMtx[i];}
    bool copyNativePalette(const dusk::pica::UniformBinding&,dusk::pica::Palette&);
};
'''+''.join(body(model,s) for s in [
        'u32 nativeColor(', 'dusk::pica::Row nativeColorRow(', 'bool nativeFog(',
        'bool nativeSampler(', 'bool nativeBillboard(', 'bool nativeTexGen(', 'bool J3DModel::copyNativeMaterial(',
        'bool J3DModel::copyNativeParameters(', 'bool J3DModel::copyNativePalette(',
        'bool nativeGameParameters('])+''.join(body(resource,s) for s in [
        'void dRes_info_c::onWarpMaterial(', 'void dRes_info_c::offWarpMaterial('])+'''
struct Archive {unsigned size{};unsigned getExpandedResSize(const void*)const{return size;}};
struct Resource {
    Archive* mArchive{};const char* mArchiveName="host";void* mRes[1]{};Data model;
    void OSReport_Error(const char*,...){}
    int run(unsigned nodeType,std::span<std::byte> original){
        unsigned fileIndex=0;void* res=original.data();
'''+capture+'''
        if(nativeSource.bytes){for(auto& b:original)b=std::byte{0xcd};res=&model;}
'''+attach+'''
        return 0;
    }
};
}
#undef DEBUG
#undef TARGET_PC
#undef ASM
#undef __REGISTER
#undef ASSERTMSGLINE
#undef __memcpy
#undef J3D_sqrtf
#undef VECNormalize
'''
