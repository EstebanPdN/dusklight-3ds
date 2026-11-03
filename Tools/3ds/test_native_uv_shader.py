#!/usr/bin/env python3
"""Check assembled native affine shader ABIs; GPU behavior is a separate scenario."""
import argparse
from pathlib import Path
from test_native_shader import write_receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    names = ['scene', 'scene_rigid', 'scene_uv', 'scene_rigid_uv']
    write_receipt(args.output, {name: args.build_dir / ('native_' + name + '.shbin') for name in names})


if __name__ == '__main__':
    main()
