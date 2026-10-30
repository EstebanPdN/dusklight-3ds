#!/usr/bin/env python3
"""Build the asset-free Citro3D RGBA lookup probe; never launch an emulator.

Run the resulting .3dsx on hardware or in an already isolated emulator SD tree.
Evidence: sdmc:/3ds/dusklight-color-lookup/run-<tick>/{receipt.txt,metrics.csv,...}.
A build is not a correctness result: only a completed GPU receipt can pass.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]
PLATFORM = ROOT / 'Source/platforms/3ds'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--devkitpro', type=Path, default=Path(os.environ.get(
        'DEVKITPRO', '/opt/devkitpro')))
    parser.add_argument('--devkitarm', type=Path)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'Build/color-lookup-probe')
    args = parser.parse_args()
    sdk = args.devkitpro.expanduser().resolve()
    arm = (args.devkitarm or Path(os.environ.get('DEVKITARM', str(sdk / 'devkitARM')))).expanduser().resolve()
    build = args.build_dir.expanduser().resolve()
    compiler = arm / 'bin/arm-none-eabi-g++'
    c_compiler = arm / 'bin/arm-none-eabi-gcc'
    picasso = sdk / 'tools/bin/picasso'
    package = sdk / 'tools/bin/3dsxtool'
    specs = arm / 'arm-none-eabi/lib/3dsx.specs'
    for path in (compiler, c_compiler, picasso, package, specs,
                 sdk / 'libctru/include/citro3d.h', sdk / 'libctru/lib/libcitro3d.a',
                 sdk / 'libctru/lib/libctru.a'):
        if not path.is_file():
            parser.error(f'installed SDK prerequisite missing: {path}')
    build.mkdir(parents=True, exist_ok=True)
    shader_source = PLATFORM / 'gx_color.v.pica'
    shader_binary = build / 'gx_color.shbin'
    embedded_source = build / 'color_lookup_shader.c'
    embedded_object = build / 'color_lookup_shader.o'
    elf = build / 'color-lookup-probe.elf'
    output = build / 'color-lookup-probe.3dsx'
    commands = []

    def run(command):
        command = [str(part) for part in command]
        commands.append(command)
        print('+ ' + shlex.join(command), flush=True)
        subprocess.run(command, cwd=ROOT, check=True)

    # Assemble the actual source shader, not a prebuilt blob or substitute shader.
    run([picasso, '-o', shader_binary, shader_source])
    shader = shader_binary.read_bytes()
    if not shader:
        raise RuntimeError('Picasso emitted an empty shader')
    padded = shader + b'\0' * (-len(shader) % 4)
    rows = [', '.join(f'0x{byte:02x}' for byte in padded[start:start + 12])
            for start in range(0, len(padded), 12)]
    embedded_source.write_text(
        '/* Generated shader bytes; rebuild from gx_color.v.pica. */\n'
        '__attribute__((aligned(4))) const unsigned char color_lookup_shader[] = {\n'
        + ''.join('    ' + row + ',\n' for row in rows)
        + '};\nconst unsigned int color_lookup_shader_size = ' + str(len(shader)) + ';\n',
        encoding='utf-8')
    flags = ['-march=armv6k', '-mtune=mpcore', '-mfloat-abi=hard', '-mtp=soft',
             '-mword-relocations', '-fno-fast-math', '-ffp-contract=off',
             '-ffunction-sections', '-fdata-sections', '-D_GNU_SOURCE',
             '-D__3DS__', '-D_3DS', '-DARM11', '-O2', '-g', '-Wall', '-Wextra',
             '-I' + str(sdk / 'libctru/include')]
    run([c_compiler, *flags, '-c', embedded_source, '-o', embedded_object])
    sources = [PLATFORM / 'tests/color_lookup_gpu_probe.cpp',
               PLATFORM / 'runtime/gx_pica_material.cpp', PLATFORM / 'gx_material_pica.cpp']
    run([compiler, *flags, '-std=gnu++20', *sources, embedded_object,
         '-specs=' + str(specs), '-Wl,--gc-sections', '-Wl,-Map,' + str(build / 'color-lookup-probe.map'),
         '-L' + str(sdk / 'libctru/lib'), '-lcitro3d', '-lctru', '-lm', '-o', elf])
    run([package, elf, output])
    inputs = [Path(__file__).resolve(), shader_source, *sources,
              PLATFORM / 'native/color_lookup.hpp', PLATFORM / 'runtime/gx_material.hpp',
              PLATFORM / 'runtime/gx_pica_material.hpp', PLATFORM / 'gx_material_pica.hpp']
    receipt = {
        'status': 'built-not-gpu-verified',
        'devkitpro': str(sdk), 'devkitarm': str(arm), 'commands': commands,
        'inputs_sha256': {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in inputs},
        'outputs_sha256': {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                           for path in (shader_binary, elf, output)},
        'capture_root': 'sdmc:/3ds/dusklight-color-lookup/run-<system-tick>',
        'raw_format': '240x400 physical, linear ABGR8, no GX vertical flip; 384000 bytes',
        'lcd_format': '240x400 physical, linear BGR8; 288000 bytes; double buffering disabled',
        'readback_validation': 'CPU-check known corner before filesystem IPC; flat RGBA plus analytical gradient/coverage calibration; independent direct-SDK ordinary baseline',
        'limits': 'No GPU/emulator execution performed by this builder. See runtime receipt and metrics.',
    }
    (build / 'build-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print(f'Built: {output}\nGPU correctness: NOT RUN\nCaptures: {receipt["capture_root"]}', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
