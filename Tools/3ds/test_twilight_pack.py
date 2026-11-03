#!/usr/bin/env python3
"""Synthetic USA/PAL bundle identity and payload round trips; no game data."""
import hashlib
import io
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

import twilight_pack as pack


def disc(game=b'GZ2E01\0\0'):
    raw = bytearray(0x3000)
    raw[:8] = game
    for offset, value in ((0x1c, 0xc2339f3d), (0x420, 0x500), (0x424, 0x1000),
                          (0x428, 64), (0x500, 256), (0x590, 16)):
        struct.pack_into('>I', raw, offset, value)
    struct.pack_into('>III', raw, 0x1000, 0x01000000, 0, 2)
    struct.pack_into('>III', raw, 0x100c, 0, 0x2000, 4)
    raw[0x1018:0x101e] = b'A.bin\0'
    raw[0x2000:0x2004] = b'DATA'
    return raw


class Regions(unittest.TestCase):
    def test_both_regions_preserve_identity_and_indexed_payload(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for game in (b'GZ2E01\0\0', b'GZ2P01\0\0'):
                raw = disc(game)
                source, output = root / (game[:6].decode() + '.iso'), root / (game[:6].decode() + '.twilight')
                source.write_bytes(raw)
                result = subprocess.run([sys.executable, str(Path(pack.__file__)), '--input', str(source),
                                         '--output', str(output)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                with output.open('rb') as stream:
                    bundle = pack.Bundle(stream)
                    self.assertEqual(bundle.read(0, 8), game)
                    self.assertEqual(bundle.read(0x2000, 4), b'DATA')
                    self.assertEqual(bundle.source_hash, hashlib.sha256(raw).digest())
                    bundle.verify()
                self.assertEqual(source.read_bytes(), raw)

    def test_unsupported_identity_and_revision_stay_rejected(self):
        for game in (b'GZ2J01\0\0', b'GZ2P01\0\1', b'GZ2E01\0\1', b'GZ2P01\1\0',
                     b'GZ2P02\0\0', b'RZDP01\0\0'):
            with self.subTest(game=game):
                raw = disc(game)
                stream = io.BytesIO(raw)
                with self.assertRaises(ValueError):
                    pack.layout(lambda offset, size: pack.exact(stream, offset, size), len(raw))


if __name__ == '__main__':
    unittest.main()
