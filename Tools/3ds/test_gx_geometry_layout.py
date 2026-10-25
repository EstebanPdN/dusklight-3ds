#!/usr/bin/env python3
"""Check shared projection regression and measure indexed geometry ARM storage."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'Diagnostics/implementation-18/geometry-layout'


def main():
    global OUT
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("--output",type=Path,default=OUT)
    OUT=parser.parse_args().output.resolve()
    OUT.mkdir(parents=True, exist_ok=True)
    native = ROOT / 'Source/platforms/3ds'
    common = ['-std=c++20', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1',
              '-I' + str(ROOT / 'Source/extern/aurora/include'), '-Wall', '-Wextra', '-Werror']
    compiler = '/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++'
    arm = [compiler, *common, '-O2', '-g', '-march=armv6k', '-mtune=mpcore',
           '-mfloat-abi=hard', '-mfpu=vfp', '-mtp=soft', '-D__3DS__', '-fstack-usage']
    fixture = OUT / 'sizes.cpp'
    fixture.write_text('#include "gx_geometry.hpp"\nusing namespace dusk::native::gx;\n'
                       'extern "C" {\nchar transform_plan_bytes[sizeof(VertexTransformPlan)];\n'
                       'char decoded_vertex_bytes[sizeof(DecodedVertex)];\n'
                       'char transformed_vertex_bytes[sizeof(TransformedVertex)];\n'
                       'char prepared_geometry_bytes[sizeof(PreparedGeometry)];\n}\n')
    commands = [
        ['clang++', *common, '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
         str(native / 'tests/gx_pica_math_test.cpp'), str(native / 'runtime/gx_pica_math.cpp'), '-o', str(OUT / 'math-test')],
        [str(OUT / 'math-test')],
        *[[*arm, '-c', str(native / ('runtime/' + name + '.cpp')), '-o', str(OUT / (name + '.o'))]
          for name in ['gx_geometry', 'gx_vertex_transform', 'gx_pica_math']],
        [*arm, '-I' + str(native / 'runtime'), '-c', str(fixture), '-o', str(OUT / 'sizes.o')],
        ['/opt/devkitpro/devkitARM/bin/arm-none-eabi-nm', '-S', '--size-sort', str(OUT / 'sizes.o')],
    ]
    inputs = [native / 'tests/gx_pica_math_test.cpp', Path(__file__).resolve(), fixture]
    for name in ['gx_geometry', 'gx_vertex_transform', 'gx_pica_math', 'gx_vertex_decode', 'gx_commands', 'gx_state']:
        inputs.extend(native / ('runtime/' + name + ext) for ext in ['.cpp', '.hpp'])
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    receipt = {'status': 'running', 'commands': commands, 'input_sha256': hashes,
               'origin': 'Host sanitizer projection/alpha/scissor regression plus ARM compile/symbol sizes; no GPU or hardware execution'}
    report = OUT / 'results.json'
    report.write_text(json.dumps(receipt) + '\n')
    try:
        with (OUT / 'build-test.log').open('w') as log:
            for command in commands:
                result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        text=True, timeout=90)
                log.write(result.stdout)
                result.check_returncode()
        receipt['arm_bytes'] = {line.split()[-1]: int(line.split()[1], 16)
                                for line in result.stdout.splitlines() if line.split()[-1].endswith('_bytes')}
        receipt['arm_stack_usage'] = {p.name: p.read_text() for p in OUT.glob('*.su')}
        assert hashes == {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        receipt.update(status='pass', input_unchanged=True)
    except (OSError, subprocess.SubprocessError, AssertionError) as error:
        receipt.update(status='fail', error=str(error))
    report.write_text(json.dumps(receipt, indent=2) + '\n')
    print((OUT / 'build-test.log').read_text())
    return int(receipt['status'] != 'pass')


if __name__ == '__main__':
    raise SystemExit(main())
