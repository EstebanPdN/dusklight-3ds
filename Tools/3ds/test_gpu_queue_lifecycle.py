#!/usr/bin/env python3
"""Compile unchanged wrapper and pinned gxqueue.c against bounded host events."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / 'Source/platforms/3ds/runtime'
CTRU = ROOT / 'Upstream/extracted/libctru-2.7.0/libctru'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',required=True,type=Path)
    out=parser.parse_args().output.resolve()
    out.mkdir(exist_ok=False)
    stubs = {
        '3ds/types.h': '''#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef uint64_t u64; typedef int64_t s64; typedef int32_t Result;
#define U64_MAX UINT64_MAX
#define BIT(n) (1u << (n))
''',
        '3ds/svc.h': '''#pragma once
#include <3ds/types.h>
enum { USERBREAK_PANIC = 0 };
u64 svcGetSystemTick(void); void svcSleepThread(s64); void svcBreak(int);
''',
        '3ds/synchronization.h': '''#pragma once
typedef int LightLock;
void LightLock_Lock(LightLock*); int LightLock_TryLock(LightLock*); void LightLock_Unlock(LightLock*);
''',
        '3ds/services/gspgpu.h': '''#pragma once
#include <3ds/types.h>
typedef enum {GSPGPU_EVENT_PSC0, GSPGPU_EVENT_PSC1, GSPGPU_EVENT_VBlank0,
GSPGPU_EVENT_VBlank1, GSPGPU_EVENT_PPF, GSPGPU_EVENT_P3D, GSPGPU_EVENT_DMA} GSPGPU_Event;
Result gspSubmitGxCommand(const u32*); void gspWaitForAnyEvent(void);
''',
        '3ds.h': '''#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <3ds/types.h>
#include <3ds/svc.h>
#include <3ds/synchronization.h>
#include <3ds/gpu/gx.h>
#include <3ds/services/gspgpu.h>
void GPUCMD_GetBuffer(u32**,u32*,u32*); void GPUCMD_SetBuffer(u32*,u32,u32);
#ifdef __cplusplus
}
#endif
''',
        'citro3d.h': '''#pragma once
#include <3ds.h>
enum { C3D_FRAME_SYNCDRAW=1, C3D_FRAME_NONBLOCK=2 };
extern "C" void C3D_FrameSplit(u8);
extern "C" void C3D_FrameSync();
extern "C" bool C3D_FrameBegin(u8);
extern "C" void C3D_FrameEnd(u8);
''',
    }
    for name, content in stubs.items():
        path = out / 'include' / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
    sources = [NATIVE / 'gpu_sync_ctru.cpp', NATIVE / 'ctru_gxqueue.c', NATIVE.parent / 'tests/gpu_queue_lifecycle_test.cpp']
    inputs = [*sources, Path(__file__).resolve(), NATIVE / 'gpu_sync.hpp', NATIVE / 'clock.hpp',
              CTRU / 'include/3ds/gpu/gx.h']
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    common = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
              '-I' + str(out / 'include'), '-I' + str(CTRU / 'include'), '-I' + str(NATIVE)]
    commands = [
        ['clang', '-std=c11','-DgxCmdQueueInterrupt=duskOriginalGxCmdQueueInterrupt', *common, '-c', str(sources[1]), '-o', str(out / 'gxqueue.o')],
        ['clang++', '-std=c++20', *common, '-Wall', '-Wextra', '-Werror',
         str(sources[0]), str(sources[2]), str(out / 'gxqueue.o'), '-o', str(out / 'probe')],
    ]
    receipt = {'schema': 1, 'status': 'running', 'commands': commands, 'input_sha256': hashes,
               'origin': 'Actual native lifecycle gate + unchanged pinned queue body, host SDK/device fixtures',
               'limits': ['Host event-driven device fixture, not GPU/emulator/hardware acceptance',
                          'Host mutex-backed LightLock including a concurrent held callback; native scheduler/ARM behavior unproved',
                          'C3D_FrameSplit and GSP/device timing are modeled; real Citro3D splitting is source-audited separately',
                          'Unrelated-IRQ characterization remains a known SDK accounting limitation; all other cases are contract checks']}
    try:
        with (out / 'build.log').open('w') as log:
            for command in commands:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=45)
        receipt['cases'] = []
        for name in ('delayed', 'postcopy', 'timeout', 'flushfailure', 'foreign',
                     'stoppedfull', 'stoppedquery', 'unrelatedirq', 'headroom', 'tail', 'reserve-timeout', 'bad-reserve', 'callback', 'frame-tail', 'begin', 'frame-timeout'):
            result = subprocess.run([str(out / 'probe'), name], capture_output=True, text=True,
                                    timeout=5, env={**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1',
                                                    'UBSAN_OPTIONS': 'halt_on_error=1'})
            receipt['cases'].append({'name': name, 'returncode': result.returncode,
                                     'output': result.stdout + result.stderr,
                                     'kind': 'defect characterization' if name in
                                     ('unrelatedirq',) else 'contract check'})
            print(result.stdout + result.stderr, end='')
            result.check_returncode()
        assert hashes == {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        receipt.update(status='pass', input_unchanged=True)
    except (OSError, subprocess.SubprocessError, AssertionError) as error:
        receipt.update(status='fail', error=str(error))
        print((out / 'build.log').read_text())
    (out / 'results.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return int(receipt['status'] != 'pass')


if __name__ == '__main__':
    raise SystemExit(main())
