#!/usr/bin/env python3
"""Select original profile pointers for a bounded LINK diagnostic, not gameplay.

All slots and version guards stay in original order. Unselected slots are null;
the explicitly enabled native probe must stop on such a runtime lookup. No
replacement profile, callback, or successful game-service stub is generated.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ORIGINAL = ROOT / 'Source/src/f_pc/f_pc_profile_lst.cpp'


def generate(original, selection):
    anchor = '#else\nDUSK_GAME_DATA process_profile_definition'
    if original.count(anchor) != 1:
        raise ValueError('original profile-list boundary changed')
    native = original.split(anchor, 1)[1]
    native = 'DUSK_GAME_DATA process_profile_definition' + native
    pattern = re.compile(r'&(?P<name>g_profile_[A-Za-z0-9_]+)(?:\.base)+')
    available = set(pattern.findall(native))
    selected = set(selection)
    if not selected or not all(isinstance(x, str) and re.fullmatch(r'g_profile_[A-Za-z0-9_]+', x) for x in selected):
        raise ValueError('selection must contain original profile symbol names')
    if selected - available:
        raise ValueError('unknown original profiles: ' + ', '.join(sorted(selected - available)))
    native = pattern.sub(lambda m: m.group(0) if m['name'] in selected else 'nullptr', native)
    # Remove only the closing MWERKS branch; all interior VERSION guards survive.
    if not native.endswith('#endif\n'):
        raise ValueError('original profile-list terminator changed')
    native = native[:-len('#endif\n')]
    result = '// Generated original-profile link probe. NOT gameplay acceptance.\n'
    result += '#include "f_pc/f_pc_profile_lst.h"\n'
    result += '#if !defined(DUSK_TARGET_3DS) || !defined(DUSK_PROFILE_LINK_PROBE)\n#error Native link probe only\n#endif\n'
    return result + native, available


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--selection', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    config = json.loads(args.selection.read_text())
    source = ORIGINAL.read_text()
    generated, available = generate(source, config['profiles'])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(generated)
    args.output.with_suffix('.json').write_text(json.dumps({
        'scope': 'bounded original-profile executable link only; dynamic spawn closure unverified',
        'selected': len(set(config['profiles'])), 'available': len(available),
        'original_sha256': hashlib.sha256(source.encode()).hexdigest(),
        'selection_sha256': hashlib.sha256(args.selection.read_bytes()).hexdigest(),
        'generated_sha256': hashlib.sha256(generated.encode()).hexdigest(),
    }, indent=2) + '\n')


if __name__ == '__main__':
    main()
