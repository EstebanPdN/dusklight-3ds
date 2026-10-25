#!/usr/bin/env python3
"""Verify the native CPU command pipeline with the owner's title model in memory.

Metadata-only receipts; no model/image payloads or ISO digest are saved.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'Diagnostics/implementation-09/title-commands'


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    native = ROOT / 'Source/platforms/3ds'
    aurora = ROOT / 'Source/extern/aurora'
    sources = [native/'tests/title_command_probe.cpp', native/'runtime/disc.cpp',
               native/'runtime/resource_archive.cpp', native/'runtime/gx_commands.cpp', native/'runtime/gd_native.cpp', aurora/'lib/dolphin/gd/GDGeometry.cpp', native/'runtime/gx_vertex_decode.cpp', aurora/'lib/gx/dl.cpp', aurora/'lib/gx/attr_fmt.cpp']
    inputs = [*sources, native/'runtime/gx_commands.hpp', aurora/'include/dolphin/gd/GDBase.h', native/'runtime/gx_vertex_decode.hpp', aurora/'include/aurora/dl.hpp', aurora/'lib/gx/native_dl_support.hpp', Path(__file__).resolve()]
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    binary = OUT/'probe'
    command = ['clang++', '-std=c++20', '-O1', '-g', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1',
               '-I'+str(aurora/'include'), '-Wall', '-Wextra', '-Werror', '-Wno-switch', '-fsanitize=address,undefined',
               '-fno-omit-frame-pointer', *map(str,sources), '-o', str(binary)]
    receipt = {'status': 'running', 'source_read_only': True, 'payload_saved': False,
               'input_sha256': hashes, 'command': command,
               'origin': 'host sanitizer execution; original Aurora optimizer; no GPU execution'}
    report = OUT/'receipt.json'
    report.write_text(json.dumps(receipt)+'\n')
    try:
        with (OUT/'build.log').open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=90)
        iso = ROOT/'twilight.iso'
        before = iso.stat()
        result = subprocess.run([str(binary), str(iso)], capture_output=True, text=True, timeout=60,
                                env={**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1',
                                     'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})
        after = iso.stat()
        (OUT/'stdout.jsonl').write_text(result.stdout)
        (OUT/'stderr.log').write_text(result.stderr)
        receipt.update(exit=result.returncode,
                       iso_size_mtime_unchanged=(before.st_size,before.st_mtime_ns)==(after.st_size,after.st_mtime_ns))
        result.check_returncode()
        assert receipt['iso_size_mtime_unchanged']
        assert hashes == {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        receipt.update(status='pass', input_unchanged=True, metrics=[json.loads(line) for line in result.stdout.splitlines()])
        print(result.stdout, end='')
    except (OSError, subprocess.SubprocessError, AssertionError) as error:
        receipt.update(status='fail', error=str(error))
        print(str(error))
    finally:
        report.write_text(json.dumps(receipt,indent=2)+'\n')
    return int(receipt['status'] != 'pass')

if __name__ == '__main__':
    raise SystemExit(main())
