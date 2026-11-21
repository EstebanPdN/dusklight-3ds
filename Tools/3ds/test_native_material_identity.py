#!/usr/bin/env python3
"""Plan identity of the PICA material planner against a reference planner.

With the sum rules off the current planner must equal the reference on every
material; with them on, every reference plan must stay byte-identical and
every added plan must carry a sum-rule bit and match the GX reference
evaluator. The reference defaults to the Alpha 7 wave 1 snapshot. Survey
files (one material per line, numbers after the fourth tab, as written by the
disc material survey) are checked too when given or present.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'Diagnostics/implementation-21/native-material-identity'
REFERENCE = ROOT / 'Build/alpha7/snapshots/wave1/Source/platforms/3ds/runtime'
SURVEYS = [Path('/tmp/alpha6-analysis/tev-coverage/materials.tsv'),
           Path('/tmp/alpha6-analysis/tev-coverage/livegood.tsv')]
REFERENCE_FILES = ['gx_pica_material.cpp', 'gx_pica_material.hpp', 'gx_material.hpp']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=OUT)
    parser.add_argument('--reference', type=Path, default=REFERENCE,
                        help='directory with the reference gx_pica_material.cpp/.hpp and gx_material.hpp')
    parser.add_argument('--materials', type=Path, nargs='*', default=None,
                        help='survey files (default: the Alpha 6 disc and dump surveys when present)')
    args = parser.parse_args()
    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    native = ROOT / 'Source/platforms/3ds'
    sources = [native/'tests/native_material_identity_test.cpp',
               native/'tests/native_material_identity_reference.cpp',
               native/'runtime/gx_pica_material.cpp']
    headers = [native/'runtime/gx_pica_material.hpp', native/'runtime/gx_material.hpp',
               native/'tests/material_equivalence.hpp']
    reference = [args.reference/name for name in REFERENCE_FILES]
    surveys = args.materials if args.materials is not None else [p for p in SURVEYS if p.is_file()]
    receipt = {'status': 'running', 'reference': str(args.reference), 'surveys': [str(p) for p in surveys],
               'origin': 'host ASan/UBSan execution; no GPU or console execution',
               'limits': ['normalized real arithmetic only for added plans; GPU fixed-point rounding is not modelled']}
    report = out/'results.json'
    try:
        missing = [str(p) for p in reference + sources + headers + surveys if not p.is_file()]
        if missing:
            raise FileNotFoundError('missing inputs: ' + ', '.join(missing))
        copy = out/'reference'
        copy.mkdir(exist_ok=True)
        for path in reference:
            shutil.copyfile(path, copy/path.name)
        inputs = [*sources, *headers, *reference, Path(__file__).resolve()]
        receipt['input_sha256'] = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        command = ['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I' + str(out),
                   *map(str, sources), '-o', str(out/'test')]
        receipt['commands'] = [command]
        report.write_text(json.dumps(receipt, indent=2) + '\n')
        with (out/'build.log').open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
        result = subprocess.run([str(out/'test'), *map(str, surveys)], capture_output=True, text=True, timeout=1800,
                                env={'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1',
                                     'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1',
                                     'PATH': '/usr/bin:/bin'})
        (out/'test.log').write_text(result.stdout + result.stderr)
        print(result.stdout + result.stderr, end='')
        result.check_returncode()
        receipt.update(status='pass')
    except (OSError, subprocess.SubprocessError) as error:
        receipt.update(status='fail', error=str(error))
        if (out/'build.log').is_file():
            print((out/'build.log').read_text())
        print(error)
    finally:
        report.write_text(json.dumps(receipt, indent=2) + '\n')
    return int(receipt['status'] != 'pass')


if __name__ == '__main__':
    raise SystemExit(main())
