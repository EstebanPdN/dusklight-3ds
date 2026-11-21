#!/usr/bin/env python3
"""Verify the indirect warp sampler and its constant-offset GPU plan
(gx_indirect.hpp, gx_indirect_gpu.hpp, gx_rigid_mt.v.pica) on the host.

Steps: the hookup test on the real draw adapter (see hookup below), and the
host ASan/UBSan run of tests/gx_indirect_gpu_test.cpp (with the
renderer's texture decoder as the reference for the new image formats), ARM
compile of the same test (-Werror), picasso assembly of gx_rigid_mt.v.pica
with the output-write gate (check_pica_shader.py), and the shader source run
by test_gx_rigid_texgen.py's interpreter on the uniform blocks the test
folds (per-pixel projected unit, texgen and normal units, and the unchanged
per-vertex divide), against the CPU route's coordinates. With --hookup-patch, the
gx_command_draw.cpp hookup is applied to a copy, compiled with the ARM flags
of test_gx_command_draw.py, and two programs run against the copy: the
draw-adapter suite (non-indirect paths unchanged) and
tests/gx_indirect_hookup_test.cpp (the hookup itself: GPU acceptance with
folded rows, refusals, early and late restarts equal to the path off).

usage: test_gx_indirect.py --output DIR [--hookup-patch FILE]
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--hookup-patch', type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    native = ROOT/'Source/platforms/3ds'
    aurora = ROOT/'Source/extern/aurora'
    devkitpro = Path(os.environ.get('DEVKITPRO') or
                     next((p for p in (Path.home()/'devkitpro', Path('/opt/devkitpro')) if p.is_dir()), '/opt/devkitpro'))
    sources = [native/'tests/gx_indirect_gpu_test.cpp', native/'runtime/texture_gx.cpp']
    model = ROOT/'Tools/3ds/test_gx_rigid_texgen.py'
    headers = [native/'runtime/gx_indirect.hpp', native/'runtime/gx_indirect_gpu.hpp', native/'runtime/gx_vertex_warp.hpp',
               native/'runtime/gx_rigid_vertex.hpp',
               native/'runtime/gx_pica_material.hpp', native/'runtime/texture_gx.hpp', native/'gx_rigid_mt.v.pica']
    inputs = [*sources, *headers, model, Path(__file__).resolve(), native/'gx_command_draw.cpp', native/'tests/gx_indirect_hookup_test.cpp']
    if args.hookup_patch:
        inputs += [args.hookup_patch.resolve(), native/'tests/gx_command_draw_test.cpp']
    hashes = {str(p.relative_to(ROOT)) if p.is_relative_to(ROOT) else str(p): sha(p) for p in inputs}
    common = ['-std=c++20', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1', '-I'+str(aurora/'include'),
              '-Wall', '-Wextra', '-Werror', '-Wno-switch']
    arm = [str(devkitpro/'devkitARM/bin/arm-none-eabi-g++'), *common, '-O2', '-march=armv6k', '-mtune=mpcore',
           '-mfloat-abi=hard', '-mfpu=vfp', '-mtp=soft', '-D__3DS__', '-fno-short-enums']
    picasso = devkitpro/'tools/bin/picasso'
    steps = [
        ('host', ['clang++', *common, '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                  *map(str, sources), '-o', str(out/'test')]),
        ('arm', [*arm, '-c', str(native/'tests/gx_indirect_gpu_test.cpp'), '-o', str(out/'gx_indirect_gpu_test-arm.o')]),
        ('picasso', [str(picasso), '-o', str(out/'gx_rigid_mt.shbin'), str(native/'gx_rigid_mt.v.pica')]),
        ('shader-gate', ['python3', '-I', str(ROOT/'Tools/3ds/check_pica_shader.py'), str(out/'gx_rigid_mt.shbin')]),
    ]
    receipt = {'status': 'running', 'steps': [s[0] for s in steps], 'input_sha256': hashes,
               'origin': 'host ASan/UBSan execution, ARM compilation and shader assembly; no GPU or console execution',
               'limits': ['the shader texgen is modelled on the host from gx_rigid_mt.v.pica; float24 execution is an Azahar/console check',
                          'statistics bound the original per-pixel warp; the picture itself needs an Azahar look']}
    report = out/'results.json'
    report.write_text(json.dumps(receipt)+'\n')
    log = out/'build.log'
    try:
        with log.open('w') as stream:
            for name, command in steps:
                stream.write(f'== {name}: {" ".join(command)}\n'); stream.flush()
                subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True, timeout=300)
        result = subprocess.run([str(out/'test'), '--shader-cases', str(out/'shader-cases.json')], capture_output=True, text=True, timeout=300,
                                env={**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1',
                                     'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})
        (out/'test.log').write_text(result.stdout+result.stderr)
        print(result.stdout+result.stderr, end='')
        result.check_returncode()
        receipt['shader_model'] = shader_model(out/'shader-cases.json', native/'gx_rigid_mt.v.pica')
        print('PASS shader source on the folded uniform blocks: %(cases)d blocks, %(vertices)d vertices '
              '(per-pixel projected %(per_pixel)d, per-vertex projected %(per_vertex)d, texgen/normal %(rows)d, '
              'with the warp of the vertex %(warped)d)' % receipt['shader_model'])
        receipt['hookup'] = hookup(args.hookup_patch.resolve() if args.hookup_patch else None, out/'hookup', native, aurora, common, arm)
        if receipt['hookup']['status'] != 'pass':
            raise AssertionError('hookup check failed: '+receipt['hookup'].get('error', ''))
        changed = [k for k, v in hashes.items() if sha(ROOT/k if not Path(k).is_absolute() else Path(k)) != v]
        if changed:
            raise AssertionError('inputs changed during the run: '+', '.join(changed))
        receipt.update(status='pass', input_unchanged=True)
    except (OSError, subprocess.SubprocessError, AssertionError) as error:
        receipt.update(status='fail', error=str(error))
        print(log.read_text()[-6000:])
    finally:
        report.write_text(json.dumps(receipt, indent=2)+'\n')
    print('test_gx_indirect:', receipt['status'], receipt.get('error', ''))
    return int(receipt['status'] != 'pass')


def shader_model(cases_path, shader):
    """Run gx_rigid_mt.v.pica with the host interpreter of test_gx_rigid_texgen.py
    (float32 per operation, every output component written exactly once)."""
    spec = importlib.util.spec_from_file_location('gx_rigid_texgen_model', ROOT/'Tools/3ds/test_gx_rigid_texgen.py')
    model = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(model)
    source = shader.read_text()
    data = json.loads(cases_path.read_text())
    counts = {'cases': 0, 'vertices': 0, 'per_pixel': 0, 'per_vertex': 0, 'rows': 0, 'warped': 0}
    for case in data['cases']:
        role = int(case['name'].rsplit('role', 1)[1])
        for vertex in case['vertices']:
            got = model.execute(source, case, vertex)['outtex']
            for c in range(3):
                want = vertex['expected'][c]
                assert abs(got[c]-want) <= 2e-5*max(1, abs(want)), (case['name'], c, got[c], want)
            counts['vertices'] += 1
            counts['warped' if role >= 20 else 'per_pixel' if role == 2 else 'per_vertex' if role == 12 else 'rows'] += 1
        counts['cases'] += 1
    assert counts['per_pixel'] and counts['per_vertex'] and counts['rows'] and counts['warped']
    return counts


def hookup(patch, out, native, aurora, common, arm):
    """Run the hookup test on the draw adapter. Without a patch: the tree's
    own gx_command_draw.cpp. With one: the patch is applied to a copy, the
    copy is compiled for ARM, and the draw-adapter suite
    (test_gx_command_draw.py's sources) runs on it as well."""
    if out.exists():
        shutil.rmtree(out)
    tree = out/'tree'
    (tree/'Source/platforms/3ds').mkdir(parents=True)
    result = {'patch': str(patch) if patch else None}
    log = out/'hookup.log'
    with log.open('w') as stream:
        def run(name, command, cwd=None):
            stream.write(f'== {name}: {" ".join(map(str, command))}\n'); stream.flush()
            return subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, cwd=cwd, timeout=300).returncode
        quote = ['-iquote', str(native)]
        patched = native/'gx_command_draw.cpp'
        if patch:
            shutil.copy2(native/'gx_command_draw.cpp', tree/'Source/platforms/3ds/gx_command_draw.cpp')
            if run('apply', ['git', 'apply', '--whitespace=nowarn', str(patch)], cwd=tree):
                result.update(status='fail', error='patch does not apply to the current gx_command_draw.cpp'); return result
            patched = tree/'Source/platforms/3ds/gx_command_draw.cpp'
            if run('arm', [*arm, *quote, '-g', '-fstack-usage', '-c', str(patched), '-o', str(out/'gx_command_draw-arm.o')]):
                result.update(status='fail', error='ARM compile of the patched copy failed'); return result
        # Mirrors Tools/3ds/test_gx_command_draw.py with the patched copy.
        library = [native/'runtime/gx_texture_commands.cpp', patched, native/'runtime/gx_gpu_geometry.cpp', native/'runtime/gx_material.cpp',
                   native/'runtime/gx_pica_material.cpp', native/'runtime/gx_raster_plan.cpp', native/'runtime/gx_raster_color.cpp',
                   native/'runtime/gd_native.cpp', aurora/'lib/dolphin/gd/GDGeometry.cpp', aurora/'lib/dolphin/gd/GDTev.cpp',
                   aurora/'lib/dolphin/gd/GDIndirect.cpp', native/'runtime/gx_vertex_transform.cpp',
                   native/'runtime/gx_geometry.cpp', native/'runtime/gx_pica_math.cpp', native/'runtime/gx_commands.cpp', native/'runtime/gx_worker.cpp',
                   native/'runtime/gx_stream.cpp', native/'runtime/gx_vertex_decode.cpp', native/'runtime/gx_rigid_decode.cpp',
                   native/'runtime/gx_transform_commands.cpp', native/'runtime/gx_vertex_commands.cpp', native/'runtime/gx_lighting_commands.cpp',
                   native/'runtime/gx_light_objects.cpp', native/'runtime/gx_tev_commands.cpp', native/'gx_api.cpp', native/'gx_command_api.cpp',
                   native/'runtime/gx_state.cpp', native/'runtime/resource_archive.cpp', aurora/'lib/gx/dl.cpp', aurora/'lib/gx/attr_fmt.cpp',
                   native/'runtime/pica_blocks.cpp']
        env = {**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
        # Both always run, so a failure of one does not hide the other's result.
        failed = []
        tests = [('hookup', native/'tests/gx_indirect_hookup_test.cpp')]
        if patch:
            tests.append(('suite', native/'tests/gx_command_draw_test.cpp'))
        for name, test in tests:
            binary = out/f'{name}-test'
            if run(f'host-{name}', ['clang++', *common, *quote, '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                                    *map(str, library), str(test), '-o', str(binary)]):
                result[f'{name}_status'] = 'build failed'; failed.append(name); continue
            run_result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=300, env=env)
            (out/f'{name}.log').write_text(run_result.stdout+run_result.stderr)
            print(run_result.stdout+run_result.stderr, end='')
            passes = sum(line.startswith('PASS') for line in run_result.stdout.splitlines())
            ok = not run_result.returncode and passes
            result.update({f'{name}_pass_lines': passes, f'{name}_exit': run_result.returncode, f'{name}_status': 'pass' if ok else 'fail'})
            if not ok:
                failed.append(name)
        if failed:
            result.update(status='fail', error=' and '.join(failed)+' failed (see hookup/*.log)'); return result
    result.update(status='pass')
    return result


if __name__ == '__main__':
    raise SystemExit(main())
