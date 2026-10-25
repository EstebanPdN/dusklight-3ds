#!/usr/bin/env python3
"""Verify original J3D texture lists resolve native registered image tokens."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT/'Diagnostics/implementation-21/texture-commands'


def main():
    global OUT
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=OUT)
    OUT=parser.parse_args().output.resolve()
    OUT.mkdir(parents=True,exist_ok=False)
    native=ROOT/'Source/platforms/3ds'; aurora=ROOT/'Source/extern/aurora'
    common=['-std=c++20','-O1','-g','-DTARGET_PC=1','-DDUSK_TARGET_3DS=1',
            '-Wall','-Wextra','-Werror','-Wno-switch','-Wno-unused-parameter','-Wno-invalid-utf8','-fsanitize=address,undefined','-fno-omit-frame-pointer','-Wno-invalid-noreturn']
    includes=['Source/include','Source/libs/JSystem/include','Source/extern/aurora/include',
              'Source/extern/aurora/include/dolphin','Source/sdk/include','Source/src']
    common += ['-I'+str(ROOT/p) for p in includes]
    game=[*[f for f in common if f not in ('-Wall','-Wextra','-Werror')],'-DDUSK_BUILDING_GAME=1','-DVERSION=0','-DPARTIAL_DEBUG=1','-DAVOID_UB=1','-DMTX_USE_PS=1',
          '-include',str(aurora/'include/dolphin/types.h'),'-include',str(ROOT/'Source/include/global.h'),
          '-Wno-multichar','-Wno-deprecated-declarations']
    original=[ROOT/'Source/libs/JSystem/src/J3DGraphBase/J3DGD.cpp',native/'tests/gx_texture_commands_test.cpp']
    sources=[native/'runtime/gx_texture_commands.cpp',native/'runtime/gx_state.cpp',native/'runtime/resource_archive.cpp',native/'runtime/texture_gx.cpp',native/'runtime/gx_commands.cpp',native/'runtime/gx_vertex_decode.cpp',native/'runtime/gd_native.cpp',aurora/'lib/gx/dl.cpp',aurora/'lib/gx/attr_fmt.cpp']
    sources += [native/'runtime/gx_texture_cache.cpp',native/'runtime/gx_stream.cpp',native/'gx_command_api.cpp']
    # Compare the real native descriptor with the pinned original pure owner.
    # Only its CPU region layout/initializer is extracted, without desktop/PPC
    # GPU dependencies. Keep generated reference code outside Source.
    reference_input=ROOT/'Source/libs/dolphin/src/gx/GXTexture.c'
    text=reference_input.read_text()
    struct_start=text.index('typedef struct __GXTexRegionInt_struct')
    struct_end=text.index('} __GXTexRegionInt;',struct_start)+len('} __GXTexRegionInt;')
    start=text.index('void GXInitTexCacheRegion(')
    body=text.index('{',start);depth=1;end=body+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    reference=OUT/'original-cache-region.cpp'
    reference.write_text('''#include <dolphin/gx.h>
#include <stdexcept>
#define ASSERTMSGLINE(line,condition,message) do {if(!(condition))throw std::runtime_error(message);}while(0)
#define ASSERTMSGLINEV(line,condition,...) do {if(!(condition))throw std::runtime_error("original region rejected");}while(0)
#define CHECK_GXBEGIN(line,message) do {}while(0)
#define SET_REG_FIELD(line,reg,width,shift,value) reg=(reg&~(((1u<<width)-1)<<shift))|((u32(value)&((1u<<width)-1))<<shift)
'''+text[struct_start:struct_end]+'\n'+text[start:end].replace('GXInitTexCacheRegion','OriginalGXInitTexCacheRegion',1)+'\n')
    objects=[OUT/(p.stem+'-host.o') for p in original]
    commands=[['clang++',*game,'-c',str(s),'-o',str(o)] for s,o in zip(original,objects)]
    commands.append(['clang++',*common,*map(str,sources),*map(str,objects),str(reference),'-Wl,-dead_strip','-o',str(OUT/'test')])
    arm='/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++'
    flags=['-std=c++20','-O2','-g','-march=armv6k','-mtune=mpcore','-mfloat-abi=hard','-mfpu=vfp',
           '-mtp=soft','-D__3DS__','-DTARGET_PC=1','-DDUSK_TARGET_3DS=1','-I'+str(aurora/'include')]
    for source in [sources[0],native/'runtime/gx_texture_cache.cpp']:
        commands.append([arm,*flags,'-Wall','-Wextra','-Werror','-fstack-usage','-c',str(source),'-o',str(OUT/(source.stem+'-arm.o'))])
    headers=[native/'runtime/gx_texture_commands.hpp',native/'runtime/gx_state.hpp',native/'runtime/gx_stream.hpp',native/'runtime/gx_commands.hpp',native/'runtime/gx_vertex_decode.hpp',aurora/'include/aurora/dl.hpp',aurora/'include/dolphin/gd/GDBase.h',aurora/'include/dolphin/os.h',
             ROOT/'Source/libs/JSystem/include/JSystem/J3DGraphBase/J3DPacket.h',
             ROOT/'Source/libs/JSystem/include/JSystem/J3DGraphBase/J3DFifo.h',
             ROOT/'Source/libs/JSystem/include/JSystem/J3DGraphBase/J3DGD.h']
    inputs=[*original,*sources,*headers,Path(__file__).resolve()]
    inputs += [native/'runtime/gx_copy_setup.hpp',reference_input]
    hashes={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    receipt={'status':'running','origin':'host ASan/UBSan original J3D texture list encoding and native resource decoding; ARM compilation only',
             'commands':commands,'input_sha256':hashes,
             'limits':['synthetic registered resources, no GPU/hardware draw or original full model','Original cached TMEM descriptors map to the actual bounded converted-image cache; preload rejects, immutable embedded palettes retain ordered TLUT selection','No raw ARM pointer in BP image3/TLUT; no native physical TMEM partition claim'],
             'reference':str(reference.relative_to(ROOT)),
             'reference_sha256':hashlib.sha256(reference.read_bytes()).hexdigest()}
    report=OUT/'results.json';report.write_text(json.dumps(receipt)+'\n')
    try:
        with (OUT/'build.log').open('w') as log:
            for cmd in commands:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=90)
        run=subprocess.run([str(OUT/'test')],capture_output=True,text=True,timeout=60,
                           env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1',
                                'UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
        (OUT/'test.log').write_text(run.stdout+run.stderr);print(run.stdout+run.stderr,end='');run.check_returncode()
        assert hashes=={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        receipt.update(status='pass',input_unchanged=True)
    except (subprocess.SubprocessError,AssertionError) as error:
        receipt.update(status='fail',error=str(error));print((OUT/'build.log').read_text())
    finally:report.write_text(json.dumps(receipt,indent=2)+'\n')
    return int(receipt['status']!='pass')

if __name__=='__main__':raise SystemExit(main())
