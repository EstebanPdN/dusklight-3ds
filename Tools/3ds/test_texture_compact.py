#!/usr/bin/env python3
"""Alpha 12 compact texture storage: 16-bit PICA plans must hold the RGBA8 picture.

Builds Source/platforms/3ds/tests/texture_compact_test.cpp with the runtime
decoder (runtime/texture_gx.cpp) under the address and UB sanitizers and runs it.
"""
import argparse, subprocess, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / 'Source/platforms/3ds'
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'Diagnostics/alpha12/texture-compact')
    out = parser.parse_args().output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    binary = out / 'texture_compact_test'
    subprocess.run(['clang++', '-std=c++20', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-Wall', '-Wextra', '-Werror', '-o', str(binary),
                    str(NATIVE / 'tests/texture_compact_test.cpp'), str(NATIVE / 'runtime/texture_gx.cpp')], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    sys.stdout.write(result.stdout); sys.stderr.write(result.stderr)
    return result.returncode
if __name__ == '__main__':
    sys.exit(main())
