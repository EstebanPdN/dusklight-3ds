#!/usr/bin/env python3
import json
from pathlib import Path
import tempfile
import unittest
import verify_application_dump as audit


def fnv(data):
    result = 2166136261
    for byte in data: result = ((result ^ byte) * 16777619) & 0xffffffff
    return result


class DumpVerifierTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.map = ('schema=1 coherence=chunked-live-non-atomic\n'
                    'range base size perm state pageflags decision\n'
                    'range 00000000 00100000 0 0 00000000 not-readable\n'
                    'range 00100000 00001000 5 4 00000000 eligible\n'
                    f'chunk memory-00000.bin 00100000 00001000 fnv1a32={fnv(bytes([17])*4096):08x}\n'
                    'range 00101000 006ff000 0 0 00000000 not-readable\n'
                    'range 00800000 00002000 3 5 00000000 eligible\n'
                    'exclude 00800000 00001000 diagnostic-workspace\n'
                    f'chunk memory-00001.bin 00801000 00001000 fnv1a32={fnv(bytes([29])*4096):08x}\n'
                    'range 00802000 3f7fe000 0 0 00000000 not-readable\n')
        (self.root/'memory-00000.bin').write_bytes(bytes([17])*4096)
        (self.root/'memory-00001.bin').write_bytes(bytes([29])*4096)
        (self.root/'top.rgba8').write_bytes(bytes(384000))
        (self.root/'bottom.rgb565').write_bytes(bytes(153600))
        self.publish()

    def publish(self):
        (self.root/'map.txt').write_text(self.map)
        manifest = ('format=DUSKLIGHT_APPLICATION_DUMP_1\ncoherence=chunked-live-non-atomic\n'
                    'frame=300\nscene=TEST\nbuild=host-fixture\nram_bytes=8192\nchunks=2\n')
        for name in ('top.rgba8','bottom.rgb565','map.txt'):
            data=(self.root/name).read_bytes()
            manifest += f'file={name} bytes={len(data)} fnv1a32={fnv(data):08x}\n'
        (self.root/'manifest.txt').write_text(manifest)
        (self.root/'COMPLETE').write_text(f'verified=1\nmanifest_fnv1a32={fnv(manifest.encode()):08x}\nmap_fnv1a32={fnv(self.map.encode()):08x}\n')

    def add_extras(self, bad_address=False):
        names={'vram.bin':0x600000,'lcd-top-left.bin':400*240*3,'lcd-bottom.bin':320*240*2}
        extra=('schema=1\nsource=GSP-ImportDisplayCaptureInfo-after-GPU-drain-and-VBlank\n'
               'vram_base=1f000000\nvram_size=6291456\n')
        for name,n in names.items():
            data=bytes(n);(self.root/name).write_bytes(data)
            address=0x1f000000 if name=='vram.bin' else 0x14000000
            if bad_address and name=='vram.bin': address+=4096
            extra+=f'file={name} bytes={n} fnv1a32={fnv(data):08x} address={address:08x} layout=fixture\n'
        data=extra.encode();(self.root/'extras.txt').write_bytes(data)
        manifest=(self.root/'manifest.txt').read_text()+f'file=extras.txt bytes={len(data)} fnv1a32={fnv(data):08x}\n'
        manifest+=f'supplement_bytes={sum(names.values())}\ndisplay_source=GSP-presented-LCD-frontbuffers\n'
        (self.root/'manifest.txt').write_text(manifest)
        (self.root/'COMPLETE').write_text(f'verified=1\nmanifest_fnv1a32={fnv(manifest.encode()):08x}\nmap_fnv1a32={fnv(self.map.encode()):08x}\n')

    def test_lcd_vram_supplement(self):
        self.add_extras();result=audit.inspect(self.root)
        self.assertEqual(result['supplement_bytes'],6733056)
        self.assertEqual(result['display_source'],'GSP-presented-LCD-frontbuffers')

    def test_supplement_wrong_vram_address(self):
        self.add_extras(bad_address=True)
        with self.assertRaisesRegex(ValueError,'address'):audit.inspect(self.root)

    def test_supplement_corrupt_payload(self):
        self.add_extras();(self.root/'lcd-bottom.bin').write_bytes(bytes([1])*(320*240*2))
        with self.assertRaisesRegex(ValueError,'Checksum'):audit.inspect(self.root)

    def test_complete(self):
        result=audit.inspect(self.root)
        self.assertEqual(result['ram_bytes'],8192)
        self.assertEqual(result['workspace_excluded_bytes'],4096)
        self.assertFalse(result['atomic_snapshot'])

    def test_corrupt_chunk(self):
        (self.root/'memory-00000.bin').write_bytes(bytes(4096))
        with self.assertRaisesRegex(ValueError,'Checksum'): audit.inspect(self.root)

    def test_missing_marker(self):
        (self.root/'COMPLETE').unlink()
        with self.assertRaises(ValueError): audit.inspect(self.root)

    def test_coverage_gap_even_with_valid_checksums(self):
        self.map=self.map.replace('exclude 00800000 00001000 diagnostic-workspace\n',''); self.publish()
        with self.assertRaisesRegex(ValueError,'coverage'): audit.inspect(self.root)

    def test_out_of_policy_payload(self):
        self.map=self.map.replace('range 00100000 00001000 5 4','range 00100000 00001000 0 0'); self.publish()
        with self.assertRaisesRegex(ValueError,'policy'): audit.inspect(self.root)

    def test_unknown_file(self):
        (self.root/'extra.bin').write_bytes(b'x')
        with self.assertRaisesRegex(ValueError,'Unexpected'): audit.inspect(self.root)

    def test_symlink(self):
        original=self.root/'memory-00000.bin'; original.rename(self.root/'hidden.bin'); original.symlink_to('hidden.bin')
        with self.assertRaisesRegex(ValueError,'Invalid'): audit.inspect(self.root)

    def test_map_scope_missing(self):
        self.map=self.map.replace('3f7fe000','00001000'); self.publish()
        with self.assertRaisesRegex(ValueError,'scope'): audit.inspect(self.root)

if __name__ == '__main__': unittest.main()
