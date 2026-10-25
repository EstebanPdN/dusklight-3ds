#!/usr/bin/env python3
"""Reject damaged or incorrectly labeled copies of the generated diagnostic CIA.

Run after package_diagnostic.py has produced the three local artifacts. Every
mutation is written inside TemporaryDirectory; the real outputs are read only.
"""

import hashlib
from pathlib import Path
import tempfile
import unittest

from verify_diagnostic_package import verify

ROOT = Path(__file__).resolve().parents[2]
INPUTS = (
    ROOT / 'Build/new3ds-diagnostic-0.1/dusklight-diagnostic-0.1.cia',
    ROOT / 'Build/new3ds-native/dusklight-diagnostic.cxi',
    ROOT / 'Build/new3ds-native/diagnostic.smdh',
)


class DiagnosticPackageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not all(path.is_file() for path in INPUTS):
            raise unittest.SkipTest('Build the diagnostic CIA/CXI/SMDH before these artifact tests')
        cls.originals = tuple(path.read_bytes() for path in INPUTS)
        cia = cls.originals[0]
        align = lambda value: (value + 63) & ~63
        cert_size = int.from_bytes(cia[8:12], 'little')
        tik_size = int.from_bytes(cia[12:16], 'little')
        cls.ticket = align(align(0x2020) + cert_size)
        cls.tmd = align(cls.ticket + tik_size)
        cls.content = align(cls.tmd + int.from_bytes(cia[16:20], 'little'))

    def check_mutation(self, which, mutate, error):
        copies = [bytearray(data) for data in self.originals]
        mutate(copies[which])
        with tempfile.TemporaryDirectory(prefix='dusklight-cia-validation-') as temp:
            paths = [Path(temp) / name for name in ('test.cia', 'test.cxi', 'test.smdh')]
            for path, data in zip(paths, copies):
                path.write_bytes(data)
            with self.assertRaisesRegex(ValueError, error):
                verify(*paths)

    def test_original_package_passes_without_hardware_claim(self):
        result = verify(*INPUTS)
        self.assertEqual(result['title_id'], '000400000d051000')
        self.assertEqual(result['title_version'], 1)
        self.assertFalse(result['signature_validation'])
        self.assertFalse(result['physical_console_test'])
        self.assertFalse(result['gameplay_verified'])

    def test_truncated_cia(self):
        self.check_mutation(0, lambda data: data.__delitem__(slice(-1, None)), 'truncated')

    def test_trailing_cia_data(self):
        self.check_mutation(0, lambda data: data.extend(b'x'), 'trailing')

    def test_wrong_content_bitmap(self):
        self.check_mutation(0, lambda data: data.__setitem__(32, 0xC0), 'content index')

    def test_changed_cia_content(self):
        self.check_mutation(0, lambda data: data.__setitem__(self.content + 0x200, 1), 'content differs')

    def test_wrong_ticket_title_id(self):
        offset = self.ticket + 0x140 + 0x9C
        self.check_mutation(0, lambda data: data.__setitem__(offset, 1), 'Ticket: TitleID')

    def test_wrong_tmd_version(self):
        offset = self.tmd + 0x140 + 0x9D
        self.check_mutation(0, lambda data: data.__setitem__(offset, 2), 'TMD: title version')

    def test_corrupt_tmd_info_hash(self):
        offset = self.tmd + 0x140 + 0xA4
        self.check_mutation(0, lambda data: data.__setitem__(offset, data[offset] ^ 1), 'info-record table')

    def test_corrupt_tmd_chunk_hash(self):
        def mutate(data):
            body = self.tmd + 0x140
            data[body + 0x9C4 + 16] ^= 1
            data[body + 0xC4 + 4:body + 0xC4 + 36] = hashlib.sha256(data[body + 0x9C4:body + 0x9F4]).digest()
            data[body + 0xA4:body + 0xC4] = hashlib.sha256(data[body + 0xC4:body + 0x9C4]).digest()
        self.check_mutation(0, mutate, 'TMD content: SHA-256 mismatch')

    def test_wrong_ncch_title_id(self):
        self.check_mutation(1, lambda data: data.__setitem__(0x108, 1), 'partition TitleID')

    def test_romfs_is_rejected(self):
        self.check_mutation(1, lambda data: data.__setitem__(0x1B4, 1), 'no RomFS')

    def test_corrupt_exheader(self):
        self.check_mutation(1, lambda data: data.__setitem__(0x200, data[0x200] ^ 1), 'ExHeader: SHA-256')

    def test_wrong_cpu_and_memory_profile_even_with_repaired_hash(self):
        for offset, value, reason in [(0x40C, 1, 'CPU 804MHz'),
                                      (0x40C, 2, 'L2 cache'),
                                      (0x40D, 0, 'SystemModeExt')]:
            with self.subTest(offset=hex(offset), value=value):
                def mutate(data):
                    data[offset] = value
                    data[0x160:0x180] = hashlib.sha256(data[0x200:0x600]).digest()
                self.check_mutation(1, mutate, reason)

    def test_changed_code_payload(self):
        def mutate(data):
            exefs = int.from_bytes(data[0x1A0:0x1A4], 'little') * 512
            for index in range(8):
                header = exefs + index * 16
                if data[header:header + 8].split(b'\0')[0] == b'.code':
                    payload = exefs + 512 + int.from_bytes(data[header + 8:header + 12], 'little')
                    data[payload] ^= 1
                    return
            self.fail('fixture has no .code entry')
        self.check_mutation(1, mutate, 'ExeFS .code: SHA-256')

    def test_missing_new3ds_flag(self):
        self.check_mutation(2, lambda data: data.__setitem__(0x2029, data[0x2029] & ~0x10), 'New3DS-exclusive')

    def test_misleading_label(self):
        self.check_mutation(2, lambda data: data.__setitem__(0x208, ord('X')), 'diagnostic-only labels')


if __name__ == '__main__':
    unittest.main(verbosity=2)
