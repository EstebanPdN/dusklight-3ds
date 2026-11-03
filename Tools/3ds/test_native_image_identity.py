#!/usr/bin/env python3
"""Behavioral image provenance/variant oracle; no original game content."""
import hashlib
import struct
import zlib
from pathlib import Path

import native_asset_pack as assets


def rejects(action):
    try:
        action()
    except ValueError:
        return
    raise AssertionError('Invalid image provenance admitted')


def vectors():
    # Different sampler/header metadata over identical image bytes is a variant;
    # palette entries and all authored tiled bytes are identity, including tails.
    header = bytearray(32)
    header[0] = 3
    struct.pack_into('>2H', header, 2, 4, 4)
    header[20:22] = b'\x01\x01'
    header[24] = 1
    struct.pack_into('>I', header, 28, 32)
    direct = bytes(header) + bytes(range(32))
    sampler = bytearray(direct); sampler[6] = 1
    pixel = bytearray(direct); pixel[-1] ^= 1
    header[0] = 8; header[8] = 1; header[9] = 2
    struct.pack_into('>2H', header, 2, 8, 8)
    struct.pack_into('>H', header, 10, 2)
    struct.pack_into('>I', header, 12, 32)
    struct.pack_into('>I', header, 28, 36)
    indexed = bytes(header) + b'\xff\xff\x80\x00' + bytes(32)
    palette = bytearray(indexed); palette[33] ^= 1
    result = []
    for bti in [direct, bytes(sampler), bytes(pixel), indexed, bytes(palette)]:
        canonical = bytearray(bti[:32]); canonical[12:16] = bytes(4); canonical[28:32] = bytes(4)
        image_at = int.from_bytes(bti[28:32], 'big')
        image = bti[image_at:]
        pal = bti[32:image_at] if bti[0] == 8 else b''
        expected = hashlib.sha256(b'DUSKIMG1' + canonical + struct.pack('<2I', len(image), len(pal)) + image + pal).digest()
        assert assets.image_identity(bti) == expected
        result.append((bti, expected))
    assert len({digest for _, digest in result}) == 5
    # Relocation, unused gaps and trailing allocation padding do not change the
    # exact original header fields, image or palette that native rendering uses.
    relocated = bytearray(direct[:32]) + bytes(32) + direct[32:] + bytes(17)
    struct.pack_into('>I', relocated, 28, 64)
    assert assets.image_identity(bytes(relocated)) == result[0][1]
    for bti in [b'', direct[:31], direct[:-1]]:
        rejects(lambda: assets.image_identity(bti))
    broken = bytearray(indexed);struct.pack_into('>I', broken, 12, len(indexed))
    rejects(lambda: assets.image_identity(bytes(broken)))
    return result


def external_names():
    from test_native_model_plan import fixture, rebuild
    from j3d_native_geometry import sections
    source = fixture(); blocks = sections(source); tex = bytearray(blocks['TEX1'])
    base = struct.unpack_from('>I', tex, 12)[0]
    labels = b'I_kumo\0second\0'; names = len(tex)
    struct.pack_into('>I', tex, 16, names)
    tex.extend(struct.pack('>6H', 2, 0xffff, 0, 12, 0, 19) + labels)
    for index in range(2):struct.pack_into('>I', tex, base + 32*index + 28, 0)
    struct.pack_into('>I', tex, 4, len(tex)); blocks['TEX1'] = bytes(tex)
    source = rebuild(list(blocks.values())); first, second = vectors()[0][0], vectors()[1][0]
    resources = {'i_kumo.bti':first,'SECOND.BTI':second}
    resolved = assets.resolve_external_images(source, resources)
    assert resolved == {'i_kumo':first,'second':second}
    expected = hashlib.sha256(b'DUSKCTX1' + struct.pack('<I', 2) + assets.image_identity(first) + assets.image_identity(second)).digest()
    assert assets.image_context(source, resolved) == expected
    changed = assets.resolve_external_images(source, {**resources,'i_kumo.bti':second})
    assert assets.image_context(source, changed) != expected
    assert assets.filename(source, expected) != assets.filename(source, assets.image_context(source, changed))
    rejects(lambda: assets.resolve_external_images(source, {'i_kumo':first,'second.bti':second}))
    rejects(lambda: assets.resolve_external_images(source, {**resources,'I_KUMO.BTI':second}))


def named_source(source, labels, zero_indices=()):
    from j3d_native_geometry import sections
    from test_native_model_plan import rebuild
    blocks = sections(source); tex = bytearray(blocks['TEX1'])
    count = struct.unpack_from('>H', tex, 8)[0]
    assert len(labels) == count
    base = struct.unpack_from('>I', tex, 12)[0]
    names = len(tex); entries = bytearray(); strings = bytearray()
    for index, label in enumerate(labels):
        entries.extend(struct.pack('>HH', 0, 4 + count * 4 + len(strings)))
        strings.extend(label + b'\0')
        if index in zero_indices:
            struct.pack_into('>I', tex, base + index * 32 + 28, 0)
    tex.extend(struct.pack('>HH', count, 0xffff) + entries + strings)
    struct.pack_into('>I', tex, 16, names)
    struct.pack_into('>I', tex, 4, len(tex)); blocks['TEX1'] = bytes(tex)
    return rebuild(list(blocks.values()))


def framebuffer_roles():
    from j3d_material_source import input_declaration
    from j3d_native_geometry import sections
    from j3d_native_model import assemble
    import j3d_native_textures as textures
    import native_texture_compression as compression
    import native_texture_pack as pack
    from test_native_model_plan import fixture, rebuild

    source = fixture()
    original = list(textures.source_images(source))
    vector = bytes.fromhex('f0503c86ef37cab32ae14f76ea478ba76ccd3e8d1f037de0aa9cc852a7717c7c')
    assert hashlib.sha256(b'DUSKROLE1framebuffer-color').digest() == vector
    for name in (b'fbtex_dummy', b'dummy'):
        for zero in (False, True):
            named = named_source(source, [name, b'ordinary'], [0] if zero else [])
            reference_context = assets.image_context(named)
            blocks = sections(named); tex = bytearray(blocks['TEX1'])
            base = struct.unpack_from('>I', tex, 12)[0]
            tex[base] = 255  # Invalid image data must never be interpreted.
            if not zero:
                pixels = base + struct.unpack_from('>I', tex, base + 28)[0]
                tex[pixels] ^= 0xff
                struct.pack_into('>I', tex, base + 28, 0xffffffff)
            blocks['TEX1'] = bytes(tex); named = rebuild(list(blocks.values()))
            assert assets.resolve_external_images(named, {}) == {}
            assert assets.resolve_external_images(named, {name.decode()+'.bti':original[0][1]}) == {}
            images = list(textures.source_images(named))
            assert list(textures.source_images(named, {name.decode():original[0][1]})) == images
            assert images[0][0] == 0 and images[0][1] is None
            assert images[0][2]['role'] == pack.FRAMEBUFFER_COLOR
            assert images[1] == original[1]
            assert assets.source_image_identity(images[0][1], images[0][2]) == vector
            expected = hashlib.sha256(b'DUSKCTX1' + struct.pack('<I', 2) +
                                      vector + assets.image_identity(original[1][1])).digest()
            declaration = input_declaration(named)
            assert assets.image_context(named) == reference_context == assets.image_context(named, declaration=declaration) == expected
            plan, _, receipt = assemble(named, allow_normalized_arithmetic=True,
                                        allow_sdk_sampler_model=True, declaration=declaration)
            role_sampler = next(row for row in receipt['samplers'] if row['id'] == 1)
            assert role_sampler['role'] == pack.FRAMEBUFFER_COLOR
            unit = plan['uniforms'][0]['texture_slots'].index(0)
            assert plan['parts'][0]['samplers'][unit] == {
                'wrap_s':'clamp', 'wrap_t':'clamp', 'magnify':'linear', 'minify':'linear',
                'mipmap':'nearest', 'first_level':0, 'last_level':0, 'lod_bias':0}

    # JUTNameTab returns no name for an out-of-range embedded texture.
    short = named_source(source, [b'dummy', b'ordinary'])
    blocks = sections(short); tex = bytearray(blocks['TEX1'])
    names = struct.unpack_from('>I', tex, 16)[0]
    struct.pack_into('>H', tex, names, 1)
    blocks['TEX1'] = bytes(tex); short = rebuild(list(blocks.values()))
    short_images = list(textures.source_images(short))
    assert short_images[0][2]['role'] == pack.FRAMEBUFFER_COLOR and short_images[1] == original[1]
    assert assets.resolve_external_images(short, {}) == {}
    base = struct.unpack_from('>I', tex, 12)[0]
    struct.pack_into('>I', tex, base + 32 + 28, 0)
    blocks['TEX1'] = bytes(tex); short_external = rebuild(list(blocks.values()))
    rejects(lambda: assets.resolve_external_images(short_external, {}))
    rejects(lambda: list(textures.source_images(short_external)))

    # Only setIndirectTex's exact case is special; archive lookups still fold ASCII.
    for name in (b'Dummy', b'FBTEX_DUMMY', b'zbuffer', b'dummy_suffix'):
        embedded = named_source(source, [name, b'ordinary'])
        assert list(textures.source_images(embedded))[0] == original[0]
        external = named_source(source, [name, b'ordinary'], [0])
        resource = original[0][1]
        resolved = assets.resolve_external_images(external, {name.decode().lower()+'.bti':resource})
        assert list(textures.source_images(external, resolved))[0][1] == resource
    for zero in (False, True):
        depth = named_source(source, [b'Zbuffer', b'ordinary'], [0] if zero else [])
        for action in (lambda: assets.resolve_external_images(depth, {}),
                       lambda: list(textures.source_images(depth))):
            try: action()
            except textures.Unsupported as error: assert 'Zbuffer' in str(error)
            else: raise AssertionError('Zbuffer role accepted')
    rejects(lambda: assets.source_image_identity(b'pixels', {'role':pack.FRAMEBUFFER_COLOR}))
    rejects(lambda: assets.source_image_identity(None, {'role':'other'}))

    # With no decoder or scratch directory, both aliases must still compile;
    # a synthetic lookup follows them without colliding with source indices.
    both = named_source(source, [b'fbtex_dummy', b'dummy'], [0])
    both_images = list(textures.source_images(both))
    assert all(assets.source_image_identity(bti, metadata) == vector for _, bti, metadata in both_images)
    assert assets.image_context(both) == hashlib.sha256(b'DUSKCTX1' + struct.pack('<I', 2) + vector * 2).digest()
    helper = {'id':3, 'format':'rgba8', 'width':8, 'height':8, 'mip_levels':1,
              'data_hex':bytes(range(256)).hex(), 'key':'test-helper'}
    packed, receipt = textures.convert(both, Path('/missing-decoder'), Path('/missing-scratch'),
                                       synthetic_images=[helper])
    assert receipt['images'][0]['role'] == receipt['images'][1]['role'] == pack.FRAMEBUFFER_COLOR
    assert receipt['images'][2]['kind'] == 'mathematical'
    assert all('normalized_bti_sha256' not in row and 'native_sha256' not in row
               for row in receipt['images'][:2])
    assert struct.unpack_from('<8s6I', packed, 0)[:6] == (b'DUSKTX01', 3, len(packed), 3, 32, 96)
    assert struct.unpack_from('<8I', packed, 32) == (1, 0, 0, 4, 1, 128, 0, 0)
    assert struct.unpack_from('<8I', packed, 64) == (2, 0, 0, 4, 1, 128, 0, 0)
    assert struct.unpack_from('<8I', packed, 96)[:7] == (3, 8, 8, 0, 1, 128, 256)
    assert packed[128:] == bytes(range(256))
    decoded = compression.images_from_pack(packed)
    assert [row['format'] for row in decoded] == [pack.FRAMEBUFFER_COLOR, pack.FRAMEBUFFER_COLOR, 'rgba8']
    same, profile = compression.compile_profile(packed, Path('/missing-encoder'), Path('/missing-scratch'),
                                                {1, 2}, hashlib.sha256(packed).hexdigest())
    assert same == packed and profile['encoder_sha256'] is None
    assert [row['status'] for row in profile['images'][:2]] == ['runtime_role', 'runtime_role']

    # Stored images remain byte-identical when a role is inserted between them.
    first = {'id':1, 'width':8, 'height':8, 'format':'rgba8', 'levels':[bytes(range(256))]}
    role = {'id':2, 'width':0, 'height':0, 'format':pack.FRAMEBUFFER_COLOR, 'levels':[b'']}
    last = {'id':3, 'width':8, 'height':8, 'format':'rgb565', 'levels':[bytes(range(128))]}
    mixed = pack.compile_images([last, role, first])
    assert compression.images_from_pack(mixed) == [first, role, last]
    assert struct.unpack_from('<8I', mixed, 64) == (2, 0, 0, 4, 1, 384, 0, 0)
    assert mixed[128:] == first['levels'][0] + last['levels'][0]
    for offset, value in ((36, 1), (40, 1), (48, 0), (48, 2), (52, 144), (56, 1), (60, 1)):
        bad = bytearray(packed); struct.pack_into('<I', bad, offset, value)
        struct.pack_into('<I', bad, 28, zlib.crc32(bad[:28] + bad[32:128]))
        rejects(lambda: compression.images_from_pack(bytes(bad)))
    old = bytearray(packed); struct.pack_into('<I', old, 8, 1)
    struct.pack_into('<I', old, 28, zlib.crc32(old[:28] + old[32:128]))
    rejects(lambda: compression.images_from_pack(bytes(old)))
    for bad in ({**role,'width':8}, {**role,'height':8}, {**role,'levels':[]},
                {**role,'levels':[b'x']}, {**role,'levels':[b'',b'']}):
        rejects(lambda: pack.compile_images([bad]))

if __name__ == '__main__':
    vectors(); external_names(); framebuffer_roles()
    print('PASS original and framebuffer-role image identities, name resolution, sampler, pack and compression contracts')
