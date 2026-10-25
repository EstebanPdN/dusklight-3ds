#!/usr/bin/env python3
"""Host-only native RGB565 admission checks. No ARM build, emulator, ISO or installed-tool changes.

Uses sanitizer fixtures and installed Citro3D headers with allocation/bind/flush
stubs. Optional --snapshot checks all exported mips against --baseline-census
and the existing offline decoder workflow, preserving the installed helpers.
"""
import argparse
import ast
import datetime
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / 'Source/platforms/3ds'
DEVKITPRO = Path(os.environ.get('DEVKITPRO', '/opt/devkitpro'))
ENV = {**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1',
       'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--snapshot', type=Path, action='append', default=[])
    p.add_argument('--baseline-census', type=Path)
    args = p.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    sources = [NATIVE / name for name in ['runtime/texture_gx.cpp', 'runtime/texture_gx.hpp',
               'gx_texture_pica.cpp', 'gx_texture_pica.hpp', 'runtime/gx_state.hpp',
               'runtime/copy_lifecycle_probe.cpp', 'runtime/copy_lifecycle_probe.hpp',
               'runtime/gpu_sync.hpp', 'runtime/gx_profile.hpp', 'tests/texture_rgb565_test.cpp',
               'tests/texture_gx_test.cpp', 'tests/gx_texture_backend_test.cpp',
               'runtime/disc.cpp', 'runtime/resource_archive.cpp']]
    sources += [NATIVE/n for n in ['runtime/fused_color.cpp','runtime/fused_color.hpp',
                'runtime/gx_copy_pixels.cpp','runtime/gx_copy_pixels.hpp','tests/host_fused_copy_off.hpp']]
    sources += [Path(__file__).resolve(), ROOT / 'Tools/3ds/decode_snapshot_texture.cpp',
                ROOT / 'Tools/3ds/test_texture_gx.py', ROOT / 'Source/extern/aurora/lib/gfx/texture_convert.cpp',
                ROOT / 'Tools/3ds/analyze_texture_dump.py', ROOT / 'Tools/3ds/texture_compression_lab.py']
    sources += [DEVKITPRO / 'libctru/include' / name for name in
                ['3ds/types.h', '3ds/result.h', '3ds/services/gspgpu.h', '3ds/gpu/enums.h', 'c3d/types.h', 'c3d/texture.h']]
    sources += [ROOT / 'Tools/3ds/capture_to_png.py']
    sources += args.snapshot + ([args.baseline_census] if args.baseline_census else [])
    before = {str(f): sha(f) for f in sources}
    receipt = {'status': 'running', 'input_sha256': before, 'commands': [],
               'started_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
               'sanitizer_environment': {k: ENV[k] for k in ('ASAN_OPTIONS', 'UBSAN_OPTIONS')},
               'limits': ['Host ASan/UBSan only; GPU allocation/bind/flush/completion substituted.',
                          'No native compilation, GPU sampling, physical pixel equivalence or FPS/RAM claim.']}

    def run(name, command):
        receipt['commands'].append({'name': name, 'command': list(map(str, command))})
        r = subprocess.run(list(map(str, command)), capture_output=True, text=True, env=ENV, timeout=90)
        (out / (name + '.log')).write_text(r.stdout + r.stderr)
        r.check_returncode()
        return r.stdout

    common = ['clang++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
              '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    texture = NATIVE / 'runtime/texture_gx.cpp'
    try:
        receipt['compiler'] = run('compiler', ['clang++', '--version']).strip()
        run('focused-build', [*common, texture, NATIVE / 'tests/texture_rgb565_test.cpp', '-o', out / 'focused'])
        print(run('focused-test', [out / 'focused']), end='', flush=True)
        # Reuse the existing runner's exact pinned Aurora excerpt wrapper,
        # without running its ARM compiler, tex3ds or private-disc actions.
        tree = ast.parse((ROOT / 'Tools/3ds/test_texture_gx.py').read_text())
        literals = {}
        for node in ast.walk(tree):
            if isinstance(node, ast.Assign):
                for target in node.targets:
                    if isinstance(target, ast.Name) and target.id in ('prefix', 'suffix'):
                        literals[target.id] = ast.literal_eval(node.value)
        original = (ROOT / 'Source/extern/aurora/lib/gfx/texture_convert.cpp').read_text()
        section = original[original.index('template <uint8_t v>'):original.index('static ByteBuffer BuildRGBA8FromBC1')]
        oracle = out / 'original_decoder.cpp'
        oracle.write_text(literals['prefix'] + section + literals['suffix'])
        run('original-build', [*common, texture, NATIVE / 'tests/texture_gx_test.cpp',
                              NATIVE / 'runtime/disc.cpp', NATIVE / 'runtime/resource_archive.cpp',
                              oracle, '-o', out / 'original-test'])
        print(run('original-test', [out / 'original-test']), end='', flush=True)
        run('backend-build', [*common, '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1',
                              '-I' + str(ROOT / 'Source/extern/aurora/include'),
                              '-I' + str(DEVKITPRO / 'libctru/include'), NATIVE / 'gx_texture_pica.cpp',
                              texture, NATIVE / 'runtime/copy_lifecycle_probe.cpp',
                              NATIVE / 'runtime/fused_color.cpp', NATIVE / 'runtime/gx_copy_pixels.cpp',
                              NATIVE / 'tests/gx_texture_backend_test.cpp', '-o', out / 'backend'])
        print(run('backend-test', [out / 'backend']), end='', flush=True)
        decoder = out / 'gx-decoder'
        run('decoder-build', [*common, ROOT / 'Tools/3ds/decode_snapshot_texture.cpp', texture, '-o', decoder])
        # Independent RGBA oracle for every possible source word through the
        # actual diagnostic executable, including many tile boundaries.
        with tempfile.TemporaryDirectory(dir=out) as tmp:
            scratch = Path(tmp)
            w = h = 256
            header = bytearray(32); header[0] = 4; header[24] = 1; header[31] = 32
            struct.pack_into('>HH', header, 2, w, h)
            image = bytearray(w*h*2); expected = bytearray()
            for y in range(h):
                for x in range(w):
                    value = y*w+x
                    at = ((y//4)*(w//4)+x//4)*32 + ((y%4)*4+x%4)*2
                    struct.pack_into('>H', image, at, value)
                    r, g, b = value >> 11, (value >> 5) & 63, value & 31
                    expected.extend(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255))
            inp, dest = scratch / 'synthetic.bti', scratch / 'synthetic.rgba'
            inp.write_bytes(header + image)
            run('decoder-all-words', [decoder, inp, dest])
            assert dest.read_bytes() == struct.pack('<II', w, h) + expected
            receipt['diagnostic_all_65536_original_rgba_exact'] = True
            # Retain compatibility with the existing per-mip diagnostic entry
            # API rather than adding a second decoder or changing dump records.
            import analyze_texture_dump as analyzer
            import texture_compression_lab as lab
            baseline = json.loads(args.baseline_census.read_text()) if args.baseline_census else None
            golden = {(r['scene'], r['handle']): r for r in baseline['records']} if baseline else {}
            captured = []
            for index, snapshot in enumerate(args.snapshot):
                data, frame, records = analyzer.read_snapshot(snapshot)
                scene = baseline['inputs'][index]['scene'] if baseline else f'scene{index}'
                count = 0
                for record, bti in records:
                    levels = lab.decode_mips(record, bti, decoder, scratch)
                    hashes = [hashlib.sha256(rgba).hexdigest() for rgba, _, _ in levels]
                    if baseline:
                        prior = golden[(scene, record['handle'])]
                        assert record['image_sha256'] == prior['image_sha256']
                        assert hashes == prior['decoded_mip_sha256']
                    count += len(levels)
                    # Exact full payload match to the independently emitted
                    # authoritative lab result for its captured RGB565 sample.
                    if scene == 'outside' and record['handle'] == 288:
                        inp.write_bytes(bti)
                        run('captured-native-payload', [out / 'focused', inp, scratch / 'native.pica'])
                        lab_payload = args.baseline_census.parent / 'outside-0252-h288/RGB565-full.pica'
                        assert (scratch / 'native.pica').read_bytes() == lab_payload.read_bytes()
                        preview = out / 'outside-h288-original.png'
                        lab.preview(preview, *levels[0])
                        original_preview = lab_payload.parent / 'original.png'
                        assert preview.read_bytes() == original_preview.read_bytes()
                        receipt['captured_rgb565_payload'] = {'bytes': lab_payload.stat().st_size,
                            'sha256': sha(lab_payload), 'matches_independent_lab': True,
                            'original_preview_sha256': sha(preview), 'preview_exact': True}
                captured.append({'snapshot': str(snapshot), 'frame': frame, 'records': len(records),
                                 'mips': count, 'all_rgba_hashes_match_baseline': bool(baseline)})
            receipt['captured_snapshots'] = captured
        assert before == {str(f): sha(f) for f in sources}
        receipt.update(status='pass', inputs_unchanged=True,
                       binary_sha256={name: sha(out / name) for name in ('focused', 'original-test', 'backend', 'gx-decoder')})
    except (OSError, subprocess.SubprocessError, AssertionError, ValueError, KeyError) as error:
        receipt.update(status='fail', error=repr(error))
        print(error, flush=True)
    finally:
        receipt['finished_at'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        (out / 'results.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return int(receipt['status'] != 'pass')


if __name__ == '__main__':
    raise SystemExit(main())
