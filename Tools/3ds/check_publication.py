#!/usr/bin/env python3
"""Check tracked public content, maintained guide links and open firmware integrity."""
import hashlib
from html.parser import HTMLParser
from pathlib import Path
import re
import subprocess
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[2]
ALLOWED = {'README.md', 'AI_DEVELOPMENT.md', 'LICENSE.md', 'CONTRIBUTING.md',
           'CHANGELOG.md', '.gitignore', '.gitattributes', '.github', 'Source', 'Tools', 'docs', 'assets'}
FORBIDDEN = {'.iso', '.gcm', '.rvz', '.wia', '.twilight', '.sav', '.gci', '.raw',
             '.dmp', '.log', '.wav', '.cia', '.cxi', '.3dsx', '.elf', '.o', '.pyc', '.pem', '.key'}
# Kenney's CC0 sample is upstream demo content, not captured game audio.
PUBLIC_AUDIO = {'Source/mods/audio_mod/res/go.wav':
                '781e319fbd61d80905ebd56f6dab8417384eef7e1a3b3df4c61813151c927ab7'}
FIRMWARE = 'Source/platforms/3ds/vendor/opendsp/opendsp.cdc'
SHA256 = '1d4f126106e1715eed63393f9787d4fa796412bc60292a23b31b4b6650ca1ab4'


class Links(HTMLParser):
    def __init__(self):
        super().__init__()
        self.targets = []

    def handle_starttag(self, tag, attrs):
        self.targets.extend(value for name, value in attrs if name in {'src', 'href'} and value)


def main():
    entries = subprocess.check_output(['git', 'ls-files', '--stage', '-z'], cwd=ROOT).decode().split('\0')
    errors, tracked = [], set()
    for entry in filter(None, entries):
        meta, name = entry.split('\t', 1)
        path = Path(name)
        tracked.add(name)
        if meta.startswith('160000 '):
            errors.append('Unexpected submodule: ' + name)
        if path.parts[0] not in ALLOWED or '.git' in path.parts or '__pycache__' in path.parts:
            errors.append('Unexpected tracked path: ' + name)
        if (path.suffix.lower() in FORBIDDEN and name not in PUBLIC_AUDIO) or path.name in {'.env', '.DS_Store'} or path.name.startswith('.env.'):
            errors.append('Private/generated file: ' + name)
        full = ROOT / path
        if not full.is_file():
            errors.append('Missing tracked file: ' + name)
        elif full.stat().st_size > 50 * 1024 * 1024:
            errors.append('Unexpected file over 50 MiB: ' + name)
    for name, expected in PUBLIC_AUDIO.items():
        sample = ROOT / name
        if name not in tracked or not sample.is_file() or hashlib.sha256(sample.read_bytes()).hexdigest() != expected:
            errors.append('Upstream CC0 audio sample changed: ' + name)
    if not tracked:
        errors.append('No tracked public files; stage the intended source before checking')
    required = ['README.md', 'AI_DEVELOPMENT.md', 'LICENSE.md', 'CONTRIBUTING.md',
                'docs/STATUS.md', 'docs/BUILDING.md', 'docs/UPSTREAM.md',
                'Source/platforms/3ds/CMakeLists.txt', FIRMWARE,
                'Source/platforms/3ds/vendor/opendsp/LICENSE',
                'Source/platforms/3ds/vendor/opendsp/LICENSE.minimp3',
                'Source/platforms/3ds/vendor/opendsp/NOTICE.minimp3']
    errors.extend('Required file not tracked: ' + name for name in required if name not in tracked)
    firmware = ROOT / FIRMWARE
    if not firmware.is_file() or hashlib.sha256(firmware.read_bytes()).hexdigest() != SHA256:
        errors.append('OpenDSP component hash mismatch')
    public_docs = [ROOT / x for x in ('README.md', 'AI_DEVELOPMENT.md', 'CONTRIBUTING.md', 'CHANGELOG.md')]
    public_docs += [ROOT / name for name in sorted(tracked)
                    if name.startswith('docs/') and name.endswith('.md')]
    for path in public_docs:
        text = path.read_text()
        # Code examples are not navigation. Preserve inline backtick paths as prose.
        prose = re.sub(r'^```.*?^```[^\n]*$', '', text, flags=re.MULTILINE | re.DOTALL)
        parser = Links()
        parser.feed(prose)
        targets = parser.targets + re.findall(r'!?\[[^\]]*\]\(([^)]+)\)', prose)
        for target in targets:
            target = target.strip('<>')
            url = urlsplit(target)
            if url.scheme or url.netloc or not url.path:
                continue
            destination = (path.parent / unquote(url.path)).resolve()
            try:
                relative = destination.relative_to(ROOT).as_posix()
            except ValueError:
                errors.append('Guide link escapes checkout: ' + str(path.relative_to(ROOT)) + ' -> ' + target)
                continue
            if not destination.exists():
                errors.append('Broken guide link: ' + str(path.relative_to(ROOT)) + ' -> ' + target)
            elif destination.is_file() and relative not in tracked:
                errors.append('Guide links to untracked file: ' + relative)
    readme = (ROOT / 'README.md').read_text()
    if 'AI_DEVELOPMENT.md' not in readme or 'https://openai.com/codex/' not in readme:
        errors.append('Development disclosure missing')
    # Current source-only milestone: no automated release publishing belongs here.
    for workflow in (ROOT / '.github/workflows').glob('*'):
        text = workflow.read_text()
        if re.search(r'gh\s+release\s+create|softprops/action-gh-release|contents:\s*write', text):
            errors.append('Unexpected release/write workflow: ' + workflow.name)
    if errors:
        print('FAIL: publication check')
        for error in errors:
            print(' - ' + error)
        return 1
    print(f'PASS: {len(tracked)} tracked files; publication paths, maintained guide links and OpenDSP hash')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
