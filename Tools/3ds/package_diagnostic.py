#!/usr/bin/env python3
"""Build a local diagnostic CIA from native code and owner-supplied artwork only."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import wave

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / 'Build/new3ds-native'
OUT = ROOT / 'Build/new3ds-diagnostic-0.1'
EVIDENCE = ROOT / 'Diagnostics/implementation-04'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    global BUILD, OUT, EVIDENCE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--makerom', required=True, type=Path)
    parser.add_argument('--bannertool', required=True, type=Path)
    parser.add_argument('--iteration', type=int, default=1)
    parser.add_argument('--build-directory', type=Path)
    parser.add_argument('--evidence-directory', type=Path)
    args = parser.parse_args()
    if not 1 <= args.iteration <= 99:
        parser.error('Diagnostic iteration must be 1..99')
    version = f'0.{args.iteration}'
    OUT = ROOT / ('Build/new3ds-diagnostic-' + version)
    if args.build_directory:
        BUILD = args.build_directory.resolve()
    elif args.iteration != 1:
        BUILD = ROOT / ('Build/new3ds-native-' + version)
    if args.evidence_directory:
        EVIDENCE = args.evidence_directory.resolve()
    elif args.iteration != 1:
        EVIDENCE = ROOT / ('Diagnostics/diagnostic-' + version)
    OUT.mkdir(parents=True, exist_ok=True)
    EVIDENCE.mkdir(parents=True, exist_ok=True)
    commands = []

    def run(command):
        command = [str(x) for x in command]
        commands.append(command)
        result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
        with (EVIDENCE / 'package.log').open('a') as log:
            log.write(result.stdout + result.stderr)
        result.check_returncode()

    (EVIDENCE / 'package.log').write_text('Native diagnostic package, no gameplay claim.\n')
    run(['cmake', '-S', ROOT / 'Source/platforms/3ds', '-B', BUILD,
         '-DCMAKE_TOOLCHAIN_FILE=' + str(ROOT / 'Source/platforms/3ds/compile/devkitarm.cmake')])
    run(['cmake', '--build', BUILD, '-j4'])
    artwork = ROOT / 'assets/3ds'
    expected_art = {
        'banner.png': '3be856f11a1f467d23c5788c33b94c603743ed9b38dff6d0f3d3e25035e680d7',
        'icon-48.png': '7cda6cd9f808bdc0c1f0aff5ae934e3b725f72ea8028ebe8f9451b38199d17d7',
    }
    for name, expected in expected_art.items():
        if digest(artwork / name) != expected:
            raise ValueError('Owner artwork changed: ' + name)
    # Silent generated PCM; no audio or other assets are taken from the disc.
    silent = BUILD / 'diagnostic-silence.wav'
    with wave.open(str(silent), 'wb') as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(32000)
        wav.writeframes(bytes(16000))
    smdh = BUILD / 'diagnostic.smdh'
    banner = BUILD / 'diagnostic.bnr'
    run([args.bannertool, 'makesmdh', '-s', 'Dusklight Diagnostic ' + version,
         '-l', 'Native platform test - game not playable', '-p', 'Twilit Realm / local port',
         '-i', artwork / 'icon-48.png', '-f', 'visible,recordusage,new3ds', '-o', smdh])
    run([args.bannertool, 'makebanner', '-i', artwork / 'banner.png', '-a', silent, '-o', banner])
    cxi = BUILD / 'dusklight-diagnostic.cxi'
    rsf = BUILD / 'diagnostic.rsf'
    template = (ROOT / 'Source/platforms/3ds/dusklight-diagnostic.rsf').read_text()
    if template.count('RemasterVersion: 1') != 1:
        raise ValueError('Diagnostic RSF version anchor changed')
    rsf.write_text(template.replace('RemasterVersion: 1', 'RemasterVersion: ' + str(args.iteration)))
    run([args.makerom, '-f', 'ncch', '-o', cxi,
         '-rsf', rsf,
         '-elf', BUILD / 'dusklight-native.elf', '-icon', smdh, '-banner', banner])
    cia = OUT / ('dusklight-diagnostic-' + version + '.cia')
    run([args.makerom, '-f', 'cia', '-o', cia, '-content', str(cxi) + ':0:0', '-ver', str(args.iteration)])
    # The Homebrew Launcher artifact uses the same executable and explicit label.
    three_dsx = OUT / ('dusklight-diagnostic-' + version + '.3dsx')
    run(['/opt/devkitpro/tools/bin/3dsxtool', BUILD / 'dusklight-native.elf', three_dsx,
         '--smdh=' + str(smdh)])
    from verify_diagnostic_package import verify
    verified = verify(cia, cxi, smdh, iteration=args.iteration)
    receipt = {
        'status': 'structural-validation-pass', 'profile': 'diagnostic-only',
        'gameplay': False, 'physical_console_test': False,
        'source': subprocess.check_output(['git', '-C', str(ROOT / 'Source'), 'rev-parse', 'HEAD'], text=True).strip(),
        'uncommitted_port_changes': True,
        'commands': commands, 'verification': verified,
        'tools': {str(p): digest(p) for p in (args.makerom, args.bannertool)},
        'executable_inputs': {p.name: digest(p) for p in (BUILD / 'dusklight-native.elf', cxi, smdh)},
        'artwork': expected_art,
        'outputs': {p.name: {'bytes': p.stat().st_size, 'sha256': digest(p)} for p in (cia, three_dsx)},
    }
    text = json.dumps(receipt, indent=2) + '\n'
    (OUT / 'build-receipt.json').write_text(text)
    (EVIDENCE / 'package-receipt.json').write_text(text)
    print(json.dumps({'cia': str(cia), '3dsx': str(three_dsx), 'verification': verified}, indent=2))


if __name__ == '__main__':
    main()
