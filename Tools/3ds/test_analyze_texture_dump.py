#!/usr/bin/env python3
"""Offline export/parser/metric regressions; synthetic data, no emulator or ISO."""
import copy
import io
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import analyze_texture_dump as analyzer


def header(fmt=0, width=8, height=8, mips=1, colors=0):
    data = bytearray(32)
    data[0] = fmt
    struct.pack_into('>HH', data, 2, width, height)
    data[24] = mips
    if colors:
        data[8] = 1
        data[9] = 2
        struct.pack_into('>H', data, 10, colors)
        struct.pack_into('>I', data, 12, 32)
    struct.pack_into('>I', data, 28, 32+colors*2)
    return data


def record(handle=1, metadata=None, image=None, palette=None):
    metadata = header() if metadata is None else metadata
    image_size, palette_size = analyzer.payload_extents(metadata)
    image = bytes(image_size) if image is None else image
    palette = bytes(palette_size) if palette is None else palette
    return (struct.pack('<QII', handle, 0x1000+handle*0x100, 0x2000 if palette else 0) +
            metadata + struct.pack('<II', len(image), len(palette)) + image + palette)


def seal(body):
    return body + b'END!' + struct.pack('<I', analyzer.fnv(body))


def snapshot(items=(), count=None, reserved=0):
    return seal(b'DSKTEX01' + struct.pack('<QII', 300, len(items) if count is None else count, reserved) + b''.join(items))


def mutate(data, offset, value):
    body = bytearray(data[:-8])
    body[offset:offset+len(value)] = value
    return seal(body)


class SnapshotTests(unittest.TestCase):
    def test_valid_empty_and_exact_reconstruction(self):
        self.assertEqual(analyzer.records(snapshot()), (300, []))
        metadata = header(fmt=8, colors=16)
        original = bytes(metadata)
        frame, images = analyzer.records(snapshot([record(metadata=metadata, image=b'\x12'*32, palette=b'\x34'*32)]))
        row, bti = images[0]
        self.assertEqual(frame, 300)
        self.assertEqual(row['original_header_hex'], original.hex())
        self.assertEqual(row['source_image_bytes'], 32)
        self.assertEqual(row['palette_bytes'], 32)
        self.assertEqual(struct.unpack_from('>I', bti, 28)[0], 32)
        self.assertEqual(struct.unpack_from('>I', bti, 12)[0], 64)
        self.assertEqual(bti[32:], b'\x12'*32+b'\x34'*32)

    def test_truncation_magic_footer_checksum_and_size(self):
        valid = snapshot([record()])
        invalid = [valid[:i] for i in range(len(valid))]
        invalid += [b'WRONG001'+valid[8:], valid[:-8]+b'NOPE'+valid[-4:],
                    valid[:-1]+bytes([valid[-1]^1]), b'x'*(analyzer.MAX_SNAPSHOT_BYTES+1)]
        for data in invalid:
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                analyzer.records(data)

    def test_count_reserved_and_trailing_bytes(self):
        for data in [snapshot([record()], count=0), snapshot([record()], count=2),
                     snapshot(count=1025), snapshot(reserved=1),
                     seal(snapshot([record()])[:-8]+b'extra'), snapshot(count=0xffffffff)]:
            with self.assertRaises(ValueError):
                analyzer.records(data)

    def test_maximum_record_count(self):
        data = snapshot([record(handle=i+1) for i in range(analyzer.MAX_RECORDS)])
        self.assertEqual(len(analyzer.records(data)[1]), analyzer.MAX_RECORDS)

    def test_handles_zero_duplicate_and_uint64(self):
        for items in [[record(handle=0)], [record(), record()]]:
            with self.assertRaisesRegex(ValueError, 'handle'):
                analyzer.records(snapshot(items))
        # Handle width is uint64; CPU image address remains uint32.
        data = mutate(snapshot([record()]), 24, struct.pack('<Q', 0xffffffffffffffff))
        self.assertEqual(analyzer.records(data)[1][0][0]['handle'], 0xffffffffffffffff)

    def test_exact_lengths_not_just_containment(self):
        # Correct checksums isolate structural failures from checksum failures.
        for image in [b'', bytes(31), bytes(33)]:
            with self.assertRaisesRegex(ValueError, 'lengths'):
                analyzer.records(snapshot([record(image=image)]))
        for palette in [bytes(30), bytes(34)]:
            with self.assertRaisesRegex(ValueError, 'lengths'):
                analyzer.records(snapshot([record(metadata=header(fmt=8, colors=16), palette=palette)]))
        valid = snapshot([record()])
        for image_bytes, palette_bytes in [(0xffffffff, 0), (32, 0xffffffff), (0xffffffff, 0xffffffff)]:
            with self.assertRaisesRegex(ValueError, 'extent'):
                analyzer.records(mutate(valid, 24+48, struct.pack('<II', image_bytes, palette_bytes)))
        with self.assertRaisesRegex(ValueError, 'lengths'):
            analyzer.records(snapshot([record(palette=b'extra')]))

    def test_block_rounding_and_full_mip_spans(self):
        self.assertEqual(analyzer.payload_extents(header(fmt=6, width=5, height=5)), (256, 0))
        self.assertEqual(analyzer.payload_extents(header(width=16, height=16, mips=5)), (256, 0))
        # Export format accepts these mips; a native plan can still reject them.
        self.assertEqual(len(analyzer.records(snapshot([record(metadata=header(width=16, height=16, mips=5))]))[1]), 1)

    def test_invalid_texture_metadata(self):
        base = snapshot([record()])
        for offset, value in [(0,b'\x07'), (2,b'\0\0'), (4,b'\x04\x01'), (24,b'\0'),
                              (24,b'\x05'), (28,b'\0\0\0\x1f')]:
            with self.assertRaises(ValueError):
                analyzer.records(mutate(base, 24+16+offset, value))
        indexed = snapshot([record(metadata=header(fmt=8, colors=16))])
        for offset, value in [(8,b'\0'), (9,b'\x03'), (10,b'\0\0'), (10,b'\0\x11'),
                              (12,b'\0\0\0\x1f'), (28,b'\0\0\0\x20')]:
            with self.assertRaises(ValueError):
                analyzer.records(mutate(indexed, 24+16+offset, value))

    def test_read_is_bounded(self):
        stream = io.BytesIO(snapshot())
        path = unittest.mock.MagicMock()
        path.open.return_value.__enter__.return_value = stream
        with patch.object(stream, 'read', wraps=stream.read) as read:
            analyzer.read_snapshot(path)
            read.assert_called_once_with(analyzer.MAX_SNAPSHOT_BYTES+1)


class MetricTests(unittest.TestCase):
    def test_nonzero_bounds_uniform_and_weighted_centroids(self):
        metrics = analyzer.mask_metrics(bytes([0,1,0,0,3,0]), 3, 2)
        self.assertEqual(metrics['nonzero_pixels'], 2)
        self.assertEqual(metrics['bounds_xyxy_exclusive'], [1,0,2,2])
        self.assertEqual(metrics['centroid_xy'], [1,0.5])
        self.assertEqual(metrics['value_weighted_centroid_xy'], [1,0.75])
        self.assertAlmostEqual(metrics['coverage'], 1/3)
        empty = analyzer.mask_metrics(bytes(6), 3, 2)
        self.assertEqual(empty['coverage'], 0)
        self.assertIsNone(empty['bounds_xyxy_exclusive'])
        self.assertIsNone(empty['centroid_xy'])

    def test_transparent_rgb_is_measured_and_previewed(self):
        rgba = bytes([255,0,0,0, 0,128,0,0])
        metrics = analyzer.image_metrics(rgba, 2, 1)
        self.assertEqual(metrics['channels']['a']['nonzero_pixels'], 0)
        self.assertEqual(metrics['rgb_any_nonzero']['nonzero_pixels'], 2)
        self.assertEqual(analyzer.channel_rgb(rgba, 0), bytes([255,255,255,0,0,0]))
        self.assertEqual(analyzer.channel_rgb(rgba, 3), bytes(6))
        with self.assertRaises(ValueError):
            analyzer.image_metrics(rgba, 3, 1)

    def test_comparison_and_duplicate_scope(self):
        row = analyzer.records(snapshot([record()]))[1][0][0]
        row.update(status='decoded', decoded_rgba_sha256='same', metrics={})
        second = {**row, 'handle': 2, 'image_address': '0x9999'}
        groups = analyzer.duplicate_groups([row, second])
        self.assertEqual(groups[0]['logical_repeated_payload_bytes'], 32)
        self.assertEqual(groups[0]['distinct_image_addresses'], 2)
        # Identical base pixels do not prove equal full payloads or layouts.
        third = {**second, 'mips': 2, 'pixels_sha256': 'changed'}
        self.assertEqual(analyzer.duplicate_groups([row, third]), [])
        self.assertEqual(len(analyzer.duplicate_groups([row, third], decoded=True)), 1)
        before = {'frame':300, 'snapshot_sha256':'a', 'images':[row, second]}
        after = {'frame':400, 'snapshot_sha256':'b', 'images':[row, third, {**row, 'handle':3}]}
        result = analyzer.compare_inventories(before, after)
        self.assertEqual(result['added_handles'], [3])
        self.assertEqual(result['counts'], {'unchanged_at_two_samples':1, 'registration_changed':1})
        changed = copy.deepcopy(before)
        changed['images'][0]['pixels_sha256'] = 'new payload'
        result = analyzer.compare_inventories(before, changed)
        self.assertEqual(result['common'][0]['classification'], 'changed_payload')
        self.assertTrue(result['common'][0]['same_decoded_base_mip'])
        self.assertTrue(result['same_frame_number'])
        self.assertNotIn('Frames differ', result['scope'])


class ContactSheetIntegrationTests(unittest.TestCase):
    def run_analysis(self, output, total, failures, channels=False):
        source = output.parent/'synthetic.dstx'
        source.write_bytes(snapshot([record(handle=i+1) for i in range(total)]))
        call = -1
        def fake_run(command, **kwargs):
            nonlocal call
            if command[0] == 'clang++':
                return subprocess.CompletedProcess(command, 0)
            call += 1
            code = 6 if call in failures else 0
            if not code:
                Path(command[2]).write_bytes(struct.pack('<II',8,8)+bytes([255,0,0,255])*64)
            return subprocess.CompletedProcess(command, code)
        written = {}
        def save(path, width, height, rgb):
            written[path.name] = (width,height,bytes(rgb))
        with patch.object(analyzer.subprocess, 'run', side_effect=fake_run), patch.object(analyzer, 'write_png', side_effect=save):
            result = analyzer.analyze(source, output, channels=channels)
        return result, written

    def test_final_and_boundary_failures_flush_without_stale_page(self):
        for total, failures in [(1,{0}), (64,{63}), (65,{63,64}), (66,{64,65})]:
            with self.subTest(total=total), tempfile.TemporaryDirectory() as tmp:
                result, written = self.run_analysis(Path(tmp)/'out', total, failures)
                self.assertEqual(result['decoded_count'], total-len(failures))
                self.assertEqual(result['contact_sheets'], [f'contact-{i:02d}.png' for i in range((total+63)//64)])
                for index in failures:
                    sheet = written[f'contact-{index//64:02d}.png'][2]
                    cell = index%64
                    at = (((cell//8)*100)*800+(cell%8)*100)*3
                    self.assertEqual(sheet[at:at+3], b'\xc8\x32\x32')
                if total == 65:
                    # Cell 1 of page 2 is empty, not a stale success from page 1.
                    self.assertEqual(written['contact-01.png'][2][100*3:100*3+3], bytes([30])*3)

    def test_channels_are_opt_in_and_metrics_are_always_present(self):
        for enabled in (False,True):
            with tempfile.TemporaryDirectory() as tmp:
                result, written = self.run_analysis(Path(tmp)/'out', 1, set(), enabled)
                self.assertIn('metrics', result['images'][0])
                self.assertEqual('0000-handle-1-a.png' in written, enabled)
                self.assertEqual('channel_previews' in result['images'][0], enabled)
                self.assertEqual(json.loads((Path(tmp)/'out/inventory.json').read_text())['schema'], 2)


if __name__ == '__main__':
    unittest.main(verbosity=2)
