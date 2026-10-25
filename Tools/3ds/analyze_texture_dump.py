#!/usr/bin/env python3
"""Analyze every registered CPU GX image in a private DSKTEX01 snapshot.

The native decoder is shared with the port, not an independent correctness
oracle. Base-mip metrics/previews do not inventory all RAM or GPU resources.
"""
import argparse
from collections import defaultdict
import hashlib
import html
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from capture_to_png import write_png

ROOT = Path(__file__).resolve().parents[2]
MAX_SNAPSHOT_BYTES = 16 * 1024 * 1024
MAX_RECORDS = 1024
FORMATS = {0: 'I4', 1: 'I8', 2: 'IA4', 3: 'IA8', 4: 'RGB565', 5: 'RGB5A3',
           6: 'RGBA8', 8: 'CI4', 9: 'CI8', 10: 'CI14', 14: 'CMPR'}
BLOCKS = {0: (8, 8, 32), 1: (8, 4, 32), 2: (8, 4, 32),
          3: (4, 4, 32), 4: (4, 4, 32), 5: (4, 4, 32), 6: (4, 4, 64),
          8: (8, 8, 32), 9: (8, 4, 32), 10: (4, 4, 32), 14: (8, 8, 32)}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def fnv(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def payload_extents(header):
    """Exact block-rounded spans emitted by resource_archive.cpp::inspectBti.

    This validates the export contract, not native decoder correctness. Original
    offsets refer to the original BTI, whose padding is absent from the export.
    """
    fmt, mips = header[0], header[24]
    width, height = struct.unpack_from('>HH', header, 2)
    if fmt not in BLOCKS or not (1 <= width <= 1024 and 1 <= height <= 1024):
        raise ValueError('Unsupported texture format/dimensions')
    if not 1 <= mips <= max(width, height).bit_length():
        raise ValueError('Invalid texture mip count')
    if struct.unpack_from('>I', header, 28)[0] < 32:
        raise ValueError('Invalid original image offset')
    bw, bh, size = BLOCKS[fmt]
    image_bytes = sum(((max(1, width >> level) + bw - 1) // bw) *
                      ((max(1, height >> level) + bh - 1) // bh) * size
                      for level in range(mips))
    palette_bytes = 0
    if fmt in (8, 9, 10):
        colors = struct.unpack_from('>H', header, 10)[0]
        if (not header[8] or header[9] > 2 or
                not 1 <= colors <= {8: 16, 9: 256, 10: 16384}[fmt] or
                struct.unpack_from('>I', header, 12)[0] < 32):
            raise ValueError('Invalid palette metadata')
        palette_bytes = colors * 2
        image_offset = struct.unpack_from('>I', header, 28)[0]
        palette_offset = struct.unpack_from('>I', header, 12)[0]
        if image_offset < palette_offset + palette_bytes and palette_offset < image_offset + image_bytes:
            raise ValueError('Overlapping original image/palette spans')
    return image_bytes, palette_bytes


def records(data):
    if len(data) < 32 or len(data) > MAX_SNAPSHOT_BYTES or data[:8] != b'DSKTEX01':
        raise ValueError('Invalid or oversized texture snapshot')
    end = len(data) - 8
    if data[end:end+4] != b'END!' or fnv(memoryview(data)[:end]) != struct.unpack_from('<I', data, end+4)[0]:
        raise ValueError('Incomplete snapshot or failed checksum')
    frame, count, reserved = struct.unpack_from('<QII', data, 8)
    if reserved or count > MAX_RECORDS:
        raise ValueError('Unsupported snapshot version/count')
    if count * 56 > end - 24:
        raise ValueError('Truncated texture records for declared count')
    cursor, result, seen = 24, [], set()
    for index in range(count):
        if end - cursor < 56:
            raise ValueError(f'Truncated texture record {index}')
        handle, image_address, palette_address = struct.unpack_from('<QII', data, cursor)
        header = bytearray(data[cursor+16:cursor+48])
        original_header = bytes(header)
        image_bytes, palette_bytes = struct.unpack_from('<II', data, cursor+48)
        cursor += 56
        if not handle or handle in seen:
            raise ValueError(f'Invalid or duplicate texture handle at record {index}')
        if image_bytes + palette_bytes > end - cursor:
            raise ValueError(f'Invalid payload extent at record {index}')
        if (image_bytes, palette_bytes) != payload_extents(header):
            raise ValueError(f'Non-exact image/palette lengths at record {index}')
        seen.add(handle)
        pixels = data[cursor:cursor+image_bytes]
        palette = data[cursor+image_bytes:cursor+image_bytes+palette_bytes]
        cursor += image_bytes+palette_bytes
        struct.pack_into('>I', header, 28, 32)
        struct.pack_into('>I', header, 12, 32+image_bytes if palette_bytes else 0)
        result.append(({'index': index, 'handle': handle, 'image_address': hex(image_address),
                        'palette_address': hex(palette_address), 'gx_format': FORMATS[header[0]],
                        'width': int.from_bytes(header[2:4], 'big'), 'height': int.from_bytes(header[4:6], 'big'),
                        'mips': header[24], 'source_image_bytes': image_bytes, 'palette_bytes': palette_bytes,
                        'palette_format': header[9] if palette_bytes else None,
                        'palette_entries': palette_bytes // 2,
                        'original_header_hex': original_header.hex(),
                        'image_sha256': sha256(pixels), 'palette_sha256': sha256(palette),
                        'pixels_sha256': sha256(pixels+palette)}, bytes(header)+pixels+palette))
    if cursor != end:
        raise ValueError('Unexpected trailing snapshot data')
    return frame, result


def read_snapshot(path):
    # Bound the read itself, even when handed an unexpectedly large file.
    with path.open('rb') as stream:
        data = stream.read(MAX_SNAPSHOT_BYTES + 1)
    frame, images = records(data)
    return data, frame, images


def composite(rgba, width, height):
    out = bytearray(width*height*3)
    for y in range(height):
        for x in range(width):
            i = (y*width+x)*4
            alpha = rgba[i+3]
            background = 150 if ((x//8+y//8)&1) else 95
            out[(y*width+x)*3:(y*width+x)*3+3] = bytes(
                (rgba[i+c]*alpha+background*(255-alpha)+127)//255 for c in range(3))
    return out


def mask_metrics(values, width, height):
    """Nonzero coverage and centroids in unflipped decoded base-mip texels."""
    count = sx = sy = weight = wx = wy = 0
    xmin, ymin, xmax, ymax = width, height, -1, -1
    for at, value in enumerate(values):
        if value:
            y, x = divmod(at, width)
            count += 1
            sx += x
            sy += y
            weight += value
            wx += x * value
            wy += y * value
            xmin, xmax = min(xmin, x), max(xmax, x)
            ymin, ymax = min(ymin, y), max(ymax, y)
    return {'nonzero_pixels': count, 'coverage': count / (width*height),
            'bounds_xyxy_exclusive': [xmin, ymin, xmax+1, ymax+1] if count else None,
            'centroid_xy': [sx/count, sy/count] if count else None,
            'value_weighted_centroid_xy': [wx/weight, wy/weight] if weight else None}


def image_metrics(rgba, width, height):
    if not width or not height or len(rgba) != width*height*4:
        raise ValueError('Invalid RGBA metric extent')
    channels = {}
    for offset, channel in enumerate('rgba'):
        values = rgba[offset::4]
        channels[channel] = {'min': min(values), 'max': max(values),
                             'mean': sum(values)/len(values),
                             'sha256': sha256(values), **mask_metrics(values, width, height)}
    rgb_any = bytes(bool(rgba[i] or rgba[i+1] or rgba[i+2]) for i in range(0, len(rgba), 4))
    return {'pixel_count': width*height, 'channels': channels,
            'rgb_any_nonzero': mask_metrics(rgb_any, width, height)}


def channel_rgb(rgba, offset):
    # Ignore alpha: a stored channel is data even if the RGBA preview is invisible.
    return bytes(value for value in rgba[offset::4] for _ in range(3))


class ContactSheets:
    """Stable record slots, including failed decodes; flushing is unconditional."""
    def __init__(self, output):
        self.output = output
        self.names = []
        self.sheet = bytearray([30]) * (800*800*3)

    def add(self, index, total, rgb=None, width=0, height=0):
        cell = index % 64
        ox, oy = (cell % 8)*100, (cell // 8)*100
        for y in range(96):
            for x in range(96):
                at = ((oy+y)*800+ox+x)*3
                if rgb is None:
                    color = b'\xc8\x32\x32' if abs(x-y) < 3 or abs(x+y-95) < 3 else b'\x35\x20\x20'
                else:
                    source = (min(height-1, y*height//96)*width+min(width-1, x*width//96))*3
                    color = rgb[source:source+3]
                self.sheet[at:at+3] = color
        if cell == 63 or index == total-1:
            name = f'contact-{index//64:02d}.png'
            write_png(self.output/name, 800, 800, self.sheet)
            self.names.append(name)
            self.sheet[:] = bytes([30]) * len(self.sheet)


def layout(record):
    return tuple(record.get(k) for k in ('gx_format', 'width', 'height', 'mips',
                                       'palette_format', 'palette_entries'))


def duplicate_groups(images, decoded=False):
    groups = defaultdict(list)
    for record in images:
        if decoded:
            if record.get('status') != 'decoded':
                continue
            key = (record['width'], record['height'], record['decoded_rgba_sha256'])
        else:
            key = (*layout(record), record['pixels_sha256'])
        groups[key].append(record)
    result = []
    for group in groups.values():
        if len(group) < 2:
            continue
        result.append({'handles': [r['handle'] for r in group],
                       'image_addresses': [r['image_address'] for r in group],
                       'distinct_image_addresses': len({r['image_address'] for r in group}),
                       'payload_bytes_each': [r['source_image_bytes']+r['palette_bytes'] for r in group],
                       'logical_repeated_payload_bytes': sum(r['source_image_bytes']+r['palette_bytes'] for r in group[1:])})
    return sorted(result, key=lambda g: g['logical_repeated_payload_bytes'], reverse=True)


def compare_inventories(before, after):
    old = {r['handle']: r for r in before['images']}
    new = {r['handle']: r for r in after['images']}
    common = []
    for handle in sorted(old.keys() & new.keys()):
        a, b = old[handle], new[handle]
        same_registration = (a['image_address'], a['palette_address'], layout(a)) == (
            b['image_address'], b['palette_address'], layout(b))
        same_payload = a['pixels_sha256'] == b['pixels_sha256']
        same_base = (a.get('decoded_rgba_sha256') == b.get('decoded_rgba_sha256')
                     if a.get('status') == b.get('status') == 'decoded' else None)
        common.append({'handle': handle, 'same_registration_address_and_layout': same_registration,
                       'same_full_payload': same_payload, 'same_decoded_base_mip': same_base,
                       'same_original_header': a.get('original_header_hex') == b.get('original_header_hex'),
                       'classification': ('registration_changed' if not same_registration else
                                          'unchanged_at_two_samples' if same_payload else 'changed_payload'),
                       'before_metrics': a.get('metrics') if not same_payload else None,
                       'after_metrics': b.get('metrics') if not same_payload else None})
    counts = defaultdict(int)
    for row in common:
        counts[row['classification']] += 1
    return {'before_frame': before['frame'], 'after_frame': after['frame'],
            'before_snapshot_sha256': before['snapshot_sha256'], 'after_snapshot_sha256': after['snapshot_sha256'],
            'same_frame_number': before['frame'] == after['frame'],
            'scope': 'Two samples only; matching handles/addresses are evidence, not lifetime or static-asset proof. '
                     + ('Matching frame numbers still require matching build/input/scene evidence.'
                        if before['frame'] == after['frame'] else
                        'Frames differ: this is not a before/after shadow-fix validation.'),
            'counts': dict(counts), 'added_handles': sorted(new.keys()-old.keys()),
            'removed_handles': sorted(old.keys()-new.keys()), 'common': common}


def analyze(snapshot, output, channels=False, compare=None):
    data, frame, images = read_snapshot(snapshot)
    previous = json.loads(compare.read_text()) if compare is not None else None
    if previous is not None and previous.get('schema') != 2:
        raise ValueError('Comparison requires a schema-2 inventory (rerun the baseline analyzer)')
    output.mkdir(parents=True, exist_ok=False)
    native = ROOT/'Source/platforms/3ds/runtime/texture_gx.cpp'
    source = Path(__file__).with_name('decode_snapshot_texture.cpp')
    decoder = (output/'decoder').resolve()
    command = ['clang++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
               str(source), str(native), '-o', str(decoder)]
    subprocess.run(command, check=True, capture_output=True, timeout=90)
    rows, sheets = [], ContactSheets(output)
    with tempfile.TemporaryDirectory(prefix='dusklight-textures-') as temporary:
        bti, raw = Path(temporary)/'image.bti', Path(temporary)/'image.rgba'
        for index, (record, contents) in enumerate(images):
            name = f'{index:04d}-handle-{record["handle"]}'
            bti.write_bytes(contents)
            raw.unlink(missing_ok=True)
            run = subprocess.run([str(decoder), str(bti), str(raw)], capture_output=True, timeout=15)
            record['decoder_returncode'] = run.returncode
            rgb = None
            if run.returncode:
                record['status'] = 'decode_failed'
                rows.append(f'<figure><figcaption>{name}: decoding failed ({run.returncode})</figcaption></figure>')
            else:
                pixels = raw.read_bytes()
                if len(pixels) < 8:
                    raise ValueError('Truncated decoder output')
                width, height = struct.unpack_from('<II', pixels)
                rgba = pixels[8:]
                if len(rgba) != width*height*4 or (width, height) != (record['width'], record['height']):
                    raise ValueError('Decoder output extent mismatch')
                rgb = composite(rgba, width, height)
                write_png(output/(name+'.png'), width, height, rgb)
                alpha = rgba[3::4]
                record.update(status='decoded', preview=name+'.png', alpha_min=min(alpha), alpha_max=max(alpha),
                              transparent_pixels=alpha.count(0), opaque_pixels=alpha.count(255),
                              decoded_rgba_sha256=sha256(rgba), metrics=image_metrics(rgba, width, height))
                links = ''
                if channels:
                    record['channel_previews'] = {}
                    for offset, channel in enumerate('rgba'):
                        channel_name = f'{name}-{channel}.png'
                        write_png(output/channel_name, width, height, channel_rgb(rgba, offset))
                        record['channel_previews'][channel] = channel_name
                        links += f'<a href="{channel_name}">{channel.upper()}<img loading="lazy" src="{channel_name}"></a>'
                rows.append(f'<figure id="handle-{record["handle"]}"><a href="{name}.png"><img loading="lazy" src="{name}.png"></a>'
                            f'<figcaption>{name}<br>{width}×{height} {record["gx_format"]}<br>'
                            f'{record["source_image_bytes"]} image + {record["palette_bytes"]} palette bytes<br>'
                            f'CPU address {record["image_address"]}<br>A: {min(alpha)}–{max(alpha)}</figcaption>'
                            f'<details><summary>Stored channels (ignore alpha)</summary>{links or "Enable --channels"}</details></figure>')
            # A failed last record or boundary record must still flush the page.
            sheets.add(index, len(images), rgb, record['width'], record['height'])
    result = {'schema': 2, 'frame': frame, 'snapshot_sha256': sha256(data),
              'private': True, 'scope': 'All live registered CPU GX images; base mip metrics/previews only',
              'metric_coordinates': 'Top-left origin in decoded texel order, no display flip; integer pixel centers; bounds maximum exclusive',
              'decoder': 'Current native texture decoder, not an independent correctness oracle',
              'analyzer_sha256': sha256(Path(__file__).read_bytes()),
              'decoder_command': command, 'decoder_source_sha256': sha256(native.read_bytes()),
              'decoder_helper_sha256': sha256(source.read_bytes()),
              'images': [r for r, _ in images], 'contact_sheets': sheets.names,
              'decoded_count': sum(r.get('status') == 'decoded' for r, _ in images),
              'total_source_bytes': sum(r['source_image_bytes']+r['palette_bytes'] for r, _ in images)}
    result['exact_cpu_payload_duplicates'] = duplicate_groups(result['images'])
    result['decoded_base_mip_duplicates'] = duplicate_groups(result['images'], decoded=True)
    result['duplicate_caveat'] = ('Logical repetitions only, not measured reclaimable RAM. Equal decoded base mips may differ in '
                                  'encoding, palette, other mips or lifetime. Never delete active resources from these results.')
    if compare is not None:
        comparison = compare_inventories(previous, result)
        (output/'comparison.json').write_text(json.dumps(comparison, indent=2)+'\n')
        result['comparison'] = 'comparison.json'
    (output/'inventory.json').write_text(json.dumps(result, indent=2)+'\n')
    (output/'index.html').write_text('<!doctype html><html lang="en"><meta charset="utf-8"><title>Private texture snapshot</title>'
        '<style>body{background:#17191e;color:#eee;font:14px system-ui}a{color:#aaf}main{display:flex;flex-wrap:wrap;align-items:start}'
        'figure{margin:8px;padding:8px;width:160px;border:1px solid #555}img{width:144px;height:144px;object-fit:contain;image-rendering:pixelated}'
        'details a{display:block}figcaption{overflow-wrap:anywhere}</style>'
        f'<h1>Private registered CPU textures · frame {frame}</h1><p>{len(images)} records, {result["decoded_count"]} decoded. '
        'Base mip only. Current native converter, not an independent correctness oracle. No complete GPU/RAM recovery claim. '
        'Channel views ignore alpha; a transparent RGBA preview does not imply an unused image.</p>'
        f'<p>Snapshot: {html.escape(str(snapshot))}<br>SHA-256: {sha256(data)}</p>'
        '<p><a href="inventory.json">All image metrics and duplicate candidates (JSON)</a></p><main>'
        + ''.join(rows) + '</main></html>')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('snapshot', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--channels', action='store_true', help='Write separate grayscale R/G/B/A previews, ignoring alpha')
    parser.add_argument('--compare', type=Path, help='Previous schema-2 inventory.json from the same capture series')
    args = parser.parse_args()
    result = analyze(args.snapshot, args.output, args.channels, args.compare)
    print(json.dumps({k: v for k, v in result.items() if k not in (
        'images', 'decoder_command', 'exact_cpu_payload_duplicates', 'decoded_base_mip_duplicates')}, indent=2))
    return int(result['decoded_count'] != len(result['images']))


if __name__ == '__main__':
    raise SystemExit(main())
