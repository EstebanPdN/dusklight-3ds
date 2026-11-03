#!/usr/bin/env python3
"""Synthetic regression for stage alignment and non-additive timing evidence."""
import unittest

from analyze_frame_profile import analyze, parse_lines


class FrameProfileTest(unittest.TestCase):
    def test_transitions_overlap_and_window_means(self):
        lines = '''Native static geometry resident bytes=2097152
Native renderer memory: game_free=100 linear_free=200 bytes
Original native frame 1 complete; drawn=1 rejected=0
Native audio execution=90.000 ms
Native frame profile: execution=100 command=0 texture=0 decode=0 vertex=0 submit=0 wait=0 ms decodes=0 bytes=0 evictions=0
Original world stage= room=0 player=0x0
Native ordinary frame window: count=28 execution_mean=10 loop_mean=20 ms detailed=sparse
Original native frame 30 complete; drawn=10 rejected=0
Native frame profile: execution=12 command=8 texture=3 decode=2 vertex=4 submit=1 wait=1 ms decodes=1 bytes=64 evictions=0
Original world stage= room=0 player=0x0
Native ordinary frame window: count=29 execution_mean=40 loop_mean=50 ms detailed=sparse
Native renderer memory: game_free=80 linear_free=160 bytes
Original native frame 60 complete; drawn=20 rejected=0
Native painter parent 35.000 ms calls=1
Native painter child 30.000 ms calls=2
Native audio worker elapsed=25.000 ms blocks=5 (overlaps main profiling)
Native topology cache hits=100 misses=10 bytes=1024
Native recent decode handle=1 8x8 gxfmt=9 picabytes=256 dirty 2.000 ms
Native frame profile: execution=40 command=30 texture=4 decode=2 vertex=15 submit=1 wait=1 ms decodes=1 bytes=256 evictions=0
Original world stage=A room=0 player=0x123
Native ordinary frame window: count=29 execution_mean=20 loop_mean=30 ms detailed=sparse
Original native frame 90 complete; drawn=30 rejected=0
Native topology cache hits=118 misses=12 bytes=1024
Native frame profile: execution=25 command=18 texture=4 decode=2 vertex=10 submit=1 wait=1 ms decodes=1 bytes=256 evictions=0
Native audio worker elapsed=0.000 ms blocks=0 (overlaps main profiling)
Original world stage=A room=0 player=0x123
Native ordinary frame window: count=29 execution_mean=30 loop_mean=40 ms detailed=sparse
Original native frame 120 complete; drawn=40 rejected=0
Native topology cache hits=120 misses=12 bytes=1024
Native frame profile: execution=NaN command=18 texture=4 decode=2 vertex=10 submit=1 wait=1 ms decodes=1 bytes=256 evictions=0
Original world stage=A room=0 player=0x123
Native ordinary frame window: count=29 execution_mean=50 loop_mean=60 ms detailed=sparse
Original native frame 150 complete; drawn=50 rejected=0
Native topology cache hits=2 misses=1 bytes=1024
Original world stage=A room=1 player=0x123
Original native frame 1 complete; drawn=1 rejected=0
Native topology cache hits=0 misses=0 bytes=1024
Original world stage=A room=1 player=0x123
'''.splitlines()
        records, diagnostics = parse_lines(lines)
        report = analyze(records, diagnostics, details=True)
        startup, stage, room, restart = report['segments']
        self.assertEqual(startup['phase'], 'cold_startup')
        self.assertEqual(startup['initial_frame_components_ms']['audio_main']['execution'], 90)
        self.assertEqual(startup['ordinary_window_means']['represented_frames'], 28)
        self.assertEqual(stage['observed_frames'], [60, 90, 120])
        self.assertEqual(records[2]['memory_bytes']['linear_free'], 160)
        self.assertEqual([w['ending_frame'] for w in report['transition_or_unassigned_window_means']], [60, 150])
        stats = stage['ordinary_window_means']['loop_ms']
        self.assertEqual(stats['p50_window_mean_ms'], 35)
        self.assertEqual(stats['p95_window_mean_ms'], 39.5)
        self.assertEqual(stage['sampled_components_ms']['frame']['execution']['mean'], 32.5)
        painters = stage['top_observed_inclusive_painters_ms']
        self.assertEqual(painters['parent']['mean'], 35)
        self.assertEqual(painters['child']['mean'], 30)
        self.assertEqual(stage['audio_worker_overlapping']['elapsed_ms_per_block'], 5)
        self.assertEqual(stage['caches']['topology cache']['counter_delta'], {'hits': 20, 'misses': 2})
        self.assertEqual(stage['caches']['topology cache']['delta_hit_fraction'], 0.909091)
        self.assertIsNone(room['caches']['topology cache']['counter_delta'])
        self.assertIsNone(restart['caches']['topology cache']['counter_delta'])
        self.assertEqual(restart['session'], 1)
        self.assertEqual(diagnostics['malformed_lines'], 1)
        self.assertIsNone(report['gpu_time_ms'])
        self.assertIsNone(report['individual_uninstrumented_frame_intervals_ms'])
        perf, errors = parse_lines('''frame=1 stage= loop_ms=0.00 fps=0.0 exec_ms=0.00 drawn=1 rejected=0 linear_free_kib=10
frame=30 stage=A loop_ms=40.00 fps=25.0 exec_ms=20.00 drawn=10 rejected=0 linear_free_kib=9
frame=60 stage=A loop_ms=50.00 fps=20.0 exec_ms=30.00 drawn=20 rejected=0 linear_free_kib=8
'''.splitlines())
        perf_report = analyze(perf, errors)
        self.assertNotIn('window', perf[0])
        self.assertIsNone(perf_report['segments'][1]['ordinary_window_means']['represented_frames'])
        self.assertEqual(perf_report['segments'][1]['ordinary_window_means']['loop_ms']['p95_window_mean_ms'], 50)


if __name__ == '__main__':
    unittest.main()
