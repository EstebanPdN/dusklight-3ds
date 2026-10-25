"""Independent byte/layout oracles for the private texture lab, no game changes."""
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import unittest

import texture_lab_formats as f
from texture_compression_lab import ROOT, build_tools, decode_mips, estimate


class LabTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.decoder,cls.encoder=build_tools(ROOT/'Build/texture-compression-tools')

    def test_fixed_16bit_words(self):
        # PICA little-endian words, not a self-derived roundtrip oracle.
        for fmt,rgba,expected in [('RGB565',(255,0,0,255),b'\x00\xf8'),
                                  ('RGB565',(0,255,0,255),b'\xe0\x07'),
                                  ('RGB5A1',(0,0,255,255),b'\x3f\x00'),
                                  ('RGB5A1',(255,0,0,0),b'\x00\xf8'),
                                  ('RGBA4',(17,34,51,68),b'\x34\x12')]:
            data=f.encode16(bytes(rgba)*64,8,8,fmt)
            self.assertEqual(data,expected*64)
            self.assertEqual(f.decode16(data,8,8,fmt),bytes(rgba)*64)

    def test_morton_known_addresses_and_padding(self):
        known={(0,0):0,(1,0):1,(0,1):2,(2,0):4,(0,2):8,(4,0):16,(0,4):32,(7,7):63}
        for (x,y),expected in known.items(): self.assertEqual(f.morton(x,y),expected)
        self.assertEqual(len({f.morton(x,y) for y in range(8) for x in range(8)}),64)
        src=bytes((17,34,51,68))*3*5
        self.assertEqual(f.encode16(src,3,5,'RGBA4'),b'\x34\x12'*64)
        self.assertEqual(f.decode16(b'\x34\x12'*64,3,5,'RGBA4'),src)

    def test_native_gx565_exhaustive_then_exact_pica(self):
        # Every possible GX565 word, in GX's independent row-major 4x4 tiles.
        source=bytearray(); expected=bytearray()
        for y in range(256):
            for x in range(256):
                v=y*256+x; r=v>>11; g=(v>>5)&63; b=v&31
                expected.extend(((r<<3)|(r>>2),(g<<2)|(g>>4),(b<<3)|(b>>2),255))
        for ty in range(0,256,4):
            for tx in range(0,256,4):
                for dy in range(4):
                    for dx in range(4): source.extend(struct.pack('>H',(ty+dy)*256+tx+dx))
        header=bytearray(32); header[0]=4; header[24]=1
        struct.pack_into('>HH',header,2,256,256); struct.pack_into('>I',header,28,32)
        record={'mips':1,'width':256,'height':256,'source_image_bytes':len(source)}
        with tempfile.TemporaryDirectory() as temp:
            decoded=decode_mips(record,bytes(header)+source,self.decoder,Path(temp))[0][0]
        self.assertEqual(decoded,bytes(expected))
        pica=f.encode16(decoded,256,256,'RGB565')
        self.assertEqual(f.decode16(pica,256,256,'RGB565'),decoded)
        # Independent encoded-address check; not only encode/decode mutual agreement.
        for y in range(256):
            for x in range(256):
                tile=(y//8)*32+x//8
                address=tile*128+f.morton(x,y)*2
                self.assertEqual(struct.unpack_from('<H',pica,address)[0],y*256+x)

    def test_known_etc1_individual_flip_and_selectors(self):
        self.assertEqual(f.decode_etc1_block(bytes(8)),bytes((2,2,2,255))*16)
        # Individual R bases 1,2 / G 3,4 / B 5,6; table 0; selector 0 => +2.
        for flip in (0,1):
            block=bytes((0x12,0x34,0x56,flip,0,0,0,0))
            pixels=f.decode_etc1_block(block)
            for y in range(4):
                for x in range(4):
                    second=(y>=2) if flip else (x>=2)
                    self.assertEqual(pixels[(y*4+x)*4:(y*4+x+1)*4],
                                     bytes((36,70,104,255) if second else (19,53,87,255)))
        # base=8*17, table0; x-major selectors [0,1,2,3] in first column.
        v=int.from_bytes(bytes.fromhex('88888800000c000a'),'big')
        pixels=f.decode_etc1_block(v.to_bytes(8,'big'))
        self.assertEqual([pixels[y*16] for y in range(4)],[138,144,134,128])

    def test_known_etc1_differential(self):
        # base RGB=10/15/20; differential +1/-2/+3; selector0 => +2.
        block=bytes((0x51,0x7e,0xa3,2,0,0,0,0))
        p=f.decode_etc1_block(block)
        self.assertEqual(p[:4],bytes((84,125,167,255)))
        self.assertEqual(p[8:12],bytes((92,109,191,255)))
        with self.assertRaises(ValueError): f.decode_etc1_block(bytes((7,0,0,2,0,0,0,0)))

    def test_independent_decoder_random_against_upstream(self):
        rng=random.Random(4405); corpus=[]; expected=bytearray()
        for _ in range(1800):
            block=rng.randbytes(8)
            try: pixels=f.decode_etc1_block(block)
            except ValueError: continue
            corpus.append(block); expected.extend(pixels)
        self.assertGreater(len(corpus),1000)
        with tempfile.TemporaryDirectory() as temp:
            inp,out=Path(temp)/'blocks',Path(temp)/'rgba'; inp.write_bytes(b''.join(corpus))
            subprocess.run([str(self.encoder),'decode',str(inp),str(out)],check=True,capture_output=True,timeout=15)
            self.assertEqual(out.read_bytes(),bytes(expected))

    def test_pica_endianness_block_order_and_alpha_axis(self):
        blocks=[]
        for i in range(16):
            value=i*17
            etc=bytes((value,value,value,0,0,0,0,0))
            # Each block alpha is x-major 0..15, intentionally asymmetric.
            alpha=sum((x*4+y)<<(4*(x*4+y)) for y in range(4) for x in range(4))
            blocks.append(alpha.to_bytes(8,'little')+etc[::-1])
        p=f.decode_etc(b''.join(blocks),16,16,'ETC1A4')
        # Explicit row-major tiles, TL TR BL BR subblocks.
        block_map=((0,1,4,5),(2,3,6,7),(8,9,12,13),(10,11,14,15))
        for y in range(16):
            for x in range(16):
                at=(y*16+x)*4; i=block_map[y//4][x//4]
                self.assertEqual(p[at],min(255,i*17+2))
                self.assertEqual(p[at+3],((x%4)*4+y%4)*17)

    def test_encode_spatial_pattern_independent_decode(self):
        source=bytearray()
        for y in range(16):
            for x in range(32): source.extend((x//4*30,y//4*60,(x//8+y//8)*40,((x%4)*4+y%4)*17))
        with tempfile.TemporaryDirectory() as temp:
            data,timing=f.encode_etc(bytes(source),32,16,'ETC1A4',self.encoder,Path(temp))
        proposed=f.decode_etc(data,32,16,'ETC1A4')
        met=f.metrics(bytes(source),proposed)
        self.assertLessEqual(max(met['channels'][c]['max_abs'] for c in 'rgb'),8)
        self.assertTrue(met['channels']['a']['lossless'])
        self.assertGreaterEqual(timing['codec_ms_including_init'],0)

    def test_alpha_aware_box_and_hidden_rgb(self):
        src=bytes((255,0,0,255,0,0,255,0,0,0,255,0,0,0,255,0))
        self.assertEqual(f.half_box(src,2,2),(bytes((255,0,0,64)),1,1))
        hidden=bytes((20,40,60,0))*4
        self.assertEqual(f.half_box(hidden,2,2),(bytes((20,40,60,0)),1,1))
        with self.assertRaises(ValueError): f.half_box(bytes(3*4*4),3,4)

    def test_metrics_alpha_coverage_and_padding_mips(self):
        a=bytes((10,20,30,0,40,50,60,255)); b=bytes((20,20,30,1,40,50,60,254))
        m=f.metrics(a,b)
        self.assertEqual(m['channels']['r']['max_abs'],10)
        self.assertEqual(m['channels']['r']['mean_abs'],5)
        self.assertEqual(m['alpha_coverage_at_least']['1']['changed_pixels'],1)
        self.assertEqual(m['alpha_coverage_at_least']['255']['changed_pixels'],1)
        self.assertEqual(f.allocation(144,144,1,4),256*256*4)
        self.assertEqual(f.allocation(64,32,3,.5),(64*32+32*16+16*8)//2)
        self.assertEqual(f.allocation(3,5,1,2),128)
        self.assertEqual(f.allocation(8,8,1,.5),32)

    def test_all_mip_alpha_class_and_safe_policy_estimates(self):
        record={'gx_format':'CMPR','width':32,'height':32,'mips':3}
        levels=[(bytes((0,0,0,255))*32*32,32,32),
                (bytes((0,0,0,255))*16*16,16,16),(bytes((0,0,0,100))*8*8,8,8)]
        e=estimate(record,levels)
        self.assertEqual(e['alpha_class_all_source_mips'],'smooth')
        self.assertNotIn('ETC1',e['same_dimensions'])
        self.assertNotIn('RGB5A1',e['same_dimensions'])
        self.assertEqual(e['half_dimensions']['mips'],2)


if __name__=='__main__': unittest.main()
