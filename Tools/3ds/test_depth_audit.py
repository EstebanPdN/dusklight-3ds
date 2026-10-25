#!/usr/bin/env python3
import json
from pathlib import Path
import tempfile
import unittest
import struct
from analyze_depth_audit import analyze, decode_depth, fnv1a, read_snapshot
from azahar_run_report import inspect_run
from compare_native_profiles import read_profiles


class DepthAuditTests(unittest.TestCase):
    def metadata(self):
        return dict(schema=1, frame=1, phase='post', main=True,
                    file='frame-000001-post-00.bin', format='PICA_D24S8_LE_MORTON8_ROTATED',
                    logical_width=8, logical_height=8, pixel_width=8, pixel_height=8, queries=[])

    def write(self, folder, meta, data):
        meta = {**meta, 'bytes': len(data), 'fnv1a32': fnv1a(data)}
        (folder / meta['file']).write_bytes(data)
        path = folder / (Path(meta['file']).stem + '.json')
        path.write_text(json.dumps(meta))
        return path

    def test_known_morton_corners_and_stencil(self):
        raw = struct.pack('<64I', *(n | 0xa5000000 for n in range(64)))
        depth = decode_depth(raw, self.metadata())
        # Native top-left is (u=7,v=0), top-right (7,7),
        # bottom-left (0,0), bottom-right (0,7).
        self.assertEqual([depth[y * 8 + x] for y, x in ((0, 0), (0, 7), (7, 0), (7, 7))],
                         [0xffffff - n for n in (21, 63, 0, 42)])

    def test_independent_native_asymmetric_anchors(self):
        # Sealed ordinary native chart byte anchors, derived from its asymmetric
        # rectangles, not from this decoder or the production depth consumers.
        # See Diagnostics/implementation-44/depth-copy-x/REPORT.md.
        meta = {**self.metadata(), 'logical_width': 400, 'logical_height': 240,
                'pixel_width': 400, 'pixel_height': 240}
        raw = bytearray(384000)
        anchors = ((48, 48, 52052, 0x123456), (300, 68, 289684, 0x456789),
                   (96, 188, 93716, 0xabcdef), (0, 239, 0, 0x010203))
        for x, y, offset, word in anchors:
            struct.pack_into('<I', raw, offset, 0xa5000000 | word)
        depth = decode_depth(raw, meta)
        old = decode_depth(raw, meta, legacy_reflected_x=True)
        for x, y, offset, word in anchors:
            self.assertEqual(depth[y * 400 + x], 0xffffff - word)
            self.assertNotEqual(old[y * 400 + x], 0xffffff - word)

    def test_historical_queries_require_explicit_legacy_mode(self):
        with tempfile.TemporaryDirectory() as name:
            raw = struct.pack('<64I', *range(64))
            meta = self.metadata()
            meta['queries'] = [dict(x=0, y=7, gx_depth=0xffffff - 42)]
            path = self.write(Path(name), meta, raw)
            with self.assertRaisesRegex(ValueError, 'query mismatch'):
                read_snapshot(path)
            self.assertEqual(read_snapshot(path, legacy_reflected_x=True)[2][56], 0xffffff - 42)

    def test_retention_motion_and_real_query_evidence(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            old = struct.pack('<64I', *range(64))
            new = struct.pack('<64I', *range(1, 65))
            for frame, phase, data in ((1, 'post', old), (2, 'pre', old), (2, 'peek', old), (2, 'post', new)):
                meta = {**self.metadata(), 'frame': frame, 'phase': phase,
                        'file': f'frame-{frame:06}-{phase}-00.bin'}
                if phase == 'peek':
                    meta['queries'] = [dict(x=0, y=7, gx_depth=0xffffff)]
                self.write(root, meta, data)
            result = analyze(root, root / 'analysis')
            self.assertTrue(result['retention_verified'])
            self.assertTrue(result['queued_retention_verified'])
            self.assertEqual(result['frame_depth_changes'], [dict(frame=2, changed_logical_pixels=64)])

    def test_reject_checksum_bad_query_and_path(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            data = bytes(256)
            meta = self.metadata()
            path = self.write(root, meta, data)
            (root / meta['file']).write_bytes(b'X' + data[1:])
            with self.assertRaisesRegex(ValueError, 'checksum'):
                read_snapshot(path)
            meta['queries'] = [dict(x=0, y=0, gx_depth=1)]
            path = self.write(root, meta, data)
            with self.assertRaisesRegex(ValueError, 'query mismatch'):
                read_snapshot(path)
            meta['file'] = '../unrelated.bin'
            path.write_text(json.dumps(meta))
            with self.assertRaisesRegex(ValueError, 'basename'):
                read_snapshot(path)

    def test_depth_audit_excluded_from_timing(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            path = root / 'guest.log'
            path.write_text('Native developer depth-audit frame=390 phase=peek written\n'
                            'Original native frame 390 complete; drawn=1 rejected=0\n'
                            'Native frame profile: execution=800\n')
            self.assertEqual(inspect_run(root)['profiles'], {})
            self.assertTrue(read_profiles(path)[0]['diagnostic'])


if __name__ == '__main__':
    unittest.main()
