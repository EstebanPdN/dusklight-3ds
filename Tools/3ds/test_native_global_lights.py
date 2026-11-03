#!/usr/bin/env python3
"""Execute original global-light declarations/helpers in a bounded host harness.

Extract production definitions, not a Python light model. GX submission and the
view holder are host boundaries; types, copy, attenuation and matrix math are
original. This does not exercise GX command encoding or native raster output.
"""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'Source'


def definition(text, start, semicolon=False):
    begin = text.index(start)
    opening = text.index('{', begin)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[begin:end + int(semicolon)] + '\n'


HARNESS = r'''
#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include "JSystem/J3DGraphBase/J3DStruct.h"

struct ViewHolder {
    Mtx view{{0,-1,0,10},{1,0,0,-20},{0,0,1,30}};
    MtxP getViewMtx() { return view; }
} j3dSys;
bool dKy_getNativeGlobalLight(unsigned, J3DLightInfo&);
static bool reloading;
static J3DLightInfo previous[8];
// No GPU dependency: assertions below concern original CPU declarations only.
extern "C" {
void GXInitLightPos(GXLightObj*, f32, f32, f32) {}
void GXInitLightDir(GXLightObj*, f32, f32, f32) {}
void GXInitLightColor(GXLightObj*, GXColor) {}
void GXInitLightDistAttn(GXLightObj*, f32, f32, GXDistAttnFn) {}
void GXInitLightSpot(GXLightObj*, f32, GXSpotFn) {}
void GXLoadLightObjImm(GXLightObj*, GXLightID id) {
    unsigned slot = 0;
    while ((1u << slot) != unsigned(id)) { assert(slot < 7); ++slot; }
    J3DLightInfo out{};
    assert(dKy_getNativeGlobalLight(slot, out) == reloading);
    if (reloading) assert(out == previous[slot]);
}
}
'''

CHECKS = r'''
static void near(f32 got, f32 expected) {
    assert(std::fabs(got - expected) <= 0.00001f * (1 + std::fabs(expected)));
}
static void vector(const Vec& got, f32 x, f32 y, f32 z) {
    near(got.x,x); near(got.y,y); near(got.z,z);
}
int main() {
    // Analytic values at 60 degrees, including SHARP/RING signs.
    const float cosine[7][3]{{1,0,0},{-500,1000,0},{-1,2,0},
        {0,-1,2},{-3,8,-4},{-8,24,-16},{-1,8,-8}};
    const float distance[4][3]{{1,0,0},{1,.1f,0},{1,.05f,.005f},{1,0,.01f}};
    J3DLightInfo out{};
    for (unsigned i=0; i<7; ++i) {
        dKy_GXInitLightSpot(&out,60,u8(i));
        vector(out.mCosAtten,cosine[i][0],cosine[i][1],cosine[i][2]);
    }
    for (unsigned i=0; i<4; ++i) {
        dKy_GXInitLightDistAttn(&out,10,.5f,u8(i));
        vector(out.mDistAtten,distance[i][0],distance[i][1],distance[i][2]);
    }
    for (float cutoff : {-1.f,0.f,91.f}) {
        dKy_GXInitLightSpot(&out,cutoff,GX_SP_COS);
        vector(out.mCosAtten,1,0,0);
    }
    for (float brightness : {-1.f,0.f,1.f}) {
        dKy_GXInitLightDistAttn(&out,10,brightness,GX_DA_STEEP);
        vector(out.mDistAtten,1,0,0);
    }
    dKy_GXInitLightDistAttn(&out,-1,.5f,GX_DA_GENTLE);
    vector(out.mDistAtten,1,0,0);

    // Nothing is published by static initialization or light reset.
    out.mColor = {9,8,7,6};
    J3DLightInfo sentinel{}; sentinel = out;
    for (unsigned i=0; i<8; ++i) {
        assert(!dKy_getNativeGlobalLight(i,out)); assert(out == sentinel);
    }
    dKy_setLight_init();
    for (unsigned i=0; i<8; ++i) {
        assert(!dKy_getNativeGlobalLight(i,out)); assert(out == sentinel);
        LightStatus& light = lightStatusData[i];
        light.position = {float(i+1),float(i+2),float(i+3)};
        light.mLightDir = {float(i)*.25f,-.5f,.75f};
        light.color = {u8(10+i),u8(30+i),u8(50+i),u8(240+i)};
        light.mCutoff = 60; light.mSpotFn = GXSpotFn(i%7);
        light.mRefDist = 10; light.mRefBrightness = .5f;
        light.mDistFn = GXDistAttnFn(i%4);
    }
    dKy_GlobalLight_set();
    for (unsigned i=0; i<8; ++i) {
        assert(dKy_getNativeGlobalLight(i,out));
        vector(out.mLightPosition,8.f-i,float(i)-19, float(i)+33);
        vector(out.mLightDirection,float(i)*.25f,-.5f,.75f);
        assert(out.mColor.r == 10+i && out.mColor.g == 30+i &&
               out.mColor.b == 50+i && out.mColor.a == 240+i);
        vector(out.mCosAtten,cosine[i%7][0],cosine[i%7][1],cosine[i%7][2]);
        vector(out.mDistAtten,distance[i%4][0],distance[i%4][1],distance[i%4][2]);
        previous[i] = out;
    }
    // A declaration or camera mutation does not change the published snapshot.
    lightStatusData[1].position.x += 100;
    lightStatusData[1].mLightDir = {-1,0,0};
    lightStatusData[1].color = {101,102,103,104};
    lightStatusData[1].mRefDist = -10;
    lightStatusData[1].mDistFn = GX_DA_GENTLE;
    j3dSys.view[2][3] += 50;
    assert(dKy_getNativeGlobalLight(1,out)); assert(out == previous[1]);
    reloading = true;
    dKy_GlobalLight_set();
    assert(dKy_getNativeGlobalLight(1,out));
    vector(out.mLightPosition,7,82,84);
    vector(out.mLightDirection,-1,0,0);
    assert(out.mColor.r == 101 && out.mColor.a == 104);
    assert(lightStatusData[1].mRefDist == .000001f);
    vector(out.mDistAtten,1,1000000,0);
    sentinel = out;
    assert(!dKy_getNativeGlobalLight(8,out)); assert(out == sentinel);
    assert(!dKy_getNativeGlobalLight(std::numeric_limits<unsigned>::max(),out));
    assert(out == sentinel);
    dKy_setLight_init();
    for (unsigned i=0; i<8; ++i) {
        assert(!dKy_getNativeGlobalLight(i,out)); assert(out == sentinel);
    }
    reloading = false;
    dKy_GlobalLight_set();
    assert(dKy_getNativeGlobalLight(1,out));
    assert(out.mColor.r == 255 && out.mColor.a == 255);
    std::puts("global lights: coefficients, eight view-space slots, direction, publication/reset passed");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    code = (SOURCE / 'src/d/d_kankyo.cpp').read_text()
    header = (SOURCE / 'include/d/d_kankyo.h').read_text()
    wrappers = (SOURCE / 'include/m_Do/m_Do_mtx.h').read_text()
    struct_code = (SOURCE / 'libs/JSystem/src/J3DGraphBase/J3DStruct.cpp').read_text()
    extracted = [definition(header, 'struct LightStatus {', True),
                 definition(code, 'static LightStatus lightStatusBase = {', True),
                 definition(code, 'static u16 lightMaskData[8] = {', True)]
    for start in ['static LightStatus lightStatusData[8];',
                  'static J3DLightInfo nativeGlobalLights[8];',
                  'static bool nativeGlobalLightPublished[8];']:
        begin = code.index(start)
        extracted.append(code[begin:code.index(';', begin)+1] + '\n')
    extracted += [definition(wrappers, 'inline void mDoMtx_multVec('),
                  definition(wrappers, 'inline void cMtx_multVec('),
                  definition(struct_code, 'bool J3DLightInfo::operator==('),
                  definition(struct_code, 'J3DLightInfo& J3DLightInfo::operator=(')]
    for start in ['void dKy_GXInitLightSpot(', 'void dKy_GXInitLightDistAttn(',
                  'void dKy_setLight_init(', 'bool dKy_getNativeGlobalLight(',
                  'void dKy_GlobalLight_set(']:
        extracted.append(definition(code, start))
    harness = out / 'global_lights.cpp'
    harness.write_text(HARNESS + '\n'.join(extracted) + CHECKS)
    math_flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                  '-fno-strict-aliasing', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1',
                  '-DMTX_USE_PS=1']
    flags = [*math_flags, '-DDUSK_PICA_GAME_DRAW=1', '-DDUSK_BUILDING_GAME=1',
             '-DVERSION=0', '-DPARTIAL_DEBUG=1', '-DAVOID_UB=1', '-Wno-multichar']
    for path in ['include', 'libs/JSystem/include', 'extern/aurora/include',
                 'extern/aurora/include/dolphin', 'sdk/include', 'src']:
        flags += ['-I' + str(SOURCE / path)]
    flags += ['-include', str(SOURCE / 'extern/aurora/include/dolphin/types.h'),
              '-include', str(SOURCE / 'include/global.h')]
    matrix = out / 'mtxvec.o'
    subprocess.run(['clang', '-std=c11', *math_flags,
                    '-I' + str(SOURCE / 'extern/aurora/include'), '-c',
                    str(SOURCE / 'extern/aurora/lib/dolphin/mtx/mtxvec.c'),
                    '-o', str(matrix)], check=True)
    binary = out / 'test'
    subprocess.run(['clang++', '-std=c++20', *flags, str(harness), str(matrix),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
