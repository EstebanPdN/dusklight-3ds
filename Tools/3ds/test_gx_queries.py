#!/usr/bin/env python3
"""Verify actual native state queries/restoration and raw-list changes."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
out=a.output;out.mkdir(parents=True,exist_ok=False)
n=ROOT/'Source/platforms/3ds';aurora=ROOT/'Source/extern/aurora'
sources=[n/x for x in ['runtime/gx_pixel_commands.cpp','runtime/gx_stream.cpp','runtime/gx_queries.cpp','runtime/gx_tev_commands.cpp','gx_api.cpp','gx_command_api.cpp','runtime/gx_raster_plan.cpp','runtime/gx_pica_math.cpp','runtime/gx_transform_commands.cpp','runtime/gx_vertex_commands.cpp','runtime/gx_lighting_commands.cpp','runtime/gx_state.cpp','runtime/gx_commands.cpp','runtime/gx_vertex_decode.cpp','runtime/resource_archive.cpp','runtime/gd_native.cpp','tests/gx_queries_test.cpp']]+[aurora/x for x in ['lib/gx/dl.cpp','lib/gx/attr_fmt.cpp','lib/dolphin/gd/GDGeometry.cpp','lib/dolphin/gd/GDPixel.cpp','lib/dolphin/gd/GDTev.cpp','lib/dolphin/gd/GDIndirect.cpp']]
sources.append(aurora/'lib/dolphin/gx/GXDraw.cpp')
sources += [n/'runtime/gx_gpu_sync.cpp', n/'tests/gpu_sync_test.cpp']
headers=[n/x for x in ['runtime/gx_tev_commands.hpp','runtime/gx_stream.hpp','runtime/gx_raster_plan.hpp','runtime/gx_commands.hpp','runtime/gx_state.hpp','runtime/gx_vertex_decode.hpp']]+[aurora/x for x in ['include/dolphin/gx/GXEnum.h','include/dolphin/gx/GXVert.h','include/dolphin/gx/GXPixel.h','include/dolphin/gd/GDPixel.h']]
headers += [n/'runtime/gpu_sync.hpp',n/'runtime/gx_copy_setup.hpp',n/'gx_texture_pica.hpp']
inputs=[Path(__file__).resolve(),*sources,*headers,n/'runtime/gpu_sync_ctru.cpp',n/'runtime/clock.hpp'];hashes={str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs}
common=['-std=c++20','-DTARGET_PC=1','-DDUSK_TARGET_3DS=1','-I'+str(aurora/'include'),'-Wall','-Wextra','-Werror','-Wno-switch']
commands=[['clang++',*common,'-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',*map(str,sources),'-o',str(out/'test')]]
arm=['/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++',*common,'-O2','-g','-march=armv6k','-mtune=mpcore','-mfloat-abi=hard','-mfpu=vfp','-mtp=soft','-D__3DS__','-fstack-usage']
for s in [n/'runtime/gx_queries.cpp',n/'runtime/gx_state.cpp',aurora/'lib/dolphin/gx/GXDraw.cpp']:commands.append([*arm,'-c',str(s),'-o',str(out/(s.stem+'-arm.o'))])
for s in [n/'runtime/gx_gpu_sync.cpp',n/'runtime/gpu_sync_ctru.cpp']:
 commands.append([*arm,'-I/opt/devkitpro/libctru/include','-c',str(s),'-o',str(out/(s.stem+'-arm.o'))])
receipt={'status':'running','origin':'host ASan/UBSan actual native command execution and ARM compile; not GPU or hardware acceptance','commands':commands,'input_sha256':hashes,'limitations':['Reads real command registers or real immediate state; no guessed defaults for missing state','No allocation in typed setters or queries; GPU submission/performance is not exercised','GXFlush is command completion, not a GPU wait']}
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
