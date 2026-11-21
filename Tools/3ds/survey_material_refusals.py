#!/usr/bin/env python3
"""Survey the PICA material planner over material lists (Mac only).

Builds Source/platforms/3ds/tests/material_refusal_survey.cpp with the
runtime planner and runs it on survey files: one material per line, four
tab-separated header fields, then the numbers of the disc/dump survey format
(material_equivalence.hpp). For every material the planner runs with the
Alpha 12 operation rules off and on; refusals are counted by planner error
and, for operation refusals, by rule (MaterialOperationReason). Added plans are
checked against the GX reference evaluator. --group adds per-stage counts
(/res/Stage/<stage>/ archives, else the first header field); --list prints
every material still refused for an operation, with its TEV stages.

The surveys hold TEV/raster numbers only (no image or game data) and stay on
this Mac. Typical inputs: /tmp/alpha6-analysis/tev-coverage/materials.tsv
(disc MAT3), Build/alpha12/tev-survey/live-alpha11.tsv (live J3D materials of
the Alpha 11 console dumps, extracted with livemats.py).
"""
import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'Diagnostics/implementation-21/material-refusal-survey'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('surveys', type=Path, nargs='+')
    parser.add_argument('--output', type=Path, default=OUT)
    parser.add_argument('--group', action='store_true')
    parser.add_argument('--list', action='store_true')
    parser.add_argument('--exact', action='store_true', help='exact-constant policy (default: quantized, as the draw path)')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    native = ROOT / 'Source/platforms/3ds'
    binary = args.output / 'survey'
    subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-O2', '-g',
                    str(native / 'tests/material_refusal_survey.cpp'), str(native / 'runtime/gx_pica_material.cpp'),
                    '-o', str(binary)], check=True, timeout=300)
    flags = (['--group'] if args.group else []) + (['--list'] if args.list else []) + (['--exact'] if args.exact else [])
    result = subprocess.run([str(binary), *flags, *map(str, args.surveys)], capture_output=True, text=True, timeout=1800)
    (args.output / 'survey.txt').write_text(result.stdout + result.stderr)
    sys.stdout.write(result.stdout + result.stderr)
    return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
