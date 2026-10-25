#!/usr/bin/env python3
"""Check original native static-profile lookup without simulating actors."""
import argparse
import hashlib
import json
import os
import signal
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--bounded-probe', action='store_true')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = ROOT / 'Source'
    inputs = [source / p for p in [
        'src/f_pc/f_pc_profile.cpp', 'include/f_pc/f_pc_profile.h',
        'include/f_pc/f_pc_name.h', 'include/global.h',
        'platforms/3ds/tests/process_profile_test.cpp',
    ]] + [Path(__file__).resolve()]
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in inputs}
    common = ['-std=c++20', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1',
              '-DDUSK_BUILDING_GAME=1', '-DVERSION=0', '-DPARTIAL_DEBUG=1',
              '-DAVOID_UB=1', '-DMTX_USE_PS=1',
              '-I' + str(source / 'include'),
              '-I' + str(source / 'extern/aurora/include'),
              '-I' + str(source / 'extern/aurora/include/dolphin'),
              '-include', str(source / 'extern/aurora/include/dolphin/types.h'),
              '-include', str(source / 'include/global.h'),
              '-Wall', '-Wextra', '-Werror', '-Wno-multichar']
    if args.bounded_probe:
        common.append('-DDUSK_PROFILE_LINK_PROBE=1')
    commands = [
        ['clang++', *common, '-O1', '-g', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer', str(inputs[0]), str(inputs[4]),
         '-o', str(out / 'test')],
        ['/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++', *common, '-O2',
         '-march=armv6k', '-mtune=mpcore', '-mfloat-abi=hard', '-mfpu=vfp',
         '-mtp=soft', '-D__3DS__', '-c', str(inputs[0]),
         '-o', str(out / 'profile-arm.o')],
    ]
    receipt = {'status': 'running', 'input_sha256': hashes, 'commands': commands,
               'origin': 'Original profile lookup under host sanitizers and ARM compilation',
               'limits': ['Only host profile-list data is substituted',
                          'No process creation, actor execution, gameplay or hardware acceptance']}
    try:
        with (out / 'build.log').open('w') as log:
            for command in commands:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                               check=True, timeout=90)
        result = subprocess.run([str(out / 'test')], capture_output=True, text=True,
                                timeout=30, env={**os.environ,
                                    'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1',
                                    'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})
        (out / 'test.log').write_text(result.stdout + result.stderr)
        print(result.stdout + result.stderr, end='')
        result.check_returncode()
        if args.bounded_probe:
            stopped = subprocess.run([str(out / 'test'), '--unsupported'],
                                     capture_output=True, text=True, timeout=30)
            (out / 'unsupported.log').write_text(stopped.stdout + stopped.stderr)
            assert stopped.returncode == -signal.SIGABRT
            assert 'unsupported process 1' in stopped.stderr
            receipt['unsupported_profile_aborts'] = True
        nm = subprocess.check_output([
            '/opt/devkitpro/devkitARM/bin/arm-none-eabi-nm', '-u', '-C',
            str(out / 'profile-arm.o')], text=True)
        (out / 'undefined.txt').write_text(nm)
        assert 'dusk::mods' not in nm
        assert hashes == {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in inputs}
        receipt.update(status='pass', input_unchanged=True,
                       native_mod_service_dependency=False)
    except (OSError, subprocess.SubprocessError, AssertionError) as error:
        receipt.update(status='fail', error=str(error))
        print((out / 'build.log').read_text())
    (out / 'results.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return int(receipt['status'] != 'pass')


if __name__ == '__main__':
    raise SystemExit(main())
