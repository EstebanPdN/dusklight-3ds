#!/usr/bin/env python3
"""Complete exact gitlink revisions without creating remotes, branches or commits."""
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
PIN = '422d7bb1b6c8d973cccf8b3d0b226a57ac3cc8c7'

def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args], text=True).strip()

def complete(path):
    entries = []
    modules = path / '.gitmodules'
    if modules.exists():
        for line in git(path, 'config', '-f', '.gitmodules', '--get-regexp', r'^submodule\..*\.path$').splitlines():
            key, rel = line.split(' ', 1)
            url = git(path, 'config', '-f', '.gitmodules', '--get', key[:-4] + 'url')
            if not url.startswith('https://github.com/'):
                raise RuntimeError('Review non-GitHub or relative submodule URL: ' + url)
            target = path / rel
            assert target.resolve().is_relative_to(ROOT / 'Source')
            tree = git(path, 'ls-tree', 'HEAD', '--', rel).split()
            assert tree[0] == '160000' and tree[1] == 'commit'
            revision = tree[2]
            if not (target / '.git').exists():
                target.mkdir(parents=True, exist_ok=True)
                assert not list(target.iterdir()), 'Refusing nonempty non-Git destination'
                git(target, '-c', 'init.defaultBranch=unused', 'init')
            try:
                git(target, 'cat-file', '-e', revision + '^{commit}')
            except subprocess.CalledProcessError:
                subprocess.run(['git', '-C', str(target), '-c', 'http.version=HTTP/1.1', 'fetch', '--depth=1', '--no-tags', url, revision], check=True)
            # Existing modified work must never be overwritten on reruns.
            if git(target, 'status', '--porcelain', '--untracked-files=no'):
                raise RuntimeError('Dirty submodule: ' + str(target))
            git(target, 'checkout', '--detach', revision)
            assert git(target, 'rev-parse', 'HEAD') == revision
            entries.append({'path': str(target.relative_to(ROOT)), 'revision': revision, 'url': url})
            entries.extend(complete(target))
            git(path, 'config', key[:-4] + 'active', 'true')
    return entries

if __name__ == '__main__':
    source = ROOT / 'Source'
    assert git(source, 'rev-parse', 'HEAD') == PIN
    entries = [{'path': 'Source', 'revision': PIN}] + complete(source)
    out = ROOT / 'Docs/3ds/source_checkout_receipt.json'
    out.write_text(json.dumps({'schema': 1, 'pins': entries}, indent=2) + '\n')
    print('Verified', len(entries), 'local checkouts;', out)
