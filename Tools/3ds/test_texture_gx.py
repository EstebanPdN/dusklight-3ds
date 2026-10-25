#!/usr/bin/env python3
"""Compare native PICA texture storage with unchanged pinned Aurora decoder functions.

The temporary oracle wraps public source excerpts; no desktop/GPU dependency is
linked. Owner model texels remain transient. tex3ds is used as an installed CLI
format oracle on synthetic pixels only; no tex3ds source is copied or linked.
"""
import ast
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'Diagnostics/implementation-21/texture'
def main():
    global OUT
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    if args.output:OUT=args.output.resolve()
    OUT.mkdir(parents=True,exist_ok=not bool(args.output))
    native=ROOT/'Source/platforms/3ds'
    original=ROOT/'Source/extern/aurora/lib/gfx/texture_convert.cpp'
    source=original.read_text()
    section=source[source.index('template <uint8_t v>'):source.index('static ByteBuffer BuildRGBA8FromBC1')]
    oracle=OUT/'original_decoder.cpp'
    prefix='''#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>
#include <stdexcept>
using namespace std;
struct ByteBuffer:vector<uint8_t>{explicit ByteBuffer(size_t n):vector<uint8_t>(n){}};
template<class T>using ArrayRef=span<const T>;
static uint16_t bswap(uint16_t v){return (v>>8)|(v<<8);}
struct RGBA8{uint8_t r,g,b,a;};
'''
    suffix='''
vector<uint8_t> originalTexture(unsigned f,unsigned w,unsigned h,span<const uint8_t> input){
  vector<uint8_t> aligned(input.begin(),input.end());span<const uint8_t> b(aligned);
  switch(f){
    case 0:return DecodeTiled<TextureDecoderI4>(w,h,1,b);
    case 1:return DecodeTiled<TextureDecoderI8>(w,h,1,b);
    case 2:return DecodeTiled<TextureDecoderIA4>(w,h,1,b);
    case 3:return DecodeTiled<TextureDecoderIA8>(w,h,1,b);
    case 4:return DecodeTiled<TextureDecoderRGB565>(w,h,1,b);
    case 5:return DecodeTiled<TextureDecoderRGB5A3>(w,h,1,b);
    case 6:return BuildRGBA8FromGCN(w,h,1,b);
    case 14:return BuildRGBA8FromCMPR(w,h,1,b);
    default:throw runtime_error("unsupported oracle format");
  }
}
vector<uint8_t> originalPaletteTexture(unsigned f,unsigned w,unsigned h,span<const uint8_t> input,
                                      unsigned pf,unsigned count,span<const uint8_t> palette){
  vector<uint8_t> aligned(input.begin(),input.end()), palAligned(palette.begin(),palette.end());
  ByteBuffer indices(0), colors(0);
  switch(f){
    case 8:indices=DecodeTiled<TextureDecoderC4>(w,h,1,aligned);break;
    case 9:indices=DecodeTiled<TextureDecoderC8>(w,h,1,aligned);break;
    case 10:indices=DecodeTiled<TextureDecoderC14X2>(w,h,1,aligned);break;
    default:throw runtime_error("oracle CI format");
  }
  switch(pf){
    case 0:colors=DecodeLinear<TextureDecoderIA8>(count,palAligned);break;
    case 1:colors=DecodeLinear<TextureDecoderRGB565>(count,palAligned);break;
    case 2:colors=DecodeLinear<TextureDecoderRGB5A3>(count,palAligned);break;
    default:throw runtime_error("oracle TLUT format");
  }
  vector<uint8_t> output(size_t(w)*h*4);
  for(size_t n=0;n<size_t(w)*h;++n){uint16_t index;memcpy(&index,indices.data()+n*2,2);
    if(index<count)memcpy(output.data()+n*4,colors.data()+index*4,4);
  }
  return output;
}
'''
    oracle.write_text(prefix+section+suffix)
    inputs=[native/'runtime/texture_gx.cpp',native/'runtime/texture_gx.hpp',native/'tests/texture_gx_test.cpp',native/'runtime/disc.cpp',native/'runtime/disc.hpp',native/'runtime/resource_archive.cpp',native/'runtime/resource_archive.hpp',original,Path(__file__).resolve()]
    hashes={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    common=['-std=c++20','-Wall','-Wextra','-Werror']
    commands=[['clang++',*common,'-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(inputs[0]),str(inputs[2]),str(inputs[3]),str(inputs[5]),str(oracle),'-o',str(OUT/'test')],['/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++',*common,'-O2','-march=armv6k','-mtune=mpcore','-mfloat-abi=hard','-mfpu=vfp','-mtp=soft','-fstack-usage','-c',str(inputs[0]),'-o',str(OUT/'texture-arm.o')]]
    receipt={'status':'running','input_sha256':hashes,'commands':commands,'origin':'host sanitizer comparison and ARM compilation only; no GPU acceptance','oracle':'unchanged pinned decoder excerpt with standard-library wrapper','payload_saved':False}
    try:
        with (OUT/'build.log').open('w') as log:
            for command in commands:subprocess.run(command,check=True,stdout=log,stderr=subprocess.STDOUT,timeout=90)
        iso=ROOT/'twilight.iso';before=iso.stat()
        result=subprocess.run([str(OUT/'test'),str(iso)],text=True,capture_output=True,timeout=90,env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
        (OUT/'test.log').write_text(result.stdout+result.stderr);print(result.stdout+result.stderr,end='');result.check_returncode()
        after=iso.stat();assert (before.st_size,before.st_mtime_ns)==(after.st_size,after.st_mtime_ns)
        fixture=OUT/'layout.pam';fixture.write_bytes(b'P7\nWIDTH 8\nHEIGHT 8\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n'+bytes([170,170,170,51])*64)
        cli=[]
        for fmt,pixel in [('la4',bytes([0xa3])),('la8',bytes([0x33,0xaa])),('rgba8',bytes([0x33,0xaa,0xaa,0xaa]))]:
            output=OUT/(fmt+'.bin');cmd=['/opt/devkitpro/tools/bin/tex3ds','-f',fmt,'-r','-z','none','-o',str(output),str(fixture)]
            subprocess.run(cmd,check=True,capture_output=True,timeout=30)
            data=output.read_bytes();assert data[0]==0 and int.from_bytes(data[1:4],'little')==len(pixel)*64 and data[4:]==pixel*64;cli.append(cmd)
        assert hashes=={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        receipt.update(status='pass',input_unchanged=True,iso_size_mtime_unchanged=True,metrics=[json.loads(line) for line in result.stdout.splitlines()],tex3ds_commands=cli,arm_stack_usage=(OUT/'texture-arm.su').read_text())
    except (OSError,subprocess.SubprocessError,AssertionError) as error:
        receipt.update(status='fail',error=str(error));print((OUT/'build.log').read_text())
    finally:(OUT/'results.json').write_text(json.dumps(receipt,indent=2)+'\n')
    return int(receipt['status']!='pass')
if __name__=='__main__':raise SystemExit(main())
