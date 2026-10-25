#!/usr/bin/env python3
"""Read-only verification of a private DUSKLIGHT_APPLICATION_DUMP_1 directory.

A valid inventory does not establish atomicity, mapping safety or hardware
correctness. No input is uploaded or modified. Output is a small JSON receipt.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


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

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(inspect(args.directory), indent=2))
    except (ValueError, OSError, KeyError, UnicodeError) as error:
        parser.exit(1, f'Invalid application dump: {error}\n')
