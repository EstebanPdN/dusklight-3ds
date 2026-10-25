#!/usr/bin/env python3
"""Host-only color-copy oracle anchored to preserved R2/R3 native probe captures.

Expected pixels come from independently decoded LCD PNGs or row-major synthetic
charts, never from gx_copy_reference.hpp or a production encoder/decoder. Check
both the complete GX payload and the decoded native texture (including padding).
Compile a private old-X-reflection control and require its orientation failure.
No ARM builds, emulator/process control, game assets or production ownership mocks.
"""
import argparse
import binascii
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / 'Source/platforms/3ds'
EVIDENCE = ROOT / 'Diagnostics/implementation-44'
FORMATS = (0, 1, 2, 3, 4, 5, 6, 0x28)
ENV = {**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1',
       'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}

BRIDGE = r'''
#include "gx_copy_pixels.hpp"
#include "texture_gx.hpp"
#include <cassert>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
using namespace dusk::native;
int main(int argc,char** argv){
    assert(argc==14);
    auto n=[&](unsigned i){return unsigned(std::stoul(argv[i]));};
    gx::TargetExtent e{n(1),n(2),n(3),n(4)};
    ColorCopyPlan p;assert(planColorCopy(e,n(5),n(6),n(7),n(8),n(9),n(10),n(11),p));
    std::ifstream in(argv[12],std::ios::binary);
    std::vector<char> raw{std::istreambuf_iterator<char>(in),{}};
    assert(in.eof()||in.good());
    std::vector<std::uint8_t> bti(32+p.bytes,0xcd);
    std::fill(bti.begin(),bti.begin()+32,0);
    bti[0]=p.format;bti[2]=p.width>>8;bti[3]=p.width;
    bti[4]=p.height>>8;bti[5]=p.height;bti[24]=1;bti[31]=32;
    assert(encodeColorCopy(p,{reinterpret_cast<const std::byte*>(raw.data()),raw.size()},std::span{bti}.subspan(32)));
    NativeTexturePlan t;assert(inspectNativeTexture(bti,t));
    std::vector<std::uint8_t> decoded(t.outputBytes,0xcd);
    assert(decodeNativeTexture(bti,t,decoded));
    const std::string prefix=argv[13];
    std::ofstream gx(prefix+".gx",std::ios::binary),native(prefix+".native",std::ios::binary);
    gx.write(reinterpret_cast<const char*>(bti.data()+32),p.bytes);
    native.write(reinterpret_cast<const char*>(decoded.data()),decoded.size());
    assert(gx.good()&&native.good());
}
'''


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def decode_png(path):
    """Independent PNG reader: signature, lengths, CRC, RGB8 and filter-0 rows."""
    data = path.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos, compressed, dimensions = 8, bytearray(), None
    while pos < len(data):
        size = struct.unpack_from('>I', data, pos)[0]
        kind, body = data[pos+4:pos+8], data[pos+8:pos+8+size]
        assert len(body) == size
        assert binascii.crc32(kind+body) == struct.unpack_from('>I', data, pos+8+size)[0]
        pos += size+12
        if kind == b'IHDR':
            w, h, bits, color, compression, filtering, interlace = struct.unpack('>IIBBBBB', body)
            assert (bits, color, compression, filtering, interlace) == (8, 2, 0, 0, 0)
            dimensions = w, h
        elif kind == b'IDAT':
            compressed.extend(body)
        elif kind == b'IEND':
            assert not body and pos == len(data)
            break
    else:
        raise AssertionError('PNG missing IEND')
    w, h = dimensions
    raw = zlib.decompress(compressed)
    assert len(raw) == h*(1+3*w)
    rows = []
    for y in range(h):
        row = raw[y*(1+3*w):(y+1)*(1+3*w)]
        assert row[0] == 0
        rows.extend(tuple(row[x:x+3])+(255,) for x in range(1, len(row), 3))
    return w, h, rows


def native_sources(out, receipt):
    converter_path = ROOT / 'Tools/3ds/capture_to_png.py'
    converter = module(converter_path, 'color_copy_capture_png')
    receipt['input_sha256'][str(converter_path.relative_to(ROOT))] = sha(converter_path)
    sources = []
    for revision, run in (('r2', 'qprobe-software-02'), ('r2', 'qprobe-vulkan-02'),
                          ('r3', 'qprobe-vulkan-03')):
        probe = EVIDENCE / ('projective-native-probe-'+revision)
        directory = probe / 'build/azahar-runs' / run / 'xdg-data/azahar-emu/sdmc/3ds/dusklight/projective-probe'
        paths = [directory/'00.abgr8', directory/'captures.ndjson', probe/'build/spec.json', probe/'reference.py']
        for path in paths:
            receipt['input_sha256'][str(path.relative_to(ROOT))] = sha(path)
        raw = paths[0].read_bytes()
        assert len(raw) == 400*240*4 and set(raw[::4]) == {255}
        ledger = json.loads(paths[1].read_text().splitlines()[0])
        fnv = 2166136261
        for byte in raw:
            fnv = ((fnv ^ byte)*16777619) & 0xffffffff
        assert ledger['file'] == '00.abgr8' and ledger['bytes'] == len(raw)
        assert ledger['fnv1a32'] == f'{fnv:08x}'
        # Use the established capture utility, then decode its PNG independently.
        # Expected copy samples below index this row-major image, not the raw offset.
        name = revision+'-'+run
        png = out / (name+'.png')
        w, h, rgb = converter.to_lcd_rgb(240, 400, raw)
        converter.write_png(png, w, h, rgb)
        w, h, lcd = decode_png(png)
        assert (w, h) == (400, 240)
        spec = json.loads(paths[2].read_text())
        case = spec['cases'][0]
        ref = module(paths[3], 'color_copy_probe_'+revision+'_'+run.replace('-', '_'))
        anchors = []
        for point in case['samples']:
            x, y = point['x'], point['y']
            u, v, _ = ref.rational(case, x+.5, y+.5)
            expected = ref.sample(u, v, False, False)
            assert tuple(point['expected']) == expected
            actual = lcd[y*w+x]
            assert max(abs(a-b) for a, b in zip(actual, expected)) <= 1
            anchors.append({'lcd': [x, y], 'chart_expected': expected, 'png_rgba': actual})
        for x, y in ((4, 4), (395, 4), (4, 235), (395, 235)):
            assert lcd[y*w+x] == ref.CLEAR
        # An interior chart anchor has a clear background at its reflected X.
        assert lcd[25*w+34] != lcd[25*w+365]
        receipt['native_evidence'].append({'name': name, 'raw_sha256': sha(paths[0]),
            'png_sha256': sha(png), 'chart_anchors': anchors, 'clear_anchors': 4,
            'note': 'Preserved synthetic native probe readback; not a new native copy/GPU test.'})
        sources.append((name, paths[0], lcd))
    return sources


def synthetic_source(out, name, w, h):
    alphas = (0, 31, 32, 63, 64, 95, 127, 128, 159, 160, 191, 192, 223, 224, 225, 254, 255)
    lcd = [((37*x+13*y+19) % 256, (11*x+71*y+53) % 256,
            (97*x+7*y+101) % 256, alphas[(x+3*y) % len(alphas)])
           for y in range(h) for x in range(w)]
    assert len({lcd[0], lcd[w-1], lcd[(h-1)*w], lcd[-1]}) == 4
    # Serialize the independently constructed row-major chart as the unflipped
    # LCD contract: columns left-to-right; each column bottom-to-top; ABGR bytes.
    raw = bytes(channel for x in range(w) for y in reversed(range(h))
                for channel in reversed(lcd[y*w+x]))
    path = out / (name+'.abgr8')
    path.write_bytes(raw)
    return path, lcd


def source_point(e, crop, size, x, y):
    # Select the containing logical pixel of each destination pixel center,
    # then scale that logical pixel to the physical LCD. Integer arithmetic.
    lw, lh, pw, ph = e
    left, top, sw, sh = crop
    w, h = size
    return ((left+((2*x+1)*sw)//(2*w))*pw//lw,
            (top+((2*y+1)*sh)//(2*h))*ph//lh)


def intensity(rgba, fmt):
    r, g, b, _ = rgba
    return r if fmt == 0x28 else (77*r+150*g+29*b+128)//256


def gx_pixel(rgba, fmt):
    """Independent format oracle. Bytes in a GX tile; I4 returns a nibble."""
    r, g, b, a = rgba
    i = intensity(rgba, fmt)
    if fmt == 0:
        return i//16
    if fmt in (1, 0x28):
        return bytes([i])
    if fmt == 2:
        return bytes([(a//16)*16+i//16])
    if fmt == 3:
        return bytes([a, i])
    if fmt == 4:
        return struct.pack('>H', (r//8)*2048+(g//4)*32+b//8)
    if fmt == 5:
        value = (32768+(r//8)*1024+(g//8)*32+b//8 if a >= 224 else
                 (a//32)*4096+(r//16)*256+(g//16)*16+b//16)
        return struct.pack('>H', value)
    assert fmt == 6
    return bytes([a, r, g, b])


def expected_gx(samples, w, h, fmt):
    """Traverse tiles and rows, independent of production axis tables."""
    bw, bh = (8, 8) if fmt == 0 else (8, 4) if fmt in (1, 2, 0x28) else (4, 4)
    result = bytearray()
    for by in range(0, h, bh):
        for bx in range(0, w, bw):
            values = [gx_pixel(samples[(by+y)*w+bx+x], fmt) if bx+x < w and by+y < h
                      else (0 if fmt == 0 else bytes(4 if fmt == 6 else 1 if bw == 8 else 2))
                      for y in range(bh) for x in range(bw)]
            if fmt == 0:
                result.extend(values[n]*16+values[n+1] for n in range(0, len(values), 2))
            elif fmt == 6:
                result.extend(b''.join(v[:2] for v in values)+b''.join(v[2:] for v in values))
            else:
                result.extend(b''.join(values))
    return bytes(result)


def native_pixel(rgba, fmt):
    """Expected PICA storage bytes derived directly from source color rules."""
    r, g, b, a = rgba
    i = intensity(rgba, fmt)
    if fmt == 0:
        return bytes([(i//16)*17])  # LA4 intensity also supplies alpha.
    if fmt in (1, 0x28):
        return bytes([i, i])       # LA8: intensity also supplies alpha.
    if fmt == 2:
        return bytes([(i//16)*16+a//16])  # LA4 native nibble order.
    if fmt == 3:
        return bytes([a, i])
    if fmt == 4:
        return struct.pack('<H', (r//8)*2048+(g//4)*32+b//8)  # Two bytes, preserved.
    if fmt == 5:
        if a >= 224:
            expand = lambda v: (v//8)*8+(v//8)//4
            return bytes([255, expand(b), expand(g), expand(r)])
        qa = a//32
        return bytes([qa*32+qa*4+qa//2, (b//16)*17, (g//16)*17, (r//16)*17])
    assert fmt == 6
    return bytes([a, b, g, r])


def padded(n):
    return max(8, 1 << (n-1).bit_length())


def morton_xy(index):
    # Decode a native tile's six interleaved bits, rather than using the
    # production decoder's precomputed GX byte offsets.
    x = sum(((index >> (2*bit)) & 1) << bit for bit in range(3))
    y = sum(((index >> (2*bit+1)) & 1) << bit for bit in range(3))
    return x, y


def expected_native(samples, w, h, fmt):
    return b''.join(native_pixel(samples[min(ty+y, h-1)*w+min(tx+x, w-1)], fmt)
                    for ty in range(0, padded(h), 8) for tx in range(0, padded(w), 8)
                    for x, y in map(morton_xy, range(64)))


def correspondence(e, crop, size, fmt, lcd, gx, decoded):
    w, h = size
    bw, bh = (8, 8) if fmt == 0 else (8, 4) if fmt in (1, 2, 0x28) else (4, 4)
    result = []
    for x, y in sorted({(0, 0), (w-1, 0), (0, h-1), (w-1, h-1),
                        (w//3, h//3), (w//2, h//2), (w*2//3, h*2//3)}):
        sx, sy = source_point(e, crop, size, x, y)
        rgba = lcd[sy*e[2]+sx]
        tile = ((y//bh)*((w+bw-1)//bw)+x//bw)*(64 if fmt == 6 else 32)
        pixel = (y % bh)*bw+x % bw
        offsets = ([tile+pixel//2] if fmt == 0 else [tile+pixel] if bw == 8 else
                   [tile+pixel*2, tile+pixel*2+1] + ([tile+32+pixel*2, tile+33+pixel*2] if fmt == 6 else []))
        m = next(i for i in range(64) if morton_xy(i) == (x % 8, y % 8))
        bpp = len(native_pixel(rgba, fmt))
        at = (((y//8)*(padded(w)//8)+x//8)*64+m)*bpp
        result.append({'destination': [x, y], 'source_lcd': [sx, sy], 'source_rgba': rgba,
                       'raw_abgr_offset': (sx*e[3]+e[3]-1-sy)*4,
                       'gx_offsets': offsets, 'gx_bytes': [gx[i] for i in offsets],
                       'native_offset': at, 'native_bytes': list(decoded[at:at+bpp]),
                       'expected_native_bytes': list(native_pixel(rgba, fmt))})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='Fresh private host evidence directory')
    args = parser.parse_args()
    out = args.output.resolve()
    if not out.is_relative_to(ROOT / 'Diagnostics'):
        parser.error('output must be under private Diagnostics')
    out.mkdir(parents=True, exist_ok=False)
    runtime = NATIVE / 'runtime'
    paths = [runtime/name for name in ('gx_copy_pixels.cpp', 'gx_copy_pixels.hpp', 'gx_target_extent.hpp',
                                      'gx_profile.hpp', 'texture_gx.cpp', 'texture_gx.hpp')]+[Path(__file__).resolve()]
    receipt = {'status': 'running', 'origin': 'Host ASan/UBSan actual encoder and native decoder; independent Python expectations',
               'limitations': ['Preserved R2/R3 synthetic probe evidence, no new native GPU execution',
                              'Color only; raw depth orientation is not inferred',
                              'Scene A/B, physical hardware acceptance and performance remain pending'],
               'input_sha256': {str(p.relative_to(ROOT)): sha(p) for p in paths},
               'commands': [], 'native_evidence': [], 'copies': [], 'negative_control': {}}
    def run(name, command):
        command = list(map(str, command))
        result = subprocess.run(command, capture_output=True, text=True, env=ENV, timeout=90)
        (out/(name+'.log')).write_text(result.stdout+result.stderr)
        receipt['commands'].append({'name': name, 'argv': command, 'returncode': result.returncode})
        result.check_returncode()

    try:
        bridge = out/'bridge.cpp'
        bridge.write_text(BRIDGE)
        common = ['clang++', '-std=c++20', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1', '-I'+str(runtime),
                  '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        run('build', [*common, runtime/'gx_copy_pixels.cpp', runtime/'texture_gx.cpp', bridge, '-o', out/'test'])
        # Derive the negative control by restoring ONLY the old color column
        # expression in a private copy. Production Source is never reverted.
        fixed = (runtime/'gx_copy_pixels.cpp').read_text()
        expression = 'columns[x]=sx*p.extent.pixelHeight*4;'
        assert fixed.count(expression) == 1
        old = out/'old-x-reflection.cpp'
        old.write_text(fixed.replace(expression, 'columns[x]=(p.extent.pixelWidth-1-sx)*p.extent.pixelHeight*4;'))
        run('build-old-control', [*common, old, runtime/'texture_gx.cpp', bridge, '-o', out/'old-test'])
        sources = native_sources(out, receipt)
        jobs = []
        for name, path, lcd in sources:
            for label, e, crop, size in (
                ('lcd', (400, 240, 400, 240), (0, 0, 400, 240), (400, 240)),
                ('game', (608, 448, 400, 240), (0, 0, 608, 448), (304, 224)),
                ('odd-crop', (608, 448, 400, 240), (37, 19, 543, 411), (137, 91))):
                jobs.append((name+'-'+label, path, lcd, e, crop, size))
        for label, e, crop, size in (
            ('padded', (61, 29, 64, 32), (0, 0, 61, 29), (61, 29)),
            ('padded-crop', (61, 29, 64, 32), (3, 5, 55, 21), (17, 9)),
            ('odd-upscale', (19, 11, 24, 16), (2, 1, 15, 9), (31, 23)),
            ('odd-physical', (19, 11, 23, 13), (0, 0, 19, 11), (9, 7)),
            ('single', (8, 8, 8, 8), (3, 2, 1, 1), (1, 1))):
            path, lcd = synthetic_source(out, label, e[2], e[3])
            jobs.append((label, path, lcd, e, crop, size))
        for name, path, lcd, e, crop, size in jobs:
            w, h = size
            samples = [lcd[sy*e[2]+sx] for y in range(h) for x in range(w)
                       for sx, sy in [source_point(e, crop, size, x, y)]]
            for fmt in FORMATS:
                label = name+'-f'+str(fmt)
                prefix = out/label
                run(label, [out/'test', *e, *crop, *size, fmt, path, prefix])
                actual_gx, actual_native = Path(str(prefix)+'.gx').read_bytes(), Path(str(prefix)+'.native').read_bytes()
                want_gx, want_native = expected_gx(samples, w, h, fmt), expected_native(samples, w, h, fmt)
                assert actual_gx == want_gx, label+': GX payload / LCD orientation'
                assert actual_native == want_native, label+': decoded native texture / LCD orientation'
                receipt['copies'].append({'name': label, 'extent': e, 'crop': crop, 'size': size, 'format': fmt,
                    'sampled_alphas': sorted({rgba[3] for rgba in samples}),
                    'red_differs_from_luminance': sum(intensity(rgba, 0x28) != intensity(rgba, 1) for rgba in samples),
                    'gx_bytes': len(actual_gx), 'gx_sha256': hashlib.sha256(actual_gx).hexdigest(),
                    'native_bytes': len(actual_native), 'native_sha256': hashlib.sha256(actual_native).hexdigest(),
                    'correspondence': correspondence(e, crop, size, fmt, lcd, actual_gx, actual_native)})
        name, path, lcd = sources[-1]
        e, crop, size = (400, 240, 400, 240), (0, 0, 400, 240), (400, 240)
        prefix = out/'negative-old'
        run('negative-old', [out/'old-test', *e, *crop, *size, 6, path, prefix])
        actual = Path(str(prefix)+'.native').read_bytes()
        expected = expected_native(lcd, 400, 240, 6)
        # This is the SAME intended-orientation assertion as above, and must fail
        # cleanly on the old encoder, not due to a sanitizer or process error.
        try:
            assert actual == expected, 'decoded native texture / LCD orientation'
        except AssertionError as error:
            receipt['negative_control'] = {'status': 'expected assertion failure', 'assertion': str(error),
                'mismatched_bytes': sum(a != b for a, b in zip(actual, expected)),
                'old_source_sha256': sha(old), 'fixed_source_sha256': sha(runtime/'gx_copy_pixels.cpp'),
                'correspondence': correspondence(e, crop, size, 6, lcd, Path(str(prefix)+'.gx').read_bytes(), actual)}
        else:
            raise AssertionError('Old reflection unexpectedly passes orientation oracle')
        assert len(actual) == len(expected)
        # Also pin one named asymmetric chart sample through the actual native
        # texture. Its old sample must be the opposite side's clear background.
        x, y = 34, 25
        m = next(i for i in range(64) if morton_xy(i) == (x % 8, y % 8))
        at = (((y//8)*(padded(400)//8)+x//8)*64+m)*4
        assert actual[at:at+4] == native_pixel(lcd[y*400+399-x], 6)
        assert actual[at:at+4] != native_pixel(lcd[y*400+x], 6)
        receipt['negative_control']['asymmetric_anchor'] = {'lcd': [x, y],
            'expected': list(native_pixel(lcd[y*400+x], 6)), 'old_actual': list(actual[at:at+4]),
            'old_reads_lcd': [399-x, y]}
        synthetic_copies = [c for c in receipt['copies'] if c['name'].startswith('padded-f')]
        assert all({0, 223, 224, 225, 255}.issubset(c['sampled_alphas']) for c in synthetic_copies)
        assert all(c['red_differs_from_luminance'] > 0 for c in synthetic_copies)
        assert all(sha(ROOT/p) == digest for p, digest in receipt['input_sha256'].items())
        receipt.update(status='pass', input_unchanged=True, copy_comparisons=len(receipt['copies']),
                       logical_texels=sum(c['size'][0]*c['size'][1] for c in receipt['copies']),
                       native_bytes_compared=sum(c['native_bytes'] for c in receipt['copies']),
                       gx_bytes_compared=sum(c['gx_bytes'] for c in receipt['copies']))
    except (OSError, ValueError, AssertionError, subprocess.SubprocessError) as error:
        receipt.update(status='fail', error=str(error))
    finally:
        (out/'results.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print(json.dumps({k: v for k, v in receipt.items() if k in
                     ('status', 'error', 'copy_comparisons', 'logical_texels', 'native_bytes_compared', 'gx_bytes_compared')}, indent=2))
    return int(receipt['status'] != 'pass')


if __name__ == '__main__':
    raise SystemExit(main())
