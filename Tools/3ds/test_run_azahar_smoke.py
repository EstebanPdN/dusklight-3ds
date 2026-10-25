#!/usr/bin/env python3
"""Offline lifecycle tests: no real emulator, fork, signal, or debugger socket."""
from contextlib import ExitStack, redirect_stdout, redirect_stderr
import hashlib
import io
import os
import json
from pathlib import Path
import signal
import struct
import zlib
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import Mock, patch

import run_azahar_smoke as runner

DIAGNOSTICS = runner.ROOT / 'Diagnostics/implementation-44/harness'


def native_card(payload=32768):
    # ARM32 serialized layout, deliberately independent of host sizeof/alignment.
    header = bytearray(120)
    header[:8] = b'DSVCARD1'
    header[8:12] = (1).to_bytes(4, 'little')
    header[12:20] = b'fixture\0'
    header[44:48] = payload.to_bytes(4, 'little')
    return bytes(header) + bytes(payload)


class Clock:
    now = 0.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class HarnessTests(unittest.TestCase):
    def setUp(self):
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.root = Path(self.stack.enter_context(tempfile.TemporaryDirectory(dir=DIAGNOSTICS)))
        self.image = self.root / 'image.cxi'
        self.image.write_bytes(b'offline test image')
        self.out = self.root / 'evidence/test'
        self.runtime = self.root / 'Build/azahar-runs/test'
        self.clock = Clock()
        self.events = []
        self.handlers = {sig: signal.SIG_DFL for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
        self.old_handlers = dict(self.handlers)
        self.stack.enter_context(patch.object(runner, 'ROOT', self.root))
        self.stack.enter_context(patch.object(runner.time, 'monotonic', self.clock.monotonic))
        self.sleep = self.stack.enter_context(patch.object(runner.time, 'sleep', side_effect=self.clock.sleep))
        self.stack.enter_context(patch.object(runner.signal, 'signal', side_effect=self.set_handler))
        self.stack.enter_context(patch.object(runner.subprocess, 'run', side_effect=self.subprocess))
        self.process = Mock(pid=12345, identity='12300 Wed Oct 21 00:00:00 2026', returncode=None)
        self.process.start.side_effect = self.start
        self.process.poll.return_value = None
        self.process.terminate.side_effect = self.terminate
        self.stack.enter_context(patch.object(runner, 'DetachedProcess', return_value=self.process))
        self.stub = Mock(last_error=None, deadline=None)
        self.stub.command.return_value = b'T05;thread:1;'
        self.stub.require_stop.side_effect = lambda reply: reply
        self.stub.resume.side_effect = lambda **kwargs: self.events.append('resume')
        self.stub.close.side_effect = lambda: self.events.append('close')
        self.stub_factory = self.stack.enter_context(patch.object(runner.gdb_sampler, 'Stub', return_value=self.stub))
        self.stack.enter_context(patch.object(runner.azahar_debug, 'capabilities', return_value={'features': {}}))
        self.inventory = self.stack.enter_context(patch.object(runner.azahar_debug, 'inventory', side_effect=self.inventory_result))
        self.sample = self.stack.enter_context(patch.object(runner.gdb_sampler, 'sample', side_effect=self.sample_result))
        self.stack.enter_context(patch.object(runner.gdb_sampler, 'symbolize'))
        self.log = ('Original native frame 480 complete; drawn=100 rejected=0\n'
                    'Native developer dump frame=480 bytes=4 checksum=1234 written\n'
                    'Native developer textures frame=480 images=1 bytes=48 checksum=abc12345 verified\n'
                    'Native developer exit after frame 480\n')

    def set_handler(self, signum, handler):
        old = self.handlers[signum]
        self.handlers[signum] = handler
        return old

    def subprocess(self, command, **kwargs):
        self.assertIn(command[0], ('pgrep', 'ps'))
        return SimpleNamespace(stdout='' if command[0] == 'pgrep' else '42 1.0', returncode=0)

    def start(self, command, runtime, env, log_path):
        log = runtime / 'xdg-data/azahar-emu/log/azahar_log.txt'
        log.parent.mkdir(parents=True)
        log.write_text(self.log)
        dumps = runtime / 'xdg-data/azahar-emu/sdmc/3ds/dusklight/dumps'
        dumps.mkdir(parents=True)
        for name in ('frame-000480.bin', 'frame-000480-textures.dstx', 'ignored.txt'):
            (dumps / name).write_bytes(b'unverified')
        self.events.append('start')

    def terminate(self):
        self.events.append('terminate')
        self.process.returncode = 'exited (mock)'

    def inventory_result(self, *args):
        self.events.append('inventory')
        return {'status': 'available', 'threads': []}

    def sample_result(self, *args):
        self.events.append('sample')
        return {123: 2}, {(123, 321): 2}

    def run_main(self, *extra):
        with redirect_stdout(io.StringIO()):
            return runner.main(['--name', 'test', '--image', str(self.image), '--seconds', '10',
                                '--exit-frame', '480', '--evidence-root', str(self.out.parent), *extra])

    def test_relative_emulator_path_is_resolved_before_isolated_launch(self):
        relative = Path('Tools/relative azahar.app/Contents/MacOS/azahar')
        self.assertEqual(self.run_main('--azahar', str(relative)), 0)
        command = self.process.start.call_args.args[0]
        self.assertEqual(command[3], str(relative.resolve()))
        self.assertTrue(Path(command[3]).is_absolute())

    def test_long_gameplay_duration_is_explicit_and_bounded(self):
        self.assertEqual(self.run_main('--long-gameplay', '--seconds', '3600'), 0)
        self.assert_cleaned()

    def test_ordinary_duration_still_rejects_over_ten_minutes(self):
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.run_main('--seconds', '601')
        self.process.start.assert_not_called()

    def test_long_gameplay_rejects_over_one_hour(self):
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.run_main('--long-gameplay', '--seconds', '3601')
        self.process.start.assert_not_called()

    def receipt(self):
        return json.loads((self.out / 'process.json').read_text())

    def assert_cleaned(self):
        self.process.terminate.assert_called_once()
        self.process.close.assert_called_once()
        self.assertEqual(self.handlers, self.old_handlers)

    def make_bundle(self):
        # Independent, tiny GZ2E01 header/DOL/FST with one retained payload.
        raw = bytearray(0x3000)
        raw[:8] = b'GZ2E01\0\0'
        for offset, value in ((0x1c, 0xc2339f3d), (0x420, 0x500),
                              (0x424, 0x1000), (0x428, 64),
                              (0x500, 256), (0x590, 16)):
            struct.pack_into('>I', raw, offset, value)
        struct.pack_into('>III', raw, 0x1000, 0x1000000, 0, 2)
        struct.pack_into('>III', raw, 0x100c, 0, 0x2000, 4)
        raw[0x1018:0x101e] = b'A.bin\0'
        raw[0x2000:0x2004] = b'DATA'
        ranges = ((0, 0x440), (0x500, 0x110), (0x1000, 64), (0x2000, 4))
        cursor = 128 + len(ranges) * 64
        index, payload = bytearray(), bytearray()
        for lo, count in ranges:
            piece = raw[lo:lo+count]
            index += struct.pack('>QQQ32s8x', lo, cursor, count, hashlib.sha256(piece).digest())
            payload += piece
            cursor += count
        header = bytearray(struct.pack('>8sIIQQIIQQII32s32x', b'DLTWPK01', 1, 128,
                                      len(raw), cursor, len(ranges), 64, 128,
                                      128 + len(index), zlib.crc32(index), 0,
                                      hashlib.sha256(raw).digest()))
        struct.pack_into('>I', header, 60, zlib.crc32(header))
        dest = self.root / 'Diagnostics/zelda.twilight'
        dest.parent.mkdir()
        dest.write_bytes(header + index + payload)
        return dest

    def test_bundle_only_staging_never_needs_iso(self):
        bundle = self.make_bundle()
        before = bundle.read_bytes()
        self.assertFalse((self.root / 'twilight.iso').exists())
        self.assertEqual(self.run_main('--data-bundle', str(bundle), '--boot-stage', 'R_SP01,4,0,-1'), 0)
        sd = self.runtime / 'xdg-data/azahar-emu/sdmc/3ds/DuskLight'
        self.assertEqual((sd / 'zelda.twilight').read_bytes(), before)
        self.assertFalse((sd / 'twilight.iso').exists())
        self.assertEqual(bundle.read_bytes(), before)
        self.assertEqual(self.receipt()['data_bundle']['sha256'], hashlib.sha256(before).hexdigest())
        self.assertEqual(self.receipt()['private_iso'], 'absent test')
        self.assert_cleaned()

    def test_bundle_payload_corruption_refuses_launch(self):
        bundle = self.make_bundle()
        raw = bytearray(bundle.read_bytes()); raw[-1] ^= 1; bundle.write_bytes(raw)
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.run_main('--data-bundle', str(bundle))
        self.process.start.assert_not_called()

    def test_bundle_symlink_refuses_launch(self):
        bundle = self.make_bundle()
        link = bundle.with_name('alias.twilight'); link.symlink_to(bundle)
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.run_main('--data-bundle', str(link))
        self.process.start.assert_not_called()

    def test_bundle_mutation_is_reported_after_owned_cleanup(self):
        bundle = self.make_bundle()
        def mutate():
            self.terminate()
            raw = bytearray(bundle.read_bytes()); raw[-1] ^= 1; bundle.write_bytes(raw)
        self.process.terminate.side_effect = mutate
        with self.assertRaisesRegex(RuntimeError, 'bundle_integrity_check'):
            self.run_main('--data-bundle', str(bundle))
        self.assertEqual(self.receipt()['runner_status'], 'error')
        self.assertTrue(any(e['step'] == 'bundle_integrity_check' for e in self.receipt()['cleanup_errors']))
        self.assert_cleaned()

    def test_bundle_and_iso_options_are_exclusive(self):
        bundle = self.make_bundle()
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.run_main('--private-disc', '--data-bundle', str(bundle))
        self.process.start.assert_not_called()

    def test_dump_chord_is_explicit_and_isolated(self):
        self.assertEqual(self.run_main('--dump-chord-on-a'), 0)
        config = (self.runtime/'xdg-config/azahar-emu/qt-config.ini').read_text()
        for button in ('a','l','r'):
            self.assertIn(f'profiles\\1\\button_{button}="code:65,engine:keyboard"', config)
            self.assertIn(f'profiles\\1\\button_{button}\\default=false', config)
        self.assertTrue(self.receipt()['dump_chord_on_a'])
        self.assert_cleaned()

    def test_success_and_dump_listing_not_verification(self):
        self.assertEqual(self.run_main(), 0)
        self.assert_cleaned()
        receipt = self.receipt()
        self.assertEqual(receipt['runner_status'], 'complete')
        self.assertTrue(receipt['guest_final_log_scanned'])
        self.assertTrue(receipt['guest_exit_requested'])
        self.assertFalse(receipt['guest_shutdown_complete_reported'])
        self.assertEqual(len(receipt['dumps_in_runtime']), 2)
        self.assertTrue(any(p.endswith('.dstx') for p in receipt['dumps_in_runtime']))
        self.assertIn('listing only', receipt['dump_verification'])
        report = json.loads((self.out / 'analysis.json').read_text())
        self.assertEqual(report['status'], 'completed_smoke_test')
        self.assertIn('checksum=abc12345 verified', report['dump_guest_evidence'][1])
        self.stub_factory.assert_not_called()

    def test_fault_appended_during_cleanup_is_in_final_receipt(self):
        def late_fault():
            self.terminate()
            log = self.runtime / 'xdg-data/azahar-emu/log/azahar_log.txt'
            with log.open('a') as stream:
                stream.write('HW.Memory <Error> unmapped Read32 @ 0x0C092020 at PC 0x001B8140\n')
        self.process.terminate.side_effect = late_fault
        self.assertEqual(self.run_main(), 0)  # Historic launcher exit convention.
        self.assert_cleaned()
        receipt = self.receipt()
        self.assertTrue(receipt['guest_exit_requested'])
        self.assertTrue(receipt['guest_fatal_detected'])
        self.assertTrue(receipt['guest_final_log_scanned'])
        self.assertFalse(receipt['guest_shutdown_complete_reported'])
        report = json.loads((self.out / 'analysis.json').read_text())
        self.assertEqual(report['status'], 'guest_failure')
        self.assertTrue(any('after requested exit' in p for p in report['problems']))

    def test_fast_process_exit_still_scans_guest_exit_request(self):
        self.process.poll.return_value = 0
        self.assertEqual(self.run_main(), 0)
        self.assertTrue(self.receipt()['guest_exit_requested'])
        self.assertTrue(self.receipt()['guest_final_log_scanned'])

    def test_shutdown_report_is_separate_from_request_and_late_failure(self):
        self.log += 'Native shutdown complete: workers=0 stackBytes=0 waitingQueues=0 graphics=stopped\n'
        self.process.poll.return_value = 0
        self.assertEqual(self.run_main(), 0)
        self.assertTrue(self.receipt()['guest_shutdown_complete_reported'])
        self.assertFalse(self.receipt()['guest_fatal_detected'])

    def test_shutdown_report_never_masks_later_fault(self):
        self.log += ('Native shutdown complete: workers=0 stackBytes=0 waitingQueues=0 graphics=stopped\n'
                     'HW.Memory <Error> unmapped Write32 @ 0x00000000 at PC 0x001AEDFC\n')
        self.process.poll.return_value = 0
        self.assertEqual(self.run_main(), 0)
        self.assertTrue(self.receipt()['guest_shutdown_complete_reported'])
        self.assertTrue(self.receipt()['guest_fatal_detected'])
        self.assertEqual(json.loads((self.out / 'analysis.json').read_text())['status'], 'guest_failure')

    def test_nonzero_worker_report_is_not_a_completed_shutdown(self):
        self.log += 'Native shutdown complete: workers=1 stackBytes=32768 waitingQueues=1 graphics=stopped\n'
        self.assertEqual(self.run_main(), 0)
        self.assertFalse(self.receipt()['guest_shutdown_complete_reported'])

    def test_rgb565_reference_isolated_sd_and_receipt(self):
        (self.root / 'twilight.iso').write_bytes(b'offline disc placeholder')
        self.assertEqual(self.run_main('--private-disc', '--rgb565-rgba8-reference'), 0)
        self.assert_cleaned()
        sd = self.runtime / 'xdg-data/azahar-emu/sdmc/3ds/dusklight'
        self.assertEqual((sd / 'rgb565-rgba8-reference.txt').read_text(), '1\n')
        self.assertTrue(self.receipt()['rgb565_rgba8_reference'])
        self.assertTrue(json.loads((self.out / 'launch.json').read_text())['rgb565_rgba8_reference'])

    def test_audio_output_setting_and_verified_component_copy(self):
        (self.root / 'twilight.iso').write_bytes(b'offline disc placeholder')
        data = bytearray(0x300);data[0x100:0x104] = b'DSP1'
        data[0x104:0x108] = len(data).to_bytes(4, 'little')
        component = self.root / 'component.cdc';component.write_bytes(data)
        self.assertEqual(self.run_main('--private-disc', '--audio-engine', 'cubeb',
                                     '--audio-emulation', 'lle', '--dsp-component', str(component)), 0)
        self.assert_cleaned()
        config = (self.runtime / 'xdg-config/azahar-emu/qt-config.ini').read_text()
        self.assertIn('output_type=2', config);self.assertNotIn('output_engine=', config)
        self.assertIn('audio_emulation=1', config)
        target = self.runtime / 'xdg-data/azahar-emu/sdmc/3ds/dspfirm.cdc'
        self.assertEqual(target.read_bytes(), data)
        self.assertEqual(target.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.receipt()['audio']['component']['bytes'], len(data))
        self.assertFalse(self.receipt()['audio']['audible_output_verified'])
        self.assertEqual(component.read_bytes(), data)

    def test_pcm_capture_isolated_configuration_and_receipt(self):
        (self.root / 'twilight.iso').write_bytes(b'offline disc placeholder')
        self.assertEqual(self.run_main('--private-disc', '--audio-pcm', '120', '300'), 0)
        self.assert_cleaned()
        config = self.runtime / 'xdg-data/azahar-emu/sdmc/3ds/dusklight/audio-pcm.txt'
        self.assertEqual(config.read_bytes(), b'120 300\n')
        self.assertEqual(config.stat().st_mode & 0o777, 0o600)
        request = self.receipt()['audio']['pcm_request']
        self.assertEqual((request['start_block'], request['block_count']), (120, 300))
        self.assertFalse(self.receipt()['audio']['audible_output_verified'])

    def test_texture_half_policy_isolated_copy_and_receipt(self):
        (self.root / 'twilight.iso').write_bytes(b'offline disc placeholder')
        source = self.root / 'allowlist.txt'; source.write_text('AB'*32+'\n')
        self.assertEqual(self.run_main('--private-disc','--texture-half-allowlist',str(source)), 0)
        config = self.runtime / 'xdg-data/azahar-emu/sdmc/3ds/dusklight/texture-half-sha256.txt'
        self.assertEqual(config.read_text(), 'ab'*32+'\n')
        self.assertEqual(config.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.receipt()['texture_half_policy']['fingerprints'], ['ab'*32])
        self.assert_cleaned()

    def test_signed_q_experiment_is_explicit_and_isolated(self):
        (self.root / 'twilight.iso').write_bytes(b'offline disc placeholder')
        self.assertEqual(self.run_main('--private-disc', '--signed-q-experiment'), 0)
        config = self.runtime / 'xdg-data/azahar-emu/sdmc/3ds/dusklight/signed-q.txt'
        self.assertEqual(config.read_bytes(), b'enabled=1\n')
        self.assertTrue(self.receipt()['signed_q_experiment'])
        self.assert_cleaned()

    def test_signed_q_default_is_disabled(self):
        (self.root / 'twilight.iso').write_bytes(b'offline disc placeholder')
        self.assertEqual(self.run_main('--private-disc'), 0)
        self.assertFalse((self.runtime / 'xdg-data/azahar-emu/sdmc/3ds/dusklight/signed-q.txt').exists())
        self.assertFalse(self.receipt()['signed_q_experiment'])

    def test_signed_q_requires_game_runtime(self):
        from contextlib import redirect_stderr
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.run_main('--signed-q-experiment')
        self.process.start.assert_not_called()

    def test_alpha_interval_experiment_is_explicit_and_isolated(self):
        (self.root / 'twilight.iso').write_bytes(b'offline disc placeholder')
        self.assertEqual(self.run_main('--private-disc', '--alpha-interval-experiment'), 0)
        config = self.runtime / 'xdg-data/azahar-emu/sdmc/3ds/dusklight/alpha-interval.txt'
        self.assertEqual(config.read_bytes(), b'enabled=1\n')
        self.assertTrue(self.receipt()['alpha_interval_experiment'])
        self.assert_cleaned()

    def test_alpha_interval_default_is_disabled(self):
        (self.root / 'twilight.iso').write_bytes(b'offline disc placeholder')
        self.assertEqual(self.run_main('--private-disc'), 0)
        self.assertFalse((self.runtime / 'xdg-data/azahar-emu/sdmc/3ds/dusklight/alpha-interval.txt').exists())
        self.assertFalse(self.receipt()['alpha_interval_experiment'])

    def test_alpha_interval_requires_game_runtime(self):
        from contextlib import redirect_stderr
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.run_main('--alpha-interval-experiment')
        self.process.start.assert_not_called()

    def test_invalid_texture_half_policy_rejected_before_launch(self):
        from contextlib import redirect_stderr
        source = self.root / 'allowlist.txt'
        for text in ['', '*', 'x'*64, 'ab'*32+'\n'+'AB'*32, ' '*8193]:
            source.write_text(text)
            with self.subTest(text=text[:65]),redirect_stderr(io.StringIO()),self.assertRaises(SystemExit):
                self.run_main('--private-disc','--texture-half-allowlist',str(source))
        self.process.start.assert_not_called()

    def test_pcm_capture_bounds_before_launch(self):
        from contextlib import redirect_stderr
        for values in [('--audio-pcm','0','1'),
                       ('--private-disc','--audio-pcm','-1','1'),
                       ('--private-disc','--audio-pcm','4294967296','1'),
                       ('--private-disc','--audio-pcm','0','0'),
                       ('--private-disc','--audio-pcm','0','328')]:
            with self.subTest(values=values), redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                self.run_main(*values)
        self.process.start.assert_not_called()

    def test_invalid_component_rejected_before_launch(self):
        data = bytearray(0x300);component = self.root / 'component.cdc';component.write_bytes(data)
        with self.assertRaises(ValueError):runner.read_dsp_component(component)
        data[0x100:0x104] = b'DSP1';data[0x104:0x108] = (0x301).to_bytes(4, 'little')
        component.write_bytes(data)
        with self.assertRaises(ValueError):runner.read_dsp_component(component)
        self.process.start.assert_not_called()

    def test_retained_depth_requires_audit_and_later_exit(self):
        from contextlib import redirect_stderr
        for frames,audits in [('10',''),('0','0'),('480','480'),('10,10','10')]:
            with self.subTest(frames=frames),redirect_stderr(io.StringIO()),self.assertRaises(SystemExit):
                self.run_main('--private-disc','--depth-retained-frames',frames,'--depth-audit-frames',audits)
        self.process.start.assert_not_called()

    def test_start_failure_preserves_exception_and_receipt(self):
        original = OSError('exec failed')
        self.process.start.side_effect = original
        with self.assertRaises(OSError) as caught:
            self.run_main()
        self.assertIs(caught.exception, original)
        self.assert_cleaned()
        self.assertEqual(self.receipt()['runner_error']['message'], 'exec failed')

    def test_pid_receipt_write_failure_cleans_owned_process(self):
        write_text = Path.write_text

        def write(path, *args, **kwargs):
            if path == self.out / 'pid':
                raise OSError('pid receipt disk failure')
            return write_text(path, *args, **kwargs)
        with patch.object(Path, 'write_text', write), self.assertRaisesRegex(OSError, 'pid receipt'):
            self.run_main()
        self.assert_cleaned()
        self.assertEqual(self.receipt()['runner_status'], 'error')

    def test_connection_failure_cleans_process(self):
        self.stub_factory.side_effect = runner.gdb_sampler.GDBError('connect failed')
        with self.assertRaisesRegex(runner.gdb_sampler.GDBError, 'connect failed'):
            self.run_main('--debug-after-frame', '300')
        self.assert_cleaned()
        self.stub.close.assert_not_called()
        self.assertEqual(self.receipt()['runner_error']['message'], 'connect failed')

    def test_startup_stop_failure_closes_socket(self):
        self.stub.command.side_effect = EOFError('startup stop failed')
        with self.assertRaisesRegex(EOFError, 'startup stop failed'):
            self.run_main('--debug-after-frame', '300')
        self.assert_cleaned()
        self.stub.close.assert_called_once()
        self.stub.resume.assert_not_called()

    def test_inventory_and_sampling_share_one_connection(self):
        self.assertEqual(self.run_main('--debug-after-frame', '300', '--profile-after-frame', '330',
                                      '--profile-elf', str(self.image)), 0)
        self.assert_cleaned()
        self.stub_factory.assert_called_once()
        self.inventory.assert_called_once()
        self.sample.assert_called_once()
        self.assertEqual(self.events, ['start', 'resume', 'inventory', 'resume', 'sample', 'close', 'terminate'])
        self.assertEqual(self.stub.resume.call_count, 2)
        record = json.loads((self.out / 'debug-inventory.json').read_text())
        self.assertEqual(record['status'], 'complete')
        self.assertEqual(record['session']['declared_initial_state'], 'running')
        self.assertIn('continue_acknowledged', record['session']['resume'])
        self.assertEqual((self.out / 'debug-inventory.json').stat().st_mode & 0o777, 0o600)
        config = (self.runtime / 'xdg-config/azahar-emu/qt-config.ini').read_text()
        self.assertIn('use_gdbstub=true', config)
        self.assertIn('instant_debug_log=true', config)
        self.assertIn('localhost:*', (self.out / 'sandbox.sb').read_text())

    def test_inventory_failure_resumes_and_closes_socket(self):
        self.inventory.side_effect = runner.gdb_sampler.GDBTimeout('inventory expired')
        with self.assertRaisesRegex(runner.gdb_sampler.GDBError, 'inventory expired'):
            self.run_main('--debug-after-frame', '300')
        self.assert_cleaned()
        self.assertEqual(self.stub.resume.call_count, 2)
        self.stub.close.assert_called_once()
        record = json.loads((self.out / 'debug-inventory.json').read_text())
        self.assertEqual(record['status'], 'error')
        self.assertEqual(self.receipt()['debug_inventory']['error']['type'], 'GDBTimeout')

    def test_profile_error_preserves_actual_last_error(self):
        def fail_sample(*args):
            self.stub.last_error = 'RSP checksum mismatch'
            return {}, {}
        self.sample.side_effect = fail_sample
        with self.assertRaisesRegex(runner.gdb_sampler.GDBError, 'RSP checksum mismatch'):
            self.run_main('--profile-after-frame', '300', '--profile-elf', str(self.image))
        self.assert_cleaned()
        self.assertEqual(self.receipt()['profile']['last_error'], 'RSP checksum mismatch')
        self.stub.close.assert_called_once()

    def test_zero_profile_samples_are_not_success(self):
        self.sample.side_effect = lambda *args: ({}, {})
        with self.assertRaisesRegex(runner.gdb_sampler.GDBError, 'no observed PC samples'):
            self.run_main('--profile-after-frame', '300', '--profile-elf', str(self.image))
        self.assert_cleaned()
        self.assertIsNone(self.receipt()['profile']['last_error'])
        self.assertEqual(self.receipt()['profile']['samples'], 0)
        self.assertEqual(self.receipt()['profile']['status'], 'error')

    def test_inventory_interrupt_survives_resume_failure(self):
        original = runner.RunInterrupted(signal.SIGTERM)
        self.inventory.side_effect = original
        self.stub.resume.side_effect = [None, EOFError('resume disconnected')]
        with self.assertRaises(runner.RunInterrupted) as caught:
            self.run_main('--debug-after-frame', '300')
        self.assertIs(caught.exception, original)
        self.assert_cleaned()
        record = json.loads((self.out / 'debug-inventory.json').read_text())
        self.assertEqual(record['error']['type'], 'RunInterrupted')
        self.assertEqual(record['session']['resume_error'], 'resume disconnected')
        self.assertEqual(self.receipt()['runner_error']['type'], 'RunInterrupted')

    def test_conversion_failure_keeps_log_dump_listing_and_receipt(self):
        start = self.process.start.side_effect

        def with_capture(*args):
            start(*args)
            captures = self.runtime / 'xdg-data/azahar-emu/sdmc/3ds/dusklight/captures'
            captures.mkdir()
            (captures / 'frame-000480.rgba').write_bytes(b'invalid')
        self.process.start.side_effect = with_capture
        with patch.object(runner.capture_to_png, 'convert', side_effect=ValueError('bad capture')), \
                self.assertRaisesRegex(RuntimeError, 'finalization failed'):
            self.run_main()
        self.assert_cleaned()
        self.assertTrue((self.out / 'guest.log').is_file())
        self.assertEqual(len(self.receipt()['dumps_in_runtime']), 2)
        self.assertEqual(self.receipt()['cleanup_errors'][0]['step'], 'capture_convert:frame-000480.rgba')

    def test_primary_exception_survives_receipt_write_failure(self):
        original = EOFError('capture connection failed')
        self.stub.command.side_effect = original
        write_text = Path.write_text

        def write(path, *args, **kwargs):
            if path == self.out / 'process.json':
                raise OSError('receipt disk failure')
            return write_text(path, *args, **kwargs)
        with patch.object(Path, 'write_text', write), self.assertRaises(EOFError) as caught:
            self.run_main('--debug-after-frame', '300')
        self.assertIs(caught.exception, original)
        self.assert_cleaned()
        self.stub.close.assert_called_once()

    def test_signal_interrupt_preserves_receipts_and_restores_handlers(self):
        def interrupt(seconds):
            self.handlers[signal.SIGTERM](signal.SIGTERM, None)
        self.sleep.side_effect = interrupt
        with self.assertRaises(runner.RunInterrupted) as caught:
            self.run_main('--debug-after-frame', '300')
        self.assertEqual(caught.exception.signum, signal.SIGTERM)
        self.assert_cleaned()
        self.stub.close.assert_called_once()
        self.assertEqual(self.receipt()['signal'], signal.SIGTERM)
        self.assertEqual(self.receipt()['runner_error']['type'], 'RunInterrupted')

    def test_cleanup_failure_does_not_replace_primary_error(self):
        original = EOFError('original launch error')
        self.process.start.side_effect = original
        self.process.terminate.side_effect = TimeoutError('cleanup timed out')
        with self.assertRaises(EOFError) as caught:
            self.run_main()
        self.assertIs(caught.exception, original)
        receipt = self.receipt()
        self.assertEqual(receipt['runner_error']['message'], str(original))
        self.assertEqual(receipt['cleanup_errors'][0]['step'], 'process_terminate')
        self.assertEqual(receipt['cleanup_errors'][0]['type'], 'TimeoutError')
        self.process.close.assert_called_once()

    def test_cleanup_signal_does_not_abort_receipt_writes(self):
        def cleanup():
            self.handlers[signal.SIGINT](signal.SIGINT, None)
            self.process.returncode = 'exited (mock)'
        self.process.terminate.side_effect = cleanup
        self.run_main()
        self.assert_cleaned()
        self.assertEqual(self.receipt()['signal'], signal.SIGINT)

    def test_incomplete_timeout_records_pending_inventory(self):
        self.log = 'Original native frame 30 complete; drawn=100 rejected=0\n'
        self.run_main('--debug-after-frame', '300')
        self.assert_cleaned()
        self.assertTrue(self.receipt()['bounded_stop'])
        self.assertEqual(self.receipt()['debug_inventory']['status'], 'pending')
        self.inventory.assert_not_called()
        self.stub.close.assert_called_once()
        self.assertEqual(json.loads((self.out / 'analysis.json').read_text())['status'], 'incomplete')

    def test_reached_exit_before_inventory_trigger_is_incomplete(self):
        self.run_main('--debug-after-frame', '600')
        self.assert_cleaned()
        self.assertEqual(self.receipt()['debug_inventory']['status'], 'pending')
        self.assertEqual(json.loads((self.out / 'analysis.json').read_text())['status'], 'incomplete')


    def initial_card_fixture(self, data=None):
        (self.root / 'twilight.iso').write_bytes(b'offline disc placeholder')
        self.out = self.root / 'Diagnostics/evidence/test'
        self.card = self.root / 'Diagnostics/fixtures/native.bin'
        self.card.parent.mkdir(parents=True, exist_ok=True)
        self.card.write_bytes(native_card() if data is None else data)
        return self.card

    def seed_arguments(self):
        return ('--private-disc', '--initial-card', str(self.card))

    def seeded_path(self):
        return self.runtime / 'xdg-data/azahar-emu/sdmc/3ds/dusklight/saves/native-card0.bin'

    def assert_card_rejected(self, *args):
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.run_main(*args)
        self.process.start.assert_not_called()
        self.assertFalse(self.runtime.exists())
        self.assertFalse(self.out.exists())

    def test_initial_card_arm32_validation_boundaries_and_uninterpreted_metadata(self):
        for payload in (8192, 32768, 1024*1024):
            data = native_card(payload)
            runner.validate_native_card(data)
            self.assertEqual(len(data), payload + 120)
        # Runtime does not constrain time, game/company IDs, padding or icon offsets.
        data = bytearray(native_card(8192))
        data[12:44] = b'\0' + b'\xff'*31
        data[48:120] = b'\xff'*72
        runner.validate_native_card(data)
        data[12:44] = b'x'*31 + b'\0'
        runner.validate_native_card(data)

    def test_initial_card_invalid_envelopes_fail_before_run_creation(self):
        card = self.initial_card_fixture()
        original = native_card()
        variants = [b'', original[:119], original[:-1], original+b'x',
                    b'RAWGCN!!'+original[8:], original[:8]+(2).to_bytes(4,'little')+original[12:],
                    original[:8]+(1).to_bytes(4,'big')+original[12:],
                    original[:12]+b'x'*32+original[44:],
                    b'x'*(runner.NATIVE_CARD_MAX+1)]
        for length in (0, 1, 8191, 8193, 32768+8192, 1024*1024+8192):
            variants.append(original[:44]+length.to_bytes(4,'little')+original[48:])
        previous = self.root / 'Build/azahar-runs/previous'
        previous.mkdir(parents=True); sentinel = previous / 'untouched'
        sentinel.write_bytes(b'previous run')
        for data in variants:
            with self.subTest(size=len(data), header=data[:12]):
                card.write_bytes(data)
                self.assert_card_rejected(*self.seed_arguments())
                self.assertEqual(card.read_bytes(), data)
                self.assertEqual(sentinel.read_bytes(), b'previous run')

    def test_initial_card_requires_private_disc(self):
        self.initial_card_fixture()
        self.assert_card_rejected('--initial-card', str(self.card))

    def test_initial_card_copies_and_receipts_before_launch_source_unchanged(self):
        card = self.initial_card_fixture()
        before = card.stat(); original = card.read_bytes()
        digest = hashlib.sha256(original).hexdigest()
        def start(*args):
            self.assertEqual(self.seeded_path().read_bytes(), original)
            launch = json.loads((self.out/'launch.json').read_text())['initial_card']
            self.assertEqual(launch['status'], 'seeded')
            self.assertEqual(launch['input']['sha256'], digest)
            self.assertEqual(launch['initial']['sha256'], digest)
            self.assertIsNone(launch['final'])
            self.start(*args)
        self.process.start.side_effect = start
        self.assertEqual(self.run_main(*self.seed_arguments(), '--signed-q-experiment'), 0)
        self.assert_cleaned()
        receipt = self.receipt()['initial_card']
        self.assertEqual(receipt['input']['bytes'], 32888)
        self.assertEqual(receipt['input']['mtime_ns'], before.st_mtime_ns)
        self.assertEqual(receipt['final']['sha256'], digest)
        self.assertTrue(receipt['final']['matches_seed_snapshot'])
        self.assertEqual(card.read_bytes(), original)
        self.assertEqual(card.stat().st_mtime_ns, before.st_mtime_ns)
        self.assertEqual(self.seeded_path().stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.seeded_path().stat().st_nlink, 1)
        self.assertNotEqual(self.seeded_path().stat().st_ino, before.st_ino)
        self.assertFalse((self.seeded_path().parent/'.native-card0.seed').exists())
        self.assertFalse(self.receipt()['gameplay_validated'])
        self.assertTrue(self.receipt()['signed_q_experiment'])
        self.assertFalse(self.receipt()['alpha_interval_experiment'])

    def test_initial_card_final_is_separate_post_stop_observation(self):
        self.initial_card_fixture()
        def terminate():
            self.terminate()
            final = bytearray(native_card()); final[-1] = 7
            self.seeded_path().write_bytes(final)
        self.process.terminate.side_effect = terminate
        self.run_main(*self.seed_arguments())
        record = self.receipt()['initial_card']
        self.assertNotEqual(record['initial']['sha256'], record['final']['sha256'])
        self.assertFalse(record['final']['matches_seed_snapshot'])
        self.assertEqual(record['input']['sha256'], record['initial']['sha256'])
        self.assertTrue(record['final']['native_structure_valid'])

    def test_initial_card_final_absent_truncated_oversized_and_symlink_are_explicit(self):
        self.initial_card_fixture()
        self.assertEqual(runner.final_native_card(self.card.parent/'missing')['status'], 'absent')
        for data in (b'', b'broken', bytes(runner.NATIVE_CARD_MAX+1)):
            self.card.write_bytes(data)
            result = runner.final_native_card(self.card)
            self.assertEqual(result['bytes'], len(data))
            if len(data) <= runner.NATIVE_CARD_MAX:
                self.assertEqual(result['sha256'], hashlib.sha256(data).hexdigest())
                self.assertFalse(result['native_structure_valid'])
            else:
                self.assertEqual(result['status'], 'unavailable')
                self.assertIsNone(result['sha256'])
        link = self.card.parent/'alias'; link.symlink_to(self.card)
        self.assertEqual(runner.final_native_card(link)['status'], 'unavailable')

    def test_initial_card_default_keeps_fresh_empty_sd_and_off_hooks(self):
        (self.root/'twilight.iso').write_bytes(b'offline disc placeholder')
        self.run_main('--private-disc')
        self.assertFalse(self.seeded_path().parent.exists())
        self.assertIsNone(self.receipt()['initial_card'])
        self.assertIsNone(json.loads((self.out/'launch.json').read_text())['initial_card'])
        self.assertFalse(self.receipt()['signed_q_experiment'])
        self.assertFalse(self.receipt()['alpha_interval_experiment'])

    def test_initial_card_rejects_external_source_git_and_missing_paths(self):
        self.initial_card_fixture()
        for path in (self.root.parent/'outside.bin', self.root/'Source/card.bin',
                     self.root/'Tools/card.bin', self.card.parent/'.git/card.bin',
                     self.card.parent/'missing'):
            with self.subTest(path=path):
                self.assert_card_rejected('--private-disc', '--initial-card', str(path))
        (self.card.parent/'.git').write_text('gitdir: private test marker')
        self.assert_card_rejected(*self.seed_arguments())

    def test_initial_card_rejects_file_and_ancestor_symlinks(self):
        self.initial_card_fixture()
        link = self.card.parent/'alias.bin'; link.symlink_to(self.card)
        parent = self.card.parent.parent/'alias'; parent.symlink_to(self.card.parent, target_is_directory=True)
        for path in (link, parent/self.card.name):
            self.assert_card_rejected('--private-disc', '--initial-card', str(path))

    def test_initial_card_rejects_hardlinks_directories_and_fifo_without_blocking(self):
        self.initial_card_fixture()
        alias = self.card.parent/'hardlink'; os.link(self.card, alias)
        self.assert_card_rejected(*self.seed_arguments())
        alias.unlink()
        fifo = self.card.parent/'fifo'; os.mkfifo(fifo)
        for path in (self.card.parent, fifo):
            self.assert_card_rejected('--private-disc', '--initial-card', str(path))

    def test_initial_card_concurrent_write_with_restored_mtime_is_rejected(self):
        self.initial_card_fixture()
        original_fstat = runner.os.fstat
        calls = []
        def fstat(fd):
            calls.append(fd)
            if len(calls) == 2:
                before = self.card.stat()
                data = bytearray(self.card.read_bytes()); data[-1] = 5
                self.card.write_bytes(data)
                os.utime(self.card, ns=(before.st_atime_ns, before.st_mtime_ns))
            return original_fstat(fd)
        with patch.object(runner.os, 'fstat', side_effect=fstat):
            self.assert_card_rejected(*self.seed_arguments())
        self.assertEqual(len(calls), 2)

    def test_initial_card_concurrent_path_replacement_is_rejected(self):
        self.initial_card_fixture()
        original_fstat = runner.os.fstat
        replacement = self.card.parent/'replacement'; replacement.write_bytes(native_card())
        count = 0
        def fstat(fd):
            nonlocal count
            count += 1
            if count == 2:
                replacement.replace(self.card)
            return original_fstat(fd)
        with patch.object(runner.os, 'fstat', side_effect=fstat):
            self.assert_card_rejected(*self.seed_arguments())

    def test_initial_card_concurrent_symlink_at_open_is_not_followed(self):
        self.initial_card_fixture()
        original_open = runner.os.open
        def open_file(path, flags, *args, **kwargs):
            if path == self.card.name:
                self.card.unlink(); self.card.symlink_to(self.image)
            return original_open(path, flags, *args, **kwargs)
        with patch.object(runner.os, 'open', side_effect=open_file):
            self.assert_card_rejected(*self.seed_arguments())

    def test_initial_card_no_reuse_existing_runtime_or_evidence(self):
        self.initial_card_fixture()
        for existing in (self.runtime, self.out):
            existing.mkdir(parents=True)
            sentinel = existing/'untouched'; sentinel.write_bytes(b'keep')
            with self.assertRaises(FileExistsError):
                self.run_main(*self.seed_arguments())
            self.assertEqual(sentinel.read_bytes(), b'keep')
            self.process.start.assert_not_called()
            # Only synthetic roots owned by this offline test are removed.
            runner.shutil.rmtree(existing)
            if self.runtime.exists(): runner.shutil.rmtree(self.runtime)

    def test_initial_card_output_must_stay_private_outside_git_symlinks(self):
        self.initial_card_fixture()
        self.out = self.root/'Source/evidence/test'
        with self.assertRaises(ValueError): self.run_main(*self.seed_arguments())
        self.process.start.assert_not_called()
        self.assertFalse((self.root/'Source').exists())

    def test_initial_card_external_path_is_rejected_before_any_open(self):
        self.initial_card_fixture()
        with patch.object(runner.os, 'open', side_effect=AssertionError('must not open external input')):
            self.assert_card_rejected('--private-disc', '--initial-card', str(self.root.parent/'outside.bin'))

    def test_initial_card_symlink_runtime_parent_and_git_evidence_rejected(self):
        self.initial_card_fixture()
        source = self.root/'Source'; source.mkdir()
        (self.root/'Build').symlink_to(source, target_is_directory=True)
        with self.assertRaises(OSError): self.run_main(*self.seed_arguments())
        self.process.start.assert_not_called()
        self.assertEqual(list(source.iterdir()), [])
        (self.root/'Build').unlink()
        self.out.parent.mkdir(parents=True)
        (self.out.parent/'.git').mkdir()
        with self.assertRaises(ValueError): self.run_main(*self.seed_arguments())
        self.process.start.assert_not_called()
        self.assertEqual(sorted(p.name for p in self.out.parent.iterdir()), ['.git'])

    def test_initial_card_signal_during_copy_cleans_and_records_original_interrupt(self):
        self.initial_card_fixture()
        def interrupt(fd):
            self.handlers[signal.SIGTERM](signal.SIGTERM, None)
        with patch.object(runner.os, 'fsync', side_effect=interrupt), self.assertRaises(runner.RunInterrupted):
            self.run_main(*self.seed_arguments())
        self.process.start.assert_not_called()
        self.assert_cleaned()
        self.assertFalse((self.seeded_path().parent/'.native-card0.seed').exists())
        self.assertEqual(self.receipt()['runner_error']['type'], 'RunInterrupted')
        self.assertEqual(self.receipt()['signal'], signal.SIGTERM)

    def test_initial_card_existing_save_directory_never_reused(self):
        self.initial_card_fixture()
        original = runner.seed_native_card
        def seed(sd, data, expected):
            directory = sd/'saves'; directory.mkdir()
            (directory/'native-card0.bin').write_bytes(b'previous card')
            return original(sd, data, expected)
        with patch.object(runner, 'seed_native_card', side_effect=seed), self.assertRaises(FileExistsError):
            self.run_main(*self.seed_arguments())
        self.assertEqual(self.seeded_path().read_bytes(), b'previous card')
        self.process.start.assert_not_called()
        self.assert_cleaned()
        self.assertEqual(self.receipt()['runner_status'], 'error')
        self.assertIsNone(self.receipt()['initial_card']['initial'])

    def test_initial_card_atomic_publish_refuses_concurrent_destination(self):
        self.initial_card_fixture()
        original_link = runner.os.link
        def link(*args, **kwargs):
            self.seeded_path().write_bytes(b'concurrent owner')
            return original_link(*args, **kwargs)
        with patch.object(runner.os, 'link', side_effect=link), self.assertRaises(FileExistsError):
            self.run_main(*self.seed_arguments())
        self.assertEqual(self.seeded_path().read_bytes(), b'concurrent owner')
        self.assertFalse((self.seeded_path().parent/'.native-card0.seed').exists())
        self.process.start.assert_not_called()
        self.assert_cleaned()

    def test_initial_card_copy_io_failure_cleans_temporary_and_keeps_receipts(self):
        self.initial_card_fixture()
        before = self.card.read_bytes(); mtime = self.card.stat().st_mtime_ns
        with patch.object(runner.os, 'fsync', side_effect=OSError('seed fsync failure')), \
                self.assertRaisesRegex(OSError, 'seed fsync failure'):
            self.run_main(*self.seed_arguments())
        self.assertFalse(self.seeded_path().exists())
        self.assertFalse((self.seeded_path().parent/'.native-card0.seed').exists())
        self.process.start.assert_not_called()
        self.assert_cleaned()
        self.assertEqual(self.receipt()['runner_error']['message'], 'seed fsync failure')
        self.assertEqual(self.card.read_bytes(), before)
        self.assertEqual(self.card.stat().st_mtime_ns, mtime)

    def test_initial_card_failed_copy_verification_never_launches(self):
        self.initial_card_fixture()
        original = runner.seed_native_card
        def seed(sd, data, expected):
            return original(sd, data, {**expected, 'sha256': '00'*32})
        with patch.object(runner, 'seed_native_card', side_effect=seed), \
                self.assertRaisesRegex(OSError, 'copy verification'):
            self.run_main(*self.seed_arguments())
        self.assertFalse(self.seeded_path().exists())
        self.assertFalse((self.seeded_path().parent/'.native-card0.seed').exists())
        self.process.start.assert_not_called()
        self.assert_cleaned()

    def test_initial_card_start_failure_retains_seed_receipt(self):
        self.initial_card_fixture()
        self.process.start.side_effect = OSError('mock launch failure')
        with self.assertRaisesRegex(OSError, 'mock launch failure'):
            self.run_main(*self.seed_arguments())
        self.assert_cleaned()
        self.assertEqual(self.receipt()['initial_card']['status'], 'seeded')
        self.assertTrue(self.receipt()['initial_card']['final']['matches_seed_snapshot'])

    def test_initial_card_unconfirmed_process_stop_never_claims_final_snapshot(self):
        self.initial_card_fixture()
        self.process.terminate.side_effect = TimeoutError('not stopped')
        with self.assertRaisesRegex(RuntimeError, 'finalization failed'):
            self.run_main(*self.seed_arguments())
        self.assertEqual(self.receipt()['initial_card']['final']['status'], 'unavailable')
        self.assertIsNone(self.receipt()['initial_card']['final']['matches_seed_snapshot'])


class OwnershipTests(unittest.TestCase):
    def test_no_pid_or_identity_never_signals(self):
        for pid, identity in ((None, None), (12345, None), (0, 'identity'), (1, 'identity')):
            process = runner.DetachedProcess()
            process.pid, process.identity = pid, identity
            with patch.object(runner.os, 'kill') as kill:
                process.terminate()
            kill.assert_not_called()

    def test_reused_pid_never_signals(self):
        process = runner.DetachedProcess()
        process.pid, process.identity = 12345, 'old identity'
        with patch.object(process, '_identity', return_value='new identity'), patch.object(runner.os, 'kill') as kill:
            process.terminate()
        kill.assert_not_called()
        self.assertIsNotNone(process.returncode)

    def test_kernel_exit_event_never_signals_reused_pid(self):
        process = runner.DetachedProcess()
        process.pid, process.identity = 12345, 'same second identity'
        process.watcher = Mock()
        process.watcher.control.return_value = ['exit event']
        with patch.object(process, '_identity', return_value=process.identity), patch.object(runner.os, 'kill') as kill:
            process.terminate()
            process.terminate()
        kill.assert_not_called()
        process.close()
        self.assertIsNone(process.watcher)

    def test_kernel_watched_sampling_poll_does_not_spawn_ps(self):
        process = runner.DetachedProcess()
        process.pid, process.identity = 12345, 'original identity'
        process.watcher = Mock()
        process.watcher.control.return_value = []
        with patch.object(process, '_identity') as identity:
            self.assertIsNone(process.poll())
        identity.assert_not_called()

    def test_termination_is_bounded_and_escalates_only_owned_pid(self):
        process = runner.DetachedProcess()
        process.pid, process.identity = 12345, 'original identity'
        clock = Clock()
        with patch.object(process, 'poll', return_value=None), \
                patch.object(process, '_identity', return_value=process.identity), \
                patch.object(runner.time, 'monotonic', clock.monotonic), \
                patch.object(runner.time, 'sleep', side_effect=clock.sleep), \
                patch.object(runner.os, 'kill') as kill:
            with self.assertRaisesRegex(TimeoutError, 'bounded TERM/KILL'):
                process.terminate(grace=0.2)
        self.assertEqual([call.args for call in kill.call_args_list],
                         [(12345, signal.SIGTERM), (12345, signal.SIGKILL)])
        self.assertLess(clock.now, 1)

    def test_pipe_allocation_failure_closes_already_opened_pipe(self):
        process = runner.DetachedProcess()
        original = OSError('pipe allocation failed')
        with patch.object(runner.os, 'pipe', side_effect=[(10, 11), original]), \
                patch.object(runner.os, 'close') as close:
            with self.assertRaises(OSError) as caught:
                process.start(['/fake'], '/fake', {}, '/fake/log')
        self.assertIs(caught.exception, original)
        self.assertEqual(sorted(call.args[0] for call in close.call_args_list), [10, 11])

    def test_handshake_failure_closes_all_fds_without_signal(self):
        process = runner.DetachedProcess()
        with patch.object(runner.os, 'pipe', side_effect=[(10, 11), (12, 13)]), \
                patch.object(runner.os, 'fork', return_value=99), \
                patch.object(runner.os, 'close') as close, \
                patch.object(runner.os, 'waitpid', return_value=(99, 0)), \
                patch.object(runner.os, 'kill') as kill, \
                patch.object(runner.select, 'select', return_value=([10], [], [])), \
                patch.object(runner.os, 'read', return_value=b'0\n'):
            with self.assertRaisesRegex(RuntimeError, 'Invalid detached'):
                process.start(['/fake'], '/fake', {}, '/fake/log')
        self.assertEqual(sorted(call.args[0] for call in close.call_args_list), [10, 11, 12, 13])
        kill.assert_not_called()
        self.assertIsNone(process.pid)


if __name__ == '__main__':
    unittest.main()
