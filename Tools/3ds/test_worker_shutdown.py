#!/usr/bin/env python3
"""Test production worker shutdown with real host threads; never builds ARM or controls an emulator."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--repeat', type=int, default=3)
    args = parser.parse_args()
    assert 1 <= args.repeat <= 100
    root = Path(__file__).resolve().parents[2]
    src = root / 'Source'
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    fixture = root / 'Diagnostics/implementation-44/worker-shutdown/worker_shutdown_test.cpp'
    runtime = src / 'platforms/3ds/runtime'
    tests = src / 'platforms/3ds/tests'
    focused = [fixture, runtime / 'os_thread.cpp', runtime / 'os_sync.cpp',
               tests / 'host_thread_backend.cpp', tests / 'host_primitives.cpp']
    regression = [tests / 'os_thread_test.cpp', tests / 'host_thread_backend.cpp', tests / 'host_primitives.cpp']
    regression += [runtime / (p + '.cpp') for p in ['os_thread', 'os_sync', 'bootstrap', 'os_arena', 'os_time', 'jas_critical_section']]
    regression += [src / p for p in ['src/helpers/string.cpp', 'libs/JSystem/src/JSupport/JSUList.cpp']]
    regression += [src / ('libs/JSystem/src/JKernel/' + p + '.cpp') for p in ['JKRHeap', 'JKRExpHeap', 'JKRSolidHeap', 'JKRDisposer', 'JKRThread']]
    regression += [src / ('libs/JSystem/src/JAudio2/' + p + '.cpp') for p in ['JASHeapCtrl', 'JASTaskThread']]
    inputs = set(focused + regression + [Path(__file__).resolve()])
    inputs.update(runtime / p for p in ['os_thread.hpp', 'thread_backend.hpp', 'thread_backend_ctru.cpp',
                                      'bootstrap.hpp', 'os_sync.hpp', 'primitives.hpp', 'clock.hpp'])
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    hashes = {str(p.relative_to(root)): sha(p) for p in sorted(inputs)}
    common = ['clang++', '-std=c++20', '-O1', '-g', '-fno-omit-frame-pointer', '-ffunction-sections', '-fdata-sections',
              '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1', '-DDUSK_NATIVE_THREAD_RUNTIME=1', '-DDUSK_TEST_KERNEL=1',
              '-DDUSK_BUILDING_GAME=1', '-DVERSION=0', '-DPARTIAL_DEBUG=1', '-DAVOID_UB=1', '-DMTX_USE_PS=1',
              '-Wno-multichar', '-Wno-deprecated-declarations', '-Wno-invalid-noreturn']
    common += ['-I' + str(src / p) for p in ['include', 'libs/JSystem/include', 'extern/aurora/include',
               'extern/aurora/include/dolphin', 'sdk/include', 'src', 'platforms/3ds/runtime']]
    for p in ['extern/aurora/include/dolphin/types.h', 'include/global.h']:
        common += ['-include', str(src / p)]
    scratch = out / 'tmp'
    scratch.mkdir()
    env = {**os.environ, 'TMPDIR': str(scratch), 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1',
           'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1', 'TSAN_OPTIONS': 'halt_on_error=1'}
    record = {'status': 'running', 'started_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'input_sha256': hashes, 'commands': [], 'cases': [],
              'scope': 'Actual production aggregate API + real host threads, ASan/UBSan and TSan; no ARM/native/emulator execution'}
    receipt = out / 'results.json'

    def save():
        receipt.write_text(json.dumps(record, indent=2) + '\n')

    def build(name, files, sanitizer, focused_build=True):
        binary = out / name
        command = common + ['-fsanitize=' + sanitizer, '-Wl,-dead_strip']
        if focused_build:
            command += ['-DDUSK_TEST_WORKER_SHUTDOWN=1']
        command += [*map(str, files), '-o', str(binary)]
        record['commands'].append(command)
        save()
        run = subprocess.run(command, capture_output=True, text=True, timeout=120, env=env)
        (out / (name + '-build.log')).write_text(run.stdout + run.stderr)
        run.check_returncode()
        return binary

    def run_case(name, binary, arguments=(), negative=False, expected_message=None):
        command = [str(binary), *arguments]
        record['commands'].append(command)
        run = subprocess.run(command, capture_output=True, text=True, timeout=30, env=env)
        (out / (name + '.log')).write_text(run.stdout + run.stderr)
        passed = (run.returncode != 0 if negative else run.returncode == 0)
        if expected_message:
            passed = passed and expected_message in run.stderr
        case = {'name': name, 'returncode': run.returncode, 'pass': passed,
                'negative_control': negative, 'binary_sha256': sha(binary)}
        record['cases'].append(case)
        print(name, 'PASS' if passed else 'FAIL', run.returncode, flush=True)
        save()
        assert passed, run.stdout + run.stderr

    save()
    try:
        for name, sanitizer in [('asan-ubsan', 'address,undefined'), ('tsan', 'thread')]:
            binary = build('shutdown-' + name, focused, sanitizer)
            for iteration in range(args.repeat):
                for case in ['mixed', 'nested', 'timeout', 'nested-cancel', 'kernel-tail', 'shared-deadline', 'mutex-timeout', 'retained-ownership', 'reuse-capacity', 'empty']:
                    run_case(f'{name}-{case}-{iteration + 1:02}', binary, [case])
            binary = build('regression-' + name, regression, sanitizer, False)
            run_case('regression-' + name, binary)
        # Mutate isolated copies only. The first omits the barrier's collection;
        # the second frees after logical done while a real TLS destructor waits.
        original = (runtime / 'os_thread.cpp').read_text()
        controls = {
            'omitted-join': (original.replace('    for (;;) {\n        Control* ready = nullptr;',
                              '    return result(WorkerShutdownStatus::complete);\n    for (;;) {\n        Control* ready = nullptr;', 1),
                             'mixed', 'result.remaining.workers'),
            'early-free': (original.replace('if (!platformJoinThread(ready->handle, remainingTime()))', 'if (false)', 1),
                           'kernel-tail', 'stack/TLS free before actual thread join'),
        }
        for name, (mutated, case, expected) in controls.items():
            assert mutated != original
            mutation = out / (name + '.cpp')
            mutation.write_text(mutated)
            files = [mutation if p == runtime / 'os_thread.cpp' else p for p in focused]
            binary = build(name, files, 'address,undefined')
            run_case(name, binary, [case], True, expected)
        assert all(sha(root / p) == digest for p, digest in hashes.items())
        record.update(status='pass', inputs_unchanged=True)
    except (OSError, subprocess.SubprocessError, AssertionError) as error:
        record.update(status='fail', error=str(error))
        print(str(error), flush=True)
    finally:
        record['finished_at'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        save()
    return record['status'] != 'pass'


if __name__ == '__main__':
    raise SystemExit(main())
