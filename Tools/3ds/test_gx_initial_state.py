#!/usr/bin/env python3
"""Exercise actual GX initialization and the first untextured original fader."""
import argparse, hashlib, json, os, subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);args=p.parse_args()
out=args.output;out.mkdir(parents=True,exist_ok=False)
native=root/'Source/platforms/3ds';aurora=root/'Source/extern/aurora'
names=['runtime/gx_initial_state.cpp','runtime/gx_queries.cpp','runtime/gx_pixel_commands.cpp',
       'runtime/gx_material.cpp','runtime/gx_pica_material.cpp','runtime/gx_raster_plan.cpp',
       'runtime/gx_raster_color.cpp','runtime/gx_vertex_transform.cpp','runtime/gx_pica_math.cpp',
       'runtime/gx_transform_commands.cpp','runtime/gx_vertex_commands.cpp','runtime/gx_lighting_commands.cpp',
       'runtime/gx_tev_commands.cpp','gx_api.cpp','gx_command_api.cpp','runtime/gx_state.cpp',
       'runtime/gx_commands.cpp','runtime/gx_stream.cpp','runtime/gx_vertex_decode.cpp',
       'runtime/resource_archive.cpp','tests/gx_initial_state_test.cpp']
sources=[native/x for x in names]+[aurora/x for x in ['lib/gx/dl.cpp','lib/gx/attr_fmt.cpp']]
inputs=[Path(__file__).resolve(),*sources,*native.glob('runtime/gx_*.hpp')]
hashes={str(x.relative_to(root)):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs}
common=['-std=c++20','-DTARGET_PC=1','-DDUSK_TARGET_3DS=1','-I'+str(aurora/'include'),'-Wall','-Wextra','-Werror','-Wno-switch']
commands=[['clang++',*common,'-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',*map(str,sources),'-o',str(out/'test')]]
commands.append(['/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++',*common,'-O2','-g','-march=armv6k','-mtune=mpcore','-mfloat-abi=hard','-mfpu=vfp','-mtp=soft','-D__3DS__','-fstack-usage','-c',str(native/names[0]),'-o',str(out/'gx_initial_state-arm.o')])
receipt={'status':'running','commands':commands,'input_sha256':hashes,'origin':'Host ASan/UBSan actual command planners and ARM compile','limitations':['No GPU, original scene process or physical New3DS execution','Resource bindings remain explicit; no default texture is invented']}
try:
    with (out/'build.log').open('w') as log:
        for command in commands:subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=90)
    result=subprocess.run([str(out/'test')],capture_output=True,text=True,timeout=60,env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
    (out/'test.log').write_text(result.stdout+result.stderr);print(result.stdout+result.stderr,end='');result.check_returncode()
    assert hashes=={str(x.relative_to(root)):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs}
    receipt.update(status='pass',input_unchanged=True,arm_stack_usage={x.name:x.read_text() for x in out.glob('*.su')})
except (OSError,subprocess.SubprocessError,AssertionError) as error:
    receipt.update(status='fail',error=str(error));print((out/'build.log').read_text())
finally:(out/'results.json').write_text(json.dumps(receipt,indent=2)+'\n')
raise SystemExit(receipt['status']!='pass')
