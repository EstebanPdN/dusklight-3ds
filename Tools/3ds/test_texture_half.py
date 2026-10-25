#!/usr/bin/env python3
"""Host-only authored-mip half policy validation. No ARM build or emulator.

Independent Python GX generator, SHA-256 and PICA Morton oracle; sanitizer
fixtures exercise admission/cache lifecycle. Existing RGB565/all-format/Aurora
reference regressions run unchanged via their host-only runner.
"""
import argparse
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / 'Source/platforms/3ds'
DEVKITPRO = Path(os.environ.get('DEVKITPRO', '/opt/devkitpro'))
ENV = {**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1',
       'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1', 'PYTHONDONTWRITEBYTECODE': '1'}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fixture(width, height, mips, offset=39):
    header = bytearray(offset)
    header[0] = 4
    struct.pack_into('>HH', header, 2, width, height)
    header[24] = mips
    struct.pack_into('>I', header, 28, offset)
    payload = bytearray()
    levels = []
    for level in range(mips):
        w, h = width >> level, height >> level
        words = [(x + y*w + (level-1)*7919) & 65535 for y in range(h) for x in range(w)]
        tiled = bytearray(w*h*2)
        for y in range(h):
            for x in range(w):
                at = ((y//4)*(w//4)+x//4)*32 + ((y%4)*4+x%4)*2
                struct.pack_into('>H', tiled, at, words[y*w+x])
        payload.extend(tiled)
        levels.append((w, h, words))
    digest = hashlib.sha256(bytes([4]) + struct.pack('>HHB', width, height, mips) + payload).hexdigest()
    return header + payload, levels, digest


def pica_oracle(levels, reference):
    result = bytearray()
    for w, h, words in levels:
        bpp = 4 if reference else 2
        target = bytearray(w*h*bpp)
        for y in range(h):
            for x in range(w):
                morton = sum(((x >> bit) & 1) << (2*bit) | ((y >> bit) & 1) << (2*bit+1) for bit in range(3))
                at = (((y//8)*(w//8)+x//8)*64+morton)*bpp
                value = words[y*w+x]
                if reference:
                    r, g, b = value >> 11, (value >> 5) & 63, value & 31
                    target[at:at+4] = bytes([255, (b << 3) | (b >> 2), (g << 2) | (g >> 4), (r << 3) | (r >> 2)])
                else:
                    struct.pack_into('<H', target, at, value)
        result.extend(target)
    return result


def lod_uv_model():
    checks = 0
    for original_mips in range(2, 9):
        for low in range(original_mips):
            for high in range(low, original_mips):
                for derivative in (0.001, 0.125, 0.5, 1, 2, 4, 32):
                    for bias in (-3.5, -0.5, 0, 1.25, 3.5):
                        lam = math.log2(derivative*1024)+bias
                        expected = min(max(lam, max(1, low)), max(1, high))
                        actual = min(max(lam-1, max(0, low-1)), max(0, high-1))+1
                        assert abs(actual-expected) < 1e-12
                        checks += 1
    # Same authored mip has the same extent/normalized texel centers after
    # rebasing, including repeat/mirror coordinates and bilinear fractions.
    for original_width in (16, 32, 64, 128, 512, 1024):
        for source_level in range(1, int(math.log2(original_width))-2):
            original_extent = original_width >> source_level
            effective_extent = (original_width//2) >> (source_level-1)
            for coord in (-2.125, -0.001, 0, 0.25, 0.999, 1, 1.75, 4.5):
                assert coord*original_extent-0.5 == coord*effective_extent-0.5
                checks += 1
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    sources = [NATIVE / p for p in ('runtime/texture_gx.hpp', 'runtime/texture_gx.cpp',
               'gx_texture_pica.hpp', 'gx_texture_pica.cpp', 'tests/texture_half_test.cpp',
               'runtime/copy_lifecycle_probe.cpp', 'runtime/copy_lifecycle_probe.hpp',
               'tests/texture_half_backend_test.cpp', 'runtime/gx_state.hpp', 'runtime/gpu_sync.hpp',
               'runtime/gx_profile.hpp', 'tests/gx_texture_backend_test.cpp', 'tests/texture_rgb565_test.cpp',
               'tests/texture_gx_test.cpp')]
    sources += [NATIVE/n for n in ['runtime/fused_color.cpp','runtime/fused_color.hpp',
                'runtime/gx_copy_pixels.cpp','runtime/gx_copy_pixels.hpp','tests/host_fused_copy_off.hpp']]
    sources += [Path(__file__).resolve(), ROOT / 'Tools/3ds/test_texture_rgb565.py']
    sources += [DEVKITPRO / 'libctru/include' / p for p in ('3ds/types.h', '3ds/result.h', '3ds/services/gspgpu.h', '3ds/gpu/enums.h', 'c3d/types.h', 'c3d/texture.h')]
    before = {str(p): sha(p) for p in sources}
    receipt = {'status': 'running', 'started_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
               'source_sha256': before, 'commands': [], 'oracle_cases': [],
               'limits': ['Host ASan/UBSan with allocation/flush/bind/fence stubs.',
                          'No native build, PICA execution/sampling, visual acceptance, FPS or measured live RAM saving.',
                          'Synthetic admitted sources only; shipped allowlist remains empty pending a role audit.']}

    def run(name, command, timeout=90):
        command = list(map(str, command))
        receipt['commands'].append({'name': name, 'argv': command})
        result = subprocess.run(command, capture_output=True, text=True, timeout=timeout, env=ENV)
        (out / (name+'.log')).write_text(result.stdout+result.stderr)
        result.check_returncode()
        print(name + ': PASS', flush=True)
        return result.stdout

    common = ['clang++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
              '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    try:
        receipt['compiler'] = run('compiler', ['clang++', '--version']).strip()
        run('focused-build', [*common, NATIVE / 'runtime/texture_gx.cpp', NATIVE / 'tests/texture_half_test.cpp', '-o', out / 'focused'])
        run('focused-test', [out / 'focused'])
        run('backend-build', [*common, '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1',
                             '-I'+str(ROOT / 'Source/extern/aurora/include'), '-I'+str(DEVKITPRO / 'libctru/include'),
                             NATIVE / 'runtime/texture_gx.cpp', NATIVE / 'gx_texture_pica.cpp',
                             NATIVE / 'runtime/copy_lifecycle_probe.cpp',
                             NATIVE / 'runtime/fused_color.cpp', NATIVE / 'runtime/gx_copy_pixels.cpp',
                             NATIVE / 'tests/texture_half_backend_test.cpp', '-o', out / 'backend'])
        run('backend-test', [out / 'backend'])
        for width, height, mips in ((16, 16, 2), (32, 16, 2), (64, 128, 4), (512, 512, 7), (1024, 512, 7)):
            image, levels, digest = fixture(width, height, mips)
            inp = out / f'{width}x{height}.bti'
            inp.write_bytes(image)
            for reference in (False, True):
                name = f'oracle-{width}x{height}-'+('rgba8' if reference else 'rgb565')
                dest = out / (name+'.pica')
                plan = json.loads(run(name, [out / 'focused', inp, dest, str(int(reference))]))
                expected = pica_oracle(levels[1:], reference)
                assert dest.read_bytes() == expected
                assert plan == {'width': width//2, 'height': height//2, 'mips': mips-1,
                                'original_width': width, 'original_height': height, 'original_mips': mips,
                                'source_mip': 1, 'bytes': len(expected), 'fingerprint': digest}
                assert inp.read_bytes() == image
                receipt['oracle_cases'].append({'name': name, 'plan': plan, 'pica_sha256': sha(dest),
                                                'input_sha256': sha(inp), 'all_words_at_mip1': width == height == 512})
        receipt['lod_uv_model_checks'] = lod_uv_model()
        run('existing-regressions', ['python3', ROOT / 'Tools/3ds/test_texture_rgb565.py', '--output', out / 'regression'], 300)
        regression = json.loads((out / 'regression/results.json').read_text())
        assert regression['status'] == 'pass'
        assert before == {str(p): sha(p) for p in sources}
        receipt.update(status='pass', inputs_unchanged=True,
                       binary_sha256={name: sha(out / name) for name in ('focused', 'backend')})
    except (OSError, subprocess.SubprocessError, AssertionError, ValueError, KeyError) as error:
        receipt.update(status='fail', error=repr(error))
        print(repr(error), flush=True)
    finally:
        receipt['finished_at'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        (out / 'results.json').write_text(json.dumps(receipt, indent=2)+'\n')
    return int(receipt['status'] != 'pass')


if __name__ == '__main__':
    raise SystemExit(main())
