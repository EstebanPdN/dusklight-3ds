#!/usr/bin/env python3
"""Check named story events against the room, stage and selected layer's disc resources."""
import argparse
import csv
from functools import lru_cache
from pathlib import Path
import struct
import sys

from list_stage_spawns import be32, rarc_files, read_fst, yaz0


def name(data):
    return data.split(b'\0', 1)[0].decode('ascii')


def linked_records(data, base, stride, index, link):
    seen = set()
    while index != -1:
        at = base + index * stride
        if index < 0 or at + stride > len(data) or index in seen:
            raise ValueError('invalid event-list record chain')
        seen.add(index)
        yield at
        index = struct.unpack_from('>i', data, at + link)[0]


def packages(data, subject):
    events, count, staff, _, cuts, _, props, _, _, _, _, _, strings, _ = struct.unpack_from('>14I', data)
    for event in range(events, events + count * 0xb0, 0xb0):
        if name(data[event:event+32]) != subject:
            continue
        files = set()
        for i in range(be32(data, event+0x7c)):
            entry = staff + be32(data, event+0x2c+4*i)*0x50
            if name(data[entry:entry+32]) != 'PACKAGE':
                continue
            for cut in linked_records(data, cuts, 0x50, be32(data, entry+0x30), 0x3c):
                if name(data[cut:cut+32]) != 'PLAY':
                    continue
                prop_index = struct.unpack_from('>i', data, cut+0x38)[0]
                for prop in linked_records(data, props, 0x40, prop_index, 0x30):
                    if name(data[prop:prop+32]) == 'FileName' and be32(data, prop+0x24) == 4:
                        files.add(name(data[strings+be32(data, prop+0x28):][:128]))
        return files
    return None


def check(disc, rows):
    fst = read_fst(disc)

    @lru_cache(maxsize=12)
    def archive(path):
        if path not in fst:
            return {}
        offset, size = fst[path]
        disc.seek(offset)
        return dict(rarc_files(yaz0(disc.read(size))))

    failures = 0
    for row in rows:
        if row.get('chapter_goal') != 'event':
            continue
        prefix = '/res/Stage/' + row['stage']
        room = archive(f"{prefix}/R{int(row['room']):02}_00.arc")
        stage = archive(prefix + '/STG_00.arc')
        dzr = room['room.dzr']
        chunks = {tag: (n, start) for tag, n, start in
                  (struct.unpack_from('>4sII', dzr, 4+i*12) for i in range(be32(dzr, 0)))}
        files = None
        for bank in (room, stage):
            if 'event_list.dat' in bank:
                files = packages(bank['event_list.dat'], row['subject'])
                if files is not None:
                    break
        if files is None:
            print(row['step'], 'event definition missing')
            failures += 1
            continue
        n, start = chunks.get(b'LBNK', (0, 0))
        names = []
        for i in range(n):
            number, part = dzr[start+i*3:start+i*3+2]
            names.append(f'/res/Object/Demo{number:02}_{part:02}.arc')
        layer = int(row['layer'])
        selected = archive(names[layer]) if 0 <= layer < n else {}
        missing = files - room.keys() - stage.keys() - selected.keys()
        if missing:
            failures += 1
            matches = [i for i, path in enumerate(names) if missing <= archive(path).keys()]
            print(row['step'], 'layer', layer, 'missing', sorted(missing), 'matching layers', matches)
        else:
            print(row['step'], 'layer', layer, 'resolved', sorted(files) or '(event staff)')
    return failures


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('disc', type=Path)
    parser.add_argument('plan', type=Path, help='story-plan.csv emitted by the target build')
    args = parser.parse_args()
    with args.disc.open('rb') as disc, args.plan.open(newline='') as plan:
        sys.exit(1 if check(disc, csv.DictReader(plan)) else 0)
