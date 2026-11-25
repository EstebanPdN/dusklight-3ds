#!/usr/bin/env python3
"""Merged matrix groups of skinned shapes (features::Id::skinPalette).

Builds and runs Source/platforms/3ds/tests/skin_palette_test.cpp: the
palette planner against GX's slot rules on hand-made and random shapes (every
vertex names the matrix its group's slot held), the merged lists byte for
byte, the refusals, and the palette's uniform rows for every light and
texture unit count (never the projection, colour, fog, light or unit rows).
"""
import argparse
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    output = parser.parse_args().output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    native = ROOT / 'Source/platforms/3ds'
    aurora = ROOT / 'Source/extern/aurora/include'
    sources = [native / 'tests/skin_palette_test.cpp', native / 'runtime/gx_vertex_decode.cpp', native / 'runtime/gx_rigid_decode.cpp',
               native / 'runtime/gx_vertex_transform.cpp', native / 'runtime/gx_pica_math.cpp']
    binary = output / 'skin-palette-test'
    subprocess.run(['clang++', '-std=c++20', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1', '-I' + str(aurora), '-Wall', '-Wextra', '-Werror',
                    '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', *map(str, sources), '-o', str(binary)],
                   check=True, timeout=300)
    subprocess.run([str(binary)], check=True, timeout=300,
                   env={**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})


if __name__ == '__main__':
    main()
