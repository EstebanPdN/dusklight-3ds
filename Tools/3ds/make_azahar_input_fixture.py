#!/usr/bin/env python3
"""Modify only button fields in a real local CTM recording; preserve poll order."""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('recording', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--start-after', type=int, default=60, help='Delay START holds so early fatal diagnostics remain observable')
    args = parser.parse_args()
    if args.start_after<0:parser.error('START delay must be nonnegative')
    data = args.recording.read_bytes()
    if len(data) <= 256 or data[:4] != b'CTM\x1b' or (len(data) - 256) % 7:
        parser.error('Expected complete CTM header and seven-byte input records')
    result = bytearray(data)
    types = collections.Counter(data[i] for i in range(256, len(data), 7))
    if set(types) - set(range(6)) or types[0] != struct.unpack_from('<Q', data, 84)[0]:
        parser.error('Invalid record type or mismatched input count')
    index = 0
    keys = collections.Counter()
    for offset in range(256, len(data), 7):
        if data[offset] != 0:
            continue
        # Real Azahar records 234 PadAndCircle polls per emulated second.
        # First two seconds neutral, then two seconds held/two released.
        # Alternate A and START after one minute to exercise original prompts.
        seconds = index / 234
        cycle = int((seconds - 2) / 4)
        pressed = seconds >= 2 and (index - 468) % 936 < 468
        button = (8 if seconds >= args.start_after and cycle % 2 else 1) if pressed else 0
        struct.pack_into('<H', result, offset + 1, button)
        keys[button] += 1
        index += 1
    if args.output.exists():
        parser.error('Preserve existing fixtures; choose a new output')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(result)
    changed = [i for i, (a, b) in enumerate(zip(data, result)) if a != b]
    assert result[:256] == data[:256]
    assert all(i >= 256 and (i - 256) % 7 in (1, 2) and data[256 + (i - 256) // 7 * 7] == 0 for i in changed)
    receipt = {
        'origin': 'Synthetic sustained emulator HID buttons applied to actual local recording; no game-state edits',
        'format_reference': 'https://raw.githubusercontent.com/azahar-emu/azahar/2126.1.2/src/core/movie.cpp',
        'recording': str(args.recording.resolve()),
        'recording_sha256': hashlib.sha256(data).hexdigest(),
        'fixture': str(args.output.resolve()),
        'fixture_sha256': hashlib.sha256(result).hexdigest(),
        'header_and_poll_order_preserved': True,
        'circle_touch_irrst_fields_preserved': True,
        'pad_count': index, 'record_types': dict(types), 'button_poll_counts': dict(keys),
        'schedule': f'Neutral 2s, then held 2s/released 2s; A, alternating A/START after {args.start_after}s',
        'physical_input_validated': False,
    }
    args.output.with_suffix('.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    main()
