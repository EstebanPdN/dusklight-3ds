#!/usr/bin/env python3
"""Build the asset-free actual native Renderer GPU probe; never launch a runner.

Usage: python3 Tools/3ds/build_native_scene_probe.py
Output: Build/native-scene-probe/native-scene-probe.3dsx
SD: sdmc:/3ds/dusklight-native-scene/run-<system-tick>/receipt.txt
A successful build is NOT GPU evidence. Parent labels the runner (latest Azahar
is emulated evidence, not physical PICA validation). No game assets are needed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
PLATFORM = ROOT / 'Source/platforms/3ds'
VARIANTS = ('scene', 'scene_rigid', 'scene_uv', 'scene_rigid_uv',
            'scene_projective', 'scene_rigid_projective')
FOG_GENERATOR_INPUTS = tuple(ROOT / 'Tools/3ds' / (name + '.py') for name in (
    'j3d_native_materials', 'j3d_native_geometry', 'j3d_material_source',
    'native_material_pack', 'native_mesh_pack'))


def fog_texture_embedding():
    """Embed the production offline mathematical helper, also for private builders."""
    sys.path.insert(0, str(ROOT / 'Tools/3ds'))
    try:
        from j3d_native_materials import fog_lookup_image
    finally:
        sys.path.pop(0)
    image = fog_lookup_image()
    data = bytes.fromhex(image['data_hex'])
    rows = [', '.join(f'0x{byte:02x}' for byte in data[start:start + 12])
            for start in range(0, len(data), 12)]
    source = ('alignas(4) extern const unsigned char native_scene_fog_texture[] = {\n'
              + ''.join('    ' + row + ',\n' for row in rows) + '};\n')
    provenance = {
        'generator': 'Tools/3ds/j3d_native_materials.py::fog_lookup_image',
        'arguments': {},
        'image': {key: value for key, value in image.items() if key != 'data_hex'},
        'byte_count': len(data), 'data_sha256': hashlib.sha256(data).hexdigest()}
    return source, provenance


def cull_fixture_embedding():
    """Compile synthetic source geometry and MAT3 states with production converters."""
    sys.path.insert(0, str(ROOT / 'Tools/3ds'))
    try:
        from test_j3d_native_geometry import model
        from test_j3d_native_materials import source_fixture
        from j3d_native_geometry import convert as convert_geometry
        from j3d_native_materials import convert as convert_material
    finally:
        sys.path.pop(0)

    source = model(primitive=0x90, groups=1)
    document, geometry, _ = convert_geometry(source)
    if len(document['meshes']) != 1 or document['meshes'][0]['indices'] != [0, 1, 2]:
        raise ValueError('source cull geometry fixture changed')
    banks = [('native_scene_cull_geometry', geometry)]
    provenance = {'geometry_source_sha256': hashlib.sha256(source).hexdigest(),
                  'geometry_bank_sha256': hashlib.sha256(geometry).hexdigest(),
                  'materials': []}
    for mode in range(4):
        source = source_fixture(mode)
        bank, result = convert_material(source, allow_normalized_arithmetic=True,
                                        specialize_initial_constants=True)
        row = {'source_cull': mode, 'source_sha256': hashlib.sha256(source).hexdigest()}
        if mode == 3:
            if bank is not None or result['materials'][0].get('reason') != 'Cull-all requires native draw visibility lowering':
                raise ValueError('source cull-all must be explicitly refused')
            row['status'] = 'refused'
        else:
            if bank is None:
                raise ValueError(f'source cull fixture {mode} was refused')
            row.update(status='compiled', bank_sha256=hashlib.sha256(bank).hexdigest())
            banks.append((f'native_scene_cull_material_{mode}', bank))
        provenance['materials'].append(row)
    embedded = []
    for symbol, data in banks:
        rows = [', '.join(f'0x{byte:02x}' for byte in data[start:start + 12])
                for start in range(0, len(data), 12)]
        embedded.append(f'alignas(4) extern const unsigned char {symbol}[] = {{\n'
                        + ''.join('    ' + row + ',\n' for row in rows)
                        + f'}};\nextern const unsigned int {symbol}_size = {len(data)};\n')
    return ''.join(embedded), provenance


def shader_metadata(data):
    """Read assembled DVLB metadata, not shader-source assertions or a GPU test."""
    if len(data) < 64 or data[:4] != b'DVLB':
        raise ValueError('Picasso output is not a DVLB')
    count, dvle = struct.unpack_from('<II', data, 4)
    dvlp = 8 + 4 * count
    if count != 1 or data[dvlp:dvlp + 4] != b'DVLP' or data[dvle:dvle + 4] != b'DVLE':
        raise ValueError('expected one native vertex-program DVLE')
    instructions = struct.unpack_from('<I', data, dvlp + 12)[0]
    uniform_offset, uniform_count, symbol_offset = struct.unpack_from('<III', data, dvle + 48)
    uniforms = {}
    for n in range(uniform_count):
        symbol, start, end = struct.unpack_from('<IHH', data, dvle + uniform_offset + 8 * n)
        at = dvle + symbol_offset + symbol
        name = data[at:data.index(b'\0', at)].decode('ascii')
        uniforms[name] = [start, end]
    # Keep observations separate from the repo's 512-slot admission expectation.
    return {'instruction_words': instructions, 'uniform_entries_raw_DVLE': uniforms,
            'repo_instruction_ceiling': 512, 'within_repo_ceiling': 0 < instructions <= 512,
            'physical_execution': 'unknown; binary metadata is not a hardware run'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--devkitpro', type=Path, default=Path(os.environ.get(
        'DEVKITPRO', '/home/sian/dkp-root/opt/devkitpro')))
    parser.add_argument('--devkitarm', type=Path)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'Build/native-scene-probe')
    args = parser.parse_args()
    sdk = args.devkitpro.expanduser().resolve()
    arm = (args.devkitarm or Path(os.environ.get('DEVKITARM', str(sdk / 'devkitARM')))).expanduser().resolve()
    build = args.build_dir.expanduser().resolve()
    compiler = arm / 'bin/arm-none-eabi-g++'
    picasso = sdk / 'tools/bin/picasso'
    package = sdk / 'tools/bin/3dsxtool'
    specs = arm / 'arm-none-eabi/lib/3dsx.specs'
    for path in (compiler, picasso, package, specs, sdk / 'libctru/include/citro3d.h',
                 sdk / 'libctru/lib/libcitro3d.a', sdk / 'libctru/lib/libctru.a'):
        if not path.is_file():
            parser.error(f'installed SDK prerequisite missing: {path}')
    build.mkdir(parents=True, exist_ok=True)
    commands = []
    sources = [PLATFORM / 'tests/native_scene_gpu_probe.cpp',
               PLATFORM / 'native/renderer.cpp', PLATFORM / 'native/contracts.cpp',
               PLATFORM / 'native/camera.cpp', PLATFORM / 'native/pack.cpp',
               PLATFORM / 'native/material_pack.cpp', PLATFORM / 'runtime/gx_pica_math.cpp',
               PLATFORM / 'runtime/gx_raster_color.cpp']
    shared = PLATFORM / 'native/scene_shared.pica'
    shader_sources = [PLATFORM / f'native/{name}.v.pica' for name in VARIANTS]
    # Reference declarations include GX headers, but no decoder implementation
    # is linked. --gc-sections drops unused register-plan functions.
    headers = [PLATFORM / ('native/' + name + '.hpp')
               for name in ('renderer', 'contracts', 'color_lookup', 'pack', 'storage',
                            'camera', 'material_pack')]
    headers += [PLATFORM / ('runtime/' + name + '.hpp') for name in (
        'gx_raster_color', 'gx_vertex_transform', 'gx_fog', 'gx_commands',
        'gx_vertex_decode', 'gx_copy_setup', 'gx_batch_cache',
        'gx_material', 'gx_pica_material', 'gx_pica_math', 'gx_state', 'gx_target_extent')]
    fixture_inputs = [ROOT / 'Tools/3ds/test_j3d_native_geometry.py',
                      ROOT / 'Tools/3ds/test_j3d_native_materials.py']
    aurora = ROOT / 'Source/extern/aurora/include'
    inputs = sorted(set([Path(__file__).resolve(), *sources, shared, *shader_sources,
                         *headers, *FOG_GENERATOR_INPUTS, *fixture_inputs, aurora / 'aurora/dl.hpp',
                         *aurora.joinpath('dolphin').rglob('*.h')]))
    hashes = {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
              for path in inputs}
    receipt = {'status': 'building-not-gpu-verified', 'commands': commands,
               'devkitpro': str(sdk), 'devkitarm': str(arm), 'inputs_sha256': hashes,
               'capture_root': 'sdmc:/3ds/dusklight-native-scene/run-<system-tick>',
               'raw_format': '240x400 linear ABGR8, flip_vert=0, 384000 bytes',
               'lcd_format': 'actual top display 240x400 BGR8, 288000 bytes',
               'expected_cases': 297,
               'expected_cull_cases': 12,
               'cull_reference': 'native Renderer with production compatibility picaCull and picaProjection as proxy; not full GX backend',
               'mesh_storage': 'two-range resident bank; range1 selected; distinct decoy vertex/index base and higher palette requirement',
               'reference': 'production CPU evaluateRasterColor/evaluateVertexFog, direct plans, no command decoder; directional N.L analytic',
               'expected_limits': {'uniform_float_rows': 'c0..c89; static constants occupy remaining c90..c95 only',
                                   'temporary_registers': 'r0..r15 (16)',
                                   'call_chain': 'main -> colors -> channel (depth 2); physical execution unverified',
                                   'instruction_words': 'repo ceiling 512; actual assembled counts recorded per variant',
                                   'pixel_tolerance_bytes': '3; LUT products 4; zero over-tolerance pixels',
                                   'minimum_interior_pixels': 20001,
                                   'minimum_drawn_pixels': 25001,
                                   'restoration': 'exact whole-image equality after every case',
                                   'physical_hardware': 'unknown; no emulator result proves physical PICA limits'}}
    report = build / 'build-receipt.json'

    def persist():
        report.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')

    def run(command):
        command = [str(part) for part in command]
        commands.append(command)
        persist()
        print('+ ' + shlex.join(command), flush=True)
        subprocess.run(command, cwd=ROOT, check=True)

    persist()
    try:
        binaries = []
        embedded = []
        metadata = {}
        for name, source in zip(VARIANTS, shader_sources):
            binary = build / f'{name}.shbin'
            # REQUIRED second Picasso input for every variant: the real shared
            # production implementation, not a probe-specific shader program.
            run([picasso, '-o', binary, source, shared])
            data = binary.read_bytes()
            metadata[name] = shader_metadata(data)
            if not metadata[name]['within_repo_ceiling']:
                raise ValueError(f'{name}: {metadata[name]["instruction_words"]} instruction words exceeds repo ceiling 512')
            symbol = 'native_' + name + '_shader'
            padded = data + b'\0' * (-len(data) % 4)
            rows = [', '.join(f'0x{byte:02x}' for byte in padded[start:start + 12])
                    for start in range(0, len(padded), 12)]
            # Renderer declarations have C++ linkage. extern const DEFINITIONS
            # are required: plain namespace const would have internal linkage.
            embedded.append(f'alignas(4) extern const unsigned char {symbol}[] = {{\n'
                            + ''.join('    ' + row + ',\n' for row in rows)
                            + f'}};\nextern const unsigned int {symbol}_size = {len(data)};\n')
            binaries.append(binary)
        receipt['assembled_shaders'] = metadata
        fog_source, receipt['synthetic_fog'] = fog_texture_embedding()
        embedded.append(fog_source)
        cull_source, receipt['synthetic_cull_fixtures'] = cull_fixture_embedding()
        embedded.append(cull_source)
        embedded_source = build / 'native_scene_shaders.cpp'
        embedded_source.write_text('/* Generated production Picasso and synthetic fixture bytes. */\n'
                                   + ''.join(embedded), encoding='utf-8')
        elf = build / 'native-scene-probe.elf'
        output = build / 'native-scene-probe.3dsx'
        flags = ['-march=armv6k', '-mtune=mpcore', '-mfloat-abi=hard', '-mfpu=vfp',
                 '-mtp=soft', '-mword-relocations', '-fno-fast-math', '-ffp-contract=off',
                 '-ffunction-sections', '-fdata-sections', '-D_GNU_SOURCE',
                 '-D__3DS__', '-D_3DS', '-DARM11', '-DDUSK_TARGET_3DS=1', '-DTARGET_PC=1',
                 '-O2', '-g', '-Wall', '-Wextra', '-std=gnu++20',
                 '-I' + str(sdk / 'libctru/include'),
                 '-I' + str(ROOT / 'Source/extern/aurora/include')]
        run([compiler, *flags, *sources, embedded_source, '-specs=' + str(specs),
             '-Wl,--gc-sections', '-Wl,-Map,' + str(build / 'native-scene-probe.map'),
             '-L' + str(sdk / 'libctru/lib'), '-lcitro3d', '-lctru', '-lm', '-o', elf])
        run([package, elf, output])
        if any(hashlib.sha256(path.read_bytes()).hexdigest() != hashes[str(path.relative_to(ROOT))]
               for path in inputs):
            raise RuntimeError('source inputs changed during build; rebuild for coherent evidence')
        receipt.update(status='built-not-gpu-verified', outputs_sha256={
            path.name: hashlib.sha256(path.read_bytes()).hexdigest()
            for path in [*binaries, embedded_source, elf, output]})
        print(f'Built: {output}\nGPU correctness: NOT RUN\nCaptures: {receipt["capture_root"]}', flush=True)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        receipt.update(status='build-failed', error=str(error))
        raise
    finally:
        persist()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
