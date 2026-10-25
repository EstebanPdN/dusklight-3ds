#!/usr/bin/env python3
"""Verify actual native typed GX draws and pixel commands against original GD."""
import argparse,hashlib,json,os,re,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
out=a.output;out.mkdir(parents=True,exist_ok=False)
n=ROOT/'Source/platforms/3ds';aurora=ROOT/'Source/extern/aurora'
sources=[n/x for x in ['runtime/gx_pixel_commands.cpp','runtime/gx_stream.cpp','runtime/gx_tev_commands.cpp','gx_api.cpp','gx_command_api.cpp','runtime/gx_raster_plan.cpp','runtime/gx_pica_math.cpp','runtime/gx_transform_commands.cpp','runtime/gx_vertex_commands.cpp','runtime/gx_lighting_commands.cpp','runtime/gx_state.cpp','runtime/gx_commands.cpp','runtime/gx_vertex_decode.cpp','runtime/resource_archive.cpp','runtime/gd_native.cpp','tests/gx_typed_draw_pixel_test.cpp']]+[aurora/x for x in ['lib/gx/dl.cpp','lib/gx/attr_fmt.cpp','lib/dolphin/gd/GDGeometry.cpp','lib/dolphin/gd/GDPixel.cpp']]
sources.append(n/'runtime/gx_framebuffer_scale.cpp')
headers=[n/x for x in ['runtime/gx_stream.hpp','runtime/gx_raster_plan.hpp','runtime/gx_commands.hpp','runtime/gx_copy_setup.hpp','runtime/gx_state.hpp','runtime/gx_vertex_decode.hpp']]+[aurora/x for x in ['include/dolphin/gx/GXEnum.h','include/dolphin/gx/GXVert.h','include/dolphin/gx/GXPixel.h','include/dolphin/gd/GDPixel.h']]
inputs=[Path(__file__).resolve(),*sources,*headers];hashes={str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs}
# Compare native allocation sizes with the exact pinned Aurora implementation.
# Extract only its pure function: compiling the full owner pulls desktop GPU
# dependencies. The generated reference lives in the receipt, outside Source.
reference_input=aurora/'lib/dolphin/gx/GXTexture.cpp'
reference_text=reference_input.read_text();start=reference_text.index('u32 GXGetTexBufferSize(')
body=reference_text.index('{',start);depth=1;end=body+1
while depth:
    depth+=(reference_text[end]=='{')-(reference_text[end]=='}');end+=1
reference=out/'original-texture-size.cpp'
reference.write_text('#include <dolphin/gx.h>\n'+reference_text[start:end].replace('GXGetTexBufferSize','OriginalGXGetTexBufferSize',1)+'\n')
inputs.append(reference_input);hashes[str(reference_input.relative_to(ROOT))]=hashlib.sha256(reference_input.read_bytes()).hexdigest()
def function(source,name):
 text=source.read_text();start=re.search(r'\b'+re.escape(name)+r'\(',text).start();start=text.rfind('\n',0,start)+1
 body=text.index('{',start);depth=1;end=body+1
 while depth:
  depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
frame_source=ROOT/'Source/libs/dolphin/src/gx/GXFrameBuf.c'
frame_reference=out/'original-copy-scale.cpp'
frame_text='\n'.join(function(frame_source,x) for x in ['__GXGetNumXfbLines','GXGetNumXfbLines','GXGetYScaleFactor'])
for name in ['__GXGetNumXfbLines','GXGetNumXfbLines','GXGetYScaleFactor']:
 frame_text=re.sub(r'\b'+re.escape(name)+r'\b','Original'+name,frame_text)
frame_reference.write_text('#include <dolphin/gx.h>\n#include <stdexcept>\n#define ASSERTMSGLINE(line,condition,message) do { if(!(condition))throw std::runtime_error(message); } while(0)\n'+frame_text+'\n')
pixel_source=aurora/'lib/dolphin/gx/GXPixel.cpp';pixel_reference=out/'original-pixel-state.cpp'
pixel_functions='\n'.join(function(pixel_source,x).replace(x,'Original'+x) for x in ['GXSetAlphaUpdate','GXSetDstAlpha','GXSetPixelFmt'])
pixel_reference.write_text('''#include <dolphin/gx.h>
#include <stdexcept>
struct PixelState { u32 cmode0,cmode1,peCtrl,genMode,dirtyState,bpSent; } state;
PixelState* __gx=&state;
#define SET_REG_FIELD(line,reg,width,shift,value) reg=(reg&~(((1u<<width)-1)<<shift))|((u32(value)&((1u<<width)-1))<<shift)
#define GX_WRITE_RAS_REG(reg) do {} while(0)
#define UNLIKELY
#define FATAL(...) throw std::runtime_error("original pixel format")
void OriginalPixelReset(u32 c0,u32 c1,u32 pe,u32 gen){state={c0,c1,pe,gen,0,0};}
u32 OriginalPixelRegister(unsigned r){switch(r){case 0:return state.genMode;case 0x41:return state.cmode0;case 0x42:return state.cmode1;case 0x43:return state.peCtrl;default:throw std::runtime_error("original register");}}
'''+pixel_functions+'\n')
for ref_input in [frame_source,pixel_source]:
 inputs.append(ref_input);hashes[str(ref_input.relative_to(ROOT))]=hashlib.sha256(ref_input.read_bytes()).hexdigest()
common=['-std=c++20','-DTARGET_PC=1','-DDUSK_TARGET_3DS=1','-I'+str(aurora/'include'),'-Wall','-Wextra','-Werror','-Wno-switch']
commands=[['clang++',*common,'-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',*map(str,sources),str(reference),str(frame_reference),str(pixel_reference),'-o',str(out/'test')]]
arm=['/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++',*common,'-O2','-g','-march=armv6k','-mtune=mpcore','-mfloat-abi=hard','-mfpu=vfp','-mtp=soft','-D__3DS__','-fstack-usage']
for s in [*sources[:5],n/'runtime/gx_framebuffer_scale.cpp']:commands.append([*arm,'-c',str(s),'-o',str(out/(s.stem+'-arm.o'))])
receipt={'status':'running','origin':'host ASan/UBSan actual native command execution and ARM compile; not GPU or hardware acceptance','commands':commands,'input_sha256':hashes,'limitations':['Fog commands are preserved; fog rendering/range correction still need PICA integration','Caller-owned scratch bounds a typed draw; no per-vertex allocation','GXFlush is command completion, not a GPU wait']}
report=out/'results.json';report.write_text(json.dumps(receipt,indent=2)+'\n')
try:
 with (out/'build.log').open('w') as log:
  for cmd in commands:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=90)
 r=subprocess.run([str(out/'test')],capture_output=True,text=True,timeout=60,env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
 (out/'test.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='');r.check_returncode()
 assert hashes=={str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs}
 receipt.update(status='pass',input_unchanged=True,arm_stack_usage={x.name:x.read_text() for x in out.glob('*.su')})
except (OSError,subprocess.SubprocessError,AssertionError) as error:
 receipt.update(status='fail',error=str(error));print((out/'build.log').read_text())
finally:report.write_text(json.dumps(receipt,indent=2)+'\n')
raise SystemExit(receipt['status']!='pass')
