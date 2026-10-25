#!/usr/bin/env python3
"""Regression checks for timing evidence alignment and exclusions."""
import tempfile
import unittest
from pathlib import Path
from compare_native_profiles import read_profiles, read_windows, matched_windows


class ComparisonTest(unittest.TestCase):
    def test_window_belongs_to_next_frame(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'guest.log'
            path.write_text('Native ordinary frame window: count=29 execution_mean=288.1 loop_mean=318.4 ms detailed=all\n'
                            'Native renderer memory: game_free=1\n'
                            'Original native frame 420 complete; drawn=17\n'
                            'Native frame profile: execution=302.1\n'
                            'Native developer capture frame=450 written\n'
                            'Original native frame 450 complete; drawn=18\n'
                            'Native frame profile: execution=303.1\n')
            self.assertEqual(list(read_windows(path)), [420])
            self.assertEqual(read_windows(path)[420]['count'], 29)
            self.assertEqual([(r['frame'], r['diagnostic']) for r in read_profiles(path)],
                             [(420, False), (450, True)])

    def test_mismatch_is_excluded(self):
        window = {420: {'count': 29, 'execution_ms': 280., 'loop_ms': 310., 'detailed': 'all'}}
        other = {420: {'count': 29, 'execution_ms': 270., 'loop_ms': 300., 'detailed': 'sparse'}}
        profile = [{'frame': 420, 'world': 'same', 'geometry': {'vertices': 12}}]
        self.assertEqual(matched_windows(window, other, profile, profile)[0]['saved_ms']['loop_ms'], 10.)
        other[420]['count'] = 28
        self.assertEqual(matched_windows(window, other, profile, profile), [])
        other[420]['count'] = 29
        self.assertEqual(matched_windows(window, other, profile, [{'frame': 420, 'world': 'different'}]), [])


if __name__ == '__main__':
    unittest.main()
