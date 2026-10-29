"""Private fixture protection after the public repository moved to ROOT."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import run_azahar_smoke as smoke


class PrivateFixtureGitTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        subprocess.run(['git', 'init', '--quiet', str(self.root)], check=True)
        (self.root / '.gitignore').write_text('/Build/\n/Diagnostics/\n/Saves/\n')
        self.mock_root = patch.object(smoke, 'ROOT', self.root)
        self.mock_root.start()
        self.addCleanup(self.mock_root.stop)
        self.source = self.root / 'Build' / 'private' / 'card.bin'
        self.source.parent.mkdir(parents=True)
        self.source.write_bytes(b'private fixture')

    def test_ignored_source_and_new_destination_are_allowed(self):
        with smoke.card_private_parent(self.source) as (parent, name, path):
            self.assertEqual(os.stat(name, dir_fd=parent).st_size, 15)
            self.assertEqual(path, self.source)
        target = self.root / 'Diagnostics' / 'new' / 'sd' / 'card.bin'
        with smoke.card_private_parent(target, create=True) as (parent, name, _):
            fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600, dir_fd=parent)
            os.close(fd)
        self.assertEqual(target.read_bytes(), b'')

    def test_force_tracked_fixture_is_refused_even_when_rule_matches(self):
        subprocess.run(['git', '-C', str(self.root), 'add', '-f', '--',
                        'Build/private/card.bin'], check=True)
        with self.assertRaisesRegex(ValueError, 'untracked and ignored'):
            with smoke.card_private_parent(self.source):
                self.fail('Tracked private input admitted')
        self.assertEqual(self.source.read_bytes(), b'private fixture')

    def test_unignored_new_path_is_refused_before_mkdir(self):
        (self.root / '.gitignore').write_text('')
        target = self.root / 'Diagnostics' / 'must-not-exist' / 'card.bin'
        with self.assertRaisesRegex(ValueError, 'untracked and ignored'):
            with smoke.card_private_parent(target, create=True):
                self.fail('Unignored output admitted')
        self.assertFalse(target.parent.exists())

    def test_nested_repository_markers_remain_refused(self):
        for directory in (False, True):
            marker = self.source.parent / '.git'
            if directory:
                marker.mkdir()
            else:
                marker.write_text('gitdir: nested-worktree')
            with self.assertRaisesRegex(ValueError, 'Git repository'):
                with smoke.card_private_parent(self.source):
                    self.fail('Nested repository admitted')
            marker.rmdir() if directory else marker.unlink()

    def test_ignored_parent_symlink_remains_refused(self):
        alias = self.root / 'Build' / 'alias'
        alias.symlink_to(self.source.parent, target_is_directory=True)
        with self.assertRaises((OSError, ValueError)):
            with smoke.card_private_parent(alias / self.source.name):
                self.fail('Parent symlink followed')

    def test_git_check_failure_refuses_before_mkdir(self):
        target = self.root / 'Diagnostics' / 'must-not-exist' / 'card.bin'
        with patch.object(smoke.subprocess, 'run', side_effect=OSError('git unavailable')):
            with self.assertRaisesRegex(ValueError, 'Cannot verify'):
                with smoke.card_private_parent(target, create=True):
                    self.fail('Unverified output admitted')
        self.assertFalse(target.parent.exists())


if __name__ == '__main__':
    unittest.main()
