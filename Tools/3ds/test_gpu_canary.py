#!/usr/bin/env python3
"""Host test of the console GPU guard (Source/platforms/3ds/runtime/gpu_canary_ctru.cpp).

Builds Source/platforms/3ds/tests/gpu_canary_test.cpp, which includes the guard
whole, against stub <3ds.h>/<citro3d.h> and host stand-ins for GSP, the GX
queue and the SD (real files under <output>/<case>/sdmc:/). Each case runs in
its own process and directory:
  parse-equivalence   Alpha 6 parse (copied in the test) and the current one
                      leave identical shadow registers, uniforms, program hash
                      and signatures on Citro3D-like and random streams;
  signature-groups    partial signature mixes equal a full mix; the
                      fragment-only signature ignores TEV/blend, not textures;
  verified-store      gpu-verified.bin: most recent signatures kept, written
                      again only when needed and in one FS call, never cleared
                      in a session;
  canary-flow         waits per canary, journal before the run and "idle"
                      before any other list, a draw first in its list waits for
                      earlier lists, the 300-picture line;
  coarse              SD switch gpu-canary-coarse.txt;
  hang-attributed, hang-other-list, journal-learn   the hang protocol;
  session-word        the word of a start (core2) found in every record format;
  font-oom-session    worker attribution with Alpha 17 OOM and GPU evidence;
  benchmark           host timing of both parses (information only).

--hang-record PATH (repeatable) adds the CMDS command lists of console hang
records (gpu-hang-NN.bin) to parse-equivalence and benchmark.

Host only: console timing, GSP, the PICA and the SD card are not modelled.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / 'Source/platforms/3ds'
CASES = ('parse-equivalence', 'signature-groups', 'verified-store', 'canary-flow', 'coarse',
         'hang-attributed', 'hang-other-list', 'journal-learn', 'session-word', 'font-oom-session',
         'watchdog-only', 'benchmark')
STUBS = {
    '3ds.h': '''#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int32_t s32; typedef int64_t s64; typedef s32 Result; typedef u32 Handle;
#define R_FAILED(r) ((Result)(r)<0)
#define SYSCLOCK_ARM11 268111856ULL
#define OS_VRAM_PADDR 0x18000000u
#define OS_VRAM_VADDR 0x1F000000u
#define OS_VRAM_SIZE 0x600000u
#define OS_FCRAM_PADDR 0x20000000u
#define OS_OLD_FCRAM_VADDR 0x14000000u
#define OS_FCRAM_VADDR 0x30000000u
#define GPUREG_EARLYDEPTH_CLEAR 0x0063
#define GPUREG_FRAMEBUFFER_INVALIDATE 0x0110
#define GPUREG_FRAMEBUFFER_FLUSH 0x0111
typedef s32 LightLock;
static inline void LightLock_Lock(LightLock* l) { *l = 0; }
static inline int LightLock_TryLock(LightLock* l) { if (*l == 0) return 1; *l = 0; return 0; }
static inline void LightLock_Unlock(LightLock* l) { *l = 1; }
static inline void svcSleepThread(s64) {}
typedef struct { u32 base_addr, size, perm, state; } MemInfo;
typedef struct { u32 flags; } PageInfo;
enum { MEMSTATE_FREE = 0, MEMPERM_READ = 1 };
#ifdef __cplusplus
extern "C" {
#endif
u64 svcGetSystemTick(void);
Result svcQueryMemory(MemInfo*, PageInfo*, u32);
u32 osConvertVirtToPhys(const void*);
void GPUCMD_GetBuffer(u32** addr, u32* size, u32* offset);
void GPUCMD_SetBuffer(u32* addr, u32 size, u32 offset);
void GPUCMD_AddRawCommands(const u32* cmd, u32 size);
void GPUCMD_Add(u32 header, const u32* param, u32 paramlength);
#ifdef __cplusplus
}
#endif
static inline void GPUCMD_AddSingleParam(u32 header, u32 param) { GPUCMD_Add(header, &param, 1); }
#define GPUCMD_AddWrite(reg, val) GPUCMD_AddSingleParam((0xFu << 16) | (reg), (val))
''',
    'citro3d.h': '''#pragma once
#include <3ds.h>
typedef enum { GPU_TRIANGLES = 0 } GPU_Primitive_t;
''',
}


def hang_record_lists(paths, out):
    """CMDS chunks of gpu-hang-NN.bin files, written as raw word files."""
    lists = []
    for index, path in enumerate(paths):
        data = Path(path).read_bytes()
        at = 0
        while at + 8 <= len(data):
            tag, size = data[at:at + 4], struct.unpack('<I', data[at + 4:at + 8])[0]
            if tag == b'CMDS':
                target = out / f'list-{index}.bin'
                target.write_bytes(data[at + 8:at + 8 + size])
                lists.append(target)
            at += 8 + size
    return lists


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--hang-record', action='append', default=[])
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    for name, content in STUBS.items():
        (out / 'include').mkdir(exist_ok=True)
        (out / 'include' / name).write_text(content)
    source = NATIVE / 'tests/gpu_canary_test.cpp'
    inputs = [source, NATIVE / 'runtime/gpu_canary_ctru.cpp', NATIVE / 'runtime/gpu_canary.hpp', Path(__file__).resolve()]
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    lists = [str(p) for p in hang_record_lists(args.hang_record, out)]
    command = ['clang++', '-std=c++20', '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               '-Wall', '-Wextra', '-Werror', '-I' + str(out / 'include'), '-I' + str(NATIVE),
               str(source), '-o', str(out / 'probe')]
    bench = ['clang++', '-std=c++20', '-O2', '-I' + str(out / 'include'), '-I' + str(NATIVE),
             str(source), '-o', str(out / 'probe-o2')]
    receipt = {'schema': 1, 'status': 'running', 'commands': [command, bench], 'input_sha256': hashes,
               'recorded_lists': len(lists),
               'limits': ['Host stand-ins for GSP, the GX queue, Citro3D splitting and the SD card',
                          'Equivalence is against the Alpha 6 parse copied into the test, not against a PICA',
                          'Benchmark is host time, not ARM11 time']}
    try:
        with (out / 'build.log').open('w') as log:
            for c in (command, bench):
                subprocess.run(c, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=240)
        receipt['cases'] = []
        for name in CASES:
            work = out / name
            (work / 'sdmc:' / '3ds' / 'DuskLight').mkdir(parents=True)
            binary = out / ('probe-o2' if name == 'benchmark' else 'probe')
            extra = lists if name in ('parse-equivalence', 'benchmark') else []
            result = subprocess.run([str(binary), name, *extra], cwd=work, capture_output=True, text=True, timeout=600,
                                    env={**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1'})
            receipt['cases'].append({'name': name, 'returncode': result.returncode, 'output': result.stdout + result.stderr})
            print(result.stdout + result.stderr, end='')
            result.check_returncode()
        assert hashes == {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        receipt.update(status='pass', input_unchanged=True)
    except (OSError, subprocess.SubprocessError, AssertionError) as error:
        receipt.update(status='fail', error=str(error))
        print((out / 'build.log').read_text() if (out / 'build.log').exists() else '')
    (out / 'results.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print('status:', receipt['status'])
    return int(receipt['status'] != 'pass')


if __name__ == '__main__':
    raise SystemExit(main())
