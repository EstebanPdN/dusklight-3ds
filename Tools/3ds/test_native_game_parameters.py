#!/usr/bin/env python3
"""Value regression for the actual J3D native exporter; host arithmetic only."""
import argparse
from pathlib import Path
import subprocess
from native_game_source_fixture import body

ROOT = Path(__file__).resolve().parents[2]


def fixture():
    base = ROOT / 'Source/libs/JSystem'
    structs = (base / 'include/JSystem/J3DGraphBase/J3DStruct.h').read_text()
    blocks = (base / 'include/JSystem/J3DGraphBase/J3DMatBlock.h').read_text()
    texture = (base / 'include/JSystem/J3DGraphBase/J3DTexture.h').read_text()
    packet = (base / 'include/JSystem/J3DGraphBase/J3DPacket.h').read_text()
    model = (base / 'src/J3DGraphAnimator/J3DModel.cpp').read_text()
    original_matrix = (ROOT / 'Source/libs/dolphin/src/mtx/mtx.c').read_text()
    original_transform = (base / 'src/J3DGraphBase/J3DTransform.cpp').read_text()
    enums = (ROOT / 'Source/libs/dolphin/include/dolphin/gx/GXEnum.h').read_text()
    pieces = ['''#pragma once
#include "native/game_source.hpp"
#include "native/camera.hpp"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#define BE(T) T
#define J3D_ASSERT_RANGE(line, condition) assert(condition)
#define ASSERTMSGLINE(line, condition, ...) assert(condition)
#define __memcpy fixtureMemcpy
#define TARGET_PC 1
#define DEBUG 1
#define ASM
#define __REGISTER [[maybe_unused]]
#define J3D_sqrtf sqrtf
#define VECNormalize C_VECNormalize
namespace nativeParametersFixture {
using u8=std::uint8_t;using s8=std::int8_t;using u16=std::uint16_t;using s16=std::int16_t;using s32=std::int32_t;using u32=std::uint32_t;using f32=float;
using Mtx=float[3][4];using Mtx44=float[4][4];using MtxPtr=float (*)[4];
struct Vec {float x{},y{},z{};};
struct GXColor {u8 r{},g{},b{},a{};};
struct GXColorS10 {s16 r{},g{},b{},a{};};
using J3DGXColor=GXColor;using J3DGXColorS10=GXColorS10;
struct GXFogAdjTable {u16 r[10]{};};
inline void* fixtureMemcpy(void* dst,const void* src,std::size_t n){return std::memcpy(dst,src,n);}
namespace JMath {
inline void gekko_ps_copy6(void* dst,const void* src){std::memcpy(dst,src,24);}
inline void gekko_ps_copy12(void* dst,const void* src){std::memcpy(dst,src,48);}
inline float fastSqrt(float value){return std::sqrt(value);}
}
// Host stand-ins for the platform trig table; rotation-zero cases below are exact.
inline float JMASSin(s16 angle){return std::sin(float(angle)*(6.2831853071795864769f/65536));}
inline float JMASCos(s16 angle){return std::cos(float(angle)*(6.2831853071795864769f/65536));}
''']
    for name in ['GXAttr', 'GXTexMtx', 'GXTexGenType', 'GXTexGenSrc', 'GXColorSrc', 'GXDiffuseFn', 'GXAttnFn']:
        # Include the brace: GXTexMtx is a prefix of GXTexMtxType.
        pieces.append(body(enums, 'typedef enum _' + name + ' {') + ' ' + name + ';\n')
    for signature in ['struct J3DLightInfo', 'struct J3DTextureSRTInfo', 'enum J3DTexMtxMode',
                      'struct J3DTexMtxInfo', 'struct J3DFogInfo', 'struct J3DColorChanInfo', 'struct J3DTexCoordInfo']:
        pieces.append(body(structs, signature) + ';\n')
    pieces.append(body((base / 'src/J3DGraphBase/J3DStruct.cpp').read_text(), 'J3DLightInfo& J3DLightInfo::operator=('))
    pieces.append('inline const J3DColorChanInfo j3dDefaultColorChanInfo{};\n')
    pieces.append(body(blocks, 'inline u16 calcColorChanID('))
    channel = body(blocks, 'class J3DColorChan {')
    # Loading commands is deliberately outside this parameter-only fixture.
    channel = channel.replace(body(channel, 'void load() const').strip(), 'void load() const;')
    pieces.append(channel + ';\n')
    pieces.append('inline const J3DTexCoordInfo j3dDefaultTexCoordInfo[8]{};\n')
    pieces.append(body(texture, 'struct J3DTexCoord :') + ';\n')
    for signature in ['void C_MTXIdentity(', 'void C_MTXCopy(', 'void C_MTXConcat(', 'u32 C_MTXInverse(']:
        pieces.append(body(original_matrix, signature))
    pieces.append('''#define MTXIdentity C_MTXIdentity
#define MTXCopy C_MTXCopy
#define MTXConcat C_MTXConcat
#define MTXInverse C_MTXInverse
''')
    pieces.append(body((ROOT / 'Source/libs/dolphin/src/mtx/vec.c').read_text(), 'void C_VECNormalize('))
    for signature in ['void J3DCalcBBoardMtx(', 'void J3DCalcYBBoardMtx(',
                      'void J3DGetTextureMtx(', 'void J3DGetTextureMtxOld(',
                      'void J3DGetTextureMtxMaya(', 'void J3DGetTextureMtxMayaOld(',
                      'ASM void J3DMtxProjConcat(']:
        pieces.append(body(original_transform, signature))
    pieces.append(body(packet, 'class J3DTexMtxObj {') + ';\n')
    pieces.append(body((base / 'include/JSystem/JUtility/JUTTexture.h').read_text(), 'struct ResTIMG {') + ';\n')
    # Containers only supply authored declarations. All bitfield interpretation,
    # coefficient conversion, compaction, source-space choice and affine math
    # below are extracted from production, never recreated in these facades.
    pieces.append('''using J3DFog=J3DFogInfo;
struct J3DLightObj {J3DLightInfo info{};J3DLightInfo* getLightInfo(){return &info;}};
struct J3DTexMtx {J3DTexMtxInfo info{};Mtx matrix{};J3DTexMtxInfo& getTexMtxInfo(){return info;}Mtx& getMtx(){return matrix;}};
struct J3DColorBlock {
    unsigned count=2;J3DColorChan channels[4];J3DGXColor material[2]{},ambient[2]{};J3DLightObj* lights[8]{};
    unsigned getColorChanNum(){return count;}J3DColorChan* getColorChan(unsigned i){return &channels[i];}
    J3DGXColor* getMatColor(unsigned i){return &material[i];}J3DGXColor* getAmbColor(unsigned i){return &ambient[i];}
    J3DLightObj* getLight(unsigned i){return lights[i];}
};
struct J3DMaterial {
    J3DColorBlock color;J3DGXColorS10 registers[3]{};J3DGXColor konst[4]{};J3DFog fog{};bool hasFog=true;
    J3DTexCoord coords[8];J3DTexMtx matrices[8];unsigned generators=8;
    J3DColorBlock* getColorBlock(){return &color;}void* getTevBlock(){return this;}void* getPEBlock(){return this;}
    J3DGXColorS10* getTevColor(unsigned i){return &registers[i];}J3DGXColor* getTevKColor(unsigned i){return &konst[i];}
    J3DFog* getFog(){return hasFog?&fog:nullptr;}
    unsigned getTexGenNum(){return generators;}J3DTexCoord* getTexCoord(unsigned i){return &coords[i];}
    J3DTexMtx* getTexMtx(unsigned i){return &matrices[i];}
};
struct J3DModel {static bool copyNativeMaterial(J3DMaterial&,const dusk::pica::UniformBinding&,dusk::pica::DrawUniforms&);};
inline std::array<J3DLightInfo,8> globalLights{};
inline unsigned availableLights{},globalReads{};
inline bool dKy_getNativeGlobalLight(unsigned slot,J3DLightInfo& out){
    globalReads|=1u<<slot;if(!(availableLights&(1u<<slot)))return false;out=globalLights[slot];return true;
}
struct System {Mtx view{};Mtx& getViewMtx(){return view;}};
inline System j3dSys;
''')
    for signature in ['u32 nativeColor(', 'dusk::pica::Row nativeColorRow(', 'bool nativeFog(',
                      'bool nativeSampler(', 'bool nativeBillboard(', 'bool nativeTexGen(', 'bool J3DModel::copyNativeMaterial(']:
        pieces.append(body(model, signature))
    pieces.append('''using dusk::pica::DrawUniforms;
using dusk::pica::CameraRegion;using dusk::pica::CameraProjection;
inline bool initialized=true,lifetimeFailure=false,cameraValid=true;
struct Draw {bool active=true;bool frameActive(){return active;}};
inline Draw nativeDraw;
inline int output;
inline void* currentOutput=&output;inline void* mainOutput=&output;
inline CameraRegion cameraRegion;inline CameraProjection camera;
''')
    pieces.append(body((ROOT / 'Source/platforms/3ds/native/game_draw.cpp').read_text(), 'bool applyCamera('))
    pieces.append('}\n#undef BE\n#undef J3D_ASSERT_RANGE\n#undef ASSERTMSGLINE\n#undef __memcpy\n#undef MTXIdentity\n#undef MTXCopy\n#undef MTXConcat\n#undef MTXInverse\n#undef TARGET_PC\n#undef DEBUG\n#undef ASM\n#undef __REGISTER\n#undef J3D_sqrtf\n#undef VECNormalize\n')
    return ''.join(pieces)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / 'native_game_parameters_fixture.hpp').write_text(fixture())
    native = ROOT / 'Source/platforms/3ds'
    command = ['clang++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined',
               '-I' + str(output), '-I' + str(native), str(native / 'native/contracts.cpp'),
               str(native / 'tests/native_game_parameters_test.cpp'), '-o', str(output / 'native-game-parameters-test')]
    subprocess.run(command, check=True)
    subprocess.run([str(output / 'native-game-parameters-test')], check=True)


if __name__ == '__main__':
    main()
