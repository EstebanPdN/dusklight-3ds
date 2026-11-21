#!/usr/bin/env python3
"""Levels of detail for the 3DS from the owner's own zelda.twilight (Mac only, private data).

Builds the converter (model_lod_build.cpp: the runtime's disc and archive
readers, the runtime's batching rule, the simplifier of
model_lod_simplify.hpp), reduces every J3D model of the disc and writes the
store the game reads from the SD (runtime/model_lod_store.hpp), next to
zelda.twilight:

  python3 -I Tools/3ds/model_lod.py --disc Build/perf30-data/zelda.twilight \\
      --out Build/alpha21/ws-lod/lod [--test]
  python3 -I Tools/3ds/model_lod.py --unit

writes <out>/zelda.lod (copy it to sdmc:/3ds/DuskLight/zelda.lod), <out>/report.json
(per stage: triangles before and at each level) and <out>/shapes.txt (every
shape with levels, its triangles and errors). --test also runs the proof that
the tool cuts every display list of the disc into exactly the draws, vertex
records and indices the runtime's command processor makes, and reads the
store back with the runtime's reader. Never overwrites a file; reads the disc
only; nothing derived from the game leaves this Mac.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
NATIVE = ROOT / 'Source/platforms/3ds'
AURORA = ROOT / 'Source/extern/aurora'
RUNTIME = NATIVE / 'runtime'
HEADERS = [HERE / 'model_prep_bmd.hpp', HERE / 'model_prep_walk.hpp', HERE / 'model_lod_batches.hpp',
           HERE / 'model_lod_simplify.hpp', RUNTIME / 'disc.hpp', RUNTIME / 'resource_archive.hpp',
           RUNTIME / 'model_lod_store.hpp']
READERS = [RUNTIME / 'disc.cpp', RUNTIME / 'resource_archive.cpp', RUNTIME / 'model_lod_store.cpp']
PROCESSOR = [RUNTIME / 'gx_commands.cpp', RUNTIME / 'gx_vertex_decode.cpp', RUNTIME / 'gd_native.cpp',
             AURORA / 'lib/dolphin/gd/GDGeometry.cpp', AURORA / 'lib/gx/dl.cpp', AURORA / 'lib/gx/attr_fmt.cpp']
PROCESSOR_HEADERS = [RUNTIME / 'gx_commands.hpp', RUNTIME / 'gx_batch_cache.hpp', RUNTIME / 'gx_compact_cache.hpp',
                     RUNTIME / 'gx_vertex_decode.hpp', AURORA / 'include/aurora/dl.hpp']


def digest(paths):
    h = hashlib.sha256()
    for p in paths:
        h.update(str(p).encode())
        h.update(Path(p).read_bytes())
    return h.hexdigest()


def build(tools):
    """Builds the converter and the proof; reuses binaries whose inputs are unchanged."""
    tools.mkdir(parents=True, exist_ok=True)
    targets = {
        'model_lod_build': (['clang++', '-std=c++20', '-O2', '-Wall', '-Wextra', str(HERE / 'model_lod_build.cpp'),
                             *map(str, READERS)],
                            [HERE / 'model_lod_build.cpp', *HEADERS, *READERS]),
        'model_lod_unit_test': (['clang++', '-std=c++20', '-Wall', '-Wextra', '-O1', '-g', '-fsanitize=address,undefined',
                                 '-fno-omit-frame-pointer', str(HERE / 'model_lod_unit_test.cpp'), str(RUNTIME / 'model_lod_store.cpp')],
                                [HERE / 'model_lod_unit_test.cpp', *HEADERS, RUNTIME / 'model_lod_store.cpp']),
        'model_lod_batch_test': (['clang++', '-std=c++20', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1', '-I' + str(AURORA / 'include'),
                                  '-Wall', '-Wextra', '-Wno-switch', '-O1', '-g', '-fsanitize=address,undefined',
                                  '-fno-omit-frame-pointer', str(HERE / 'model_lod_batch_test.cpp'), *map(str, PROCESSOR),
                                  *map(str, READERS)],
                                 [HERE / 'model_lod_batch_test.cpp', *HEADERS, *READERS, *PROCESSOR, *PROCESSOR_HEADERS]),
    }
    built = {}
    for name, (command, inputs) in targets.items():
        target = tools / name
        receipt = target.with_suffix('.build.json')
        key = digest(inputs)
        prior = json.loads(receipt.read_text()) if receipt.exists() else {}
        if not (target.exists() and prior.get('inputs') == key):
            subprocess.run(command + ['-o', str(target)], check=True)
            receipt.write_text(json.dumps({'inputs': key, 'command': command}, indent=1) + '\n')
        built[name] = target
    return built


def summary(report):
    data = json.loads(report.read_text())
    rows = [('all', data['all'])] + sorted(data['stages'].items(), key=lambda kv: -kv[1]['triangles'])
    print(f"{'stage':12} {'models':>6} {'shapes':>7} {'levels':>6} {'triangles':>10} {'L1':>9} {'L2':>9} {'L3':>9}")
    for name, t in rows[:40]:
        full = max(t['triangles'], 1)
        print(f"{name:12} {t['models']:6} {t['shapes']:7} {t['shapes_with_levels']:6} {t['triangles']:10} "
              f"{t['level1']:9} {t['level2']:9} {t['level3']:9}   {100 * t['level1'] / full:5.1f}% "
              f"{100 * t['level2'] / full:5.1f}% {100 * t['level3'] / full:5.1f}%")
    print(f"store {data['store_bytes']} bytes, {data['records']} records")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--disc', type=Path, help='zelda.twilight bundle or GameCube image (never uploaded)')
    parser.add_argument('--out', type=Path, help='folder for zelda.lod, report.json, shapes.txt (must not hold them)')
    parser.add_argument('--tools', type=Path, default=ROOT / 'Build/model-lod-tools')
    parser.add_argument('--test', action='store_true', help='prove the batching against the runtime processor and read the store back')
    parser.add_argument('--unit', action='store_true', help='only the unit test (simplifier, batching, record parser; no disc)')
    args = parser.parse_args()
    if args.unit:
        tools = build(args.tools.resolve())
        subprocess.run([str(tools['model_lod_unit_test'])], check=True)
        return 0
    if not args.disc or not args.out:
        parser.error('--disc and --out are required')
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    store, report, shapes = out / 'zelda.lod', out / 'report.json', out / 'shapes.txt'
    for p in (store, report, shapes):
        if os.path.lexists(p):
            raise SystemExit(f'{p} exists; refusing overwrite')
    tools = build(args.tools.resolve())
    subprocess.run([str(tools['model_lod_build']), '--disc', str(args.disc), '--store', str(store), '--report', str(report),
                    '--log', str(shapes)], check=True)
    summary(report)
    if args.test:
        env = {**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
        subprocess.run([str(tools['model_lod_batch_test']), str(args.disc), str(store)], check=True, env=env)
    return 0


if __name__ == '__main__':
    sys.exit(main())
