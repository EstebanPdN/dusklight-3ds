"""Host fixture for the actual source packet entry bodies; never a game run."""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def generate():
    source=(ROOT/'Source/libs/JSystem/src/J3DGraphBase/J3DPacket.cpp').read_text()
    def body(signature):
        at=source.index(signature);start=source.index('{',at);depth=1;end=start+1
        while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
        return source[at:end]+'\n'
    head='''#pragma once
#include "native/game_draw_api.hpp"
#include <stdexcept>
#define DUSK_PICA_GAME_DRAW 1
namespace nativePacketFixture {inline void* surface{};inline bool copySafe=true;inline unsigned invalidations{};}
namespace dusk::native {
inline bool alphaIntervalCopySafe(){return nativePacketFixture::copySafe;}
inline void* activeFramebuffer(){return nativePacketFixture::surface;}
inline void invalidatePicaCommandState(){++nativePacketFixture::invalidations;}
}
namespace nativePacketFixture {
[[noreturn]] inline void OSPanic(const char*,int,const char* reason,...){throw std::runtime_error(reason);}
constexpr unsigned J3DShpFlag_Hidden=1;
struct J3DShape {unsigned index;unsigned getIndex()const{return index;}};
struct J3DModel {
    const void* identity{};
    bool drawNativeShape(unsigned shape){return dusk::pica::game::drawShape(identity,shape);}
};
struct J3DShapePacket {
    J3DModel* model{};J3DShape* shape{};J3DShapePacket* next{};bool hidden{};
    bool checkFlag(unsigned)const{return hidden;}
    J3DModel* getModel()const{return model;}J3DShape* getShape()const{return shape;}
    J3DShapePacket* getNextPacket()const{return next;}
    void draw();void drawFast();
};
struct J3DDisplayListObj {void callDL()const;};
struct J3DDrawPacket {void draw();};
struct SourceState {const void* model{};void* texture{};void setModel(const void* p){model=p;}void setTexture(void* p){texture=p;}};
inline SourceState j3dSys;
struct J3DMatPacket {J3DShapePacket* shape{};void* mpTexture{};J3DShapePacket* getShapePacket()const{return shape;}void draw();};
'''
    return head+body('void drawNativeGameShape(const J3DShapePacket& packet)')+''.join(body(s) for s in ['void J3DMatPacket::draw()','void J3DShapePacket::draw()','void J3DShapePacket::drawFast()','void J3DDrawPacket::draw()','void J3DDisplayListObj::callDL() const'])+'\n}\n'
