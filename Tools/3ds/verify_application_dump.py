#!/usr/bin/env python3
"""Read-only verification of a private DuskLight application dump.

Two formats: a DUSKLIGHT_APPLICATION_DUMP_1 directory (one file per 64 KiB
chunk) and a DUSKLIGHT_APPLICATION_DUMP_2 packed file (*.dpk: blocks, zero
blocks elided, zstd, then a text index and a trailer).

A valid inventory does not establish atomicity, mapping safety or hardware
correctness. No input is uploaded or modified. Output is a small JSON receipt;
--extract writes the screens (PNG), VRAM and RAM regions of a packed dump into
a new local directory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import zlib


def inspect(directory):
    directory = Path(directory)
    if directory.is_symlink() or not directory.is_dir():
        raise ValueError('Expected a real dump directory')
    consumed = {}

    def read(name, limit, size=None, checksum=None):
        p = directory / name
        if p.is_symlink() or not p.is_file() or p.stat().st_size > limit:
            raise ValueError('Invalid or oversized file: ' + name)
        data = p.read_bytes()
        if len(data) > limit or (size is not None and len(data) != size):
            raise ValueError('Wrong length: ' + name)
        value = 2166136261
        for byte in data:
            value = ((value ^ byte) * 16777619) & 0xffffffff
        if checksum is not None and value != checksum:
            raise ValueError('Checksum mismatch: ' + name)
        consumed[name] = {'bytes': len(data), 'fnv1a32': f'{value:08x}',
                          'sha256': hashlib.sha256(data).hexdigest()}
        return data

    marker = dict(line.split('=', 1) for line in read('COMPLETE', 512).decode('ascii').splitlines())
    if marker.get('verified') != '1':
        raise ValueError('No complete marker')
    manifest = read('manifest.txt', 4096, checksum=int(marker['manifest_fnv1a32'], 16)).decode('ascii')
    fields = dict(line.split('=', 1) for line in manifest.splitlines() if '=' in line)
    if fields.get('format') != 'DUSKLIGHT_APPLICATION_DUMP_1' or fields.get('coherence') != 'chunked-live-non-atomic':
        raise ValueError('Unsupported dump format/coherence')
    files = re.findall(r'^file=(\S+) bytes=(\d+) fnv1a32=([0-9a-f]{8})', manifest, re.M)
    expected={'top.rgba8', 'bottom.rgb565', 'map.txt'}
    if any(f[0]=='extras.txt' for f in files): expected.add('extras.txt')
    if {f[0] for f in files} != expected or len(files) != len(expected):
        raise ValueError('Invalid manifest inventory')
    data = {}
    for name, count, checksum in files:
        cap = {'top.rgba8': 384000, 'bottom.rgb565': 153600, 'map.txt': 1024*1024, 'extras.txt':4096}[name]
        data[name] = read(name, cap, int(count), int(checksum, 16))
    if consumed['map.txt']['fnv1a32'] != marker['map_fnv1a32']:
        raise ValueError('Map marker mismatch')
    if len(data['top.rgba8']) != 384000 or len(data['bottom.rgb565']) != 153600:
        raise ValueError('Wrong screen dimensions')
    extras_total=0
    if 'extras.txt' in data:
        extra=data['extras.txt'].decode('ascii')
        descriptions=dict(line.split('=',1) for line in extra.splitlines() if '=' in line)
        if descriptions.get('source')!='GSP-ImportDisplayCaptureInfo-after-GPU-drain-and-VBlank':
            raise ValueError('Unknown LCD source')
        if int(descriptions['vram_base'],16)!=0x1f000000 or int(descriptions['vram_size'])!=0x600000:
            raise ValueError('Invalid VRAM scope')
        records=re.findall(r'^file=(\S+) bytes=(\d+) fnv1a32=([0-9a-f]{8}) address=([0-9a-f]{8}) layout=(\S+)$',extra,re.M)
        sizes={'vram.bin':0x600000,'lcd-top-left.bin':400*240*3,'lcd-top-right.bin':400*240*3,'lcd-bottom.bin':320*240*2}
        names=[r[0] for r in records]
        if (len(set(names))!=len(names) or not {'vram.bin','lcd-top-left.bin','lcd-bottom.bin'}<=set(names)
                or not set(names)<=set(sizes)):
            raise ValueError('Invalid LCD/VRAM inventory')
        for name,n,h,address,layout in records:
            if int(n)!=sizes[name] or (name=='vram.bin' and int(address,16)!=0x1f000000):
                raise ValueError('Invalid LCD/VRAM length or address')
            read(name,sizes[name],sizes[name],int(h,16));extras_total+=sizes[name]
        if extras_total!=int(fields['supplement_bytes']):
            raise ValueError('Supplement total mismatch')
    regions, chunks, exclusions = [], [], []
    for line in data['map.txt'].decode('ascii').splitlines():
        parts = line.split()
        if parts and parts[0] == 'range':
            if parts == ['range', 'base', 'size', 'perm', 'state', 'pageflags', 'decision']:
                continue
            if len(parts) != 7: raise ValueError('Malformed region')
            base, size, perm = (int(v, 16) for v in parts[1:4])
            state = int(parts[4]); flags = int(parts[5], 16); decision = parts[6]
            if not size or base % 4096 or size % 4096 or base + size > 0x40000000:
                raise ValueError('Invalid mapped region bounds')
            eligible = (bool(perm & 1) and not perm & ~7 and state in (4, 5, 7, 11)
                        and not (base + size > 0x1e800000 and base < 0x1ff82000))
            if (decision == 'eligible') != eligible:
                raise ValueError('Region policy mismatch')
            regions.append((base, size, eligible, state, flags, decision))
        elif parts and parts[0] == 'chunk':
            if len(parts) != 5 or not re.fullmatch(r'memory-\d{5}\.bin', parts[1]):
                raise ValueError('Invalid chunk name')
            base, size = int(parts[2], 16), int(parts[3], 16)
            if not 0 < size <= 65536 or base % 4096 or size % 4096:
                raise ValueError('Invalid chunk extent')
            if parts[1] != f'memory-{len(chunks):05d}.bin': raise ValueError('Chunk order mismatch')
            if not re.fullmatch(r'fnv1a32=[0-9a-f]{8}', parts[4]): raise ValueError('Invalid chunk checksum')
            read(parts[1], 65536, size, int(parts[4].split('=')[1], 16))
            chunks.append((base, size))
        elif parts and parts[0] == 'exclude':
            if len(parts) != 4 or parts[3] != 'diagnostic-workspace': raise ValueError('Unknown exclusion')
            exclusions.append((int(parts[1], 16), int(parts[2], 16)))
        elif not line.startswith('schema=1 coherence=chunked-live-non-atomic'):
            raise ValueError('Unknown map line')
    cursor = 0
    for base, size, *_ in regions:
        if base != cursor: raise ValueError('Map gap/overlap')
        cursor = base + size
    if cursor != 0x40000000 or len(regions) > 1024 or len(chunks) > 5120:
        raise ValueError('Map scope or count invalid')
    coverage = sorted(chunks + exclusions)
    index = 0
    for base, size, eligible, *_ in regions:
        if not eligible: continue
        cursor = base
        while index < len(coverage) and coverage[index][0] < base + size:
            start, count = coverage[index]
            if start != cursor or count <= 0 or start + count > base + size:
                raise ValueError('Eligible coverage gap/overlap')
            cursor += count; index += 1
        if cursor != base + size: raise ValueError('Uncaptured eligible region')
    if index != len(coverage): raise ValueError('Out-of-policy payload')
    total = sum(n for _, n in chunks)
    if total != int(fields['ram_bytes']) or len(chunks) != int(fields['chunks']) or total > 256*1024*1024:
        raise ValueError('Payload total mismatch')
    if {p.name for p in directory.iterdir()} != set(consumed):
        raise ValueError('Unexpected or uninventoried files')
    return {'status': 'verified_inventory', 'frame': int(fields['frame']), 'scene': fields['scene'],
            'build': fields['build'], 'ram_bytes': total, 'chunks': len(chunks),
            'regions': len(regions), 'workspace_excluded_bytes': sum(n for _, n in exclusions),
            'coherence': fields['coherence'], 'hardware_validated': False,
            'atomic_snapshot': False, 'display_source':fields.get('display_source','render-readback-v1'),
            'supplement_bytes':extras_total, 'files': consumed}

def fnv1a32(data, value=2166136261):
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def write_png(path, width, height, rgb):
    def chunk(tag, body):
        return struct.pack('>I', len(body)) + tag + body + struct.pack('>I', zlib.crc32(tag + body))
    rows = b''.join(b'\0' + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
                     + chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))


def lcd_rgb(data, width, bytes_per_pixel):
    """GSP front buffer (240 pixels per column, columns left to right, bottom row first) as RGB rows."""
    out = bytearray(width * 240 * 3)
    for x in range(width):
        column = data[x * 240 * bytes_per_pixel:(x + 1) * 240 * bytes_per_pixel]
        for y in range(240):
            at = (239 - y) * bytes_per_pixel
            if bytes_per_pixel == 3:
                b, g, r = column[at], column[at + 1], column[at + 2]
            else:
                v = column[at] | column[at + 1] << 8
                r, g, b = (v >> 11) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31
            o = (y * width + x) * 3
            out[o] = r; out[o + 1] = g; out[o + 2] = b
    return bytes(out)


def inspect_packed(path, extract=None):
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        raise ValueError('Expected a real packed dump file')
    size = path.stat().st_size
    if size < 48 or size > 512 * 1024 * 1024:
        raise ValueError('Packed dump size out of range')
    data = path.read_bytes()
    if data[:16] != b'DUSKDPK2' + bytes(8):
        raise ValueError('Not a DUSKDPK2 file')
    if data[-32:-24] != b'DPK2END\0':
        raise ValueError('Trailer missing: the dump did not finish')
    low, high, index_size, index_hash, block_count, reserved = struct.unpack('<6I', data[-24:])
    index_offset = low | high << 32
    if reserved or index_offset < 16 or index_offset + index_size != size - 32:
        raise ValueError('Trailer does not locate the index')
    index = data[index_offset:index_offset + index_size]
    if fnv1a32(index) != index_hash:
        raise ValueError('Index checksum mismatch')
    text = index.decode('ascii')
    fields, blobs, blocks, regions, exclusions, layouts = {}, {}, [], [], [], {}
    cursor, stored_total, kinds = 16, 0, {'zero': 0, 'raw': 0, 'zstd': 0}
    decompress = None

    def payload(parts, logical):
        """Bytes of one block record (parts from the kind on); None for a zero block."""
        nonlocal cursor, stored_total, decompress
        kind = parts[0]
        if kind == 'zero':
            if len(parts) != 1: raise ValueError('Malformed zero block')
            kinds['zero'] += 1
            return None
        if kind not in ('raw', 'zstd') or len(parts) != 4 or not re.fullmatch(r'fnv1a32=[0-9a-f]{8}', parts[3]):
            raise ValueError('Malformed block record')
        offset, stored = int(parts[1]), int(parts[2])
        if offset != cursor or stored <= 0 or offset + stored > index_offset:
            raise ValueError('Block payload out of order or out of bounds')
        raw = data[offset:offset + stored]
        if kind == 'zstd':
            if stored >= logical: raise ValueError('Compressed block is not smaller')
            if decompress is None:
                from compression import zstd
                decompress = zstd.decompress
            raw = decompress(raw)
        if len(raw) != logical: raise ValueError('Block length mismatch')
        if fnv1a32(raw) != int(parts[3][8:], 16): raise ValueError('Block checksum mismatch')
        cursor += stored; stored_total += stored; kinds[kind] += 1
        return raw

    for line in text.splitlines():
        parts = line.split()
        if not parts: raise ValueError('Empty index line')
        if parts[0] == 'blob':
            if len(parts) < 5: raise ValueError('Malformed blob record')
            name, at, count = parts[1], int(parts[2], 16), int(parts[3], 16)
            held = blobs.setdefault(name, bytearray())
            if at != len(held) or not 0 < count <= 1024 * 1024: raise ValueError('Blob block out of order')
            raw = payload(parts[4:], count)
            held += raw if raw is not None else bytes(count)
        elif parts[0] == 'block':
            if len(parts) < 4: raise ValueError('Malformed RAM block record')
            base, count = int(parts[1], 16), int(parts[2], 16)
            if not 0 < count <= 1024 * 1024 or base % 4096 or count % 4096: raise ValueError('Invalid RAM block extent')
            blocks.append((base, count, payload(parts[3:], count)))
        elif parts[0] == 'range':
            if parts == ['range', 'base', 'size', 'perm', 'state', 'pageflags', 'decision']: continue
            if len(parts) != 7: raise ValueError('Malformed region')
            base, count, perm = (int(v, 16) for v in parts[1:4])
            state = int(parts[4]); decision = parts[6]
            if not count or base % 4096 or count % 4096 or base + count > 0x40000000:
                raise ValueError('Invalid mapped region bounds')
            eligible = (bool(perm & 1) and not perm & ~7 and state in (4, 5, 7, 11)
                        and not (base + count > 0x1e800000 and base < 0x1ff82000))
            if (decision == 'eligible') != eligible: raise ValueError('Region policy mismatch')
            regions.append((base, count, eligible))
        elif parts[0] == 'exclude':
            if len(parts) != 4 or parts[3] != 'diagnostic-workspace': raise ValueError('Unknown exclusion')
            exclusions.append((int(parts[1], 16), int(parts[2], 16)))
        elif parts[0] == 'layout':
            if len(parts) != 4 or not parts[3].startswith('address='): raise ValueError('Malformed layout')
            layouts[parts[1]] = parts[2]
        elif parts[0] in ('lcd', 'memory'):
            fields[parts[0]] = line
        elif '=' in parts[0] and len(parts) >= 1:
            key, value = line.split('=', 1); fields[key] = value
        else:
            raise ValueError('Unknown index line: ' + line[:40])
    if fields.get('format') != 'DUSKLIGHT_APPLICATION_DUMP_2' or fields.get('coherence') != 'chunked-live-non-atomic':
        raise ValueError('Unsupported dump format/coherence')
    if cursor != index_offset: raise ValueError('Unindexed bytes before the index')
    if sum(kinds.values()) != block_count: raise ValueError('Block count mismatch')
    sizes = {'lcd-top-left': 400 * 240 * 3, 'lcd-top-right': 400 * 240 * 3, 'lcd-bottom': 320 * 240 * 2, 'vram': 0x600000}
    for name, expected in sizes.items():
        if name in blobs and len(blobs[name]) != expected: raise ValueError('Wrong size: ' + name)
    # Since Alpha 2: recent frame times and the small shared/kernel pages
    # (page-<address>) ride along as extra blobs; Alpha 2 dumps may also hold
    # PICA hardware registers. Since Alpha 3: the last GPU jobs (gpu-jobs).
    extras = {'pica-registers', 'pica-uniforms', 'gpu-hw-registers', 'frames-csv', 'gpu-jobs'}
    def known(name):
        if name in sizes or name in extras: return True
        return (len(name) == 13 and name.startswith('page-')
                and all(c in '0123456789abcdef' for c in name[5:]) and len(blobs[name]) <= 0x10000)
    if not {'lcd-top-left', 'lcd-bottom', 'vram'} <= set(blobs) or not all(known(name) for name in blobs):
        raise ValueError('Invalid LCD/VRAM inventory')
    cursor = 0
    for base, count, _ in regions:
        if base != cursor: raise ValueError('Map gap/overlap')
        cursor = base + count
    if cursor != 0x40000000 or len(regions) > 1024: raise ValueError('Map scope or count invalid')
    coverage = sorted([(b, n) for b, n, _ in blocks] + exclusions)
    position = 0
    for base, count, eligible in regions:
        if not eligible: continue
        cursor = base
        while position < len(coverage) and coverage[position][0] < base + count:
            start, n = coverage[position]
            if start != cursor or n <= 0 or start + n > base + count: raise ValueError('Eligible coverage gap/overlap')
            cursor += n; position += 1
        if cursor != base + count: raise ValueError('Uncaptured eligible region')
    if position != len(coverage): raise ValueError('Out-of-policy payload')
    total = sum(n for _, n, _ in blocks)
    zero = sum(n for _, n, raw in blocks if raw is None)
    if total != int(fields['ram_bytes']) or total > 256 * 1024 * 1024: raise ValueError('Payload total mismatch')
    if stored_total != int(fields['stored_bytes']): raise ValueError('Stored total mismatch')
    extracted = None
    if extract is not None:
        extract = Path(extract)
        extract.mkdir(parents=True, exist_ok=False)
        (extract / 'index.txt').write_bytes(index)
        write_png(extract / 'lcd-top.png', 400, 240, lcd_rgb(blobs['lcd-top-left'], 400, 3))
        if 'lcd-top-right' in blobs:
            write_png(extract / 'lcd-top-right.png', 400, 240, lcd_rgb(blobs['lcd-top-right'], 400, 3))
        write_png(extract / 'lcd-bottom.png', 320, 240, lcd_rgb(blobs['lcd-bottom'], 320, 2))
        for name in sorted(blobs):
            if name.startswith('lcd-'): continue
            target = 'frames.csv' if name == 'frames-csv' else name + '.bin'
            (extract / target).write_bytes(blobs[name])
        files = 0
        for base, count, eligible in regions:
            if not eligible: continue
            inside = [(b, n, raw) for b, n, raw in blocks if base <= b < base + count]
            if not inside: continue
            with (extract / f'ram-{base:08x}-{count:08x}.bin').open('wb') as out:
                for b, n, raw in sorted(inside, key=lambda item: item[0]):
                    out.seek(b - base); out.write(raw if raw is not None else bytes(n))
                out.truncate(count)
            files += 1
        extracted = {'directory': str(extract), 'ram_files': files}
    timing = {key: float(fields[key]) for key in fields if key.startswith('seconds_')}
    return {'status': 'verified_inventory', 'format': 2, 'frame': int(fields['frame']), 'scene': fields['scene'],
            'build': fields['build'], 'file_bytes': size, 'ram_bytes': total, 'zero_bytes': zero,
            'stored_bytes': stored_total, 'blocks': kinds, 'regions': len(regions),
            'workspace_excluded_bytes': sum(n for _, n in exclusions), 'compression': fields.get('compression'),
            'coherence': fields['coherence'], 'hardware_validated': False, 'atomic_snapshot': False,
            'display_source': 'GSP-ImportDisplayCaptureInfo', 'blobs': {k: len(v) for k, v in blobs.items()},
            'console_seconds': timing, 'sha256': hashlib.sha256(data).hexdigest(), 'extracted': extracted}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dump', type=Path, help='dump directory (format 1) or .dpk file (format 2)')
    parser.add_argument('--extract', type=Path, help='new directory for the contents of a packed dump')
    args = parser.parse_args()
    try:
        if args.dump.is_file():
            print(json.dumps(inspect_packed(args.dump, args.extract), indent=2))
        else:
            if args.extract: raise ValueError('--extract applies to packed dumps')
            print(json.dumps(inspect(args.dump), indent=2))
    except (ValueError, OSError, KeyError, UnicodeError, ImportError) as error:
        parser.exit(1, f'Invalid application dump: {error}\n')
