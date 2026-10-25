#!/usr/bin/env python3
"""Build the existing native target locally; never tag, upload or publish."""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', choices=['diagnostic', 'game'], default='diagnostic')
    parser.add_argument('--devkitpro', type=Path, default=Path(os.environ.get('DEVKITPRO', '/opt/devkitpro')))
    parser.add_argument('--devkitarm', type=Path)
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--jobs', type=int, default=min(os.cpu_count() or 1, 8))
    parser.add_argument('--configure-only', action='store_true')
    parser.add_argument('--cmake-option', action='append', default=[], metavar='-DNAME=VALUE')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if any(not item.startswith('-D') or '=' not in item for item in args.cmake_option):
        parser.error('--cmake-option must be -DNAME=VALUE')
    sdk = args.devkitpro.expanduser().resolve()
    arm = (args.devkitarm or Path(os.environ.get('DEVKITARM', str(sdk / 'devkitARM')))).expanduser().resolve()
    if not (arm / 'bin/arm-none-eabi-g++').is_file():
        parser.error('devkitARM compiler missing; set DEVKITPRO/DEVKITARM or pass their directory arguments')
    build = (args.build_dir or ROOT / ('Build/public-' + args.profile)).expanduser().resolve()
    source = ROOT / 'Source/platforms/3ds'
    enabled = 'ON' if args.profile == 'game' else 'OFF'
    options = [f'-D{key}={enabled}' for key in (
        'DUSK_SCENE_STARTUP', 'DUSK_SCENE_ENVIRONMENT_STARTUP',
        'DUSK_GAMEPLAY_LINK_GATE', 'DUSK_GAME_STARTUP')]
    options += [f'-D{key}=OFF' for key in (
        'DUSK_COMMAND_PROBE_STARTUP', 'DUSK_SCENE_FOG_INSPECTION',
        'DUSK_SCENE_MATERIAL_INSPECTION', 'DUSK_BG_BUDGET_EXPERIMENT_76',
        'DUSK_SCENE_CREATE_PROBE', 'DUSK_GROUND_CREATE_PROBE')]
    options += ['-DDUSK_SCENE_TEXTURES=ON', '-DDUSK_DIAGNOSTIC_EXIT_FRAMES=0',
                '-DDUSK_PROFILE_LINK_SELECTION:FILEPATH=']
    command = ['cmake', '-S', str(source), '-B', str(build),
               '-DCMAKE_TOOLCHAIN_FILE=' + str(source / 'compile/devkitarm.cmake'),
               '-DDEVKITPRO=' + str(sdk), '-DDEVKITARM=' + str(arm),
               *options, *args.cmake_option]
    subprocess.run(command, cwd=ROOT, check=True)
    if not args.configure_only:
        subprocess.run(['cmake', '--build', str(build), '--parallel', str(args.jobs)], cwd=ROOT, check=True)
    print('Local output: ' + str(build), flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
