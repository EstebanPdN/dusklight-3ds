#!/usr/bin/env python3
"""Synthetic J3D BMWR warp-off inputs; optional real decoder bank smoke."""
import argparse
import copy
import hashlib
from pathlib import Path
import struct
import tempfile

from j3d_material_source import input_declaration, materials
from j3d_native_geometry import sections, u32
import j3d_native_materials as fragments
import j3d_native_model as models
from j3d_native_textures import source_images
import native_asset_pack as assets
import prepare_native_assets as preparation
from test_native_model_plan import fixture as model_fixture, rebuild


def fixture():
    blocks = sections(model_fixture())
    mat = bytearray(blocks['MAT3'])
    record = u32(mat, 12)
    # Actual MAT3 tables, not a mocked declaration: two raster channels, one
    # sampled source image, two active TEV stages, two original texgens.
    for index in range(4):
        struct.pack_into('>H', mat, record + 12 + index * 2, index)
    mat[u32(mat, 36)] = 2
    struct.pack_into('>I', mat, 40, len(mat))
    mat.extend(bytes((1, 0, 255, 2, 1, 0, 255, 255)) * 4)
    mat[u32(mat, 88)] = 2
    at = u32(mat, 92)
    mat[at:at + 40] = bytes((255, 15, 10, 8, 15, 0, 0, 0, 1, 0,
                             7, 5, 4, 7, 0, 0, 0, 1, 0, 255,
                             255, 15, 0, 10, 15, 0, 0, 0, 1, 0,
                             7, 0, 5, 7, 0, 0, 0, 1, 0, 255))
    at = u32(mat, 76)
    mat[at:at + 8] = bytes((0, 0, 4, 255, 255, 255, 5, 255))
    struct.pack_into('>I', mat, 120, len(mat))
    mat.extend(b'\1')
    mat[record + 5] = 0
    mat.extend(bytes((-len(mat)) % 32))
    struct.pack_into('>I', mat, 4, len(mat))
    blocks['MAT3'] = bytes(mat)
    source = rebuild(list(blocks.values()))
    warp = bytearray(next(source_images(source))[1])
    warp[-1] ^= 127
    return source, bytes(warp)


def selector(archive, entry, resource_id, name, payload):
    return {'archive': archive, 'entry': entry, 'resource_id': resource_id,
            'name_hex': name.encode().hex(), 'source_sha256': hashlib.sha256(payload).hexdigest()}


def selection(source, warp):
    resource = selector('/res/Object/Bmdl.arc', 15, 15, 'bl.bmd', source)
    resource['readied'] = {
        'loader_recipe': 'bmwr-warp-off',
        'warp_texture': selector(preparation.WARP_ARCHIVE, 93, 93, 'warp_tex.bti', warp)}
    return {'schema': 'dusklight-native-model-selection/2', 'models': [resource]}


def rejects(action, message):
    try:
        action()
    except ValueError as error:
        assert message in str(error), str(error)
    else:
        raise AssertionError('Readied invalid input accepted: ' + message)


def check_inputs():
    source, warp = fixture()
    manifest = selection(source, warp)
    readied = manifest['models'][0]['readied']
    declaration = input_declaration(source, loader_recipe=readied['loader_recipe'],
                                    warp_resource=warp, warp_selector=readied['warp_texture'])
    original_images = list(source_images(source))
    images = list(source_images(source, declaration=declaration))
    assert images[:2] == original_images and images[2][0] == 2 and images[2][1] == warp
    assert images[2][2]['readied_append']['native_image_id'] == 3
    context = hashlib.sha256(b'DUSKCTX1' + struct.pack('<I', 3) + b''.join(
        assets.image_identity(bti) for bti in [original_images[0][1], original_images[1][1], warp])).digest()
    assert assets.image_context(source, declaration=declaration) == context
    assert context != assets.image_context(source)
    assert assets.identity(source) == hashlib.sha256(source).digest()
    old = list(materials(source))[0]
    new = list(materials(source, declaration=declaration))[0]
    assert new['states']['alpha'][:5] == (4, 128, 0, 3, 255)
    assert old['states']['early_depth'] == 1 and new['states']['early_depth'] == 0
    assert new['stages'] == old['stages'] and new['generators'] == old['generators']
    assert new['textures'] == old['textures'] and new['textures'][3] is None
    for key in ('depth', 'blend', 'cull', 'fog', 'dither'):
        assert new['states'][key] == old['states'][key]
    assert new['channels'] == old['channels']
    inactive = new['readied_delta']['inactive_warp']
    assert not inactive['active'] and inactive['stage_index'] == inactive['generator_index'] == 2
    assert inactive['texture_slot'] == 3 and inactive['texture_index'] == 2
    assert inactive['order'] == (2, 3, 255, 0) and inactive['generator'] == (0, 0, 36, 0)
    _, original = fragments.convert(source, allow_normalized_arithmetic=True, preserve_color_parameters=True)
    bank, converted = fragments.convert(source, allow_normalized_arithmetic=True,
                                        preserve_color_parameters=True, declaration=declaration)
    record = converted['records'][0]
    assert bank is not None and (record['alpha'], record['alpha_reference']) == ('greaterEqual', 129)
    for value in range(256):
        assert (value >= record['alpha_reference']) == (value > 128)
    assert original['records'][0]['images'] == [1, 3, 4]
    assert record['images'] == [1, 4, 5], 'Helper images overlapped appended warp'
    metadata = converted['materials'][0]
    assert (metadata['source_stage_count'], metadata['source_texgen_count']) == (2, 2) and metadata['source_early_depth'] == 0
    assert [image['id'] for image in metadata['synthetic_images']] == [4, 5]
    assert metadata['readied_delta']['runtime_channel_producer']['snapshot_selected'] is False
    plan, native, association = models.assemble(source, allow_normalized_arithmetic=True,
                                                allow_sdk_sampler_model=True, declaration=declaration)
    assert [image['id'] for image in association['synthetic_images']] == [4, 5]
    assert plan['parts'][0]['sampler_mask'] == 7
    assert [row['id'] for row in association['samplers']] == [1], 'Inactive warp was sampled'
    assert plan['uniforms'][0]['texture_slots'] == [0, 255, 255]
    assert plan['uniforms'][0]['texture_generators'] == [0, 255, 255]
    assert all((slot['source_stage_count'], slot['source_texgen_count']) == (2, 2) for slot in plan['uniforms'])
    uniform_offset = struct.unpack_from('<I', native, 32)[0]
    assert all(native[uniform_offset + index * 80 + 72:uniform_offset + index * 80 + 74] == b'\2\2'
               for index in range(len(plan['uniforms'])))
    # A framebuffer role remains in the original table when the warp texture
    # and mathematical helpers are appended.
    from test_native_image_identity import named_source
    role_source = named_source(source, [b'dummy', b'ordinary'], [0])
    role_declaration = input_declaration(role_source, loader_recipe=readied['loader_recipe'],
                                         warp_resource=warp, warp_selector=readied['warp_texture'])
    assert role_declaration['images'][0][1] is None
    assert role_declaration['images'][0][2]['role'] == 'framebuffer-color'
    assert role_declaration['images'][2][2]['readied_append']['native_image_id'] == 3
    role_plan, _, role_association = models.assemble(
        role_source, allow_normalized_arithmetic=True, allow_sdk_sampler_model=True,
        declaration=role_declaration)
    assert [image['id'] for image in role_association['synthetic_images']] == [4, 5]
    assert role_plan['uniforms'][0]['texture_slots'] == [0, 255, 255]
    assert role_association['samplers'][0]['role'] == 'framebuffer-color'
    bad = copy.deepcopy(role_declaration)
    bad['images'][0][2]['source_name'] = 'fbtex_dummy'
    rejects(lambda: assets.image_context(role_source, declaration=bad), 'source image role mismatch')
    bad = copy.deepcopy(role_declaration)
    bad['images'] = ((0, original_images[0][1], bad['images'][0][2]), *bad['images'][1:])
    rejects(lambda: assets.image_context(role_source, declaration=bad), 'source image role mismatch')
    bad = copy.deepcopy(role_declaration)
    bad['images'] = (bad['images'][1], *bad['images'][1:])
    rejects(lambda: assets.image_context(role_source, declaration=bad), 'source image role mismatch')
    bad = copy.deepcopy(role_declaration)
    bad['images'] = (bad['images'][0], (1, None, copy.deepcopy(bad['images'][0][2])), bad['images'][2])
    rejects(lambda: assets.image_context(role_source, declaration=bad), 'source image role mismatch')
    altered_role = bytearray(role_source); altered_role[31] ^= 1
    rejects(lambda: assets.image_context(bytes(altered_role), declaration=role_declaration), 'source hash mismatch')
    # J3D's unused slot sentinel can be encoded through a real texture table
    # entry as well as an absent MAT3 record index.
    blocks = sections(source); mat = bytearray(blocks['MAT3'])
    struct.pack_into('>H', mat, u32(mat, 12) + 138, 2)
    struct.pack_into('>I', mat, 72, len(mat)); mat.extend(struct.pack('>3H', 0, 1, 65535))
    mat.extend(bytes((-len(mat)) % 32)); struct.pack_into('>I', mat, 4, len(mat))
    blocks['MAT3'] = bytes(mat); sentinel_source = rebuild(list(blocks.values()))
    sentinel = input_declaration(sentinel_source, loader_recipe='bmwr-warp-off',
                                 warp_resource=warp, warp_selector=readied['warp_texture'])
    assert sentinel['materials'][0]['textures'][3] == 65535
    mat = bytearray(mat); struct.pack_into('>H', mat, u32(mat, 72) + 4, 0)
    blocks['MAT3'] = bytes(mat); occupied_source = rebuild(list(blocks.values()))
    rejects(lambda: input_declaration(occupied_source, loader_recipe='bmwr-warp-off',
                                      warp_resource=warp, warp_selector=readied['warp_texture']), 'unused texture slot 3')
    rejects(lambda: input_declaration(source, loader_recipe='bmwr-warp-off'), 'Missing readied warp')
    rejects(lambda: input_declaration(source, loader_recipe='bmwr-warp-off', warp_resource=warp,
                                      warp_selector={**readied['warp_texture'], 'source_sha256': '0' * 64}),
            'Stale readied warp')
    altered = bytearray(source); altered[31] ^= 1
    rejects(lambda: assets.image_context(bytes(altered), declaration=declaration), 'source hash mismatch')
    archives = ['/res/Object/Bmdl.arc']
    assert preparation.extraction_archives(archives, None) == archives
    assert preparation.extraction_archives(archives, manifest) == archives + [preparation.WARP_ARCHIVE]
    assert preparation.extraction_archives(archives + [preparation.WARP_ARCHIVE], manifest) == archives + [preparation.WARP_ARCHIVE]
    model_row = {key: value for key, value in manifest['models'][0].items() if key != 'readied'}
    model_row['sha256'] = model_row.pop('source_sha256')
    assert preparation.select_models([model_row], manifest) == [model_row]
    bad = copy.deepcopy(manifest); bad['models'][0]['name_hex'] = b'ef_ktglow.bmd'.hex()
    rejects(lambda: preparation.selection_requests(bad), 'supported BMWR')
    bad = copy.deepcopy(manifest); del bad['models'][0]['readied']['warp_texture']
    rejects(lambda: preparation.selection_requests(bad), 'recipe fields')
    bad = copy.deepcopy(manifest); bad['models'][0]['readied']['warp_texture']['resource_id'] = 92
    rejects(lambda: preparation.selection_requests(bad), 'Always resource 93')
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        row = {**readied['warp_texture'], 'kind': 'texture', 'path': 'warp.bin'}
        row['sha256'] = row.pop('source_sha256')
        (root / 'warp.bin').write_bytes(warp)
        resolved = preparation.readied_resources(manifest, [row], root)
        assert resolved['/res/Object/Bmdl.arc', 15] == (readied, warp)
        rejects(lambda: preparation.readied_resources(manifest, [], root), 'Missing or stale')
        rejects(lambda: preparation.readied_resources(manifest, [{**row, 'entry': 94}], root), 'Missing or stale')
        rejects(lambda: preparation.readied_resources(manifest, [{**row, 'sha256': '0' * 64}], root), 'Missing or stale')
        (root / 'warp.bin').write_bytes(warp[:-1] + bytes((warp[-1] ^ 1,)))
        rejects(lambda: preparation.readied_resources(manifest, [row], root), 'Stale readied warp resource hash')


def check_conversion(decoder, output):
    """Reuse the parent's real decoder; no compiler or emulator is started here."""
    source, warp = fixture()
    readied = selection(source, warp)['models'][0]['readied']
    output.mkdir(mode=0o700)
    packed, receipt = preparation.convert_model(source, decoder, output, readied=readied, warp_resource=warp)
    assert packed[24:56] == assets.identity(source)
    assert packed[100:132].hex() == receipt['image_context_sha256']
    assert receipt['external_context_required']
    rows = receipt['images']['images']
    assert [row['id'] for row in rows] == [1, 2, 3, 4, 5]
    assert rows[2]['readied_append']['resource_sha256'] == hashlib.sha256(warp).hexdigest()
    assert rows[3]['kind'] == rows[4]['kind'] == 'mathematical'
    image_at, image_size = struct.unpack_from('<2I', packed, 64)
    image_bank = packed[image_at:image_at + image_size]
    assert image_bank[:8] == b'DUSKTX01' and struct.unpack_from('<2I', image_bank, 8)[0] == 3
    assert struct.unpack_from('<I', image_bank, 16)[0] == 5
    assert receipt['geometry']['source_sha256'] == hashlib.sha256(source).hexdigest()
    assert receipt['association']['source_correspondence'] == [
        {'part': index, 'source_joint': 0, 'source_material': 0, 'source_shape': 0, 'source_group': index}
        for index in range(2)]

    from test_native_image_identity import named_source
    role_source = named_source(source, [b'dummy', b'ordinary'], [0])
    role_scratch = output / 'role'; role_scratch.mkdir(mode=0o700)
    role_packed, role_receipt = preparation.convert_model(
        role_source, decoder, role_scratch, readied=readied, warp_resource=warp)
    assert role_receipt['readied_input']['image_table'][0]['image_sha256'] == \
        'f0503c86ef37cab32ae14f76ea478ba76ccd3e8d1f037de0aa9cc852a7717c7c'
    rows = role_receipt['images']['images']
    assert [row['id'] for row in rows] == [1, 2, 3, 4, 5]
    assert rows[0]['role'] == 'framebuffer-color'
    assert rows[3]['kind'] == rows[4]['kind'] == 'mathematical'
    image_at, _ = struct.unpack_from('<2I', role_packed, 64)
    assert struct.unpack_from('<8I', role_packed, image_at + 32) == (1, 0, 0, 4, 1, 192, 0, 0)

    role_only_scratch = output / 'role-only'; role_only_scratch.mkdir(mode=0o700)
    role_only, role_only_receipt = preparation.convert_model(role_source, decoder, role_only_scratch)
    assert role_only_receipt['external_context_required'] and struct.unpack_from('<I', role_only, 96)[0] == 1
    assert all('external_resource_sha256' not in row for row in role_only_receipt['images']['images'])
    role_only_at, _ = struct.unpack_from('<2I', role_only, 64)
    assert role_only[role_only_at:role_only_at + 12] == b'DUSKTX01\x03\x00\x00\x00'
    assert struct.unpack_from('<8I', role_only, role_only_at + 32) == (1, 0, 0, 4, 1, 160, 0, 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--decoder', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.decoder is not None and args.output is None:
        parser.error('--decoder requires a new --output directory')
    check_inputs()
    if args.decoder is not None:
        check_conversion(args.decoder.resolve(), args.output.resolve())
    print('PASS synthetic BMWR readied image order/context, inactive warp, helper shift, alpha/depth and selector refusals')


if __name__ == '__main__':
    main()
