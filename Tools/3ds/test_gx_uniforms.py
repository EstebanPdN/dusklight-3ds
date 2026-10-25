#!/usr/bin/env python3
"""Verify exact Citro3D uniform shadow updates; no GPU/hardware timing claim."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    out = parser.parse_args().output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'Source/platforms/3ds/tests/gx_uniform_shadow_test.cpp'
    header = ROOT / 'Source/platforms/3ds/gx_uniform_pica.hpp'
    sdk = Path('/opt/devkitpro/libctru/include')
    inputs = [source, header, sdk / 'c3d/uniforms.h', sdk / 'c3d/maths.h', Path(__file__).resolve()]
    hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    commands = [
        ['clang++', '-std=c++20', '-O1', '-g', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer', '-I'+str(sdk), str(source), '-o', str(out / 'test')],
        [str(out / 'test')],
        ['/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++', '-std=c++20', '-O2',
         '-march=armv6k', '-mtune=mpcore', '-mfloat-abi=hard', '-mfpu=vfp',
         '-mtp=soft', '-D__3DS__', '-I'+str(sdk), '-Wall', '-Wextra', '-Werror',
         '-fstack-usage', '-c', str(source), '-o', str(out / 'uniform-arm.o')],
    ]
    receipt = {'status': 'running', 'origin': 'Host ASan/UBSan and ARM compilation using installed Citro3D headers; shared GPU shadow substituted by test globals',
               'commands': commands, 'input_sha256': hashes, 'physical_hardware': False}
    try:
        with (out / 'test.log').open('w') as log:
            for command in commands:
                result = subprocess.run(command, capture_output=True, text=True, timeout=90)
                log.write(result.stdout + result.stderr)
                result.check_returncode()
        assert hashes == {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        receipt.update(status='pass', inputs_unchanged=True)
    except (OSError, subprocess.SubprocessError, AssertionError) as error:
        receipt.update(status='fail', error=str(error))
    (out / 'results.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print((out / 'test.log').read_text())
    return int(receipt['status'] != 'pass')


if __name__ == '__main__':
    raise SystemExit(main())
