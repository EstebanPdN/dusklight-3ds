#!/usr/bin/env python3
"""Private offline ISO/zelda.twilight -> complete, source-identified native banks.

All requested archives must extract and every selected model must convert, or
publication is refused. Without a manifest every discovered model is selected.
A failed attempt retains its private .partial directory and receipt.
Native banks are parameter templates, NOT rendering/gameplay/30 FPS acceptance.
The selected policy uses normalized native arithmetic and modeled SDK samplers;
source integer equivalence, scene producers and external runtime context remain
unverified. No input is modified, no missing bank or source part is synthesized.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import zlib

import j3d_native_geometry as geometry
import j3d_native_materials as materials
import j3d_native_model as model
import j3d_native_textures as textures
from j3d_material_source import input_declaration
import native_asset_pack
import twilight_pack
from run_azahar_smoke import card_private_parent

ROOT = Path(__file__).resolve().parents[2]
MAX_ARCHIVES = 16
MAX_RESOURCES = 4096
MAX_MODELS = 256
MAX_NATIVE_BYTES = 512 << 20
MAX_SELECTION_BYTES = 256 << 10
SELECTOR_FIELDS = {'archive', 'entry', 'resource_id', 'name_hex', 'source_sha256'}
READIED_MODELS = {13: b'al_face.bmd', 15: b'bl.bmd', 16: b'bl_hands.bmd', 17: b'bl_head.bmd'}
WARP_ARCHIVE = '/res/Object/Always.arc'


class PreparationError(ValueError):
    def __init__(self, message, receipt_path):
        super().__init__(message)
        self.receipt_path = receipt_path


def sha(data):
    return hashlib.sha256(data).hexdigest()


def write_new(path, payload):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'wb') as stream:
        twilight_pack.require(stream.write(payload) == len(payload), 'Short preparation write')
        stream.flush()
        os.fsync(stream.fileno())


def write_json(path, data):
    write_new(path, (json.dumps(data, indent=2, ensure_ascii=True) + '\n').encode())


def build_extractor(root, output):
    source = root / 'Source/platforms/3ds'
    command = ['clang++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I' + str(source),
               str(root / 'Tools/3ds/extract_native_inputs.cpp'),
               str(source / 'runtime/disc.cpp'), str(source / 'runtime/resource_archive.cpp'),
               '-o', str(output)]
    result = subprocess.run(command, text=True, capture_output=True, timeout=120)
    write_new(output.with_suffix('.build.log'), (result.stdout + result.stderr).encode())
    result.check_returncode()
    return command


def publish_directory(parent, staging, output):
    """Atomic no-replace rename; even an empty concurrent destination wins."""
    libc = ctypes.CDLL(None, use_errno=True)
    if sys.platform == 'darwin':
        rename, flag = libc.renameatx_np, 4  # RENAME_EXCL
    elif sys.platform.startswith('linux'):
        rename, flag = libc.renameat2, 1  # RENAME_NOREPLACE
    else:
        raise ValueError('Atomic directory no-replace publication requires Linux or macOS')
    rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    rename.restype = ctypes.c_int
    if rename(parent, os.fsencode(staging), parent, os.fsencode(output), flag):
        code = ctypes.get_errno()
        raise OSError(code, os.strerror(code), output)


def verify_asset(packed, source, banks, shapes, external_context_required, image_context):
    """Read back exact source binding, bank extents/bytes, shape ownership, CRC."""
    require = twilight_pack.require
    require(160 <= len(packed) <= native_asset_pack.MAX_BYTES and packed[:8] == b'DUSKNA02',
            'Native asset signature/extent')
    version, size, source_size, count = struct.unpack_from('<4I', packed, 8)
    require((version, size, source_size, count) == (2, len(packed), len(source), len(shapes)),
            'Native asset identity extent/count')
    require(packed[24:56] == native_asset_pack.identity(source), 'Native asset source SHA-256 mismatch')
    require(packed[100:132] == image_context, 'Native asset image context mismatch')
    require(struct.unpack_from('<I', packed, 96)[0] == int(external_context_required) and not any(packed[132:156]),
            'Native asset context/reserved flags')
    require(struct.unpack_from('<I', packed, 156)[0] == zlib.crc32(packed[:156] + packed[160:]),
            'Native asset CRC mismatch')
    cursor = 160
    expected = [*banks, b''.join(struct.pack('<I', s) for s in shapes)]
    for index, bank in enumerate(expected):
        offset, length = struct.unpack_from('<2I', packed, 56 + index * 8)
        require(offset == cursor and length == len(bank) and length <= len(packed) - offset,
                'Native asset bank extent mismatch')
        require(packed[offset:offset + length] == bank, 'Native asset bank readback mismatch')
        cursor += length
        end = (cursor + 15) & ~15
        require(end <= len(packed) and not any(packed[cursor:end]), 'Native asset bank padding')
        cursor = end
    require(cursor == len(packed), 'Native asset trailing bytes')


def convert_model(source, decoder, scratch, external_images=None, *, readied=None, warp_resource=None):
    """Return one verified real DNA plus complete conversion provenance."""
    native_asset_pack.identity(source)
    external_images = native_asset_pack.resolve_external_images(source, external_images or {})
    declaration = input_declaration(
        source, external_images, loader_recipe=readied['loader_recipe'] if readied else None,
        warp_resource=warp_resource, warp_selector=readied['warp_texture'] if readied else None)
    image_context = native_asset_pack.image_context(source, declaration=declaration)
    if readied:
        declaration['receipt']['image_context_sha256'] = image_context.hex()
        declaration['receipt']['image_table'] = [
            {'texture_index': index, 'native_image_id': index + 1,
             'image_sha256': native_asset_pack.source_image_identity(bti, metadata).hex()}
            for index, bti, metadata in declaration['images']]
    plan, native_plan, association = model.assemble(
        source, external_images, allow_normalized_arithmetic=True, allow_sdk_sampler_model=True,
        declaration=declaration)
    _, meshes, mesh_receipt = geometry.convert(source)
    fragments, material_receipt = materials.convert(
        source, allow_normalized_arithmetic=True, preserve_color_parameters=True, allow_native_temporaries=True,
        declaration=declaration)
    for row in material_receipt['materials']:
        twilight_pack.require(row['status'] != 'refused',
                              f"Source material {row['id']} refused: {row.get('reason', 'missing native program')}")
    images, image_receipt = textures.convert(source, decoder, scratch, external_images,
                                             synthetic_images=association['synthetic_images'], declaration=declaration)
    twilight_pack.require(fragments is not None, 'No converted material bank')
    tree = model.hierarchy(geometry.sections(source))
    groups = {}
    for group in mesh_receipt['meshes']:
        groups.setdefault(group['source_shape'], []).append(group)
    expected = []
    for joint, material, shape in tree['entries']:
        twilight_pack.require(shape in groups, f'Referenced source shape {shape} has no converted geometry')
        for group in groups[shape]:
            expected.append({'part': len(expected), 'source_joint': joint, 'source_material': material,
                             'source_shape': shape, 'source_group': group['source_group']})
    twilight_pack.require(expected == association['source_correspondence'] and len(expected) == len(plan['parts']),
                          'Incomplete source model part correspondence')
    source_hash = sha(source)
    twilight_pack.require(all(r['source_sha256'] == source_hash for r in
                              [association, mesh_receipt, material_receipt, image_receipt]),
                          'Conversion source provenance mismatch')
    shapes = [row['source_shape'] for row in expected]
    context = any('external_resource_sha256' in row or row.get('role') == textures.FRAMEBUFFER_COLOR
                  for row in image_receipt['images'])
    banks = (meshes, images if images is not None else b'', fragments, native_plan)
    packed = native_asset_pack.compile_asset(source, *banks, shapes, image_context=image_context, external_context_required=context)
    verify_asset(packed, source, banks, shapes, context, image_context)
    return packed, {'source_sha256': source_hash, 'native_sha256': sha(packed), 'native_bytes': len(packed),
                    'image_context_sha256': image_context.hex(), 'native_filename': native_asset_pack.filename(source, image_context),
                    'readied_input': declaration['receipt'] if readied else None,
                    'shapes': shapes, 'requirements': plan['requirements'], 'external_context_required': context,
                    'runtime_external_context_supported': True,
                    'rendering_ready': False, 'source_integer_equivalence': False,
                    'association': association, 'geometry': mesh_receipt, 'materials': material_receipt,
                    'images': image_receipt}


def extraction_inventory(text, folder, archives, read, files):
    """Validate the numeric extraction protocol against original disc extents."""
    require = twilight_pack.require
    inventory, archive_rows = [], []
    lines = text.splitlines()
    require(bool(lines), 'Missing extractor metadata')
    disc = lines.pop(0).split('\t')
    require(len(disc) == 4 and disc[0] == 'disc' and disc[1].encode() == read(0, 6) and disc[2] == '0' and
            disc[3] in ('iso', 'bundle'), 'Extractor disc identity mismatch')
    seen = set()
    for line in lines:
        fields = line.split('\t')
        if fields[0] == 'archive':
            require(len(fields) == 6, 'Invalid extractor archive metadata')
            ordinal, entry, offset, stored, expanded = map(int, fields[1:])
            require(ordinal == len(archive_rows) and ordinal < len(archives), 'Extractor archive order')
            matches = [f for f in files if textures.archive_key(f[1].decode('ascii', 'backslashreplace')) ==
                       textures.archive_key(archives[ordinal])]
            require(len(matches) == 1 and matches[0][0] == entry and matches[0][2:] == (offset, stored),
                    'Extractor archive source extent mismatch')
            path = folder / f'archive-{ordinal:04}.arc'
            raw = read(offset, stored)
            payload = path.read_bytes()
            require(64 <= expanded <= 64 << 20 and len(payload) == expanded, 'Expanded archive extent')
            if not raw.startswith(b'Yaz0'):
                require(raw == payload, 'Original archive extraction mismatch')
            archive_rows.append({'archive': archives[ordinal], 'fst_entry': entry, 'disc_offset': offset,
                                 'source_bytes': stored, 'source_sha256': sha(raw), 'expanded_bytes': expanded,
                                 'expanded_sha256': sha(payload), 'path': 'inputs/' + path.name})
        elif fields[0] == 'entry':
            require(len(fields) == 10 and len(inventory) < MAX_RESOURCES, 'Invalid/excess extractor entries')
            ordinal, entry, identifier, flags, offset, stored, expanded = map(int, fields[1:8])
            require(0 <= ordinal < len(archive_rows) and 0 <= entry < 65536 and (ordinal, entry) not in seen,
                    'Extractor entry index/duplicate')
            require(fields[8] in ('model', 'texture') and 0 <= identifier <= 65535 and 0 <= flags <= 255,
                    'Extractor entry type/flags')
            name = bytes.fromhex(fields[9])
            require(0 < len(name) <= 255 and all(c >= 32 and c not in (127, 47, 92) for c in name),
                    'Extractor entry basename')
            archive = folder / f'archive-{ordinal:04}.arc'
            require(twilight_pack.extent(archive_rows[ordinal]['expanded_bytes'], offset, stored) and
                    0 <= expanded <= 16 << 20, 'Extractor entry extent')
            with archive.open('rb') as stream:
                encoded = twilight_pack.exact(stream, offset, stored)
            path = folder / f'archive-{ordinal:04}-entry-{entry:06}.bin'
            payload = path.read_bytes()
            require(len(payload) == expanded, 'Extracted resource extent mismatch')
            if not encoded.startswith(b'Yaz0'):
                require(encoded == payload, 'Original resource extraction mismatch')
            inventory.append({'archive': archives[ordinal], 'archive_index': ordinal, 'entry': entry,
                              'resource_id': identifier, 'flags': flags, 'archive_offset': offset,
                              'encoded_bytes': stored, 'encoded_sha256': sha(encoded), 'bytes': expanded,
                              'sha256': sha(payload), 'kind': fields[8], 'name_hex': name.hex(),
                              'name': name.decode('shift_jis', 'backslashreplace'), 'path': 'inputs/' + path.name})
            seen.add((ordinal, entry))
        else:
            raise ValueError('Unknown extractor metadata record')
    require(len(archive_rows) == len(archives), 'Incomplete archive extraction')
    return archive_rows, inventory


def external_resources(inventory, staging):
    """Actual original basenames only; conflicting selected resources stay errors."""
    candidates = {}
    for row in inventory:
        if row['kind'] == 'texture':
            candidates.setdefault(textures.archive_key(row['name']), []).append(row)
    images = {}
    for key, rows in candidates.items():
        if len(rows) != 1:
            continue
        row = rows[0]
        payload = (staging / row['path']).read_bytes()
        twilight_pack.require(sha(payload) == row['sha256'], 'External BTI provenance mismatch')
        images[key] = payload
    return images, {key: rows for key, rows in candidates.items() if len(rows) != 1}


def model_selector(row):
    """Exact original decoded resource identity; name_hex avoids lossy text names."""
    return {'archive': row['archive'], 'entry': row['entry'], 'resource_id': row['resource_id'],
            'name_hex': row['name_hex'], 'source_sha256': row['sha256']}


def validate_selector(selector):
    require = twilight_pack.require
    require(isinstance(selector, dict) and set(selector) == SELECTOR_FIELDS, 'Native model selector fields')
    require(type(selector['archive']) is str and type(selector['entry']) is int and
            0 <= selector['entry'] < 65536 and type(selector['resource_id']) is int and
            0 <= selector['resource_id'] < 65536, 'Native model selector archive/entry/resource')
    for field, minimum, maximum in [('name_hex', 2, 510), ('source_sha256', 64, 64)]:
        value = selector[field]
        require(type(value) is str and minimum <= len(value) <= maximum and len(value) % 2 == 0 and
                all(c in '0123456789abcdef' for c in value), 'Native model selector ' + field)


def selection_requests(selection):
    """Schema 2 adds explicit readied recipes; schema 1 keeps its exact contract."""
    require = twilight_pack.require
    if selection is None:
        return []
    require(isinstance(selection, dict) and set(selection) == {'schema', 'models'} and
            selection['schema'] in ('dusklight-native-model-selection/1', 'dusklight-native-model-selection/2'),
            'Native model-selection schema')
    requested = selection['models']
    require(isinstance(requested, list) and 1 <= len(requested) <= MAX_MODELS,
            'Model selection requires 1..256 exact selectors')
    seen = set()
    for selector in requested:
        require(isinstance(selector, dict), 'Native model selector fields')
        fields = set(selector)
        require(fields == SELECTOR_FIELDS or
                selection['schema'].endswith('/2') and fields == SELECTOR_FIELDS | {'readied'},
                'Native model selector fields')
        validate_selector({key: selector[key] for key in SELECTOR_FIELDS})
        key = selector['archive'], selector['entry']
        require(key not in seen, 'Duplicate native model selector')
        seen.add(key)
        if 'readied' not in selector:
            continue
        readied = selector['readied']
        require(isinstance(readied, dict) and set(readied) == {'loader_recipe', 'warp_texture'} and
                readied['loader_recipe'] == 'bmwr-warp-off', 'Readied loader recipe fields/value')
        require(selector['archive'] == '/res/Object/Bmdl.arc' and
                READIED_MODELS.get(selector['resource_id']) == bytes.fromhex(selector['name_hex']),
                'Readied recipe requires an explicitly supported BMWR Link resource')
        warp = readied['warp_texture']
        validate_selector(warp)
        require(warp['archive'] == WARP_ARCHIVE and warp['resource_id'] == 93 and
                warp['name_hex'] == b'warp_tex.bti'.hex(), 'Readied warp selector must identify Always resource 93 warp_tex.bti')
    return requested


def select_models(models, selection):
    """Validate the whole manifest before admitting any model, keeping source order."""
    if selection is None:
        return models
    discovered = {(row['archive'], row['entry']): row for row in models}
    requested = selection_requests(selection)
    seen = set()
    for selector in requested:
        key = selector['archive'], selector['entry']
        twilight_pack.require(key in discovered and
                              {field: selector[field] for field in SELECTOR_FIELDS} == model_selector(discovered[key]),
                              'Missing or stale native model selector: ' + repr(key))
        seen.add(key)
    return [row for row in models if (row['archive'], row['entry']) in seen]


def extraction_archives(archives, selection):
    """Only explicit recipes request the loader's Always resource archive."""
    result = list(archives)
    if any('readied' in selector for selector in selection_requests(selection)) and not any(
            textures.archive_key(path) == textures.archive_key(WARP_ARCHIVE) for path in result):
        result.append(WARP_ARCHIVE)
    return result


def readied_resources(selection, inventory, staging):
    """Authenticate all selected warp resources before any model conversion."""
    require = twilight_pack.require
    result = {}
    for selector in selection_requests(selection):
        if 'readied' not in selector:
            continue
        readied = selector['readied']
        warp = readied['warp_texture']
        matches = [row for row in inventory if row['kind'] == 'texture' and model_selector(row) == warp]
        require(len(matches) == 1, 'Missing or stale readied warp selector')
        payload = (staging / matches[0]['path']).read_bytes()
        require(sha(payload) == warp['source_sha256'], 'Stale readied warp resource hash')
        result[selector['archive'], selector['entry']] = (readied, payload)
    return result


def read_model_selection(path):
    with Path(path).open('rb') as stream:
        before = os.fstat(stream.fileno())
        payload = stream.read(MAX_SELECTION_BYTES + 1)
        after = os.fstat(stream.fileno())
    twilight_pack.require(len(payload) <= MAX_SELECTION_BYTES, 'Model-selection manifest exceeds256KiB')
    twilight_pack.require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) ==
                          (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns),
                          'Model-selection manifest changed during read')
    selection = json.loads(payload)
    twilight_pack.require(isinstance(selection, dict), 'Native model-selection schema')
    return selection


def prepare(input_path, archives, output, *, model_selection=None):
    require = twilight_pack.require
    archives = extraction_archives(archives, model_selection)
    require(1 <= len(archives) <= MAX_ARCHIVES and len({textures.archive_key(a) for a in archives}) == len(archives),
            'Select 1..16 distinct archive paths')
    for archive in archives:
        parts = archive.split('/')
        require(archive.startswith('/res/') and len(archive.encode('ascii')) <= 1023 and len(parts) <= 65 and
                all(p and p not in ('.', '..') and len(p) <= 255 and all(32 <= ord(c) < 127 and c != '\\' for c in p)
                    for p in parts[1:]), 'Invalid /res/ archive path')
    source_path = Path(input_path).resolve(strict=True)
    output = Path(os.path.abspath(output))
    receipt = {'schema': 'dusklight-native-preparation/1', 'status': 'preparing', 'archives_requested': archives,
               'rendering_ready': False, 'fps_verified': False, 'source_integer_equivalence': False,
               'conversion_policy': 'normalized native arithmetic; modeled SDK samplers; parameterized colors; no scene producers',
               'runtime_external_context_supported': True, 'models': [],
               'model_selection': model_selection, 'selection_mode': 'all' if model_selection is None else 'manifest',
               'models_discovered': 0, 'models_selected': 0}
    with card_private_parent(output, create=True) as (parent, name, absolute):
        require(not os.path.lexists(absolute), 'Output already exists; refusing overwrite')
        require(source_path != absolute, 'Input and output must differ')
        staging = Path(tempfile.mkdtemp(prefix='.' + name + '-', suffix='.partial', dir=absolute.parent))
        stage_fd = os.open(staging, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            source_fd = os.open(source_path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
            with os.fdopen(source_fd, 'rb') as src:
                before = twilight_pack.identity(src)
                require(0x440 <= before[2] <= 0x7fffffff, 'Input size exceeds bounded disc envelope')
                source_hash = twilight_pack.digest_region(src, 0, before[2]).hex()
                if twilight_pack.exact(src, 0, 8) == twilight_pack.MAGIC:
                    bundle = twilight_pack.Bundle(src)
                    for _, po, size, digest in bundle.ranges:
                        require(twilight_pack.digest_region(src, po, size) == digest, 'Bundle payload SHA-256 mismatch')
                    read, files = bundle.read, bundle.files
                    original_hash = bundle.source_hash.hex()
                    source_kind = 'bundle'
                else:
                    read = lambda offset, size: twilight_pack.exact(src, offset, size)
                    _, files = twilight_pack.layout(read, before[2])
                    original_hash, source_kind = source_hash, 'iso'
                header = read(0, 8)
                receipt['source'] = {'sha256': source_hash, 'bytes': before[2], 'kind': source_kind,
                                     'game_id': header[:6].decode('ascii'), 'disc_number': header[6], 'revision': header[7],
                                     'original_iso_sha256': original_hash,
                                     'original_iso_hash_authority': 'input bytes' if source_kind == 'iso' else 'bundle header claim, not independently reconstructed ISO'}
                tools = staging / 'tools'
                tools.mkdir(mode=0o700)
                extractor, decoder = tools / 'extract-native-inputs', tools / 'decode-native-texture'
                receipt['extractor_build'] = build_extractor(ROOT, extractor)
                receipt['decoder_build'] = textures.build_decoder(ROOT, decoder)
                paths = [Path(module.__file__) for key, module in sys.modules.copy().items()
                         if key.startswith(('j3d_', 'native_')) and getattr(module, '__file__', None) and
                         Path(module.__file__).parent == ROOT / 'Tools/3ds']
                paths += [Path(__file__), ROOT / 'Tools/3ds/extract_native_inputs.cpp',
                          ROOT / 'Tools/3ds/compile_native_texture.cpp']
                paths += [ROOT / ('Source/platforms/3ds/runtime/' + p) for p in
                          ['disc.cpp', 'disc.hpp', 'resource_archive.cpp', 'resource_archive.hpp', 'texture_gx.cpp', 'texture_gx.hpp']]
                receipt['tool_sha256'] = {str(p.relative_to(ROOT)): sha(p.read_bytes()) for p in sorted(set(paths))}
                inputs = staging / 'inputs'
                inputs.mkdir(mode=0o700)
                inputs_fd = os.open(inputs, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
                try:
                    result = subprocess.run([str(extractor), f'/dev/fd/{src.fileno()}', str(inputs_fd), *archives],
                                            pass_fds=(src.fileno(), inputs_fd), text=True, capture_output=True, timeout=180)
                finally:
                    os.close(inputs_fd)
                write_new(tools / 'extraction.log', (result.stdout + result.stderr).encode())
                require(result.returncode == 0, 'Native extraction refused: ' + result.stderr.strip())
                receipt['archives'], receipt['inputs'] = extraction_inventory(result.stdout, inputs, archives, read, files)
                models = [r for r in receipt['inputs'] if r['kind'] == 'model']
                receipt['models_discovered'] = len(models)
                receipt['models'] = [{**model_selector(row), 'name': row['name'], 'selected': False,
                                      'status': 'notselected'} for row in models]
                require(1 <= len(models) <= MAX_MODELS, 'Discovery requires 1..256 original models')
                selected = select_models(models, model_selection)
                readied_inputs = readied_resources(model_selection, receipt['inputs'], staging)
                selected_keys = {(row['archive'], row['entry']) for row in selected}
                receipt['models_selected'] = len(selected)
                for record in receipt['models']:
                    if (record['archive'], record['entry']) in selected_keys:
                        record.update(selected=True, status='pending')
                external, ambiguous = external_resources(receipt['inputs'], staging)
                total_native = 0
                for row, record in zip(models, receipt['models']):
                    if not record['selected']:
                        continue
                    try:
                        source = (staging / row['path']).read_bytes()
                        require(sha(source) == row['sha256'], 'Extracted model changed before conversion')
                        with tempfile.TemporaryDirectory(prefix='convert-', dir=staging) as scratch:
                            readied, warp_resource = readied_inputs.get((row['archive'], row['entry']), (None, None))
                            packed, converted = convert_model(
                                source, decoder, Path(scratch), external, readied=readied, warp_resource=warp_resource)
                        total_native += len(packed)
                        require(total_native <= MAX_NATIVE_BYTES, 'Total native bank byte budget exceeded')
                        target = staging / converted['native_filename']
                        if target.exists():
                            require(target.read_bytes() == packed, 'Same-model/image-context native asset collision')
                        else:
                            write_new(target, packed)
                        require(sha(target.read_bytes()) == converted['native_sha256'], 'Native asset disk readback mismatch')
                        record.update(status='converted', path=target.name, **converted)
                    except (ValueError, struct.error, subprocess.SubprocessError) as error:
                        reason = str(error)
                        prefix = 'Named external texture requires native resource binding: '
                        if reason.startswith(prefix) and textures.archive_key(reason[len(prefix):] + '.bti') in ambiguous:
                            reason = 'Ambiguous external texture across selected archives: ' + reason[len(prefix):]
                        record.update(status='refused', reason=reason)
                require(twilight_pack.digest_region(src, 0, before[2]).hex() == source_hash and twilight_pack.identity(src) == before,
                        'Source changed during preparation')
                current = source_path.stat()
                require((current.st_dev, current.st_ino, current.st_size, current.st_mtime_ns, current.st_ctime_ns) == before,
                        'Source path changed during preparation')
                receipt['source_unchanged'] = True
                require(all(r['status'] == 'converted' for r in receipt['models'] if r['selected']),
                        'One or more original models are unsupported; no preparation published')
                for row in receipt['inputs']:
                    require(sha((staging / row['path']).read_bytes()) == row['sha256'], 'Extracted input changed before publication')
                receipt['status'] = 'complete'
                receipt['scope'] = 'All selected models converted with complete referenced parts; native parameter banks only, not renderer or gameplay acceptance.'
                write_json(staging / 'receipt.json', receipt)
                os.fsync(stage_fd)
                require(os.stat(staging, follow_symlinks=False).st_ino == os.fstat(stage_fd).st_ino, 'Staging directory changed')
                publish_directory(parent, staging.name, name)
                return receipt
        except BaseException as error:
            receipt['status'] = 'refused'
            receipt['reason'] = str(error)
            # Never replace an already-written receipt, including publication races.
            failure = staging / 'failure.json'
            write_json(failure, receipt)
            raise PreparationError(str(error), failure) from error
        finally:
            os.close(stage_fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--archive', action='append', required=True, help='Exact /res/... archive; repeat to include external BTIs')
    parser.add_argument('--model-selection', type=Path,
                        help='Exact dusklight-native-model-selection/1 or /2 JSON manifest; /2 supports explicit BMWR warp-off recipes; default: all discovered models')
    parser.add_argument('--output', type=Path, required=True, help='New ignored private Build/Diagnostics/Saves directory')
    args = parser.parse_args()
    try:
        selection = read_model_selection(args.model_selection) if args.model_selection is not None else None
        receipt = prepare(args.input, args.archive, args.output, model_selection=selection)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(json.dumps({'status': 'refused', 'reason': str(error),
                          'receipt': str(error.receipt_path) if isinstance(error, PreparationError) else None}), file=sys.stderr)
        return 1
    print(json.dumps({'status': receipt['status'], 'output': str(args.output.absolute()),
                      'models': len(receipt['models']),
                      'models_discovered': receipt['models_discovered'], 'models_selected': receipt['models_selected'],
                      'rendering_ready': False}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
