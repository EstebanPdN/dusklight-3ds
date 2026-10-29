"""Extract actual game source identity/pose bodies for host SDK substitutes."""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]

def body(text,signature):
    at=text.index(signature);start=text.index('{',at);depth=1;end=start+1
    while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[at:end]+'\n'

def generate():
    model=(ROOT/'Source/libs/JSystem/src/J3DGraphAnimator/J3DModel.cpp').read_text()
    resource=(ROOT/'Source/src/d/d_resorce.cpp').read_text()
    begin=resource.index('#if defined(DUSK_PICA_GAME_DRAW)',resource.index('void* res = mArchive->getIdxResource(fileIndex);'))
    end=resource.index('#if TARGET_PC && !defined(DUSK_TARGET_3DS)',begin)
    capture=resource[begin:end]
    last=resource.index('                mRes[fileIndex] = res;')
    begin=resource.rindex('#if defined(DUSK_PICA_GAME_DRAW)',0,last)
    attach=resource[begin:last]+'mRes[fileIndex] = res;'
    return '''#pragma once
#include "native/game_source.hpp"
#include "native/game_draw_api.hpp"
#include <span>
#define DEBUG 0
namespace nativeSourceFixture {
struct Data {
    unsigned flags{},count=3;bool billboard{};dusk::pica::SourceIdentity source;
    bool checkFlag(unsigned f)const{return flags&f;}bool checkBBoardFlag()const{return billboard;}
    unsigned getDrawMtxNum()const{return count;}
    void setNativeSource(const dusk::pica::SourceIdentity& s){source=s;}
};
using J3DModelData=Data;
struct Buffer {
    unsigned mCurrentViewNo{},mMtxNum=1;
    float (*p[1])[3][4]{};float (*n[1])[3][3]{};
    float (**mpDrawMtxArr[2])[3][4]{nullptr,p};float (**mpNrmMtxArr[2])[3][3]{nullptr,n};
};
constexpr unsigned J3DMdlFlag_SkinPosCpu=4,J3DMdlFlag_SkinNrmCpu=8,J3DMdlFlag_UseDefaultJ3D=2;
struct J3DModel {
    Data* mModelData{};Buffer* mMtxBuffer{};
    void* mDeformData{};void* mSkinDeform{};void* mVtxColorCalc{};void* mUnkCalc1{};void* mUnkCalc2{};
    unsigned flags{};unsigned accesses{};
    bool checkFlag(unsigned f)const{return flags&f;}
    unsigned getMtxCalcMode()const{return flags&3;}
    float (*getDrawMtxPtr())[3][4]{++accesses;return mMtxBuffer->mpDrawMtxArr[1][mMtxBuffer->mCurrentViewNo];}
    float (*getNrmMtxPtr())[3][3]{++accesses;return mMtxBuffer->mpNrmMtxArr[1][mMtxBuffer->mCurrentViewNo];}
    bool copyNativePalette(const dusk::pica::UniformBinding&,dusk::pica::Palette&);
};
'''+body(model,'bool J3DModel::copyNativePalette(')+body(model,'bool nativeGameParameters(')+'''
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
'''
