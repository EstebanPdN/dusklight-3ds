#!/usr/bin/env python3
"""Link original J3DTexture/JKR to bounded TEX1 registration under sanitizers."""
import argparse
import concurrent.futures
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'Source'
OUT = ROOT / 'Diagnostics/implementation-21/j3d-texture'

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--private-disc', type=Path)
    parser.add_argument('--output', type=Path, help='Fresh evidence directory; preserve prior session results')
    args = parser.parse_args()
    out = args.output.resolve() if args.output else OUT / ('private' if args.private_disc else 'synthetic')
    if args.output: out.mkdir(parents=True, exist_ok=False)
    build = out / 'build'; build.mkdir(parents=True, exist_ok=True)
    cxx, cc = shutil.which('clang++'), shutil.which('clang')
    flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
             '-ffunction-sections', '-fdata-sections', '-fno-strict-aliasing',
             '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1', '-DMTX_USE_PS=1',
             '-I' + str(SOURCE / 'extern/aurora/include')]
    game = [*flags, '-std=c++20', '-DDUSK_BUILDING_GAME=1', '-DVERSION=0', '-DPARTIAL_DEBUG=1',
            '-DAVOID_UB=1', '-DDUSK_TEST_KERNEL=1', '-Wno-multichar', '-Wno-deprecated-declarations',
            '-Wno-invalid-noreturn']
    for inc in ['include', 'libs/JSystem/include', 'extern/aurora/include/dolphin', 'sdk/include', 'src']:
        game += ['-I' + str(SOURCE / inc)]
    for inc in ['extern/aurora/include/dolphin/types.h', 'include/global.h']:
        game += ['-include', str(SOURCE / inc)]
    if args.private_disc: game += ['-DDUSK_TEST_PRIVATE_DISC=1']
    cpp = ['platforms/3ds/tests/native_j3d_texture_test.cpp', 'platforms/3ds/tests/host_primitives.cpp',
           'platforms/3ds/runtime/bootstrap.cpp', 'platforms/3ds/runtime/os_sync.cpp',
           'platforms/3ds/runtime/os_arena.cpp', 'platforms/3ds/runtime/os_time.cpp',
           'platforms/3ds/runtime/resource_archive.cpp', 'platforms/3ds/runtime/gx_state.cpp', 'platforms/3ds/runtime/texture_gx.cpp', 'libs/JSystem/src/J3DGraphBase/J3DTexture.cpp', 'libs/JSystem/src/J3DGraphBase/J3DShapeDraw.cpp',
           'platforms/3ds/runtime/gx_tev_commands.cpp', 'platforms/3ds/gx_api.cpp', 'src/helpers/string.cpp', 'libs/JSystem/src/JSupport/JSUList.cpp']
    cpp += ['platforms/3ds/runtime/gx_texture_commands.cpp','platforms/3ds/runtime/gd_native.cpp','libs/JSystem/src/J3DGraphBase/J3DGD.cpp','libs/JSystem/src/J3DGraphBase/J3DTevs.cpp','libs/JSystem/src/J3DGraphBase/J3DSys.cpp','platforms/3ds/runtime/gx_transform_commands.cpp','platforms/3ds/runtime/gx_vertex_commands.cpp','platforms/3ds/runtime/gx_lighting_commands.cpp', 'platforms/3ds/runtime/gx_stream.cpp', 'platforms/3ds/runtime/gx_commands.cpp', 'platforms/3ds/runtime/gx_vertex_decode.cpp', 'platforms/3ds/gx_command_api.cpp', 'extern/aurora/lib/gx/dl.cpp', 'extern/aurora/lib/gx/attr_fmt.cpp']
    cpp += ['platforms/3ds/runtime/texture_resources.cpp','platforms/3ds/tests/host_j3d_boundaries.cpp']
    cpp += ['platforms/3ds/runtime/'+name+'.cpp' for name in
            ['gx_copy_execution','gx_copy_pixels','gx_copy_commands','gx_framebuffer','gx_framebuffer_scale','copy_lifecycle_probe']]
    cpp += ['libs/JSystem/src/JKernel/' + name + '.cpp' for name in
            ['JKRHeap', 'JKRExpHeap', 'JKRSolidHeap', 'JKRDisposer', 'JKRFileLoader']]
    cpp += ['libs/JSystem/src/J2DGraph/' + name + '.cpp' for name in
            ['J2DGrafContext', 'J2DOrthoGraph', 'J2DPane', 'J2DPicture', 'J2DScreen', 'J2DManage']]
    cpp += ['libs/JSystem/src/JUtility/' + name + '.cpp' for name in ['JUTTexture', 'JUTPalette']]
    cpp += ['platforms/3ds/runtime/archive_bootstrap.cpp', 'platforms/3ds/runtime/dvd_provider.cpp',
            'platforms/3ds/runtime/disc.cpp']
    cpp += ['libs/JSystem/src/JKernel/' + name + '.cpp' for name in
            ['JKRArchivePri', 'JKRArchivePub', 'JKRMemArchive', 'JKRFileFinder', 'JKRDecomp']]
    math = json.loads((SOURCE / 'platforms/3ds/compile/math_sources.json').read_text())['sources']
    math = [p for p in math if Path(p['path']).stem in ['mtx', 'mtx44']]
    units = [(p, cxx, game) for p in cpp] + [(p['path'], cc, [*flags, '-std=c11']) for p in math]
    objects, commands = [], []
    for index, (relative, compiler, options) in enumerate(units):
        obj = build / f'{index:02d}-{Path(relative).stem}.o'; objects.append(str(obj))
        commands.append([compiler, *options, '-c', str(SOURCE / relative), '-o', str(obj)])
    binary = build / 'native-j3d-texture-asan-ubsan-test'
    link = [cxx, '-fsanitize=address,undefined', '-Wl,-dead_strip', '-Wl,-map,' + str(build / 'link.map'),
            *objects, '-o', str(binary)]
    environment = {'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1',
                   'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
    receipt = {'status': 'running', 'origin': 'host original-J3DTexture/JKR texture registration, retention, bulk disposal, typed capture/token refresh and conversion; OS/GPU completion/deletion/readback/invalidation substituted with synthetic snapshots, no raster/hardware claim',
               'started_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
               'source_sha256': {p: sha(SOURCE / p) for p, _, _ in units},
               'header_sha256': {p: sha(SOURCE / p) for p in [
                   'libs/JSystem/include/JSystem/JKernel/JKRHeap.h', 'libs/JSystem/include/JSystem/JKernel/JKRExpHeap.h',
                   'libs/JSystem/include/JSystem/J3DGraphBase/J3DTexture.h', 'platforms/3ds/runtime/texture_gx.hpp', 'libs/JSystem/include/JSystem/JUtility/JUTTexture.h', 'platforms/3ds/runtime/gx_state.hpp','platforms/3ds/runtime/gx_texture_commands.hpp','platforms/3ds/runtime/copy_lifecycle_probe.hpp','platforms/3ds/runtime/alpha_interval_target.hpp','platforms/3ds/gx_pica.hpp','platforms/3ds/gx_texture_pica.hpp','platforms/3ds/runtime/gx_copy_pixels.hpp','platforms/3ds/runtime/gx_profile.hpp']},
               'runner_sha256': sha(Path(__file__)), 'sanitizers': environment,
               'private_disc': 'not requested',
               'compiler': subprocess.check_output([cxx, '--version'], text=True).strip()}
    (out / 'commands.json').write_text(json.dumps({'compile': commands, 'link': link,
        'run': [str(binary)], 'environment': environment}, indent=2) + '\n')
    def compile_one(pair):
        index, command = pair
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
        (build / f'{index:02d}.log').write_text(result.stdout)
        return result.returncode
    status = 1
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            codes = list(pool.map(compile_one, enumerate(commands)))
        receipt['compile_exit'] = codes
        if any(codes): raise RuntimeError('compile failure; inspect numbered build logs')
        result = subprocess.run(link, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
        (out / 'link.log').write_text(result.stdout); receipt['link_exit'] = result.returncode
        result.check_returncode(); receipt['binary_sha256'] = sha(binary)
        if not args.build_only:
            run_command = [str(binary)]
            if args.private_disc:
                before = args.private_disc.stat(); run_command += ['--private-disc', str(args.private_disc)]
                receipt['private_command'] = [str(binary), '--private-disc', '<private-input-path-omitted>']
            result = subprocess.run(run_command, env={**os.environ, **environment}, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
            (out / 'test.log').write_text(result.stdout); print(result.stdout, end='')
            receipt['test_exit'] = result.returncode; result.check_returncode()
            if args.private_disc:
                after = args.private_disc.stat()
                receipt['private_disc_unchanged'] = before.st_size == after.st_size and before.st_mtime_ns == after.st_mtime_ns
                if not receipt['private_disc_unchanged']: raise RuntimeError('private disc size/mtime changed')
                receipt['private_disc'] = 'read-only original JKR/J3DTexture/GX bindings; no payload output; path omitted'
        receipt['sources_unchanged'] = receipt['source_sha256'] == {p: sha(SOURCE / p) for p, _, _ in units}
        receipt['headers_unchanged'] = receipt['header_sha256'] == {p: sha(SOURCE / p) for p in receipt['header_sha256']}
        if not receipt['sources_unchanged'] or not receipt['headers_unchanged']:
            raise RuntimeError('provider sources changed during the test; rerun against a stable snapshot')
        receipt['status'] = 'compile-link-only' if args.build_only else 'pass'; status = 0
    except (subprocess.SubprocessError, RuntimeError) as error:
        message = ('command failed with exit ' + str(error.returncode) + '; see commands.json and phase logs'
                   if isinstance(error, subprocess.CalledProcessError) else str(error))
        receipt['status'] = 'fail'; receipt['error'] = message; print(message, file=sys.stderr)
    finally:
        receipt['finished_at'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        (out / 'results.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return status

if __name__ == '__main__':
    raise SystemExit(main())
