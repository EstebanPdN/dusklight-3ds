#!/usr/bin/env python3
"""Host ASan/UBSan: actual CTRU backend, extracted developer probe, real dump I/O.
The C3D/libctru fixture models delayed copy completion, not GPU correctness.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / 'Source/platforms/3ds/runtime'


def function(text, name):
    start = text.index(name)
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    stub = out / 'stubs'
    stub.mkdir()
    old = ROOT / 'Source/platforms/3ds/tests/framebuffer_depth_order_stubs'
    (stub / 'citro3d.h').write_text((old / 'citro3d.h').read_text())
    (stub / '3ds.h').write_text((old / '3ds.h').read_text() + '''
enum {GFX_BOTTOM=2,KEY_A=1,KEY_B=2,KEY_X=4,KEY_Y=8,KEY_L=16,KEY_R=32,KEY_ZL=64,KEY_ZR=128,
KEY_START=256,KEY_SELECT=512,KEY_DUP=1024,KEY_DDOWN=2048,KEY_DLEFT=4096,KEY_DRIGHT=8192};
struct FS_ArchiveResource {std::uint32_t clusterSize;std::uint64_t freeClusters;};
int FSUSER_GetSdmcArchiveResource(FS_ArchiveResource*);
std::uint64_t svcGetSystemTick();
u32 osConvertVirtToPhys(const void*);
void GPUCMD_GetBuffer(u32**,u32*,u32*);
''')
    script = (RUNTIME / 'developer_script.cpp').read_text()
    # Verbatim initial script parser/config + actual depth writers/probe code;
    # omit unrelated ARM calibration and texture/arena exports.
    initial = script[script.index('namespace dusk::native {'):script.index('void runDeveloperCalibration()')]
    tail = script[script.index('bool developerDepthAuditRequested('):]
    extracted = out / 'developer_probe.cpp'
    extracted.write_text('''#include "developer_script.hpp"
#include "gx_framebuffer.hpp"
#include "gx_profile.hpp"
#include "dump_io.hpp"
#include <3ds.h>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
extern "C" void OSReport(const char*,...);
extern "C" void OSPanic(const char*,int,const char*,...);
''' + initial + '\n' + function(script, 'void setDeveloperFrame(') + '\n' +
                         function(script, 'bool developerCaptureRequested(') + '\n' + tail)
    inputs = [RUNTIME / x for x in ('gx_framebuffer_ctru.cpp', 'gx_framebuffer.hpp', 'developer_script.cpp', 'developer_script.hpp', 'game_startup.cpp', 'dump_io.cpp', 'dump_io.hpp')]
    inputs += [Path(__file__).resolve(), Path(__file__).with_name('depth_retained_probe_host.cpp')]
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    cmd = ['clang++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
           '-Wno-unused-variable', '-Wno-unused-function', '-Wno-format-security', '-fsanitize=address,undefined',
           '-fno-omit-frame-pointer', '-I' + str(stub), '-I' + str(RUNTIME),
           str(RUNTIME / 'gx_framebuffer_ctru.cpp'), str(RUNTIME / 'dump_io.cpp'),
           str(extracted), str(Path(__file__).with_name('depth_retained_probe_host.cpp')),
           '-o', str(out / 'host-test')]
    result = dict(status='running', command=cmd, input_sha256=hashes, cases=[])
    try:
        with (out / 'build.log').open('w') as f:
            subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, check=True, timeout=90)
        modes = ['ordinary', 'stable', 'mismatch', 'mutation', 'final-invalidate', 'early-exit',
                 'B-wait-fail', 'alloc-fail', 'physical-alias', 'invalidate-fail',
                 'bad-config', 'not-audit', 'at-exit', 'overflow-config']
        for mode in modes:
            case = out / mode
            (case / 'sdmc:/3ds/dusklight').mkdir(parents=True)
            p = subprocess.run([str(out / 'host-test'), mode], cwd=case, capture_output=True,
                               text=True, timeout=30, env={**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})
            (case / 'test.log').write_text(p.stdout + p.stderr)
            p.check_returncode()
            ledger = case / 'sdmc:/3ds/dusklight/depth-retained/frame-000450-ledger.json'
            record = dict(mode=mode, returncode=p.returncode)
            if ledger.exists():
                data = json.loads(ledger.read_text())
                assert len(data['observations']) <= 16 and ledger.stat().st_size < 16384
                a, b = data['ranges']['A'], data['ranges']['B']
                assert a['capacity'] == b['capacity'] == 384000 and a['va'] != b['va'] and a['physical'] != b['physical']
                assert data['complete'] == (mode not in ('early-exit', 'B-wait-fail', 'invalidate-fail'))
                observations = data['observations']
                if mode == 'stable':
                    assert len({r['A_fnv1a32'] for r in observations}) == 1
                    assert not any(r['payload_saved'] for r in observations)
                if mode == 'mismatch':
                    assert not observations[2]['A_equals_B_bytes'] and observations[2]['payload_saved']
                if mode == 'mutation':
                    assert observations[0]['A_fnv1a32'] != observations[3]['A_fnv1a32']
                if mode == 'final-invalidate':
                    assert observations[-2]['A_fnv1a32'] != observations[-1]['A_fnv1a32']
                if mode == 'B-wait-fail':
                    assert data['B_copy']['copy_completion'] == 3 and data['B_copy']['invalidate_result'] == -1
                for r in observations:
                    if r['payload_saved']:
                        for dest in ('A', 'B'):
                            path = ledger.with_name(f"frame-000450-{r['stage']}-{dest}.bin")
                            raw = path.read_bytes()
                            assert len(raw) == 384000
                            h = 2166136261
                            for byte in raw:
                                h = ((h ^ byte) * 16777619) & 0xffffffff
                            assert f'{h:08x}' == r[f'payload_{dest}_fnv1a32'] == r[f'{dest}_fnv1a32']
                record['ledger_sha256'] = hashlib.sha256(ledger.read_bytes()).hexdigest()
                record['observations'] = len(observations)
            elif mode in ('stable', 'mismatch', 'mutation', 'final-invalidate', 'early-exit', 'B-wait-fail', 'invalidate-fail'):
                raise AssertionError('expected bounded ledger')
            result['cases'].append(record)
        # Verify integration ordering against the actual startup body. Native
        # FrameBegin/FrameEnd and console behavior still require parent testing.
        startup = (RUNTIME / 'game_startup.cpp').read_text()
        anchors = ['if(!C3D_FrameBegin(C3D_FRAME_SYNCDRAW))continue;', 'finishDeveloperRetainedDepth(true);',
                   'captureDeveloperDepthBoundary(false);', 'captureDeveloperDepthBoundary(true);',
                   'captureMainFramebuffer(frame+1)', 'RetainedDepthStage::afterColor', 'C3D_FrameEnd(0);',
                   'RetainedDepthStage::afterFrameEnd', 'RetainedDepthStage::beforeConsole',
                   'RetainedDepthStage::afterConsoleWrites', 'GSPGPU_FlushDataCache(gfxGetFramebuffer(GFX_BOTTOM',
                   'RetainedDepthStage::afterConsoleFlush', 'RetainedDepthStage::afterReporting']
        positions = [startup.index(a) for a in anchors]
        assert positions == sorted(positions)
        assert hashes == {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        result['status'] = 'pass'
    except (OSError, subprocess.SubprocessError, AssertionError, ValueError) as e:
        result.update(status='fail', error=str(e))
        print((out / 'build.log').read_text())
        raise
    finally:
        (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f"PASS: {len(result['cases'])} host cases; native GPU/cache/ABI behavior untested")


if __name__ == '__main__':
    main()
