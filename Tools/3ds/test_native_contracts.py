#!/usr/bin/env python3
"""Exercise actual JKR code with native policy and test-only host OS primitives."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'Diagnostics/implementation-02'
BUILD = ROOT / 'Build/3ds-host-contracts'

def main():
    global OUT,BUILD
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--heap-only',action='store_true',help='Run actual native allocator contracts; do not claim whole-PC preprocessing parity')
    parser.add_argument('--thread-sanitizer',action='store_true',help='Use ThreadSanitizer instead of ASan/UBSan for concurrent heap/range-cache contracts')
    args=parser.parse_args()
    if args.output:
        OUT=args.output.resolve();BUILD=OUT/'build';OUT.mkdir(parents=True,exist_ok=False)
    else:OUT.mkdir(exist_ok=True)
    BUILD.mkdir(parents=True,exist_ok=True)
    receipt = OUT / 'native-contract-tests.json'
    receipt.write_text(json.dumps({'status': 'in progress; prior result superseded'}) + '\n')
    sanitizer='thread' if args.thread_sanitizer else 'address,undefined'
    cmd = ['clang++', '-std=c++20', '-O1', '-g', '-fsanitize='+sanitizer,
           '-fno-omit-frame-pointer', '-Wno-multichar', '-Wno-invalid-noreturn',
           '-Wno-deprecated-declarations']
    cmd += ['-D' + d for d in ['TARGET_PC=1', 'DUSK_TARGET_3DS=1', 'DUSK_BUILDING_GAME=1',
                              'VERSION=0', 'PARTIAL_DEBUG=1', 'AVOID_UB=1', 'MTX_USE_PS=1']]
    cmd += ['-I' + str(ROOT / p) for p in ['Source/include', 'Source/libs/JSystem/include',
            'Source/extern/aurora/include', 'Source/extern/aurora/include/dolphin', 'Source/sdk/include', 'Source/src']]
    for p in ['Source/extern/aurora/include/dolphin/types.h', 'Source/include/global.h']:
        cmd += ['-include', str(ROOT / p)]
    cmd += [str(ROOT / 'Source' / p) for p in [
        'platforms/3ds/tests/heap_test.cpp', 'platforms/3ds/tests/host_heap_environment.cpp',
        'libs/JSystem/src/JKernel/JKRHeap.cpp', 'libs/JSystem/src/JKernel/JKRExpHeap.cpp',
        'libs/JSystem/src/JKernel/JKRSolidHeap.cpp',
        'libs/JSystem/src/JKernel/JKRDisposer.cpp', 'libs/JSystem/src/JSupport/JSUList.cpp',
        'src/helpers/string.cpp']]
    cmd += ['-o', str(BUILD / 'heap-test')]
    (OUT / 'heap-build-command.json').write_text(json.dumps(cmd, indent=2) + '\n')
    with (OUT / 'heap-build.log').open('w') as log:
        subprocess.run(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
    with (OUT / 'heap-test.log').open('w') as log:
        env = {**os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1',
               'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1', 'TSAN_OPTIONS':'halt_on_error=1'}
        subprocess.run([str(BUILD / 'heap-test')], cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)

    if args.heap_only:
        sources=[Path(x)for x in cmd if x.endswith('.cpp')]+[ROOT/('Source/libs/JSystem/include/JSystem/JKernel/'+name) for name in ['JKRHeap.h','JKRExpHeap.h']]+[Path(__file__).resolve()]
        report={'status':'pass','origin':'Host actual JKR allocator; OS environment substituted, no New3DS execution','sanitizers':sanitizer,'input_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in sources},'arrays':'overflow, natural placement alignment, custom head/tail alignment, reverse destruction','live_ranges':'interior bounds, resize, reuse, tail disposal, whole-heap reset and 4000 concurrent allocation/query/free cycles; three threads share one heap','pc_process_preprocessing':'not run; heap-only scope'}
        receipt.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2));return

    db = json.loads((ROOT / 'Build/pc-trace/compile_commands.json').read_text())
    entry = next(e for e in db if e['file'].endswith('/src/f_pc/f_pc_manager.cpp'))
    pp = shlex.split(entry['command'])
    i = pp.index('-o'); del pp[i:i+2]
    pp.remove('-c'); pp = [a for a in pp if a != '-DDUSK_GX_TRACE=1']
    pp += ['-E', '-P']
    outputs = []
    for base in ['Source', 'Build/pc-baseline-source']:
        command = [a.replace(str(ROOT / 'Source'), str(ROOT / base)) for a in pp]
        r = subprocess.run(command, cwd=entry['directory'], text=True, capture_output=True, check=True)
        outputs.append(r.stdout.replace(str(ROOT / base), '<SOURCE>'))
    # Blank lines introduced by #if boundaries have no tokens at this location.
    equal = [s for s in outputs[0].splitlines() if s.strip()] == [s for s in outputs[1].splitlines() if s.strip()]
    assert equal, 'PC preprocessing changed beyond empty lines'
    report = {'status': 'pass', 'origin': 'host synthetic; not hardware acceptance',
              'sanitizers': sanitizer,
              'before_heap_allocation': 'null', 'exhaustion_without_fallback': 'pass',
              'empty_tiny_and_repeated_root_initialization': 'pass',
              'alignment_and_contents': 'pass', 'free_reclaims_arena': 'pass',
              'arrays': 'overflow rejection, aligned cookie, tail allocation, placement and reverse destructor order',
              'solid_heap': 'oversize/alignment rejection, head/tail allocations and reclaim',
              'oversized_and_invalid_alignment': 'pass', 'sanitizer_halt_on_error': True,
              'thread_local_heaps': 'four concurrent threads, 1000 allocations each; three share one heap',
              'logical_safe_area': 'pass', 'pc_process_preprocessing': 'identical except empty lines',
              'test_environment': 'host-only arena, recursive mutexes and diagnostic sinks; never linked into game'}
    receipt.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
