#!/usr/bin/env python3
"""Check ARM11 concat against actual portable C at production compile flags.

No Python packages or test framework required; uses devkitARM/libctru and the
existing isolated Azahar runner. --build-only produces a hardware-runnable 3DSX
but is NOT a passing correctness result. With --azahar, success requires the
completed guest report (process exit alone is insufficient).

Example:
  python3 Tools/3ds/test_mtx_concat_arm11.py \
      --compile-commands Build/public-game/compile_commands.json \
      --azahar /path/to/azahar --no-sandbox

On hardware, run Build/mtx-concat-arm11/mtx-concat-arm11.3dsx and inspect
sdmc:/3ds/DuskLight/mtx-concat-arm11.json. This check makes no timing claim.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'Source/extern/aurora/lib/dolphin/mtx/mtx.c'
ASM = ROOT / 'Source/platforms/3ds/runtime/mtx_concat_arm11.S'
PROBE = Path(__file__).with_name('mtx_concat_arm11_test.cpp')
EXPECTED_COMPARISONS = 8192 * 7 * 2 + 3 * 33 * 33


def compile_policy(database):
    entries = json.loads(database.read_text())
    matches = [e for e in entries if
               (Path(e['directory']) / e['file']).resolve() == SOURCE]
    if len(matches) != 1:
        raise ValueError('Expected exactly one production mtx.c compile command')
    entry = matches[0]
    command = entry.get('arguments') or shlex.split(entry['command'])
    flags = []
    skip = False
    for arg in command[1:]:
        if skip:
            skip = False
        elif arg in ('-o', '-MF', '-MT', '-MQ'):
            skip = True
        elif arg not in ('-c', '-MD', '-MMD', '-MP') and not (
                not arg.startswith('-') and
                (Path(entry['directory']) / arg).resolve() == SOURCE):
            flags.append(arg)
    if '-D__3DS__' not in flags or '-DDUSK_TARGET_3DS=1' not in flags:
        raise ValueError('Use the native 3DS math compile database, not host flags')
    return Path(command[0]), flags, Path(entry['directory'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compile-commands', type=Path,
                        default=ROOT / 'Build/public-game/compile_commands.json')
    parser.add_argument('--output', type=Path, default=ROOT / 'Build/mtx-concat-arm11')
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--build-only', action='store_true')
    mode.add_argument('--azahar', type=Path)
    parser.add_argument('--no-sandbox', action='store_true',
                        help='Pass existing runner option on non-macOS hosts')
    parser.add_argument('--seconds', type=int, default=120)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    receipt_path = output / 'results.json'
    receipt = {'status': 'running', 'commands': [],
               'origin': 'ARM execution only; no hardware performance claim',
               'reference': 'original portable mtx.c body, __3DS__ undefined; production flags otherwise unchanged',
               'input_sha256': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                for p in (SOURCE, ASM, PROBE, Path(__file__).resolve())}}
    receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')
    try:
        compiler, flags, cwd = compile_policy(args.compile_commands)
        if not compiler.is_absolute():
            raise ValueError('Production compile database must name the absolute devkitARM compiler')
        prefix = str(compiler).removesuffix('gcc')
        if prefix == str(compiler):
            raise ValueError('Expected devkitARM GCC C compiler')
        devkitarm = compiler.parent.parent
        devkitpro = devkitarm.parent
        production = output / 'mtx.o'
        reference = output / 'reference.o'
        kernel = output / 'mtx-concat-arm11.o'
        probe = output / 'probe.o'
        elf = output / 'mtx-concat-arm11.elf'
        image = output / 'mtx-concat-arm11.3dsx'
        with (output / 'build.log').open('w') as log:
            def run(command, *, capture=False):
                command = list(map(str, command))
                receipt['commands'].append(command)
                result = subprocess.run(command, cwd=cwd, check=True, timeout=180,
                                        stdout=subprocess.PIPE if capture else log,
                                        stderr=log, text=True)
                return result.stdout

            run([compiler, *flags, '-c', SOURCE, '-o', production])
            # Compile the unchanged non-3DS C entrypoint, not a hand-transcribed
            # formula. Localize its other definitions to avoid duplicate public
            # symbols while retaining reference-internal relocations/copy calls.
            run([compiler, *flags, '-U__3DS__',
                 '-DC_MTXConcat=DuskMtxConcatReference', '-c', SOURCE, '-o', reference])
            symbols = run([prefix + 'nm', '-g', '--defined-only', reference], capture=True)
            localize = ['--localize-symbol=' + row.split()[-1]
                        for row in symbols.splitlines()
                        if row.split() and row.split()[-1] != 'DuskMtxConcatReference']
            run([prefix + 'objcopy', *localize, reference])
            run([compiler, *flags, '-c', ASM, '-o', kernel])
            cxx_flags = [f for f in flags if not f.startswith('-std=')]
            run([prefix + 'g++', *cxx_flags, '-std=gnu++20', '-fno-exceptions', '-fno-rtti',
                 '-I' + str(devkitpro / 'libctru/include'), '-c', PROBE, '-o', probe])
            run([prefix + 'g++', *cxx_flags,
                 '-specs=' + str(devkitarm / 'arm-none-eabi/lib/3dsx.specs'),
                 probe, production, reference, kernel,
                 '-L' + str(devkitpro / 'libctru/lib'), '-Wl,--gc-sections',
                 '-lctru', '-lm', '-o', elf])
            run([devkitpro / 'tools/bin/3dsxtool', elf, image])
        receipt['image'] = str(image)
        if args.build_only:
            receipt['status'] = 'built; correctness NOT RUN'
        else:
            # Reuse isolated SD/config and owned-process cleanup, never real saves.
            run_name = 'mtx-concat-arm11-' + uuid.uuid4().hex[:12]
            command = [sys.executable, ROOT / 'Tools/3ds/run_azahar_smoke.py',
                       '--azahar', args.azahar.resolve(), '--image', image,
                       '--name', run_name, '--seconds', str(args.seconds),
                       '--renderer', 'software', '--evidence-root', output / 'emulator']
            if args.no_sandbox:
                command.append('--no-sandbox')
            receipt['commands'].append(list(map(str, command)))
            with (output / 'emulator.log').open('w') as log:
                subprocess.run(list(map(str, command)), cwd=ROOT, check=True,
                               timeout=args.seconds + 120, stdout=log, stderr=subprocess.STDOUT)
            report = (ROOT / 'Build/azahar-runs' / run_name /
                      'xdg-data/azahar-emu/sdmc/3ds/DuskLight/mtx-concat-arm11.json')
            receipt['guest_report'] = str(report)
            guest = json.loads(report.read_text())
            receipt['guest'] = guest
            if guest.get('status') != 'pass' or guest.get('comparisons') != EXPECTED_COMPARISONS:
                raise RuntimeError('Guest numerical/alias/ABI check failed or incomplete')
            receipt['status'] = 'pass'
        current = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                   for p in (SOURCE, ASM, PROBE, Path(__file__).resolve())}
        if current != receipt['input_sha256']:
            raise RuntimeError('Sources changed during check; result superseded')
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        receipt.update(status='fail', error=str(error))
    finally:
        receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt, indent=2))
    return int(receipt['status'] not in ('pass', 'built; correctness NOT RUN'))


if __name__ == '__main__':
    raise SystemExit(main())
