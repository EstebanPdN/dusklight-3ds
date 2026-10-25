#!/usr/bin/env python3
import json
from pathlib import Path
import tempfile
import unittest
from azahar_run_report import inspect_run


class EvidenceTests(unittest.TestCase):
    def test_worker_barrier_and_graphics_completion_are_required(self):
        from azahar_run_report import STOP_FAILURE
        with tempfile.TemporaryDirectory() as name:
            p = Path(name)
            (p / 'process.json').write_text('{}')
            base = ('Original native frame 480 complete; drawn=10 rejected=0\n'
                    'Native developer exit after frame 480\n'
                    'Native shutdown phase=worker barrier\n')
            done = 'Native shutdown complete: workers=0 stackBytes=0 waitingQueues=0 graphics=stopped\n'
            variants = (
                (base, 'guest_failure'),
                (base + done, 'completed_smoke_test'),
                (base + done.replace('workers=0', 'workers=1'), 'guest_failure'),
                (base + 'Native shutdown failure: worker barrier incomplete; kernel process exit\n', 'guest_failure'),
                (base + done + 'Native shutdown failure: later failure\n', 'guest_failure'),
            )
            for log, status in variants:
                with self.subTest(log=log):
                    (p / 'guest.log').write_text(log)
                    self.assertEqual(inspect_run(p, 480)['status'], status)
            self.assertTrue(STOP_FAILURE.search(b'Native shutdown failure: worker barrier incomplete'))
            self.assertFalse(STOP_FAILURE.search(done.encode()))

    def test_memory_fault_after_exit_is_a_failure_and_bounded(self):
        from azahar_run_report import STOP_FAILURE
        with tempfile.TemporaryDirectory() as name:
            p = Path(name)
            (p / 'process.json').write_text('{}')
            text = ('Original native frame 480 complete; drawn=10 rejected=0\n'
                    'Native developer exit after frame 480\n')
            for address in range(20):
                text += f'HW.Memory <Error> UnmappedAccess: unmapped Read32 @ 0x{address:08x} at PC 0x00001000\n'
            (p / 'guest.log').write_text(text)
            report = inspect_run(p, 480)
            self.assertEqual(report['status'], 'guest_failure')
            self.assertEqual(len(report['problems']), 4)
            self.assertTrue(all('after requested exit' in reason for reason in report['problems']))
            self.assertTrue(STOP_FAILURE.search(text.encode()))

    def test_successful_visual_fallback_callback_is_degraded(self):
        with tempfile.TemporaryDirectory() as name:
            p = Path(name)
            (p / 'process.json').write_text('{}')
            (p / 'guest.log').write_text(
                'Native draw rejected: GX visual fallback: simplified TEV material primitive=144 vertices=212\n'
                'Original native frame 480 complete; drawn=10 rejected=0\n'
                'Native developer exit after frame 480\n')
            self.assertEqual(inspect_run(p, 480)['status'], 'completed_with_degradation')
    def test_missing_or_launch_only_is_never_pass(self):
        with tempfile.TemporaryDirectory() as name:
            p = Path(name)
            self.assertEqual(inspect_run(p)['status'], 'launch_or_evidence_failure')
            (p / 'process.json').write_text('{}')
            (p / 'guest.log').write_text('emulator started\n')
            self.assertEqual(inspect_run(p)['status'], 'launch_or_evidence_failure')

    def test_completion_requires_frame_and_requested_images(self):
        with tempfile.TemporaryDirectory() as name:
            p = Path(name)
            (p / 'process.json').write_text(json.dumps({'returncode': 0}))
            log = 'Original native frame 480 complete; drawn=100 rejected=0\n'
            (p / 'guest.log').write_text(log)
            self.assertEqual(inspect_run(p, 480)['status'], 'incomplete')
            log += 'Native developer exit after frame 480\n'
            (p / 'guest.log').write_text(log)
            self.assertEqual(inspect_run(p, 480, [480])['status'], 'missing_capture')
            self.assertEqual(inspect_run(p, 600)['status'], 'incomplete')
            self.assertEqual(inspect_run(p, 480)['status'], 'completed_smoke_test')
            (p / 'guest.log').write_text(log + 'Native draw rejected: unsupported shader\n')
            self.assertEqual(inspect_run(p, 480)['status'], 'guest_failure')

    def test_profile_frame_alignment_and_intrusive_samples(self):
        with tempfile.TemporaryDirectory() as name:
            p = Path(name)
            (p / 'process.json').write_text('{"returncode":0}')
            text = ''
            for frame, cost in ((270, 1), (300, 500), (330, 20), (360, 900), (390, 30)):
                if frame == 300:
                    text += 'Native developer dump frame=300 bytes=4 checksum=1234 written\n'
                text += f'Original native frame {frame} complete; drawn=10 rejected=0\n'
                text += f'Native frame profile: execution={cost} command=5 ms decodes=1\n'
            (p / 'guest.log').write_text(text)
            report = inspect_run(p)
            self.assertEqual(report['profiles']['execution_ms']['median'], 25)
            self.assertEqual(report['profiles']['execution_ms']['samples'], 2)

    def test_fatal_reason_from_next_prefixed_log_line(self):
        with tempfile.TemporaryDirectory() as name:
            p = Path(name)
            (p / 'process.json').write_text('{}')
            (p / 'guest.log').write_text(
                'Original native frame 240 complete; drawn=100 rejected=0\n'
                '[ 11.0] Debug.Emulated <Debug> svc.cpp:OutputDebugString:1190: Native fatal: \n'
                '[ 11.1] Debug.Emulated <Debug> svc.cpp:OutputDebugString:1190: Native GX command-state failure: GXLoadTexObj/resource\n')
            report = inspect_run(p)
            self.assertEqual(report['status'], 'guest_failure')
            self.assertEqual(report['problems'], ['fatal: Native GX command-state failure: GXLoadTexObj/resource'])

    def test_inline_fatal_and_bare_fatal_are_failures(self):
        with tempfile.TemporaryDirectory() as name:
            p = Path(name)
            (p / 'process.json').write_text('{}')
            for text, expected in (('fatal: checksum mismatch\n', 'fatal: checksum mismatch'),
                                   ('fatal:\n', 'fatal:'),
                                   ('fatal:\nOriginal native frame 600 complete; drawn=1 rejected=0\n', 'fatal:')):
                (p / 'guest.log').write_text('Original native frame 480 complete; drawn=100 rejected=0\n' + text)
                report = inspect_run(p)
                self.assertEqual(report['status'], 'guest_failure')
                self.assertIn(expected, report['problems'])

    def test_completed_with_degradation_and_fatal_is_not_filtered(self):
        with tempfile.TemporaryDirectory() as name:
            p = Path(name)
            (p / 'process.json').write_text('{}')
            text = ('Original native frame 480 complete; drawn=100 rejected=0\n'
                    'Native draw rejected: optional effect primitive=7\n'
                    'Native degraded draw: optional effect\n'
                    'Native degraded draws simplified=3 skipped=4\n'
                    'Native developer exit after frame 480\n')
            (p / 'guest.log').write_text(text)
            report = inspect_run(p, 480)
            self.assertEqual(report['status'], 'completed_with_degradation')
            self.assertEqual(report['problems'], [])
            self.assertEqual(report['degraded_counts'], [3, 4])
            (p / 'guest.log').write_text(text + 'fatal:\noptional effect\n')
            self.assertEqual(inspect_run(p, 480)['status'], 'guest_failure')

    def test_runner_failure_prevents_false_completed_report(self):
        with tempfile.TemporaryDirectory() as name:
            p = Path(name)
            (p / 'process.json').write_text('{"runner_status":"error","runner_error":{"message":"interrupted"}}')
            (p / 'guest.log').write_text('Original native frame 480 complete; drawn=100 rejected=0\n'
                                        'Native developer exit after frame 480\n')
            self.assertEqual(inspect_run(p, 480)['status'], 'runner_failure')

    def test_dump_guest_checksum_logs_are_preserved_without_verified_inference(self):
        with tempfile.TemporaryDirectory() as name:
            p = Path(name)
            (p / 'process.json').write_text('{"dumps_in_runtime":["frame-000480-textures.dstx"]}')
            evidence = ['Native developer dump frame=480 bytes=4 checksum=1234 written',
                        'Native developer textures frame=480 images=1 bytes=48 checksum=abc12345 verified',
                        'Native developer textures frame=600 FAILED: insufficient SD reserve']
            (p / 'guest.log').write_text('\n'.join(evidence))
            report = inspect_run(p)
            self.assertEqual(report['dump_guest_evidence'], evidence)
            self.assertNotIn('dumps_verified', report)


if __name__ == '__main__':
    unittest.main()
