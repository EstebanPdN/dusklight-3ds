#!/usr/bin/env python3
"""Bounded FST access checks; synthetic data by default, optional local read-only probe."""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'Source/platforms/3ds'
BUILD = ROOT / 'Build/3ds-disc-files'
OUT = ROOT / 'Diagnostics/implementation-04-disc'


def main():
    global BUILD, OUT
    parser = argparse.ArgumentParser()
    parser.add_argument('--private-disc', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output:
        OUT = args.output
        BUILD = OUT / 'build'
    BUILD.mkdir(parents=True, exist_ok=True)
    OUT.mkdir(parents=True, exist_ok=True)
    binary = BUILD / 'disc-files-test'
    flags = ['-std=c++20', '-O1', '-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined',
             '-Wall', '-Wextra', '-Werror']
    command = ['clang++', *flags, str(SOURCE / 'runtime/disc.cpp'),
               str(SOURCE / 'tests/disc_file_test.cpp'), '-o', str(binary)]
    (OUT / 'build-command.json').write_text(json.dumps(command, indent=2) + '\n')
    with (OUT / 'build.log').open('w') as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
    env = {**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1',
           'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
    result = subprocess.run([str(binary), str(BUILD)], env=env, capture_output=True, text=True, timeout=60)
    (OUT / 'synthetic.log').write_text(result.stdout + result.stderr)
    print(result.stdout, end='')
    result.check_returncode()
    summary = {'synthetic': 'pass', 'sanitizers': 'ASan/UBSan', 'hardware': 'not run',
               'private_disc': 'not requested', 'payload_output': 'none'}
    if args.private_disc:
        before = args.private_disc.stat()
        result = subprocess.run([str(binary), '--private-disc', str(args.private_disc)],
                                env=env, capture_output=True, text=True, timeout=60)
        (OUT / 'private-probe.log').write_text(result.stdout + result.stderr)
        print(result.stdout, end='')
        result.check_returncode()
        after = args.private_disc.stat()
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise RuntimeError('Private source size/mtime unexpectedly changed')
        summary['private_disc'] = 'read-only bounded probe passed; size/mtime unchanged'
    (OUT / 'results.json').write_text(json.dumps(summary, indent=2) + '\n')


if __name__ == '__main__':
    main()
