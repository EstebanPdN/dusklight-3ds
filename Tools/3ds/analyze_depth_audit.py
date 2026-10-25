#!/usr/bin/env python3
"""Verify private PICA depth snapshots, queued queries, and previous-frame retention."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
from capture_to_png import write_png


def fnv1a(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return f'{value:08x}'


def decode_depth(data, meta, *, legacy_reflected_x=False):
    """Logical top-left rows, GX 24-bit depth (0 near, 0xffffff clear/far)."""
    lw, lh, pw, ph = [meta[key] for key in
                      ('logical_width', 'logical_height', 'pixel_width', 'pixel_height')]
    if any(type(n) is not int or not 0 < n <= 1024 for n in (lw, lh, pw, ph)):
        raise ValueError('invalid extent')
    if pw % 8 or ph % 8 or len(data) != pw * ph * 4:
        raise ValueError('invalid tiled allocation')
    words = struct.unpack(f'<{pw * ph}I', data)
    depths = []
    for y in range(lh):
        px = ph - 1 - y * ph // lh
        for x in range(lw):
            py = x * pw // lw
            if legacy_reflected_x:
                py = pw - 1 - py
            local = sum((((px >> bit) & 1) << (2 * bit)) |
                        (((py >> bit) & 1) << (2 * bit + 1)) for bit in range(3))
            address = ((py // 8) * (ph // 8) + px // 8) * 64 + local
            depths.append(0xffffff - (words[address] & 0xffffff))
    return depths


def read_snapshot(path, *, legacy_reflected_x=False):
    meta = json.loads(path.read_text())
    if meta.get('schema') != 1 or meta.get('format') != 'PICA_D24S8_LE_MORTON8_ROTATED':
        raise ValueError('unsupported snapshot schema/format')
    if meta.get('phase') not in ('pre', 'peek', 'post') or type(meta.get('main')) is not bool:
        raise ValueError('invalid snapshot phase/target')
    if type(meta.get('frame')) is not int or meta['frame'] < 1:
        raise ValueError('invalid frame')
    name = meta.get('file', '')
    if not re.fullmatch(r'frame-\d+-(pre|peek|post)-\d+\.bin', name):
        raise ValueError('invalid private payload basename')
    payload = path.parent / name
    if payload.stat().st_size > 4 * 1024 * 1024:
        raise ValueError('oversized depth snapshot')
    data = payload.read_bytes()
    if len(data) != meta.get('bytes') or fnv1a(data) != meta.get('fnv1a32'):
        raise ValueError('payload length/checksum mismatch')
    depth = decode_depth(data, meta, legacy_reflected_x=legacy_reflected_x)
    queries = meta.get('queries')
    if not isinstance(queries, list) or len(queries) > 64:
        raise ValueError('invalid query list')
    for query in queries:
        x, y, value = [query[k] for k in ('x', 'y', 'gx_depth')]
        if any(type(n) is not int for n in (x, y, value)) or not (
                0 <= x < meta['logical_width'] and 0 <= y < meta['logical_height'] and 0 <= value <= 0xffffff):
            raise ValueError('invalid query')
        if depth[y * meta['logical_width'] + x] != value:
            raise ValueError(f'queued query mismatch at {x},{y}')
    return meta, data, depth


def analyze(source, output, *, legacy_reflected_x=False):
    output.mkdir(parents=True, exist_ok=False)
    records, snapshots = [], {}
    for path in sorted(source.glob('*.json')):
        meta, data, depth = read_snapshot(path, legacy_reflected_x=legacy_reflected_x)
        low, high = min(depth), max(depth)
        # Local contrast is diagnostic, never a claim of linear distance.
        normalized = [0] * len(depth) if low == high else [(v - low) * 255 // (high - low) for v in depth]
        preview = path.stem + '.png'
        write_png(output / preview, meta['logical_width'], meta['logical_height'],
                  bytes(channel for value in normalized for channel in (value, value, value)))
        record = {**meta, 'metadata': str(path.resolve()),
                  'sha256': hashlib.sha256(data).hexdigest(), 'preview': preview,
                  'minimum': low, 'maximum': high, 'unique_values': len(set(depth)),
                  'clear_pixels': depth.count(0xffffff),
                  'queries_verified': len(meta['queries'])}
        records.append(record)
        key = (meta['frame'], meta['phase'])
        if meta['main']:
            snapshots.setdefault(key, []).append((record, data, depth))
    if not records:
        raise ValueError('no depth metadata found')
    retention, queued, movement = [], [], []
    for (frame, phase), values in sorted(snapshots.items()):
        if phase != 'pre':
            continue
        if len(values) != 1:
            raise ValueError('multiple pre boundaries for one frame')
        pre = values[0]
        prior = snapshots.get((frame - 1, 'post'), [])
        if len(prior) == 1:
            retention.append({'previous_frame': frame - 1, 'frame': frame,
                              'byte_identical': prior[0][1] == pre[1]})
        for peek in snapshots.get((frame, 'peek'), []):
            queued.append({'frame': frame, 'queries': peek[0]['queries_verified'],
                           'matches_pre_render': peek[1] == pre[1]})
        post = snapshots.get((frame, 'post'), [])
        if len(post) == 1:
            movement.append({'frame': frame, 'changed_logical_pixels':
                             sum(a != b for a, b in zip(pre[2], post[0][2], strict=True))})
    report = {'schema': 1, 'origin': 'Azahar guest depth snapshots; not physical hardware',
              'coordinate_contract': ('legacy incorrect reflected X, for historical query reproduction only'
                                      if legacy_reflected_x else 'native u=pixel_height-1-scaled_y, v=scaled_x'),
              'snapshots': records, 'previous_frame_retention': retention,
              'queued_previous_frame_reads': queued, 'frame_depth_changes': movement,
              'retention_verified': bool(retention) and all(r['byte_identical'] for r in retention),
              'queued_retention_verified': bool(queued) and all(r['matches_pre_render'] for r in queued),
              'motion_observed': any(r['changed_logical_pixels'] for r in movement),
              'preview_note': 'Per-image min/max normalization; nonlinear GX depth, not world distance.'}
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--legacy-reflected-x', action='store_true',
                        help='Explicitly reproduce pre-candidate22 incorrect X/query coordinates; never auto-detected')
    args = parser.parse_args()
    report = analyze(args.source, args.output, legacy_reflected_x=args.legacy_reflected_x)
    print(json.dumps({k: v for k, v in report.items() if k != 'snapshots'}, indent=2))
    return int(not report['retention_verified'] or not report['queued_retention_verified'])


if __name__ == '__main__':
    raise SystemExit(main())
