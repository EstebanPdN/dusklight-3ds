#!/usr/bin/env python3
"""Fail the build when code that runs once per vertex was left as a function.

The native vertex decoder (Source/platforms/3ds/runtime/gx_rigid_decode.cpp)
reads every field of every vertex in loops whose bodies and readers must be
part of the loop: a call per vertex costs 30 to 40 instructions on each
vertex decoded in a picture, in every zone. Whether the compiler inlines them
depends on the size of the functions around them, so a change elsewhere in
the file can undo it without a warning: it did three times while Alpha 9 was
written (docs/3ds/NATIVE_ENGINE_RESEARCH.md, 2026-11-17).

The linker drops functions nothing calls, so a symbol listed below that is
still in the image is a function something calls.

usage: check_hot_inlining.py <nm> <image.elf>
"""
import re
import subprocess
import sys

# (what the demangled symbol must match, what it is)
FORBIDDEN = (
    (re.compile(r'\(anonymous namespace\)::(readPositionAt|readNormalAt|readTexcoordAt)<'), 'per-vertex reader'),
    (re.compile(r'\(anonymous namespace\)::(readColorAt|roundCoordinate)\('), 'per-vertex reader'),
    (re.compile(r'\(anonymous namespace\)::copyNative<'), 'per-vertex copy'),
    (re.compile(r'\(anonymous namespace\)::IndexedGather::read\('), 'per-vertex index read'),
    (re.compile(r'gx::decodeCompactRigidBatchMT\(.*\{lambda\('), 'field pass of decodeCompactRigidBatchMT'),
)


def main():
    if len(sys.argv) != 3:
        print('usage: check_hot_inlining.py <nm> <image.elf>', file=sys.stderr)
        return 2
    listing = subprocess.run([sys.argv[1], '-C', '--defined-only', sys.argv[2]], capture_output=True, text=True)
    if listing.returncode:
        print(f'check_hot_inlining.py: error: {sys.argv[1]} failed: {listing.stderr.strip()}', file=sys.stderr)
        return 2
    problems = []
    for line in listing.stdout.splitlines():
        parts = line.split(' ', 2)
        if len(parts) != 3 or parts[1] not in 'tTwW':
            continue
        for pattern, what in FORBIDDEN:
            if pattern.search(parts[2]):
                problems.append(f'{what} left out of line: {parts[2][:200]}')
                break
    for problem in problems:
        print(f'check_hot_inlining.py: error: {problem}', file=sys.stderr)
    if problems:
        print('check_hot_inlining.py: error: mark it [[gnu::always_inline]] or move the code that made its caller grow '
              'into a function of its own (see gx_rigid_decode.cpp, "always inlined").', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
