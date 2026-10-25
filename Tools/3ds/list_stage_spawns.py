#!/usr/bin/env python3
"""List original player start points per stage room from the private disc.

Reads the owner's ISO read-only and saves only stage/room names, start point
numbers and positions; no archive payload is written. Used to choose valid
boot-stage.txt entries for the multi-location Azahar test matrix.
"""
import argparse
import json
from pathlib import Path
import re
import struct


def be32(data, at):
    return struct.unpack_from('>I', data, at)[0]


def read_fst(disc):
    disc.seek(0x424)
    offset, size = struct.unpack('>II', disc.read(8))
    disc.seek(offset)
    fst = disc.read(size)
    count = be32(fst, 8)
    names = fst[count * 12:]
    files = {}

    def walk(index, end, prefix):
        while index < end:
            flags = fst[index * 12]
            name_at = be32(fst, index * 12) & 0xffffff
            name = names[name_at:names.index(b'\0', name_at)].decode('ascii')
            if flags:
                following = be32(fst, index * 12 + 8)
                walk(index + 1, following, prefix + name + '/')
                index = following
            else:
                files[prefix + name] = (be32(fst, index * 12 + 4), be32(fst, index * 12 + 8))
                index += 1
    walk(1, count, '/')
    return files


def yaz0(data):
    if data[:4] != b'Yaz0':
        return data
    size = be32(data, 4)
    out = bytearray()
    at = 16
    while len(out) < size:
        header = data[at]
        at += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if header & (0x80 >> bit):
                out.append(data[at])
                at += 1
            else:
                first, second = data[at], data[at + 1]
                at += 2
                distance = ((first & 15) << 8 | second) + 1
                count = first >> 4
                if not count:
                    count = data[at] + 0x12
                    at += 1
                else:
                    count += 2
                for _ in range(count):
                    out.append(out[-distance])
    return bytes(out)


def rarc_files(data):
    if data[:4] != b'RARC':
        raise ValueError('not a RARC archive')
    data_start = be32(data, 12) + 0x20
    entries, entry_count = be32(data, 0x2c) + 0x20, be32(data, 0x28)
    strings = be32(data, 0x34) + 0x20
    for i in range(entry_count):
        at = entries + i * 0x14
        flags = data[at + 4]
        name_at = struct.unpack_from('>H', data, at + 6)[0]
        name = data[strings + name_at:data.index(b'\0', strings + name_at)].decode('ascii', 'replace')
        if flags & 2 or name in ('.', '..'):
            continue
        start, size = be32(data, at + 8), be32(data, at + 12)
        yield name, data[data_start + start:data_start + start + size]


def player_points(dzr):
    points = []
    for i in range(be32(dzr, 0)):
        tag, count, offset = struct.unpack_from('>4sII', dzr, 4 + i * 12)
        if tag != b'PLYR':
            continue
        for n in range(count):
            at = offset + n * 32
            name = dzr[at:at + 8].split(b'\0')[0].decode('ascii', 'replace')
            x, y, z = struct.unpack_from('>fff', dzr, at + 12)
            angle_z = struct.unpack_from('>h', dzr, at + 28)[0]
            points.append({'point': angle_z & 0xff, 'name': name, 'position': [round(x, 1), round(y, 1), round(z, 1)]})
    return points


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--private-disc', type=Path, required=True)
    parser.add_argument('--stages', required=True, help='Comma-separated stage names, e.g. F_SP121,D_MN05')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = {'origin': 'private disc metadata; start points only', 'stages': {}}
    with args.private_disc.open('rb') as disc:
        files = read_fst(disc)
        for stage in args.stages.split(','):
            rooms = {}
            for path, (offset, size) in sorted(files.items()):
                match = re.fullmatch(rf'/res/Stage/{re.escape(stage)}/R(\d+)_00\.arc', path)
                if not match:
                    continue
                disc.seek(offset)
                for name, payload in rarc_files(yaz0(disc.read(size))):
                    if name.endswith('.dzr'):
                        points = player_points(payload)
                        if points:
                            rooms[int(match[1])] = points
            result['stages'][stage] = rooms
    args.output.write_text(json.dumps(result, indent=1) + '\n')
    for stage, rooms in result['stages'].items():
        summary = {room: sorted({p['point'] for p in points}) for room, points in rooms.items()}
        print(stage, json.dumps(summary)[:300])


if __name__ == '__main__':
    main()
