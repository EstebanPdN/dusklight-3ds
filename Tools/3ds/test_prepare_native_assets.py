#!/usr/bin/env python3
"""Public synthetic end-to-end extraction/conversion/provenance/no-clobber checks.

Run after integration: python3 Tools/3ds/test_prepare_native_assets.py --output
Build/native-preparation-regression (a NEW ignored private directory). Builds and
runs the actual C++ parsers/decoder under ASan/UBSan; never needs a game image.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import zlib

import prepare_native_assets as preparation
from j3d_native_geometry import sections
from j3d_native_textures import source_images
from test_native_model_plan import fixture, rebuild
from test_native_large_model import source_fixture as large_source_fixture
from test_native_readied_inputs import check_inputs as check_readied_inputs, check_conversion as check_readied_conversion
import twilight_pack

ROOT = Path(__file__).resolve().parents[2]
ARCHIVE = '/res/test.arc'


def yaz0(data):
    # Literal-only independent encoder, including a partial final control group.
    return b'Yaz0' + struct.pack('>I8x', len(data)) + b''.join(
        bytes([0xff]) + data[i:i + 8] for i in range(0, len(data), 8))


def rarc(entries):
    names = bytearray(b'root\0')
    offsets = []
    for name, _ in entries:
        offsets.append(len(names))
        names.extend(name + b'\0')
    string_offset = (80 + 20 * len(entries) + 31) & ~31
    data_offset = (string_offset + len(names) + 31) & ~31
    data = bytearray(data_offset)
    data[:4] = b'RARC'
    struct.pack_into('>III', data, 8, 32, data_offset - 32, sum(len(p) for _, p in entries))
    struct.pack_into('>6I', data, 32, 1, 32, len(entries), 48, len(names), string_offset - 32)
    data[64:68] = b'ROOT'
    struct.pack_into('>H', data, 74, len(entries))
    data[string_offset:string_offset + len(names)] = names
    cursor = 0
    for index, ((_, payload), name) in enumerate(zip(entries, offsets)):
        struct.pack_into('>HH4I', data, 80 + index * 20, index, 0, 0x11000000 | name, cursor, len(payload), 0)
        data.extend(payload)
        cursor += len(payload)
    struct.pack_into('>I', data, 4, len(data))
    return bytes(data)


def iso(archive):
    # PAL r0, minimal valid DOL and FST, no original game bytes.
    data = bytearray(0x1000)
    data[:8] = b'GZ2P01\0\0'
    struct.pack_into('>I', data, 0x1c, 0xc2339f3d)
    strings = b'res\0test.arc\0'
    fst = struct.pack('>9I', 0x01000000, 0, 3, 0x01000000, 0, 3, 4, 0x1000, len(archive)) + strings
    struct.pack_into('>3I', data, 0x420, 0x500, 0x600, len(fst))
    data[0x600:0x600 + len(fst)] = fst
    return bytes(data) + archive


def bundle(data):
    index = twilight_pack.RECORD.pack(0, 192, len(data), hashlib.sha256(data).digest(), bytes(8))
    return twilight_pack.header_bytes(len(data), 192 + len(data), 1, zlib.crc32(index),
                                      hashlib.sha256(data).digest()) + index + data


def extract(binary, source, directory):
    directory.mkdir(mode=0o700)
    fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        return subprocess.run([str(binary), str(source), str(fd), ARCHIVE], pass_fds=(fd,),
                              text=True, capture_output=True, timeout=30)
    finally:
        os.close(fd)


def rejects(action, contains):
    try:
        action()
    except (ValueError, OSError) as error:
        assert contains in str(error), str(error)
        return error
    raise AssertionError('Invalid input or clobber was admitted')


def selection(*rows):
    return {'schema': 'dusklight-native-model-selection/1',
            'models': [preparation.model_selector(row) for row in rows]}


def check_selection(first):
    second = {**first, 'entry': first['entry'] + 10, 'resource_id': first['resource_id'] + 1,
              'name_hex': b'other.bmd'.hex(), 'name': 'other.bmd'}
    models = [first, second]
    assert preparation.select_models(models, None) == models
    assert preparation.select_models(models, selection(second, first)) == models, 'Manifest reordered source models'
    assert preparation.select_models(models, selection(second)) == [second]
    rejects(lambda: preparation.select_models(models, selection()), '1..256')
    rejects(lambda: preparation.select_models(models, {'schema': 'wrong', 'models': []}), 'schema')
    rejects(lambda: preparation.select_models(models, selection(first, first)), 'Duplicate')
    for field, value in [('archive', '/res/Missing.arc'), ('entry', 65535), ('resource_id', 65535),
                         ('name_hex', b'renamed.bmd'.hex()), ('source_sha256', '0' * 64)]:
        bad = selection(first)
        bad['models'][0][field] = value
        rejects(lambda: preparation.select_models(models, bad), 'Missing or stale')
    for field, value in [('entry', True), ('resource_id', True), ('name_hex', 'gg'),
                         ('source_sha256', '00'), ('archive', None)]:
        bad = selection(first)
        bad['models'][0][field] = value
        rejects(lambda: preparation.select_models(models, bad), 'selector')
    missing = selection(first)
    del missing['models'][0]['name_hex']
    rejects(lambda: preparation.select_models(models, missing), 'fields')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    with preparation.card_private_parent(args.output, create=True) as (parent, name, out):
        os.mkdir(name, 0o700, dir_fd=parent)
    source = fixture()
    normalized = list(source_images(source))
    bti = normalized[0][1]
    archive = rarc([(b'..not-a-path.bmd', yaz0(source)), (b'TEXTURE.bti', bti)])
    original = iso(yaz0(archive))
    disc, packed_disc = out / 'synthetic.iso', out / 'synthetic.twilight'
    preparation.write_new(disc, original)
    preparation.write_new(packed_disc, bundle(original))
    before = disc.read_bytes(), packed_disc.read_bytes()
    extractor, decoder = out / 'extract', out / 'decoder'
    preparation.build_extractor(ROOT, extractor)
    preparation.textures.build_decoder(ROOT, decoder)
    check_readied_inputs()
    check_readied_conversion(decoder, out / 'readied-conversion')

    # Numeric filenames, decoded entry provenance and unchanged region, for BOTH
    # supported source carriers. Compare bytes, not echoed tool metadata.
    for index, carrier in enumerate([disc, packed_disc]):
        folder = out / f'extraction-{index}'
        result = extract(extractor, carrier, folder)
        assert result.returncode == 0, result.stderr
        with carrier.open('rb') as stream:
            if index:
                container = twilight_pack.Bundle(stream)
                read, files = container.read, container.files
            else:
                read = lambda offset, size: twilight_pack.exact(stream, offset, size)
                _, files = twilight_pack.layout(read, len(original))
            archive_rows, rows = preparation.extraction_inventory(result.stdout, folder, [ARCHIVE], read, files)
        assert archive_rows[0]['source_sha256'] == preparation.sha(yaz0(archive))
        assert archive_rows[0]['expanded_sha256'] == preparation.sha(archive)
        assert rows[0]['encoded_sha256'] == preparation.sha(yaz0(source))
        assert rows[0]['sha256'] == preparation.sha(source) and rows[0]['name'] == '..not-a-path.bmd'
        assert (folder / 'archive-0000-entry-000000.bin').read_bytes() == source
        assert rows[1]['sha256'] == preparation.sha(bti)
        resource = {**rows[1], 'name':'i_kumo.bti', 'path':Path(rows[1]['path']).name}
        keyed, ambiguous = preparation.external_resources([resource], folder)
        assert keyed == {'i_kumo.bti':bti} and not ambiguous and 'i_kumo' not in keyed
        keyed, ambiguous = preparation.external_resources([resource,{**resource,'name':'I_KUMO.BTI'}],folder)
        assert not keyed and len(ambiguous['i_kumo.bti']) == 2
        assert not (folder / '..not-a-path.bmd').exists()
        check_selection(rows[0])

    # Exercise actual C++ rejection before trusting any extracted payload.
    malformed = bytearray(original)
    struct.pack_into('>I', malformed, 0x600 + 32, 0xffffffff)  # FST file extent
    bad_arc = bytearray(archive)
    struct.pack_into('>I', bad_arc, 88, 0xffffffff)  # RARC file data offset
    corruptions = [bytes(malformed), iso(bytes(bad_arc)), iso(b'Yaz0' + struct.pack('>I8x', 0xffffffff)),
                   iso(yaz0(archive)[:-1])]
    for index, payload in enumerate(corruptions):
        carrier = out / f'malformed-{index}.iso'
        preparation.write_new(carrier, payload)
        result = extract(extractor, carrier, out / f'refused-{index}')
        assert result.returncode != 0 and 'archive=0 entry=0:' in result.stderr, result.stdout
        assert not (out / f'refused-{index}' / 'archive-0000-entry-000000.bin').exists()

    # Complete command pipeline: original models -> real banks -> hashed DNA.
    destination = out / 'prepared'
    receipt = preparation.prepare(packed_disc, [ARCHIVE], destination)
    assert receipt['status'] == 'complete' and receipt['source_unchanged']
    assert receipt['models_discovered'] == receipt['models_selected'] == 1
    assert receipt['selection_mode'] == 'all' and receipt['models'][0]['selected']
    assert receipt['source']['game_id'] == 'GZ2P01' and receipt['source']['revision'] == 0
    assert receipt['source']['sha256'] == preparation.sha(before[1])
    assert receipt['source']['original_iso_sha256'] == preparation.sha(before[0])
    converted = receipt['models'][0]
    dna = destination / converted['native_filename']
    native = dna.read_bytes()
    assert native[24:56] == hashlib.sha256(source).digest()
    assert converted['shapes'] == [0, 0] and converted['association']['parts'] == 2
    assert converted['native_sha256'] == preparation.sha(native)
    assert receipt['rendering_ready'] is False and receipt['fps_verified'] is False
    committed = dna.read_bytes(), (destination / 'receipt.json').read_bytes()
    rejects(lambda: preparation.prepare(disc, [ARCHIVE], destination), 'refusing overwrite')
    assert committed == (dna.read_bytes(), (destination / 'receipt.json').read_bytes())
    rejects(lambda: preparation.prepare(disc, [ARCHIVE], ROOT / 'Tools/3ds/not-private-preparation'), 'private')
    redirect = out / 'redirect'
    redirect.symlink_to(ROOT / 'Tools', target_is_directory=True)
    rejects(lambda: preparation.prepare(disc, [ARCHIVE], redirect / 'not-created'), '')
    assert not (ROOT / 'Tools/not-created').exists()
    banks = tuple(native[offset:offset + size] for offset, size in
                  (struct.unpack_from('<2I', native, 56 + index * 8) for index in range(4)))
    assert banks[1][:12] == b'DUSKTX01\x03\x00\x00\x00'
    forged = bytearray(native)
    forged[24] ^= 1
    struct.pack_into('<I', forged, 156, zlib.crc32(forged[:156] + forged[160:]))
    context = bytes.fromhex(converted['image_context_sha256'])
    rejects(lambda: preparation.verify_asset(bytes(forged), source, banks, [0, 0], False, context), 'SHA-256 mismatch')
    forged = bytearray(native)
    struct.pack_into('<I', forged, 56, 0xffffffff)
    struct.pack_into('<I', forged, 156, zlib.crc32(forged[:156] + forged[160:]))
    rejects(lambda: preparation.verify_asset(bytes(forged), source, banks, [0, 0], False, context), 'bank extent')

    # A single refused model must prevent publication of the entire selection,
    # even when an earlier model has already produced a valid native bank.
    bad_blocks = sections(source)
    bad_shape = bytearray(bad_blocks['SHP1'])
    bad_shape[struct.unpack_from('>I', bad_shape, 12)[0]] = 4
    bad_blocks['SHP1'] = bytes(bad_shape)
    mixed = out / 'mixed.iso'
    preparation.write_new(mixed, iso(rarc([(b'ok.bmd', source), (b'invalid-mode.bmd', rebuild(list(bad_blocks.values())))])))
    failed_output = out / 'not-published'
    failure = rejects(lambda: preparation.prepare(mixed, [ARCHIVE], failed_output), 'unsupported')
    assert isinstance(failure, preparation.PreparationError) and not failed_output.exists()
    failed = json.loads(failure.receipt_path.read_text())
    assert [row['status'] for row in failed['models']] == ['converted', 'refused']
    assert 'matrix mode 4' in failed['models'][1]['reason']
    assert failed['models_discovered'] == failed['models_selected'] == 2
    assert all(row['selected'] for row in failed['models'])

    # A manifest is an exact opt-in, not a converter error filter. Keep every
    # discovered original in the receipt, and admit the whole selection first.
    model_inputs = [row for row in failed['inputs'] if row['kind'] == 'model']
    chosen = selection(model_inputs[0])
    manifest_path = out / 'model-selection.json'
    preparation.write_json(manifest_path, chosen)
    selected_output = out / 'selected'
    selected = preparation.prepare(mixed, [ARCHIVE], selected_output,
                                   model_selection=preparation.read_model_selection(manifest_path))
    assert [row['status'] for row in selected['models']] == ['converted', 'notselected']
    assert [row['selected'] for row in selected['models']] == [True, False]
    assert selected['models_discovered'] == 2 and selected['models_selected'] == 1
    assert selected['model_selection'] == chosen
    assert (selected_output / selected['models'][0]['path']).read_bytes() == native
    assert selected['models'][1]['source_sha256'] == model_inputs[1]['sha256']
    assert 'path' not in selected['models'][1], 'Unselected model received a native bank'
    selected_before = (selected_output / 'receipt.json').read_bytes()
    rejects(lambda: preparation.prepare(mixed, [ARCHIVE], selected_output, model_selection=chosen), 'refusing overwrite')
    assert (selected_output / 'receipt.json').read_bytes() == selected_before
    stale = selection(*model_inputs)
    stale['models'][1]['source_sha256'] = '0' * 64
    stale_output = out / 'stale-not-published'
    failure = rejects(lambda: preparation.prepare(mixed, [ARCHIVE], stale_output, model_selection=stale), 'stale')
    stale_receipt = json.loads(failure.receipt_path.read_text())
    assert not stale_output.exists() and not list(failure.receipt_path.parent.glob('*.dna'))
    assert len(stale_receipt['models']) == 2 and not any(row['selected'] for row in stale_receipt['models'])
    for index, payload in enumerate([b'null', b'{', b' ' * (preparation.MAX_SELECTION_BYTES + 1)]):
        invalid_manifest = out / f'invalid-selection-{index}.json'
        preparation.write_new(invalid_manifest, payload)
        rejects(lambda: preparation.read_model_selection(invalid_manifest), '')

    # Real conversion through all four bank writers and the DNA envelope, using
    # the already-built decoder. Check source triangle identities as well as the
    # high native uniform binding; no game resource or GPU result is fabricated.
    large_source = large_source_fixture(384)
    large_scratch = out / 'large-scratch'
    large_scratch.mkdir(mode=0o700)
    large, large_receipt = preparation.convert_model(large_source, decoder, large_scratch)
    assert large[24:56] == hashlib.sha256(large_source).digest()
    assert [row['source_group'] for row in large_receipt['association']['source_correspondence']] == list(range(384))
    mesh_at, _ = struct.unpack_from('<2I', large, 56)
    plan_at, _ = struct.unpack_from('<2I', large, 80)
    for group in range(384):
        _, nv, ni, vo, io, _, _, _ = struct.unpack_from('<8I', large, mesh_at + 32 + group * 32)
        assert nv == 4 and ni == 6
        assert struct.unpack_from('<6H', large, mesh_at + io) == (0, 1, 2, 2, 1, 3)
        assert struct.unpack_from('<f', large, mesh_at + vo + 9 * 4)[0] == group + .5
        assert struct.unpack_from('<3I', large, plan_at + 48 + group * 64) == (group + 1, 1, group)
    shape_at, shape_bytes = struct.unpack_from('<2I', large, 88)
    assert shape_bytes == 384 * 4 and large[shape_at:shape_at + shape_bytes] == bytes(shape_bytes)
    large_banks = tuple(large[offset:offset + size] for offset, size in
                        (struct.unpack_from('<2I', large, 56 + index * 8) for index in range(4)))
    forged_plan = bytearray(large_banks[3])
    struct.pack_into('<I', forged_plan, 16, 1025)
    rejects(lambda: preparation.native_asset_pack.compile_asset(
        large_source, *large_banks[:3], bytes(forged_plan), [0] * 1025,
        image_context=bytes.fromhex(large_receipt['image_context_sha256'])), 'shape ownership count')

    # Selecting a model never licenses dropping its unused authored materials.
    unused_blocks = sections(source)
    unused_materials = bytearray(unused_blocks['MAT3'])
    struct.pack_into('>H', unused_materials, 8, 2)
    remap = struct.unpack_from('>I', unused_materials, 16)[0]
    struct.pack_into('>H', unused_materials, remap + 2, 65535)
    unused_blocks['MAT3'] = bytes(unused_materials)
    unused_scratch = out / 'unused-material-scratch'
    unused_scratch.mkdir(mode=0o700)
    rejects(lambda: preparation.convert_model(rebuild(list(unused_blocks.values())), decoder, unused_scratch),
            'Source material 2 refused')

    # Real external BTIs carry their original resource identity and select a
    # model+context filename; original TEX1 labels append exactly ".bti".
    blocks = sections(source)
    tex = bytearray(blocks['TEX1'])
    base = struct.unpack_from('>I', tex, 12)[0]
    names_at = len(tex)
    labels = b'first\0second\0'
    struct.pack_into('>I', tex, 16, names_at)
    tex.extend(struct.pack('>6H', 2, 0xffff, 0, 12, 0, 18) + labels)
    for index in range(2):
        struct.pack_into('>I', tex, base + index * 32 + 28, 0)
    struct.pack_into('>I', tex, 4, len(tex))
    blocks['TEX1'] = bytes(tex)
    external_source = rebuild(list(blocks.values()))
    scratch = out / 'external-scratch'
    scratch.mkdir(mode=0o700)
    external_dna, external_receipt = preparation.convert_model(
        external_source, decoder, scratch, {'first.bti': normalized[0][1], 'second.bti': normalized[1][1]})
    assert struct.unpack_from('<I', external_dna, 96)[0] == 1
    assert external_receipt['external_context_required'] and external_receipt['runtime_external_context_supported']
    assert [r['external_resource_sha256'] for r in external_receipt['images']['images']] == [
        preparation.sha(normalized[0][1]), preparation.sha(normalized[1][1])]
    assert external_dna[24:56] == hashlib.sha256(external_source).digest()
    assert external_dna[100:132].hex() == external_receipt['image_context_sha256']
    forged = bytearray(external_dna);forged[100] ^= 1
    struct.pack_into('<I', forged, 156, zlib.crc32(forged[:156] + forged[160:]))
    external_banks = tuple(external_dna[offset:offset+size] for offset,size in
                           (struct.unpack_from('<2I',external_dna,56+i*8) for i in range(4)))
    rejects(lambda: preparation.verify_asset(bytes(forged),external_source,external_banks,[0,0],True,
                                             bytes.fromhex(external_receipt['image_context_sha256'])), 'context mismatch')
    # Same original BMD, different actual external pixels: different key/bank,
    # neither source identity nor the already-converted bank is overwritten.
    changed_bti = bytearray(normalized[0][1]);changed_bti[-1] ^= 1
    variant_scratch = out / 'variant-scratch';variant_scratch.mkdir(mode=0o700)
    variant, variant_receipt = preparation.convert_model(
        external_source,decoder,variant_scratch,{'first.bti':bytes(changed_bti),'second.bti':normalized[1][1]})
    assert variant[24:56] == external_dna[24:56] and variant[100:132] != external_dna[100:132]
    assert variant_receipt['native_filename'] != external_receipt['native_filename']
    rejects(lambda: preparation.native_asset_pack.resolve_external_images(
        external_source, {'first':normalized[0][1],'second.bti':normalized[1][1]}), 'binding: first')
    broken_source = bytearray(source)
    struct.pack_into('>I', broken_source, 8, len(source) + 1)
    rejects(lambda: preparation.convert_model(bytes(broken_source), decoder, scratch), 'extent/header')

    # Directory publication itself must refuse an empty directory AND a symlink,
    # not merely rely on prepare()'s initial existence check.
    stage, existing = out / 'stage', out / 'concurrent'
    stage.mkdir(mode=0o700)
    existing.mkdir(mode=0o700)
    preparation.write_new(stage / 'owned', b'synthetic')
    fd = os.open(out, os.O_RDONLY | os.O_DIRECTORY)
    try:
        rejects(lambda: preparation.publish_directory(fd, stage.name, existing.name), 'exist')
        link = out / 'concurrent-link'
        link.symlink_to(existing, target_is_directory=True)
        rejects(lambda: preparation.publish_directory(fd, stage.name, link.name), 'exist')
    finally:
        os.close(fd)
    assert (stage / 'owned').read_bytes() == b'synthetic' and existing.is_dir() and link.is_symlink()
    assert before == (disc.read_bytes(), packed_disc.read_bytes())
    print('PASS synthetic native preparation: real ISO/bundle/Yaz0/RARC conversion, malformed extents, source provenance, all-or-nothing refusal and atomic no-clobber')


if __name__ == '__main__':
    main()
