#!/usr/bin/env python3
"""Run the host J3D skin matrix batching and worker bank parity regression."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    output = parser.parse_args().output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    native = ROOT / 'Source/platforms/3ds'
    aurora = ROOT / 'Source/extern/aurora'
    sources = [
        native / 'tests/j3d_skin_matrix_runs_test.cpp',
        native / 'runtime/gx_commands.cpp',
        native / 'runtime/gx_vertex_decode.cpp',
        native / 'runtime/gd_native.cpp',
        aurora / 'lib/dolphin/gd/GDGeometry.cpp',
        aurora / 'lib/gx/dl.cpp',
        aurora / 'lib/gx/attr_fmt.cpp',
    ]
    binary = output / 'j3d-skin-matrix-runs-test'
    subprocess.run([
        'clang++', '-std=c++20', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1',
        '-I' + str(aurora / 'include'), '-Wno-switch', '-fno-strict-aliasing',
        '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
        *map(str, sources), '-o', str(binary),
    ], check=True, timeout=90)
    subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == '__main__':
    main()
