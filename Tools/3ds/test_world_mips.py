#!/usr/bin/env python3
"""worldMips host test: generated mip levels (runtime/texture_gx.cpp) against
an independent box average, alpha-tested coverage per level, refusals, and the
register rules of runtime/gx_world_mips.hpp. Host sanitizers only."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / 'Source/platforms/3ds'


def main():
    with tempfile.TemporaryDirectory() as out:
        binary = Path(out) / 'world_mips_test'
        subprocess.run(['clang++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1', '-I' + str(ROOT / 'Source/extern/aurora/include'),
                        str(NATIVE / 'tests/world_mips_test.cpp'), str(NATIVE / 'runtime/texture_gx.cpp'), '-o', str(binary)], check=True)
        env = {**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
        return subprocess.run([str(binary)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
