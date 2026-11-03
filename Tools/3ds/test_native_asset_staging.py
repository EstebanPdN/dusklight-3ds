#!/usr/bin/env python3
"""Real private-file staging checks; no emulator or native decoder required."""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zlib

import native_asset_pack
import run_azahar_smoke as runner


def asset(context=0, count=1):
    source = b'J3D2bmd3' + struct.pack('>I', 32) + bytes(20)
    def bank(magic):
        return magic + struct.pack('<3I', 2, 32, count) + bytes(12)
    context = context.to_bytes(32, 'little')
    # Opaque inner bank fixtures intentionally exercise only the outer envelope;
    # successful staging must not claim that the guest decoded these banks.
    data = native_asset_pack.compile_asset(
        source, bank(b'DUSKPN01'), b'', bank(b'DUSKMT01'), bank(b'DUSKML01'),
        [0] * count, image_context=context)
    return native_asset_pack.filename(source, context), data


def crc(data):
    struct.pack_into('<I', data, 156, zlib.crc32(data[:156] + data[160:]))
    return data


class NativeAssetStagingTests(unittest.TestCase):
    def setUp(self):
        # Stay within the same ignored project-private temp convention as the
        # runner tests, then use a real isolated Git index for boundary checks.
        with runner.card_private_parent(runner.ROOT / 'Build/native-staging-checks', create=True):
            pass
        temporary = tempfile.TemporaryDirectory(prefix='native-staging-', dir=runner.ROOT / 'Build')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        root_patch = patch.object(runner, 'ROOT', self.root)
        root_patch.start()
        self.addCleanup(root_patch.stop)
        self.git('init', '--quiet')
        (self.root / '.gitignore').write_text('Build/\n')
        self.source = self.root / 'Build/prepared'
        self.source.mkdir(parents=True)
        self.sd = self.root / 'Build/run/sdmc/3ds/DuskLight'
        self.sd.mkdir(parents=True)
        self.name, self.data = asset()
        self.path = self.source / self.name
        self.path.write_bytes(self.data)

    def git(self, *args):
        return subprocess.run(['git', '-C', str(self.root), *args], check=True,
                              capture_output=True, text=True)

    def stage(self):
        return runner.stage_native_assets(self.source, self.sd)

    def rejected(self):
        with self.assertRaises((ValueError, OSError)):
            self.stage()
        self.assertFalse((self.sd / 'native').exists())

    def test_only_bank_bytes_and_explicit_provenance_are_staged(self):
        second_name, second = asset(1, native_asset_pack.MAX_MODEL_PARTS)
        (self.source / second_name).write_bytes(second)
        (self.source / 'receipt.json').write_text('{"not": "bank bytes"}')
        (self.source / 'inputs').mkdir()
        (self.source / 'tools').symlink_to(self.root / 'absent', target_is_directory=True)
        before = {p: (p.read_bytes(), runner.card_identity(p.stat())) for p in (self.path, self.source / second_name)}
        record = self.stage()
        destination = self.sd / 'native'
        self.assertEqual(sorted(p.name for p in destination.iterdir()), sorted((self.name, second_name)))
        self.assertEqual(record['count'], 2)
        self.assertEqual(record['bytes'], len(self.data) + len(second))
        self.assertEqual(record['source'], str(self.source))
        self.assertEqual(record['destination'], str(destination))
        self.assertFalse(record['native_gameplay_accepted'])
        for row in record['banks']:
            original = Path(row['source'])
            self.assertEqual(Path(row['destination']).read_bytes(), before[original][0])
            self.assertEqual(row['sha256'], hashlib.sha256(before[original][0]).hexdigest())
            self.assertEqual((original.read_bytes(), runner.card_identity(original.stat())), before[original])

    def test_bad_identity_crc_version_extent_count_and_padding_prevent_publication(self):
        mutations = []
        for offset, value in ((8, 1), (12, len(self.data) + 16), (16, 31), (16, (16 << 20) + 1),
                              (20, 0), (20, native_asset_pack.MAX_MODEL_PARTS + 1), (56, 176), (96, 2)):
            data = bytearray(self.data)
            struct.pack_into('<I', data, offset, value)
            mutations.append(crc(data))
        for offset in (24, 100, 132, len(self.data) - 1):
            data = bytearray(self.data)
            data[offset] ^= 1
            mutations.append(crc(data))
        bad_magic = bytearray(self.data); bad_magic[0] ^= 1
        bad_crc = bytearray(self.data); bad_crc[160] ^= 1
        mutations += [bad_magic, bad_crc, self.data[:159]]
        for data in mutations:
            with self.subTest(header=bytes(data[:24])):
                self.path.write_bytes(data)
                self.rejected()
        self.path.write_bytes(self.data)
        self.path.rename(self.source / ('f' * 64 + '-' + 'e' * 64 + '.dna'))
        self.rejected()

    def test_all_banks_preflight_before_destination_is_created(self):
        name, data = asset(1)
        (self.source / name).write_bytes(data[:-1])
        self.rejected()
        self.assertEqual(self.path.read_bytes(), self.data)

    def test_empty_noncanonical_count_and_file_size_bounds(self):
        self.path.unlink()
        self.rejected()
        (self.source / 'ABC.dna').write_bytes(self.data)
        self.rejected()
        (self.source / 'ABC.dna').unlink()
        for index in range(257):
            (self.source / asset(index)[0]).touch()
        self.rejected()
        for path in self.source.iterdir():
            path.unlink()
        with self.path.open('wb') as output:
            output.truncate(native_asset_pack.MAX_BYTES + 1)
        self.rejected()

    def test_aggregate_limit_is_checked_before_any_copy(self):
        extra = native_asset_pack.MAX_BYTES - len(self.data)
        data = bytearray(self.data[:192] + bytes(extra) + self.data[192:])
        struct.pack_into('<I', data, 12, len(data))
        struct.pack_into('<I', data, 60, 32 + extra)
        for index in range(1, 5):
            offset = struct.unpack_from('<I', data, 56 + index * 8)[0]
            struct.pack_into('<I', data, 56 + index * 8, offset + extra)
        self.path.unlink()
        for index in range(17):
            context = index.to_bytes(32, 'little')
            data[100:132] = context
            crc(data)
            name = data[24:56].hex() + '-' + context.hex() + '.dna'
            # Real 16 MiB files, sparse zero body; no synthetic stat sizes.
            with (self.source / name).open('wb') as output:
                output.write(data[:192])
                output.seek(192 + extra)
                output.write(data[192 + extra:])
        with self.assertRaisesRegex(ValueError, '256 MiB aggregate'):
            self.stage()
        self.assertFalse((self.sd / 'native').exists())

    def test_existing_native_destination_is_never_reused_or_overwritten(self):
        native = self.sd / 'native'
        native.mkdir()
        target = native / self.name
        target.write_bytes(b'previous owner bytes')
        with self.assertRaises(FileExistsError):
            self.stage()
        self.assertEqual(target.read_bytes(), b'previous owner bytes')
        target.unlink()
        with self.assertRaises(FileExistsError):
            self.stage()
        native.rmdir()
        native.symlink_to(self.source, target_is_directory=True)
        with self.assertRaises(FileExistsError):
            self.stage()
        self.assertEqual(self.path.read_bytes(), self.data)

    def test_concurrent_destination_file_wins_without_clobber(self):
        original_open = runner.os.open
        def create_before_copy(path, flags, *args, **kwargs):
            if path == self.name and flags & os.O_CREAT:
                fd = original_open(path, flags, *args, **kwargs)
                with os.fdopen(fd, 'wb') as competing:
                    competing.write(b'concurrent owner bytes')
            return original_open(path, flags, *args, **kwargs)
        with patch.object(runner.os, 'open', side_effect=create_before_copy):
            with self.assertRaises(FileExistsError):
                self.stage()
        self.assertEqual((self.sd / 'native' / self.name).read_bytes(), b'concurrent owner bytes')
        self.assertEqual(self.path.read_bytes(), self.data)

    def test_symlink_file_directory_ancestor_and_destination_are_not_followed(self):
        other = self.source / 'original.bin'
        self.path.rename(other)
        self.path.symlink_to(other)
        self.rejected()
        self.path.unlink(); other.rename(self.path)
        link = self.source.parent / 'alias'
        link.symlink_to(self.source, target_is_directory=True)
        with self.assertRaises(OSError):
            runner.stage_native_assets(link, self.sd)
        ancestor = self.root / 'Diagnostics'
        ancestor.symlink_to(self.source.parent, target_is_directory=True)
        with self.assertRaises((ValueError, OSError)):
            runner.stage_native_assets(ancestor / self.source.name, self.sd)
        destination_alias = self.sd.parent / 'alias'
        destination_alias.symlink_to(self.sd, target_is_directory=True)
        with self.assertRaises((ValueError, OSError)):
            runner.stage_native_assets(self.source, destination_alias)
        self.assertFalse((self.sd / 'native').exists())

    def test_tracked_unignored_nested_and_external_sources_are_rejected(self):
        self.git('add', '-f', str(self.path.relative_to(self.root)))
        self.rejected()
        self.git('rm', '--cached', '--quiet', str(self.path.relative_to(self.root)))
        (self.root / '.gitignore').write_text('')
        self.rejected()
        (self.root / '.gitignore').write_text('Build/\n')
        (self.source / '.git').write_text('gitdir: unrelated-private-repository\n')
        self.rejected()
        (self.source / '.git').unlink()
        (self.source.parent / '.git').mkdir()
        self.rejected()
        for outside in (self.root / 'Source', self.root.parent):
            with self.assertRaises(ValueError):
                runner.stage_native_assets(outside, self.sd)

    def test_hardlinked_and_nonregular_sources_are_rejected(self):
        os.link(self.path, self.source / 'other.bin')
        self.rejected()
        (self.source / 'other.bin').unlink()
        self.path.unlink()
        os.mkfifo(self.path)
        self.rejected()

    def test_mutation_during_preflight_and_copy_is_detected(self):
        original_validate = runner.validate_native_asset
        def mutate_after_validation(data, name):
            original_validate(data, name)
            self.path.write_bytes(data[:-1])
        with patch.object(runner, 'validate_native_asset', side_effect=mutate_after_validation):
            self.rejected()
        self.path.write_bytes(self.data)
        original_open = runner.os.open
        def mutate_at_copy(path, flags, *args, **kwargs):
            if path == self.name and flags & os.O_CREAT:
                changed = bytearray(self.data); changed[160] ^= 1
                self.path.write_bytes(changed)
            return original_open(path, flags, *args, **kwargs)
        with patch.object(runner.os, 'open', side_effect=mutate_at_copy):
            with self.assertRaisesRegex(ValueError, 'changed during copy'):
                self.stage()


if __name__ == '__main__':
    unittest.main()
