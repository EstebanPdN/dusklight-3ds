#!/usr/bin/env python3
"""Exercise actual JKR code with native policy and test-only host OS primitives."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'Diagnostics/implementation-02'
BUILD = ROOT / 'Build/3ds-host-contracts'

def main():
    global OUT,BUILD
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path)
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

    sources=[Path(x)for x in cmd if x.endswith('.cpp')]+[ROOT/('Source/libs/JSystem/include/JSystem/JKernel/'+name) for name in ['JKRHeap.h','JKRExpHeap.h']]+[Path(__file__).resolve()]
    report={'status':'pass','origin':'Host actual JKR allocator; OS environment substituted, no New3DS execution',
            'sanitizers':sanitizer,
            'input_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in sources},
            'arrays':'overflow, natural placement alignment, custom head/tail alignment, reverse destruction',
            'live_ranges':'interior bounds, resize, reuse, tail disposal, whole-heap reset and 4000 concurrent allocation/query/free cycles; three threads share one heap'}
    receipt.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))

if __name__ == '__main__':
    main()
