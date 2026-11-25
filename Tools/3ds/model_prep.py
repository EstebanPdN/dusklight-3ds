#!/usr/bin/env python3
"""Offline model preparation survey for zelda.twilight (Mac only, private data).

Answers, on the user's own disc data, what the asset extractor could prepare
for the 3DS so models cost fewer draws at run time, without shipping bigger
data. Every number is a count of the draws the runtime makes (one per batch of
runtime/gx_commands.cpp), and the test runs that command processor itself on
every room display list to prove the counts and the triangle order.

  python3 -I Tools/3ds/model_prep.py --disc Build/perf30-data/zelda.twilight \\
      --tools Build/alpha13/build-extractor --report Build/alpha13/build-extractor/model-prep.json [--test]
  python3 -I Tools/3ds/model_prep.py --disc Build/perf30-data/zelda.twilight --skin \\
      [--detail /res/Object/Kmdl.arc ...]

Candidates (model_prep_survey.cpp): exact merges of room shapes that share a
material, the same with the draw order given up, merges by shared textures,
the batch cap of the command processor, and matrix-palette merges of skinned
shapes. --skin (model_prep_skin.cpp) plans every skinned shape of the disc
with the runtime's own palette planner (runtime/skin_palette.hpp) under three
uniform-row layouts and prints the draws left, per archive with --detail. Writes a JSON report (never overwrites) and prints a summary. Reads
the disc only; writes nothing next to it.
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
SKIN = [RUNTIME / 'skin_palette.hpp']
HEADERS = [HERE / 'model_prep_bmd.hpp', HERE / 'model_prep_walk.hpp', RUNTIME / 'disc.hpp', RUNTIME / 'resource_archive.hpp']
READERS = [RUNTIME / 'disc.cpp', RUNTIME / 'resource_archive.cpp']
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
    """Builds the survey and the proof; reuses binaries whose inputs are unchanged."""
    tools.mkdir(parents=True, exist_ok=True)
    targets = {
        'model_prep_survey': (['clang++', '-std=c++20', '-O2', '-Wall', '-Wextra', str(HERE / 'model_prep_survey.cpp'),
                               *map(str, READERS)],
                              [HERE / 'model_prep_survey.cpp', *HEADERS, *READERS]),
        'model_prep_batch_test': (['clang++', '-std=c++20', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1', '-I' + str(AURORA / 'include'),
                                   '-Wall', '-Wextra', '-Wno-switch', '-O1', '-g', '-fsanitize=address,undefined',
                                   '-fno-omit-frame-pointer', str(HERE / 'model_prep_batch_test.cpp'), *map(str, PROCESSOR),
                                   *map(str, READERS)],
                                  [HERE / 'model_prep_batch_test.cpp', *HEADERS, *READERS, *PROCESSOR, *PROCESSOR_HEADERS]),
    }
    targets['model_prep_skin'] = (['clang++', '-std=c++20', '-O2', '-Wall', '-Wextra', str(HERE / 'model_prep_skin.cpp'),
                                    *map(str, READERS)],
                                   [HERE / 'model_prep_skin.cpp', *HEADERS, *SKIN, *READERS])
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


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--disc', type=Path, required=True, help='zelda.twilight bundle or GameCube image (never uploaded)')
    parser.add_argument('--report', type=Path, help='JSON report to write (refuses an existing file)')
    parser.add_argument('--skin', action='store_true', help='skinned shapes under wider matrix palettes (prints, writes nothing)')
    parser.add_argument('--detail', action='append', default=[], help='archive to list shape by shape with --skin')
    parser.add_argument('--lights', type=int, default=0, help='lights of a lit draw at run time with --skin (0: the files\' masks)')
    parser.add_argument('--capacity', type=int, default=0, help='palette entries per merged draw with --skin (0: per material)')
    parser.add_argument('--tools', type=Path, default=ROOT / 'Build/model-prep-tools')
    parser.add_argument('--test', action='store_true', help='run the command-processor proof on every room display list')
    args = parser.parse_args()
    tools = build(args.tools.resolve())
    if args.skin:
        detail = [x for d in args.detail for x in ('--detail', d)]
        subprocess.run([str(tools['model_prep_skin']), '--disc', str(args.disc), '--lights', str(args.lights), '--capacity', str(args.capacity), *detail], check=True)
        return 0
    if args.report is None:
        raise SystemExit('--report is required without --skin')
    if os.path.lexists(args.report):
        raise SystemExit('Report already exists; refusing overwrite')
    subprocess.run([str(tools['model_prep_survey']), '--disc', str(args.disc), '--report', str(args.report)], check=True)
    if args.test:
        env = {**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
        subprocess.run([str(tools['model_prep_batch_test']), str(args.disc)], check=True, env=env)
    return 0


if __name__ == '__main__':
    sys.exit(main())
