"""Local source math oracles and real game publication helper for host checks."""
from pathlib import Path
from native_game_source_fixture import body
ROOT=Path(__file__).resolve().parents[2]
SDK_SOURCES=ROOT/'Upstream/extracted/citro3d-1.7.1/source/maths'

def generate():
    original=(ROOT/'Source/libs/dolphin/src/mtx/mtx44.c').read_text()
    game=(ROOT/'Source/src/m_Do/m_Do_graphic.cpp').read_text()
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
    return ''.join(pieces),sdk
