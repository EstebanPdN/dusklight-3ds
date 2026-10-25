#!/usr/bin/env python3
"""Host-only exact alpha interval oracle, real backend emission, and adapter regressions."""
import hashlib
import json
import os
from pathlib import Path
import resource
import shlex
import subprocess
import time

ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'Diagnostics/implementation-44/alpha-interval/host'
NATIVE=ROOT/'Source/platforms/3ds'
AURORA=ROOT/'Source/extern/aurora'
SDK=ROOT/'Upstream/extracted'

def main():
    OUT.mkdir(parents=True,exist_ok=True)
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    common=['clang++','-std=c++20','-DTARGET_PC=1','-DDUSK_TARGET_3DS=1',
            '-I'+str(AURORA/'include'),'-I'+str(NATIVE),'-I'+str(NATIVE/'tests'),'-I'+str(NATIVE/'runtime'),
            '-Wall','-Wextra','-Werror','-Wno-switch','-O1','-g',
            '-fsanitize=address,undefined','-fno-omit-frame-pointer']
    backend=[*common,'-I'+str(NATIVE/'tests/alpha_interval_stubs'),
             '-I'+str(SDK/'libctru-2.7.0/libctru/include'),'-I'+str(SDK/'citro3d-1.7.1/include'),
             '-ffunction-sections','-fdata-sections','-Wl,-dead_strip']
    backend_sources=[NATIVE/name for name in '''tests/gx_alpha_interval_test.cpp gx_geometry_pica.cpp
        runtime/gx_gpu_geometry.cpp runtime/gx_raster_plan.cpp runtime/gx_pica_math.cpp'''.split()]
    adapter_sources=[NATIVE/name for name in '''runtime/gx_texture_commands.cpp gx_command_draw.cpp
        runtime/gx_gpu_geometry.cpp runtime/gx_material.cpp runtime/gx_pica_material.cpp
        runtime/gx_raster_plan.cpp runtime/gx_raster_color.cpp runtime/gd_native.cpp
        tests/gx_command_draw_test.cpp runtime/gx_vertex_transform.cpp runtime/gx_geometry.cpp
        runtime/gx_pica_math.cpp runtime/gx_commands.cpp runtime/gx_stream.cpp
        runtime/gx_vertex_decode.cpp runtime/gx_rigid_decode.cpp runtime/gx_transform_commands.cpp
        runtime/gx_vertex_commands.cpp runtime/gx_lighting_commands.cpp runtime/gx_light_objects.cpp
        runtime/gx_tev_commands.cpp gx_api.cpp gx_command_api.cpp runtime/gx_state.cpp
        runtime/resource_archive.cpp'''.split()]
    adapter_sources += [AURORA/name for name in '''lib/dolphin/gd/GDGeometry.cpp lib/dolphin/gd/GDTev.cpp
        lib/dolphin/gd/GDIndirect.cpp lib/gx/dl.cpp lib/gx/attr_fmt.cpp'''.split()]
    controls=OUT/'controls';controls.mkdir(exist_ok=True)
    pica=NATIVE/'gx_pica.cpp'
    wrong=controls/'wrong-mask.cpp'
    text=pica.read_text();old='C3D_AlphaTest(true,GPU_LEQUAL,r.alphaInterval.upper);'
    assert text.count(old)==1
    wrong.write_text(text.replace(old,'C3D_AlphaTest(true,GPU_GEQUAL,r.alphaInterval.upper);'))
    test=NATIVE/'tests/gx_alpha_interval_test.cpp'
    wrong_test=controls/'wrong-mask-test.cpp'
    wrong_test.write_text(test.read_text().replace('#include "../gx_pica.cpp"','#include "'+str(wrong)+'"'))
    before=ROOT/'Diagnostics/implementation-44/alpha-interval/before/Source/platforms/3ds/runtime/gx_raster_plan.cpp'
    old_plan=controls/'before-raster.cpp';old_plan.write_bytes(before.read_bytes())
    wrapper=controls/'old-wrapper.cpp'
    wrapper.write_text('''#include "runtime/gx_raster_plan.hpp"
#include "runtime/gx_commands.hpp"
namespace dusk::native::gx {
bool planRaster(const CommandRegisters&,RasterPlan&,RasterError*,FogPolicy,ColorStoragePolicy,ClipPolicy,bool,bool);
bool planRaster(const CommandRegisters& r,RasterPlan& p,RasterError* e,FogPolicy f,ColorStoragePolicy c,ClipPolicy clip,bool early,bool tails,AlphaIntervalPolicy){
 using Old=bool (*)(const CommandRegisters&,RasterPlan&,RasterError*,FogPolicy,ColorStoragePolicy,ClipPolicy,bool,bool);
 return static_cast<Old>(&planRaster)(r,p,e,f,c,clip,early,tails);
}}
''')
    env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'}
    primary=[SDK/'citro3d-1.7.1/source'/name for name in ('effect.c','framebuffer.c','renderqueue.c')]
    inputs=set(backend_sources+adapter_sources+primary+[Path(__file__).resolve(),before,wrong,wrong_test,old_plan,wrapper])
    commands=[]
    def run(name,command,negative=None):
        started=time.monotonic()
        result=subprocess.run(list(map(str,command)),capture_output=True,text=True,timeout=90,env=env)
        output=result.stdout+result.stderr
        (OUT/(name+'.log')).write_text(output)
        commands.append({'name':name,'argv':list(map(str,command)),'returncode':result.returncode,'seconds':time.monotonic()-started})
        if negative:
            assert result.returncode!=0 and 'Assertion failed' in output and negative in output,output
            assert 'ERROR: AddressSanitizer' not in output and 'runtime error:' not in output,output
        else:
            if result.returncode:print(output[-6000:],flush=True)
            result.check_returncode()
        print(name+': '+('expected assertion' if negative else 'PASS'),flush=True)
        return output
    for name,options,sources in [('backend',backend,backend_sources+[wrong_test,wrong,old_plan,wrapper]),('adapter',common,adapter_sources)]:
        dep=run(name+'-dependencies',[*[v for v in options if not v.startswith('-Wl,')],'-MM',*sources])
        for token in shlex.split(dep.replace(chr(92)+'\n',' ')):
            if not token.endswith(':'):inputs.add(Path(token).resolve())
    def hashes():return {str(p.relative_to(ROOT) if p.is_relative_to(ROOT) else p):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(inputs)}
    receipt={'status':'running','input_sha256':hashes(),'commands':commands,
             'scope':'Host Clang ASan/UBSan; real PICA/geometry backend TU with mock SDK calls and official primary SDK headers. No ARM build/emulator/native execution.',
             'limits':['Scripted sample coverage/TEV alpha, not a rasterizer or native byte-precision proof.',
                       'Target ownership and copy/reuse guards are parent-owned missing hooks: default-off.',
                       'All failure tests use mock completion; no actual GPU queue timing.']}
    report=OUT/'results.json';report.write_text(json.dumps(receipt,indent=2)+'\n')
    try:
        run('backend-build',[*backend,*backend_sources,'-o',OUT/'backend'])
        run('backend-test',[OUT/'backend'])
        wrong_sources=[wrong_test if p==test else p for p in backend_sources]
        run('wrong-mask-build',[*backend,*wrong_sources,'-o',OUT/'wrong-mask'])
        run('wrong-mask-test',[OUT/'wrong-mask'],'pixel.red')
        old_sources=[old_plan if p==NATIVE/'runtime/gx_raster_plan.cpp' else p for p in backend_sources]
        run('before-build',[*backend,*old_sources,wrapper,'-o',OUT/'before'])
        run('before-test',[OUT/'before'],'alphaIntervalOwnership')
        run('adapter-build',[*common,*adapter_sources,'-o',OUT/'adapter'])
        run('adapter-test',[OUT/'adapter'])
        run('native-adapter-host-syntax',[*common,'-D__3DS__','-fsyntax-only',NATIVE/'gx_command_draw.cpp'])
        assert hashes()==receipt['input_sha256'],'Compiler input drift during verification'
        receipt.update(status='pass',input_unchanged=True)
    except (OSError,subprocess.SubprocessError,AssertionError) as error:
        receipt.update(status='fail',error=str(error))
    report.write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps({k:v for k,v in receipt.items() if k not in ('input_sha256','commands')},indent=2),flush=True)
    return int(receipt['status']!='pass')
if __name__=='__main__':raise SystemExit(main())
