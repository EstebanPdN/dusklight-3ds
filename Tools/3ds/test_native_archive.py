#!/usr/bin/env python3
"""Original JKR memory archive integration under host ASan/UBSan; no extracted payloads."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'Source'
OUT = ROOT / 'Diagnostics/implementation-05/archive'

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    global OUT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--private-disc', type=Path)
    parser.add_argument('--output',type=Path,default=OUT)
    args = parser.parse_args()
    OUT=args.output
    OUT.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which('clang++')
    flags = ['-std=c++20', '-O1', '-g', '-fno-omit-frame-pointer', '-ffunction-sections',
             '-fdata-sections', '-fsanitize=address,undefined',
             '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections']
    definitions = ['TARGET_PC=1', 'DUSK_TARGET_3DS=1', 'DUSK_BUILDING_GAME=1', 'VERSION=0',
                   'PARTIAL_DEBUG=1', 'AVOID_UB=1', 'MTX_USE_PS=1', 'DUSK_TEST_KERNEL=1',
                   'DUSK_NATIVE_THREAD_RUNTIME=1']
    includes = ['include', 'libs/JSystem/include', 'extern/aurora/include',
                'extern/aurora/include/dolphin', 'sdk/include', 'src']
    flags += ['-D' + value for value in definitions] + ['-I' + str(SOURCE / p) for p in includes]
    flags += ['-Wno-multichar', '-Wno-deprecated-declarations', '-Wno-invalid-noreturn']
    for header in ['extern/aurora/include/dolphin/types.h', 'include/global.h']:
        flags += ['-include', str(SOURCE / header)]
    sources = ['platforms/3ds/tests/native_archive_test.cpp', 'platforms/3ds/tests/host_primitives.cpp',
               'platforms/3ds/tests/host_thread_backend.cpp', 'platforms/3ds/runtime/os_thread.cpp',
               'platforms/3ds/runtime/aram.cpp',
               'platforms/3ds/runtime/bootstrap.cpp', 'platforms/3ds/runtime/os_sync.cpp',
               'platforms/3ds/runtime/os_arena.cpp', 'platforms/3ds/runtime/os_time.cpp',
               'platforms/3ds/runtime/archive_bootstrap.cpp', 'platforms/3ds/runtime/dvd_provider.cpp',
               'platforms/3ds/runtime/disc.cpp', 'platforms/3ds/runtime/resource_archive.cpp',
               'src/helpers/string.cpp','src/dusk/version.cpp', 'libs/JSystem/src/JSupport/JSUList.cpp']
    sources += ['libs/JSystem/src/JKernel/' + name + '.cpp' for name in
                ['JKRHeap', 'JKRExpHeap', 'JKRSolidHeap', 'JKRDisposer', 'JKRArchivePri',
                 'JKRArchivePub', 'JKRMemArchive', 'JKRFileLoader', 'JKRFileCache', 'JKRFileFinder', 'JKRDecomp', 'JKRFile', 'JKRDvdFile', 'JKRThread', 'JKRAramPiece']]
    sources += ['platforms/3ds/runtime/'+name+'.cpp' for name in
                ['texture_resources','texture_gx','gx_state']]
    sources += ['platforms/3ds/tests/host_archive_boundaries.cpp']
    binary = OUT / 'native-archive-asan-ubsan-test'
    command = [compiler, *flags, *[str(SOURCE / p) for p in sources], '-o', str(binary)]
    options = {'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1',
               'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
    receipt = {'status': 'running', 'origin': 'host original-JKR and actual archive registration integration; OS primitives substituted, GPU calls forbidden, not ARM/hardware execution',
               'started_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
               'compiler': subprocess.check_output([compiler, '--version'], text=True).strip(),
               'command': command, 'sanitizers': options,
               'source_sha256': {p: sha(SOURCE / p) for p in sources},
               'runner_sha256': sha(Path(__file__)),
               'header_sha256': {p: sha(SOURCE / p) for p in [
                   'platforms/3ds/runtime/archive_bootstrap.hpp', 'platforms/3ds/runtime/dvd_provider.hpp',
                   'platforms/3ds/runtime/resource_archive.hpp', 'platforms/3ds/runtime/disc.hpp',
                   'libs/JSystem/include/JSystem/JKernel/JKRArchive.h',
                   'libs/JSystem/include/JSystem/JKernel/JKRMemArchive.h']},
               'private_disc': 'not requested', 'payload_output': 'none'}
    (OUT / 'commands.json').write_text(json.dumps({'build': command, 'run': [str(binary), str(OUT)],
                                                'environment': options}, indent=2) + '\n')
    result_code = 1
    try:
        build = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
        (OUT / 'build.log').write_text(build.stdout)
        receipt['build_exit'] = build.returncode
        build.check_returncode()
        receipt['binary_sha256'] = sha(binary)
        run = subprocess.run([str(binary), str(OUT)], env={**os.environ, **options}, text=True,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        (OUT / 'test.log').write_text(run.stdout)
        print(run.stdout, end=''); receipt['test_exit'] = run.returncode; run.check_returncode()
        if args.private_disc:
            receipt['private_command'] = [str(binary), '--private-disc', '<private-input-path-omitted>']
            before = args.private_disc.stat()
            run = subprocess.run([str(binary), '--private-disc', str(args.private_disc)],
                                 env={**os.environ, **options}, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
            (OUT / 'private-test.log').write_text(run.stdout)
            print(run.stdout, end=''); receipt['private_exit'] = run.returncode
            after = args.private_disc.stat()
            receipt['private_disc_unchanged'] = before.st_size == after.st_size and before.st_mtime_ns == after.st_mtime_ns
            receipt['private_disc'] = 'read-only source; no payload output; path omitted'
            run.check_returncode()
            if not receipt['private_disc_unchanged']: raise RuntimeError('private disc size/mtime changed')
        receipt['sources_unchanged'] = receipt['source_sha256'] == {p: sha(SOURCE / p) for p in sources}
        if not receipt['sources_unchanged']: raise RuntimeError('test source changed during execution')
        receipt['status'] = 'pass'; result_code = 0
    except (subprocess.SubprocessError, RuntimeError) as error:
        message = str(error)
        if args.private_disc: message = message.replace(str(args.private_disc), '<private-input-path-omitted>')
        receipt['status'] = 'fail'; receipt['error'] = message; print(message, file=sys.stderr)
    finally:
        receipt['finished_at'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        (OUT / 'results.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return result_code

if __name__ == '__main__':
    raise SystemExit(main())
