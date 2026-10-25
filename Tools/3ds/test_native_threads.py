#!/usr/bin/env python3
"""Exercise native OS/real JKR worker lifecycle with host threads and compile ARM production backends."""
import argparse, datetime, hashlib, json, os, subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[2]
src = root / 'Source'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
files = ['platforms/3ds/tests/os_thread_test.cpp', 'platforms/3ds/tests/host_thread_backend.cpp', 'platforms/3ds/tests/host_primitives.cpp',
         'platforms/3ds/runtime/os_thread.cpp', 'platforms/3ds/runtime/os_sync.cpp', 'platforms/3ds/runtime/bootstrap.cpp',
         'platforms/3ds/runtime/os_arena.cpp', 'platforms/3ds/runtime/os_time.cpp', 'src/helpers/string.cpp', 'libs/JSystem/src/JSupport/JSUList.cpp']
files += ['libs/JSystem/src/JKernel/' + p + '.cpp' for p in ['JKRHeap', 'JKRExpHeap', 'JKRSolidHeap', 'JKRDisposer', 'JKRThread']]
files += ['libs/JSystem/src/JAudio2/JASHeapCtrl.cpp', 'libs/JSystem/src/JAudio2/JASTaskThread.cpp', 'platforms/3ds/runtime/jas_critical_section.cpp']
headers = ['platforms/3ds/runtime/os_thread.hpp', 'platforms/3ds/runtime/thread_backend.hpp', 'platforms/3ds/runtime/bootstrap.hpp',
           'platforms/3ds/runtime/os_sync.hpp', 'platforms/3ds/runtime/primitives.hpp', 'platforms/3ds/runtime/clock.hpp', 'include/helpers/native_thread.h',
           'libs/JSystem/include/JSystem/JAudio2/JASHeapCtrl.h', 'libs/JSystem/include/JSystem/JAudio2/JASTaskThread.h']
production = ['platforms/3ds/runtime/thread_backend_ctru.cpp', 'platforms/3ds/runtime/primitives_ctru.cpp']
hashes = {str(p.relative_to(root)): sha(p) for p in [Path(__file__).resolve(), *[src / p for p in files + headers + production]]}
common = ['-std=c++20', '-O1', '-g', '-fno-omit-frame-pointer', '-ffunction-sections', '-fdata-sections',
          '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1', '-DDUSK_NATIVE_THREAD_RUNTIME=1', '-DDUSK_TEST_KERNEL=1', '-DDUSK_BUILDING_GAME=1',
          '-DVERSION=0', '-DPARTIAL_DEBUG=1', '-DAVOID_UB=1', '-DMTX_USE_PS=1', '-Wno-multichar', '-Wno-deprecated-declarations', '-Wno-invalid-noreturn']
common += ['-I' + str(src / p) for p in ['include', 'libs/JSystem/include', 'extern/aurora/include', 'extern/aurora/include/dolphin', 'sdk/include', 'src']]
for p in ['extern/aurora/include/dolphin/types.h', 'include/global.h']: common += ['-include', str(src / p)]
rec = {'status': 'running', 'started_at': datetime.datetime.now(datetime.timezone.utc).isoformat(), 'input_sha256': hashes, 'cases': [], 'commands': [],
       'origin': 'Actual host std::thread/OS/JKR lifecycle under ASan/UBSan and TSan; ARM production compile only',
       'limitations': ['No libctru thread execution or game startup proof', 'Cancellation/suspension are cooperative with bounded acknowledgement/join',
                       'Host priority backend models API values; native scheduling needs emulator/hardware measurement']}
report = out / 'results.json'; report.write_text(json.dumps(rec, indent=2) + '\n')
try:
    for name, sanitizer in [('asan-ubsan', 'address,undefined'), ('tsan', 'thread')]:
        binary = out / name
        command = ['clang++', *common, '-fsanitize=' + sanitizer, '-Wl,-dead_strip', *[str(src / p) for p in files], '-o', str(binary)]
        rec['commands'].append(command)
        with (out / (name + '-build.log')).open('w') as log: subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60,
                             env={**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1', 'TSAN_OPTIONS': 'halt_on_error=1'})
        (out / (name + '-test.log')).write_text(run.stdout + run.stderr)
        print(name, run.returncode, run.stdout, flush=True); run.check_returncode()
        rec['cases'].append({'name': name, 'exit_code': run.returncode, 'binary_sha256': sha(binary)})
    for p in ['platforms/3ds/runtime/os_thread.cpp', *production, 'libs/JSystem/src/JKernel/JKRThread.cpp', 'libs/JSystem/src/JAudio2/JASTaskThread.cpp']:
        path = src / p
        command = ['/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++', *common, '-I/opt/devkitpro/libctru/include', '-O2', '-march=armv6k', '-mtune=mpcore', '-mfloat-abi=hard', '-mfpu=vfp', '-mtp=soft', '-D__3DS__', '-fstack-usage', '-c', str(path), '-o', str(out / (path.stem + '-arm.o'))]
        rec['commands'].append(command)
        with (out / (path.stem + '-arm.log')).open('w') as log: subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=90)
    assert all(sha(root / p) == digest for p, digest in hashes.items())
    rec.update(status='pass', input_unchanged=True, arm_stack_usage={p.name: p.read_text() for p in out.glob('*.su')})
except (OSError, subprocess.SubprocessError, AssertionError) as error:
    rec.update(status='fail', error=str(error)); print(str(error), flush=True)
finally:
    rec['finished_at'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    report.write_text(json.dumps(rec, indent=2) + '\n')
raise SystemExit(rec['status'] != 'pass')
