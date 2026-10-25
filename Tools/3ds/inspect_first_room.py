#!/usr/bin/env python3
"""Read original Ordon actor-name metadata for a private bounded LINK probe.

Uses the production bounded Disc/RARC/DZS parsers with host sanitizers. Saves
names/counts only, not archive payloads. Includes every metadata actor layer;
runtime spawn closure and gameplay remain unverified.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STARTUP_PROFILES = ['ALINK', 'MIDNA', 'CAMERA', 'PLAY_SCENE', 'ROOM_SCENE', 'BG',
                    'KANKYO', 'KYEFF', 'KYEFF2', 'ENVSE', 'VRBOX', 'VRBOX2',
                    'METER2', 'MSG_OBJECT', 'MENUWINDOW', 'SUSPEND', 'TIMER']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--private-disc', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    native = ROOT / 'Source/platforms/3ds'
    sources = [native / x for x in ['tests/first_room_inventory.cpp', 'runtime/disc.cpp',
                                    'runtime/resource_archive.cpp', 'runtime/stage_data.cpp']]
    headers = [native / ('runtime/' + x) for x in ['disc.hpp', 'resource_archive.hpp', 'stage_data.hpp']]
    stage_owner = ROOT / 'Source/src/d/d_stage.cpp'
    inputs = [Path(__file__).resolve(), *sources, *headers, stage_owner]
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    command = ['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               *map(str, sources), '-o', str(out / 'inspect')]
    receipt = {'status': 'running', 'scope': __doc__, 'command': command, 'input_sha256': hashes}
    try:
        with (out / 'build.log').open('w') as log:
            subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT, timeout=90)
        before = args.private_disc.stat()
        run = subprocess.run([str(out / 'inspect'), str(args.private_disc)], capture_output=True,
                             text=True, timeout=30, env={**os.environ,
                             'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1',
                             'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})
        (out / 'inspect.log').write_text(run.stderr)
        run.check_returncode()
        after = args.private_disc.stat()
        assert (before.st_size, before.st_mtime_ns) == (after.st_size, after.st_mtime_ns)
        records = [json.loads(line) for line in run.stdout.splitlines()]
        assert records
        lookup = {}
        for name, proc in re.findall(r'OBJNAME\("([^"\n]+)",\s*fpcNm_(\w+)_e', stage_owner.read_text()):
            lookup.setdefault(name, set()).add(proc)
        names = sorted({r['actor'] for r in records})
        assert all(len(lookup.get(name, set())) == 1 for name in names), 'unknown/ambiguous original actor name'
        profiles = {'g_profile_' + next(iter(lookup[name])) for name in names}
        profiles |= {'g_profile_' + x for x in STARTUP_PROFILES}
        selection = {'scope': 'Private F_SP103 room0 original-profile LINK probe; dynamic spawns unverified; not gameplay',
                     'profiles': sorted(profiles), 'metadata_actor_count': len(records), 'actor_names': names,
                     'startup_profile_reason': 'Original d_stage/d_s_room creation paths, player/camera roots and HUD/message processes; conservative includes'}
        (out / 'actors.jsonl').write_text(run.stdout)
        (out / 'profile-selection.json').write_text(json.dumps(selection, indent=2) + '\n')
        assert hashes == {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        receipt.update(status='pass', private_disc_size_mtime_unchanged=True,
                       metadata_records=len(records), selected_profiles=len(profiles),
                       input_unchanged=True, payload_saved=False)
    except (OSError, subprocess.SubprocessError, AssertionError) as error:
        receipt.update(status='fail', error=str(error))
    (out / 'results.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({k: v for k, v in receipt.items() if k not in ['command', 'input_sha256', 'scope']}, indent=2))
    return int(receipt['status'] != 'pass')


if __name__ == '__main__':
    raise SystemExit(main())
