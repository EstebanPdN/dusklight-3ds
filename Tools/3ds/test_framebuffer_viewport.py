#!/usr/bin/env python3
"""Host-only ASan/UBSan regression for native framebuffer raster-state scope."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    output = parser.parse_args().output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    native = ROOT / 'Source/platforms/3ds'
    aurora = ROOT / 'Source/extern/aurora'
    sources = [native / 'runtime' / name for name in (
        'gx_framebuffer.cpp', 'gx_framebuffer_api.cpp', 'gx_stream.cpp',
        'gx_commands.cpp', 'gx_vertex_decode.cpp', 'gx_vertex_transform.cpp',
        'gx_raster_plan.cpp', 'gx_pica_math.cpp')]
    sources += [native / 'gx_command_api.cpp', aurora / 'lib/gx/dl.cpp',
                aurora / 'lib/gx/attr_fmt.cpp', native / 'tests/framebuffer_viewport_test.cpp']
    headers = [native / 'runtime' / name for name in (
        'gx_framebuffer.hpp', 'gx_stream.hpp', 'gx_commands.hpp',
        'gx_vertex_decode.hpp', 'gx_vertex_transform.hpp', 'gx_raster_plan.hpp',
        'gx_pica_math.hpp', 'gx_target_extent.hpp', 'gx_state.hpp',
        'gx_batch_cache.hpp', 'gx_copy_setup.hpp', 'gx_profile.hpp')]
    inputs = [*sources, *headers, Path(__file__).resolve()]
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    command = ['clang++', '-std=c++20', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1',
               '-I' + str(aurora / 'include'), '-Wall', '-Wextra', '-Werror',
               '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               *map(str, sources), '-o', str(output / 'test')]
    receipt = {'status': 'running', 'commands': [command], 'input_sha256': hashes,
               'origin': 'Host ASan/UBSan: actual GXCreate/RestoreFrameBuffer ABI, command stream and transform/raster planners; platform framebuffer calls mocked',
               'limits': ['No ARM compilation, emulator, GPU or physical-console execution',
                          'No shadow image, GPU timing or performance acceptance']}
    try:
        with (output / 'build.log').open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=90)
        run = subprocess.run([str(output / 'test')], capture_output=True, text=True, timeout=30,
                             env={**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1',
                                  'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})
        (output / 'test.log').write_text(run.stdout + run.stderr)
        print(run.stdout + run.stderr, end='')
        run.check_returncode()
        assert hashes == {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        receipt.update(status='pass', input_unchanged=True)
    except (OSError, subprocess.SubprocessError, AssertionError) as error:
        receipt.update(status='fail', error=str(error))
        if (output / 'build.log').exists():
            print((output / 'build.log').read_text())
    finally:
        (output / 'results.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return int(receipt['status'] != 'pass')


if __name__ == '__main__':
    raise SystemExit(main())
