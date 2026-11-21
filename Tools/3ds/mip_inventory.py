#!/usr/bin/env python3
"""Mip inventory of the disc (Mac only, private data): which textures are
authored without mip levels, their PICA bytes, and how the game samples them
(mip_inventory.cpp). Reads the disc only; prints the report.

  python3 -I Tools/3ds/mip_inventory.py --disc Build/perf30-data/zelda.twilight [--list 40]
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RUNTIME = ROOT / 'Source/platforms/3ds/runtime'
SOURCES = [RUNTIME / f for f in ('pretexture_store.cpp', 'texture_gx.cpp', 'disc.cpp', 'resource_archive.cpp')]
INPUTS = [HERE / 'mip_inventory.cpp', HERE / 'pretexture_walk.hpp', *SOURCES,
          *(RUNTIME / f for f in ('pretexture_store.hpp', 'texture_gx.hpp', 'disc.hpp', 'resource_archive.hpp'))]


def build(tools):
    tools.mkdir(parents=True, exist_ok=True)
    target = tools / 'mip_inventory'
    receipt = target.with_suffix('.build.json')
    h = hashlib.sha256()
    for p in INPUTS:
        h.update(str(p).encode()); h.update(p.read_bytes())
    key = h.hexdigest()
    prior = json.loads(receipt.read_text()) if receipt.exists() else {}
    if not (target.exists() and prior.get('inputs') == key):
        command = ['clang++', '-std=c++20', '-O2', '-Wall', '-Wextra', str(HERE / 'mip_inventory.cpp'), *map(str, SOURCES), '-o', str(target)]
        subprocess.run(command, check=True)
        receipt.write_text(json.dumps({'inputs': key, 'command': command}, indent=1) + '\n')
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--disc', type=Path, required=True, help='zelda.twilight bundle or GameCube image (never uploaded)')
    parser.add_argument('--tools', type=Path, default=ROOT / 'Build/mip-inventory-tools')
    parser.add_argument('--list', type=int, default=0, help='also list the N largest world candidates')
    args = parser.parse_args()
    tool = build(args.tools.resolve())
    command = [str(tool), str(args.disc)] + (['--list', str(args.list)] if args.list else [])
    return subprocess.run(command).returncode


if __name__ == '__main__':
    sys.exit(main())
