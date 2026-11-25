"""European Worms 3D GameCube ISO to the WORMSPK1 archive used by beta-01."""
import hashlib
import os
from pathlib import Path
import struct
import tempfile

from twilight_pack import CHUNK, disc_layout, exact, identity, publish, require

HEADER = struct.Struct('<8sIIQII8x')
ENTRY = struct.Struct('<IIQII')
SKIP = {b'createzip.bat', b'menutwkps2.xom', b'menutwkxbox.xom', b'menutwkbak.xom'}
ARCHIVE_SIZE = 1048237800
ARCHIVE_SHA256 = '90e9b021856c274205329f937bcbe7358f6e4e8681be707ec72e55b5d93dc4ce'


def path_hash(name):
    value = 2166136261
    for byte in name:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def pack(source, output, report):
    with source.open('rb') as disc:
        before = identity(disc)
        _, files, (dol, dol_size) = disc_layout(
            lambda offset, size: exact(disc, offset, size), before[2], (b'GWMP8P\0\0',),
            'Choose the European Worms 3D GameCube ISO (GWMP8P, revision 0).')
        payloads = [(name[1:].lower(), offset, size) for _, name, offset, size in files
                    if name.rsplit(b'/', 1)[-1].lower() not in SKIP]
        payloads.append((b'sys/main.dol', dol, dol_size))
        entries = bytearray()
        names = bytearray()
        end = HEADER.size
        ordered = sorted((path_hash(name), name, offset, size) for name, offset, size in payloads)
        for key, name, offset, size in ordered:
            start = (end + 63) & ~63
            entries.extend(ENTRY.pack(key, len(names), start, size, 0))
            names.extend(name + b'\0')
            end = start + size
        require(end + len(entries) + len(names) == ARCHIVE_SIZE,
                'The European disc files are incomplete or modified.')
        with tempfile.TemporaryDirectory(prefix='.forge-worms-', dir=output.parent) as temp:
            staged = Path(temp) / output.name
            with staged.open('xb') as out:
                out.write(HEADER.pack(b'WORMSPK1', 1, len(ordered), end, len(entries), len(names)))
                for _, _, offset, size in ordered:
                    out.write(bytes(-out.tell() % 64))
                    disc.seek(offset)
                    while size:
                        chunk = disc.read(min(CHUNK, size))
                        require(chunk, 'Truncated Worms 3D disc payload.')
                        out.write(chunk)
                        size -= len(chunk)
                        report('Packing European game files', 85 * out.tell() // ARCHIVE_SIZE)
                out.write(entries)
                out.write(names)
                out.flush()
                os.fsync(out.fileno())
            require(identity(disc) == before, 'The input ISO changed during conversion.')
            digest = hashlib.sha256()
            with staged.open('rb') as check:
                while chunk := check.read(CHUNK):
                    digest.update(chunk)
                    report('Verifying assets.worms', 85 + 14 * check.tell() // ARCHIVE_SIZE)
            require(digest.hexdigest() == ARCHIVE_SHA256,
                    'Game data differs from the original European disc. Check your dump.')
            publish(staged, output)
