#!/usr/bin/env python3
"""Focused offline fidelity regressions. Never attach to or launch Azahar."""
import collections
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import gdb_sampler as gdb
from test_azahar_debug import MockTransport, frame


class Clock:
    now = 100.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class SelectedContextStub:
    """Models pinned selection: Ctrl-C keeps Hg, c has a separate Hc mask."""
    deadline = None
    last_error = None

    def __init__(self, contexts, *, stop=b'T02;thread:a;', hg=b'OK', hc=b'OK',
                 interrupt_error=None, resume_error=None):
        self.contexts = list(contexts)
        self.index = 0
        self.events = []
        self.stop = stop
        self.hg = hg
        self.hc = hc
        self.interrupt_error = interrupt_error
        self.resume_error = resume_error

    def interrupt(self):
        self.events.append('interrupt')
        if self.interrupt_error:
            raise self.interrupt_error
        return self.stop

    def command(self, command):
        self.events.append(command)
        result = self.hg if command.startswith(b'Hg') else self.hc
        if isinstance(result, Exception):
            raise result
        return result

    def register(self, number):
        self.events.append(number)
        value = self.contexts[self.index][number]
        if isinstance(value, Exception):
            raise value
        return value

    def resume(self, *, wait_ack=False):
        self.events.append(('resume', wait_ack))
        if self.resume_error:
            raise self.resume_error
        self.index += 1


def context(pc=0x1948bc, lr=0, sp=0x0803d800, cpsr=0x10):
    return {15: pc, 14: lr, 13: sp, 25: cpsr}


class FidelityTests(unittest.TestCase):
    def sample(self, stub, count):
        clock = Clock()
        with patch.object(gdb.time, 'monotonic', clock.monotonic), \
                patch.object(gdb.time, 'sleep', clock.sleep):
            return gdb.sample(stub, count + 0.5, 1, lambda: stub.index < count)

    def test_identical_318_saved_contexts_are_not_accepted_as_hotspot(self):
        stub = SelectedContextStub([context()] * 318)
        pcs, pairs = self.sample(stub, 318)
        self.assertIsInstance(pcs, collections.Counter)
        self.assertEqual(pcs, {0x1948bc: 318})
        self.assertEqual(pairs, {(0x1948bc, 0): 318})
        quality = pcs.sample_quality
        self.assertFalse(quality['statistical_profile_valid'])
        self.assertTrue(quality['identical_pc'])
        self.assertEqual(quality['longest_identical_pc_run'], 318)
        self.assertEqual(quality['context_changes'], 0)
        self.assertEqual(quality['null_lr_observations'], 318)
        self.assertEqual(quality['selected_threads'], {'a': 318})
        self.assertIn('unproven', stub.last_error)

    def test_explicit_selection_then_all_thread_acknowledged_resume(self):
        stub = SelectedContextStub([context(lr=0x1235)])
        pcs, pairs = self.sample(stub, 1)
        self.assertEqual(stub.events, ['interrupt', b'Hga', 15, 14, 13, 25,
                                       b'Hc-1', ('resume', True)])
        self.assertEqual(pairs, {(0x1948bc, 0x1234): 1})
        self.assertIsNone(stub.last_error)
        self.assertEqual(pcs.sample_quality['resume_acknowledged'], 1)
        self.assertEqual(pcs.sample_quality['first_context']['lr'], 0x1235)

    def test_context_variation_is_recorded_without_fabricating_cpu_progress(self):
        stub = SelectedContextStub([context(pc=4), context(pc=8), context(pc=8, sp=12)])
        pcs, _ = self.sample(stub, 3)
        quality = pcs.sample_quality
        self.assertEqual(quality['context_changes'], 2)
        self.assertEqual(quality['longest_identical_pc_run'], 2)
        self.assertEqual(quality['unique_pcs'], 2)
        self.assertAlmostEqual(quality['dominant_pc_fraction'], 2/3)
        self.assertFalse(quality['statistical_profile_valid'])
        self.assertIn('unavailable', quality['execution_progress'])
        self.assertIsNone(stub.last_error)

    def test_different_lr_with_identical_pc_still_rejects_hotspot_claim(self):
        stub = SelectedContextStub([context(lr=4), context(lr=8), context(lr=12)])
        pcs, _ = self.sample(stub, 3)
        self.assertEqual(pcs.sample_quality['context_changes'], 2)
        self.assertIn('Identical PC', stub.last_error)

    def test_missing_stop_identity_never_reads_registers(self):
        for stop in (b'S02', b'T02', b'T02reason:signal;'):
            stub = SelectedContextStub([context()], stop=stop)
            self.assertEqual(self.sample(stub, 1), ({}, {}))
            self.assertNotIn(15, stub.events)
            self.assertEqual(stub.events[-2:], [b'Hc-1', ('resume', True)])
            self.assertIn('identify', stub.last_error)

    def test_stop_identity_validation_and_no_hg0_guess(self):
        self.assertEqual(gdb.stopped_thread(b'T020f:12345678;thread:000A;'), b'a')
        for stop in (b'OK', b'T02thread:0;', b'T02thread:-1;', b'T02thread:p1.2;',
                     b'T02thread:80000000;', b'T02thread:1;thread:2;', b'T02thread:;',
                     b'T02thread:123456789;'):
            with self.subTest(stop=stop), self.assertRaises(gdb.GDBError):
                gdb.stopped_thread(stop)

    def test_selection_unsupported_remote_and_malformed_errors_are_retained(self):
        for reply, error in ((b'', 'unsupported'), (b'no', 'failed'),
                             (gdb.GDBRemoteError(b'E01'), 'E01')):
            stub = SelectedContextStub([context()], hg=reply)
            self.assertEqual(self.sample(stub, 1), ({}, {}))
            self.assertIn(error, stub.last_error)
            self.assertNotIn(15, stub.events)
            self.assertEqual(stub.events[-1], ('resume', True))

    def test_capture_failure_keeps_primary_when_resume_also_fails(self):
        stub = SelectedContextStub([context()], interrupt_error=gdb.GDBProtocolError('bad checksum'),
                                   hc=gdb.GDBRemoteError(b'E02'), resume_error=EOFError('lost resume'))
        pcs, _ = self.sample(stub, 1)
        self.assertEqual(stub.last_error, 'bad checksum')
        self.assertEqual(pcs.sample_quality['last_error'], 'bad checksum')
        self.assertEqual(len(pcs.sample_quality['resume_errors']), 2)
        self.assertEqual(stub.events[-1], ('resume', True))

    def test_resume_failure_discards_unconfirmed_observation(self):
        for hc, resume in ((b'', None), (b'OK', gdb.GDBTimeout('continue ack timeout'))):
            stub = SelectedContextStub([context()], hc=hc, resume_error=resume)
            self.assertEqual(self.sample(stub, 1), ({}, {}))
            self.assertIsNotNone(stub.last_error)
            self.assertEqual(stub.events[-1], ('resume', True))

    def test_unavailable_pc_and_lr_remain_distinct(self):
        stub = SelectedContextStub([context(pc=None)])
        self.assertEqual(self.sample(stub, 1), ({}, {}))
        self.assertIn('PC unavailable', stub.last_error)
        stub = SelectedContextStub([context(lr=None)])
        pcs, pairs = self.sample(stub, 1)
        self.assertEqual(pcs, {0x1948bc: 1})
        self.assertEqual(pairs, {})
        self.assertEqual(pcs.sample_quality['unavailable_lr_observations'], 1)

    def test_register_error_and_keyboard_interrupt_resume(self):
        stub = SelectedContextStub([context(lr=gdb.GDBRemoteError(b'E0e'))])
        self.assertEqual(self.sample(stub, 1), ({}, {}))
        self.assertIn('E0e', stub.last_error)
        stub = SelectedContextStub([context()], interrupt_error=KeyboardInterrupt())
        with self.assertRaises(KeyboardInterrupt):
            self.sample(stub, 1)
        self.assertEqual(stub.events[-1], ('resume', True))
        self.assertIsNone(stub.deadline)

    def test_zero_duration_dead_guest_and_stale_error_reset(self):
        for duration, alive in ((0, lambda: True), (1, lambda: False)):
            stub = SelectedContextStub([])
            stub.last_error = 'old diagnostic failed'
            self.assertEqual(gdb.sample(stub, duration, 0.01, alive), ({}, {}))
            self.assertEqual(stub.events, [])
            self.assertIsNone(stub.last_error)
        for duration, interval in ((-1, 1), (float('inf'), 1), (1, 0), (1, float('nan'))):
            with self.assertRaises(ValueError):
                gdb.sample(SelectedContextStub([]), duration, interval, lambda: True)

    def test_sleep_and_capture_respect_earlier_caller_deadline(self):
        clock = Clock()
        stub = SelectedContextStub([context()])
        stub.deadline = 100.25
        with patch.object(gdb.time, 'monotonic', clock.monotonic), \
                patch.object(gdb.time, 'sleep', clock.sleep):
            pcs, _ = gdb.sample(stub, 5, 10, lambda: True)
        self.assertEqual(clock.now, 100.25)
        self.assertEqual(stub.deadline, 100.25)
        self.assertEqual(sum(pcs.values()), 1)

    def test_real_transport_partial_stop_has_sampling_deadline_and_bounded_cleanup(self):
        transport = MockTransport(b'$T02', delay=0.02)
        stub = gdb.Stub(1, sock=transport, timeout=0.03)
        start = time.monotonic()
        pcs, _ = gdb.sample(stub, 0.01, 5, lambda: True)
        self.assertLess(time.monotonic() - start, 0.25)
        self.assertEqual(pcs, {})
        self.assertIn('timed out', stub.last_error)
        self.assertIn(frame(b'Hc-1'), transport.sent)
        self.assertEqual(transport.sent[-1], frame(b'c'))
        self.assertIsNone(stub.deadline)

    def test_pinned_protocol_sequence_on_real_transport(self):
        def little(value):
            return value.to_bytes(4, 'little').hex().encode()
        replies = [b'T020f:bc481900;0d:00d80308;0e:00000000;thread:a;', b'OK',
                   little(0x1948bc), little(0), little(0x0803d800), little(0x10), b'OK']
        transport = MockTransport(*(b'+' + frame(reply) for reply in replies), b'+')
        stub = gdb.Stub(1, sock=transport)
        calls = iter((True, False))
        pcs, _ = gdb.sample(stub, 1, 0.001, lambda: next(calls))
        self.assertEqual(pcs, {0x1948bc: 1})
        self.assertEqual([b for b in transport.sent if b != b'+'],
                         [b'\x03', frame(b'Hga'), frame(b'pf'), frame(b'pe'),
                          frame(b'pd'), frame(b'p19'), frame(b'Hc-1'), frame(b'c')])
        self.assertEqual(transport.chunks, collections.deque())


class SymbolTests(unittest.TestCase):
    def render(self, pcs, pairs, *, stdout='', returncode=0, error=None):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'profile.txt'
            with patch.object(gdb.subprocess, 'run', side_effect=error,
                              return_value=SimpleNamespace(stdout=stdout, returncode=returncode)) as run:
                gdb.symbolize('mock.elf', pcs, pairs, output)
            self.assertEqual(run.call_args.kwargs['timeout'], gdb.NM_TIMEOUT)
            self.assertIn('-S', run.call_args.args[0])
            return (output.read_text(), json.loads(output.with_suffix('.quality.json').read_text()),
                    output.with_suffix('.samples').read_text())

    def test_null_lr_and_symbol_gaps_are_raw_not_previous_weak_symbol(self):
        report, quality, raw = self.render({0x1948bc: 318}, {(0x1948bc, 0): 318}, stdout=(
            '00000000 00000004 W currentTargetExtent\n'
            '00194800 00000100 T encodeColorCopy\n'))
        self.assertIn('encodeColorCopy', report)
        self.assertIn('<-  0x0', report)
        self.assertNotIn('currentTargetExtent', report)
        self.assertIn('IDENTICAL PC', report)
        self.assertIn('NOT statistical', report)
        self.assertNotIn('# self time', report)
        self.assertFalse(quality['statistical_profile_valid'])
        self.assertEqual(raw, '001948bc 00000000 318\n')
        report, _, _ = self.render({0x200: 1}, {(0x200, 0x100): 1},
                                   stdout='00000100 00000004 T tiny\n')
        self.assertIn('0x200', report)
        self.assertIn('tiny', report)

    def test_quality_carried_without_changing_counter_or_symbolize_api(self):
        pcs = gdb.SampleCounts({4: 1, 8: 1})
        pcs.sample_quality = {'context_changes': 1, 'resume_acknowledged': 2}
        _, quality, _ = self.render(pcs, {})
        self.assertEqual(quality['context_changes'], 1)
        self.assertEqual(quality['resume_acknowledged'], 2)
        self.assertFalse(quality['statistical_profile_valid'])

    def test_symbol_tool_error_preserves_raw_evidence(self):
        for error, code in ((subprocess.TimeoutExpired('nm', 10), 0),
                            (FileNotFoundError('nm absent'), 0), (None, 2)):
            with self.subTest(error=error, code=code):
                report, quality, raw = self.render({4: 1}, {(4, 0): 1},
                                                   returncode=code, error=error)
                self.assertIn('0x4', report)
                self.assertIn('nm', quality['symbolization_error'])
                self.assertEqual(raw, '00000004 00000000 1\n')

    def test_zero_samples_and_malformed_symbol_lines(self):
        report, quality, _ = self.render({}, {}, stdout='broken line\nnothex 10 T bad\n')
        self.assertTrue(report.startswith('samples=0\n'))
        self.assertFalse(quality['identical_pc'])
        self.assertEqual(quality['dominant_pc_fraction'], 0)

    def test_actual_timeout_kills_and_reaps_nm_child(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            pid_file = root / 'pid'
            executable = root / 'mock-nm'
            executable.write_text(f'#!{sys.executable}\nimport os, time\n'
                                  f'open({str(pid_file)!r}, "w").write(str(os.getpid()))\n'
                                  'time.sleep(30)\n')
            executable.chmod(0o700)
            start = time.monotonic()
            with patch.object(gdb, 'NM', executable), patch.object(gdb, 'NM_TIMEOUT', 0.3):
                gdb.symbolize('mock.elf', {4: 1}, {}, root / 'profile.txt')
            self.assertLess(time.monotonic() - start, 2)
            pid = int(pid_file.read_text())
            with self.assertRaises(ProcessLookupError):
                os.kill(pid, 0)
            with self.assertRaises(ChildProcessError):
                os.waitpid(pid, os.WNOHANG)
            quality = json.loads((root / 'profile.quality.json').read_text())
            self.assertIn('TimeoutExpired', quality['symbolization_error'])


if __name__ == '__main__':
    unittest.main()
