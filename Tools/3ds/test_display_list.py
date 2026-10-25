#!/usr/bin/env python3
"""Execute the pinned Aurora display-list processor with native dependencies only."""
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'Diagnostics/implementation-06/display-list'
AURORA = ROOT / 'Source/extern/aurora'


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    receipt = {'status': 'running', 'started_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
               'limits': ['synthetic host execution and ARM compile only', 'no GPU or gameplay claim',
                          'caller owns valid input span and sentinel-terminated descriptor/format arrays']}
    path = OUT / 'results.json'
    path.write_text(json.dumps(receipt) + '\n')
    sources = [AURORA / 'lib/gx/dl.cpp', AURORA / 'lib/gx/attr_fmt.cpp']
    test = ROOT / 'Source/platforms/3ds/tests/display_list_test.cpp'
    inputs = [*sources, test, AURORA / 'include/aurora/dl.hpp', AURORA / 'lib/gx/native_dl_support.hpp', Path(__file__).resolve()]
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    common = ['-std=c++20', '-O1', '-g', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1',
              '-I' + str(AURORA / 'include'), '-Wall', '-Wextra', '-Werror', '-fno-strict-aliasing']
    binary = OUT / 'display-list-test'
    commands = [['clang++', *common, '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                 *map(str,sources), str(test), '-o', str(binary)]]
    arm = '/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++'
    for source in sources:
        commands.append([arm, *common, '-O2', '-march=armv6k', '-mtune=mpcore', '-mfloat-abi=hard',
                         '-mfpu=vfp', '-mtp=soft', '-D__3DS__', '-fstack-usage', '-c', str(source),
                         '-o', str(OUT / (source.stem + '-arm.o'))])
    receipt.update(input_sha256=hashes, build_commands=commands, run_command=[str(binary)])
    result = 1
    try:
        with (OUT / 'build.log').open('w') as log:
            for command in commands:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=90)
        with (OUT / 'test.log').open('w') as log:
            subprocess.run([str(binary)], stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60,
                           env={**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1',
                                'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})
        assert hashes == {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        receipt.update(status='pass', input_unchanged=True,
                       outputs_sha256={p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                       for p in [binary, OUT/'dl-arm.o', OUT/'attr_fmt-arm.o']})
        result = 0
    except (subprocess.SubprocessError, AssertionError) as error:
        receipt.update(status='fail', error=str(error))
        print((OUT / 'build.log').read_text())
    finally:
        receipt['finished_at'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        path.write_text(json.dumps(receipt, indent=2) + '\n')
        if (OUT / 'test.log').exists(): print((OUT / 'test.log').read_text())
    return result

if __name__ == '__main__':
    raise SystemExit(main())
