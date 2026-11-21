#!/usr/bin/env python3
"""Pre-converted PICA textures for zelda.twilight (Mac only, private data).

Builds the host converter (pretexture_build.cpp, which links the runtime's own
disc reader, archive reader and texture decoder), converts every BTI of the
game into a texture section (runtime/pretexture_store.hpp layout), and can
append that section to a twilight_pack.py bundle as one more range, so the
console finds it in /3ds/DuskLight/zelda.twilight. Never overwrites a file.

  python3 -I Tools/3ds/pretexture_pack.py --disc Build/perf30-data/zelda.twilight \
      --section Build/alpha12/pretexture/textures.dlptex [--etc] [--test]
  python3 -I Tools/3ds/pretexture_pack.py --disc zelda.twilight --section t.dlptex \
      --bundle-out zelda-textures.twilight [--verify]

Design and numbers: Build/alpha12/pretexture-design.md.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import zlib

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import twilight_pack  # noqa: E402  (same directory; -I keeps cwd out of sys.path)

RUNTIME = ROOT / 'Source/platforms/3ds/runtime'
UPSTREAM = ROOT / 'Upstream/tex3ds-host-lab'
SOURCES = [HERE / 'pretexture_walk.hpp', RUNTIME / 'pretexture_store.hpp', RUNTIME / 'pretexture_store.cpp',
           RUNTIME / 'texture_gx.hpp', RUNTIME / 'texture_gx.cpp', RUNTIME / 'disc.hpp', RUNTIME / 'disc.cpp',
           RUNTIME / 'resource_archive.hpp', RUNTIME / 'resource_archive.cpp']
TRAILER = struct.Struct('<8sIIQ')
MAX_LOGICAL = 0x7fffffff


def digest(paths):
    h = hashlib.sha256()
    for p in paths:
        h.update(str(p).encode())
        h.update(Path(p).read_bytes())
    return h.hexdigest()


def build(tools):
    """Builds the converter and the test; reuses binaries whose sources are unchanged."""
    tools.mkdir(parents=True, exist_ok=True)
    common = [str(RUNTIME / f) for f in ('pretexture_store.cpp', 'texture_gx.cpp', 'disc.cpp', 'resource_archive.cpp')]
    etc_object = tools / 'rg_etc1.o'
    targets = {
        'pretexture_build': (['clang++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-I' + str(UPSTREAM / 'include'),
                              str(HERE / 'pretexture_build.cpp'), *common, str(etc_object)],
                             SOURCES + [HERE / 'pretexture_build.cpp', UPSTREAM / 'source/rg_etc1.cpp']),
        'pretexture_store_test': (['clang++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-fsanitize=address,undefined',
                                   '-fno-omit-frame-pointer', str(HERE / 'pretexture_store_test.cpp'), *common],
                                  SOURCES + [HERE / 'pretexture_store_test.cpp']),
    }
    built = {}
    for name, (command, inputs) in targets.items():
        target = tools / name
        receipt = target.with_suffix('.build.json')
        key = digest(inputs)
        prior = json.loads(receipt.read_text()) if receipt.exists() else {}
        if not (target.exists() and prior.get('inputs') == key):
            if name == 'pretexture_build':
                subprocess.run(['clang++', '-std=c++11', '-O2', '-w', '-c', str(UPSTREAM / 'source/rg_etc1.cpp'),
                                '-I' + str(UPSTREAM / 'include'), '-o', str(etc_object)], check=True)
            subprocess.run(command + ['-o', str(target)], check=True)
            receipt.write_text(json.dumps({'inputs': key, 'command': command}, indent=1) + '\n')
        built[name] = target
    return built


def section_bytes(path):
    with open(path, 'rb') as f:
        f.seek(-64, os.SEEK_END)
        trailer = f.read(64)
    magic, version, _, size = TRAILER.unpack_from(trailer)
    if magic != b'DLPTEXTR' or version != 1:
        raise ValueError('Not a texture section (trailer)')
    if zlib.crc32(trailer[:60] + bytes(4)) != struct.unpack_from('<I', trailer, 60)[0]:
        raise ValueError('Texture section trailer checksum')
    if size != os.path.getsize(path):
        raise ValueError('Stand-alone section size differs from its trailer')
    return size


def append_to_bundle(bundle_path, section_path, output):
    """New bundle = the v1 bundle plus one range holding the section, mapped
    after the disc's logical end. Old runtimes read the disc unchanged."""
    if os.path.lexists(output):
        raise ValueError('Output already exists; refusing overwrite')
    size = section_bytes(section_path)
    with open(bundle_path, 'rb') as src:
        bundle = twilight_pack.Bundle(src)
        if bundle.ranges[-1][0] + bundle.ranges[-1][2] > bundle.logical:
            raise ValueError('Bundle ranges beyond the logical end')
        old_data = 128 + len(bundle.ranges) * 64
        logical_start = (bundle.logical + 0x7fff) & ~0x7fff
        if logical_start + size > MAX_LOGICAL:
            raise ValueError('Texture section exceeds the 2 GiB logical limit of the bundle')
        sha = hashlib.sha256()
        with open(section_path, 'rb') as sec:
            for chunk in iter(lambda: sec.read(1 << 20), b''):
                sha.update(chunk)
        count = len(bundle.ranges) + 1
        data_start = 128 + count * 64
        index = bytearray()
        cursor = data_start
        for lo, _po, n, digest_ in bundle.ranges:
            index += twilight_pack.RECORD.pack(lo, cursor, n, digest_, bytes(8))
            cursor += n
        index += twilight_pack.RECORD.pack(logical_start, cursor, size, sha.digest(), bytes(8))
        physical = cursor + size
        if physical > MAX_LOGICAL:
            raise ValueError('Bundle exceeds the native physical offset limit')
        header = twilight_pack.header_bytes(logical_start + size, physical, count, zlib.crc32(index), bundle.source_hash)
        staging = output.with_name('.' + output.name + '.partial')
        with open(staging, 'xb') as dst:
            dst.write(header)
            dst.write(index)
            src.seek(old_data)
            left = cursor - data_start
            while left:
                chunk = src.read(min(1 << 24, left))
                if not chunk:
                    raise ValueError('Truncated source bundle')
                dst.write(chunk)
                left -= len(chunk)
            with open(section_path, 'rb') as sec:
                for chunk in iter(lambda: sec.read(1 << 24), b''):
                    dst.write(chunk)
            dst.flush()
            os.fsync(dst.fileno())
    os.rename(staging, output)
    return {'logical_start': logical_start, 'section_bytes': size, 'physical_bytes': physical, 'ranges': count}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--disc', type=Path, required=True, help='zelda.twilight bundle or GameCube image (never uploaded)')
    parser.add_argument('--section', type=Path, required=True, help='texture section to write (refuses an existing file)')
    parser.add_argument('--report', type=Path, help='JSON report (default: section path + .json)')
    parser.add_argument('--tools', type=Path, default=ROOT / 'Build/pretexture-tools')
    parser.add_argument('--etc', action='store_true', help='add lossy ETC1/ETC1A4 variants (runtime opt-in)')
    parser.add_argument('--etc-scope', choices=('stage', 'all'), default='stage')
    parser.add_argument('--etc-min-texels', type=int, default=4096)
    parser.add_argument('--etc-min-psnr', type=float, default=34.0)
    parser.add_argument('--bundle-out', type=Path, help='write a new zelda.twilight with the section appended')
    parser.add_argument('--verify', action='store_true', help='re-hash every range of --bundle-out (twilight_pack)')
    parser.add_argument('--test', action='store_true', help='run the byte-exact host test on the result')
    args = parser.parse_args()
    tools = build(args.tools.resolve())
    report = args.report or args.section.with_suffix(args.section.suffix + '.json')
    command = [str(tools['pretexture_build']), '--disc', str(args.disc), '--section', str(args.section), '--report', str(report)]
    if args.etc:
        command += ['--etc', '--etc-scope', args.etc_scope, '--etc-min-texels', str(args.etc_min_texels),
                    '--etc-min-psnr', str(args.etc_min_psnr)]
    subprocess.run(command, check=True)
    store = args.section
    if args.bundle_out:
        info = append_to_bundle(args.disc, args.section, args.bundle_out.absolute())
        with open(args.bundle_out, 'rb') as f:
            bundle = twilight_pack.Bundle(f)  # structure, FST layout and coverage
            if args.verify:
                info['sha256'] = bundle.verify()
        print(json.dumps({'bundle': str(args.bundle_out), **info}, indent=1))
        store = args.bundle_out
    if args.test:
        disc = args.bundle_out if args.bundle_out else args.disc
        subprocess.run([str(tools['pretexture_store_test']), '--disc', str(disc), '--store', str(store),
                        '--scratch', str(args.section.parent)], check=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
