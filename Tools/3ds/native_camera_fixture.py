"""Local source math oracles and real game publication helper for host checks."""
from pathlib import Path
from native_game_source_fixture import body
ROOT=Path(__file__).resolve().parents[2]
SDK_SOURCES=ROOT/'Upstream/extracted/citro3d-1.7.1/source/maths'

def generate():
    original=(ROOT/'Source/libs/dolphin/src/mtx/mtx44.c').read_text()
    game=(ROOT/'Source/src/m_Do/m_Do_graphic.cpp').read_text()
    mirror=(ROOT/'Source/src/d/actor/d_a_mirror.cpp').read_text()
    pieces=['''#pragma once
#include "native/camera.hpp"
#include "native/game_draw_api.hpp"
#include <cmath>
namespace nativeCameraOracle {
using f32=float;using Mtx44=float[4][4];
inline float MTXDegToRad(float angle){return angle*0.01745329252f;}
#define ASSERTMSGLINE(line,condition,...) assert(condition)
''']
    pieces.extend(body(original,s) for s in ['void C_MTXPerspective(', 'void C_MTXOrtho(', 'void C_MTXFrustum('])
    pieces.append('#undef ASSERTMSGLINE\n')
    sdk=all((SDK_SOURCES/p).is_file() for p in ['mtx_persptilt.c','mtx_orthotilt.c'])
    pieces.append('inline constexpr bool hasSdkOracle='+str(sdk).lower()+';\n')
    if sdk:
        pieces.append('struct Vec{float x{},y{},z{},w{};};struct C3D_Mtx{Vec r[4];};inline void Mtx_Zeros(C3D_Mtx* m){*m={};}\n')
        pieces.extend(body((SDK_SOURCES/file).read_text(),signature) for file,signature in [('mtx_persptilt.c','void Mtx_PerspTilt('),('mtx_orthotilt.c','void Mtx_OrthoTilt(')])
    pieces.append('}\nnamespace nativeCameraGameFixture {\nusing Mtx44=float[4][4];inline constexpr unsigned FB_WIDTH=608,FB_HEIGHT=448;\nstruct view_port_class{float x_orig{},y_orig{},width{},height{},near_z{},far_z{};};\n')
    helper=body(game,'static void publishNativeSceneCamera(')
    # Rebind only the framebuffer provider, preserving the actual helper body.
    pieces.append('inline void* active{};\n'+helper.replace('dusk::native::activeFramebuffer()','active')+'\n}\n')
    pieces.append('namespace nativeMirrorGameFixture {\nusing Mtx44=float[4][4];using f32=float;using u32=std::uint32_t;\ninline constexpr unsigned FB_WIDTH=608,FB_HEIGHT=448;inline void* active{};\n')
    helper=body(mirror,'static bool publishNativeMirrorCamera(')
    pieces.append(helper.replace('dusk::native::activeFramebuffer()','active')+'\n}\n')
    pieces.append('''
#define TARGET_PC 1
#define JUT_ASSERT(line,condition) assert(condition)
namespace nativeMirrorModelFixture {
using f32=float;using u8=std::uint8_t;using u16=std::uint16_t;using u32=std::uint32_t;
using Mtx=float[3][4];using MtxP=float (*)[4];
struct cXyz {float x{},y{},z{};cXyz()=default;cXyz(float a,float b,float c):x(a),y(b),z(c){}};
inline void cMtx_multVec(const Mtx matrix,const cXyz* in,cXyz* out){
    out->x=matrix[0][0]*in->x+matrix[0][1]*in->y+matrix[0][2]*in->z+matrix[0][3];
    out->y=matrix[1][0]*in->x+matrix[1][1]*in->y+matrix[1][2]*in->z+matrix[1][3];
    out->z=matrix[2][0]*in->x+matrix[2][1]*in->y+matrix[2][2]*in->z+matrix[2][3];
}
struct GXColor {u8 r{},g{},b{},a{};};
struct dScnKy_env_light_c {GXColor bg_amb_col[1]{};};
inline dScnKy_env_light_c environment;
inline dScnKy_env_light_c* dKy_getEnvlight(){return &environment;}
[[noreturn]] inline void OSPanic(const char*,int,const char* reason,...){throw std::runtime_error(reason);}
struct J3DShape {unsigned index{};bool hidden{};unsigned resets{};
    bool checkFlag(unsigned)const{return hidden;}unsigned getIndex()const{return index;}
    void resetVcdVatCache(){++resets;}};
struct J3DModel;
struct J3DShapePacket {J3DModel* model{};J3DShape* shape{};Mtx* base{};
    J3DShape* getShape()const{return shape;}J3DModel* getModel()const{return model;}
    void setBaseMtxPtr(Mtx* m){base=m;}void drawFast();};
struct J3DMatPacket {J3DShapePacket* packet{};J3DShapePacket* getShapePacket()const{return packet;}};
struct J3DModelData {unsigned count{};unsigned getMaterialNum()const{return count;}
    unsigned getShapeNum()const{return count;}void* getTexture(){return nullptr;}};
struct J3DModel {J3DModelData* data{};J3DMatPacket* materials{};J3DShapePacket* shapes{};Mtx base{};
    Mtx& getBaseTRMtx(){return base;}J3DModelData* getModelData(){return data;}
    J3DMatPacket* getMatPacket(unsigned i){return materials+i;}
    J3DShapePacket* getShapePacket(unsigned i){return shapes+i;}};
inline void (*onDraw)(J3DShapePacket*){};
inline void J3DShapePacket::drawFast(){assert(onDraw);onDraw(this);}
struct SourceState {Mtx view{};MtxP getViewMtx(){return view;}void setTexture(void*){}};
inline SourceState j3dSys;
struct dMirror_packet_c {cXyz mViewScale;void modelDraw(J3DModel*,Mtx);};
''')
    pieces.append(body(mirror,'void dMirror_packet_c::modelDraw('))
    pieces.append('}\n#undef JUT_ASSERT\n#undef TARGET_PC\n')
    return ''.join(pieces),sdk
