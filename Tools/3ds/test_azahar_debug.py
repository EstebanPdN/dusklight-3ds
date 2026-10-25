#!/usr/bin/env python3
"""Offline RSP/diagnostic tests; stdlib only, no emulator or native build."""
from collections import deque
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import socket
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

import azahar_debug as debug
import gdb_sampler as gdb


def frame(wire):
    """Independent wire fixture (input is already escaped, if necessary)."""
    return b'$' + wire + b'#' + f'{sum(wire) & 255:02x}'.encode()


class MockTransport:
    def __init__(self, *chunks, delay=0):
        self.chunks = deque(chunks)
        self.sent = []
        self.closed = False
        self.delay = delay
        self.timeout = 1

    def settimeout(self, value):
        self.timeout = value

    def sendall(self, data):
        self.sent.append(data)

    def recv(self, count):
        if self.delay:
            time.sleep(min(self.delay, self.timeout))
            if self.delay >= self.timeout:
                raise socket.timeout('mock partial receive timed out')
        item = self.chunks.popleft() if self.chunks else b''
        if isinstance(item, BaseException):
            raise item
        return item

    def close(self):
        self.closed = True


class MockServer:
    """One local TCP mock. Protocol peer runs on a bounded daemon thread."""
    def __init__(self, handler=None):
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(1)
        self.listener.settimeout(2)
        self.port = self.listener.getsockname()[1]
        self.commands = []
        self.errors = []
        self.handler = handler or self.reply
        self.thread = threading.Thread(target=self.serve, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *args):
        self.thread.join(3)
        self.listener.close()
        if self.thread.is_alive():
            raise AssertionError('Mock server failed to finish')
        if self.errors:
            raise self.errors[0]

    def serve(self):
        try:
            connection, _ = self.listener.accept()
            with connection:
                connection.settimeout(2)
                buffer = b''
                while True:
                    chunk = connection.recv(4096)
                    if not chunk:
                        return
                    buffer += chunk
                    while buffer:
                        if buffer[:1] in (b'+', b'-'):
                            buffer = buffer[1:]
                            continue
                        if buffer[:1] != b'$':
                            raise AssertionError('Unexpected mock command prefix')
                        end = buffer.find(b'#')
                        if end < 0 or len(buffer) < end + 3:
                            break
                        command = buffer[1:end]
                        if frame(command) != buffer[:end + 3]:
                            raise AssertionError('Client command checksum is invalid')
                        buffer = buffer[end + 3:]
                        self.commands.append(command)
                        reply = self.handler(command)
                        # Split replies across writes to exercise the TCP framing path.
                        wire = b'+' + frame(reply) if reply is not None else b'+'
                        connection.sendall(wire[:2])
                        connection.sendall(wire[2:])
        except BaseException as exc:
            self.errors.append(exc)

    @staticmethod
    def reply(command):
        if command == b'?':
            return b'T05thread:1;'
        if command == b'qSupported':
            return b'PacketSize=80;qXfer:threads:read+'
        if command == b'qfThreadInfo':
            return b'm1,2'
        if command == b'qsThreadInfo':
            return b'l'
        if command.startswith(b'qXfer:threads:read:'):
            return b'l<threads><thread id="1" name="Thread 1"/><thread id="2" name="Thread 2"/></threads>'
        if command.startswith(b'Hg'):
            return b'OK'
        if command == b'p19':
            return b'E01'
        if command.startswith(b'p'):
            return b'34120000'
        if command.startswith(b'm'):
            address, length = (int(x, 16) for x in command[1:].split(b','))
            return bytes((address + i) & 255 for i in range(length)).hex().encode()
        if command == b'c':
            return None
        return b''


class TransportTests(unittest.TestCase):
    def stub(self, *chunks, **kwargs):
        transport = MockTransport(*chunks)
        return gdb.Stub(1, sock=transport, **kwargs), transport

    def test_packet_checksum_and_escaping(self):
        self.assertEqual(gdb.packet(b'pf'), b'$pf#d6')
        wire = gdb.packet(b'$#}*')
        stub, _ = self.stub(wire)
        self.assertEqual(stub.read_packet(), b'$#}*')

    def test_fragmented_ack_and_coalesced_packets(self):
        wire = b'+' + frame(b'1234') + frame(b'next')
        stub, transport = self.stub(*(bytes([x]) for x in wire))
        self.assertEqual(stub.command(b'pf'), b'1234')
        self.assertEqual(stub.read_packet(), b'next')
        self.assertEqual(transport.sent, [frame(b'pf'), b'+', b'+'])
        stub, _ = self.stub(b'++' + frame(b'first') + frame(b'second'))
        self.assertEqual(stub.read_packet(), b'first')
        self.assertEqual(stub.read_packet(), b'second')

    def test_bad_checksum_and_nonhex_are_nacked(self):
        for wire in (b'$data#00', b'$data#zz'):
            with self.subTest(wire=wire):
                stub, transport = self.stub(wire)
                with self.assertRaises(gdb.GDBProtocolError):
                    stub.command(b'?')
                self.assertEqual(transport.sent[-1], b'-')
                self.assertNotIn(b'+', transport.sent)

    def test_nack_is_error_and_command_not_retried(self):
        stub, transport = self.stub(b'-')
        with self.assertRaisesRegex(gdb.GDBProtocolError, 'NACK'):
            stub.command(b'pf')
        self.assertEqual(transport.sent, [frame(b'pf')])

    def test_eof_and_timeout_during_partial_packet(self):
        for chunks, error in (((b'$abc#', b''), EOFError),
                              ((b'$abc#', socket.timeout()), gdb.GDBTimeout)):
            stub, _ = self.stub(*chunks)
            with self.assertRaises(error):
                stub.command(b'?')

    def test_trickle_cannot_reset_operation_deadline(self):
        transport = MockTransport(*(b'+' for _ in range(100)), delay=0.01)
        stub = gdb.Stub(1, sock=transport, timeout=0.035)
        start = time.monotonic()
        with self.assertRaises(gdb.GDBTimeout):
            stub.command(b'?')
        self.assertLess(time.monotonic() - start, 0.2)

    def test_whole_session_deadline_applies_to_commands(self):
        stub, _ = self.stub(frame(b'OK'))
        stub.deadline = time.monotonic() - 1
        with self.assertRaises(gdb.GDBTimeout):
            stub.command(b'?')

    def test_wire_and_expanded_payload_limits(self):
        for wire in (b'$' + b'a' * 10, frame(b'a' * 9), frame(b'0*~')):
            stub, _ = self.stub(wire, max_payload=8)
            with self.assertRaises(gdb.GDBProtocolError):
                stub.read_packet()

    def test_bad_escape_repeat_and_unexpected_bytes(self):
        for wire in (frame(b'abc}'), frame(b'*a'), frame(b'a*'),
                     frame(b'a*\x00'), frame(b'a*+'), frame(b'a$b'), b'noise'):
            with self.subTest(wire=wire):
                stub, _ = self.stub(wire)
                with self.assertRaises(gdb.GDBProtocolError):
                    stub.read_packet()

    def test_valid_run_length_encoding(self):
        stub, _ = self.stub(frame(b'0* '))
        self.assertEqual(stub.read_packet(), b'0000')

    def test_error_and_unsupported_reply_differ(self):
        stub, _ = self.stub(frame(b'E0e'))
        with self.assertRaises(gdb.GDBRemoteError) as result:
            stub.command(b'm1000,4')
        self.assertEqual(result.exception.reply, b'E0e')
        stub, _ = self.stub(frame(b''))
        self.assertEqual(stub.command(b'qUnknown'), b'')

    def test_register_syntax_width_endianness_and_unavailable(self):
        stub, transport = self.stub(frame(b'78563412'), frame(b'xxxxxxxx'),
                                    frame(b'0100000000000000'))
        self.assertEqual(stub.register(15), 0x12345678)
        self.assertIsNone(stub.register(25))
        self.assertEqual(stub.register(26, bits=64), 1)
        self.assertEqual(transport.sent[0], frame(b'pf'))
        self.assertIn(frame(b'p19'), transport.sent)
        for reply in (b'1234', b'0000000000', b'12 45678', b'xx000000', b'abcdefgh', b'xx'):
            stub, _ = self.stub(frame(reply))
            with self.assertRaises(gdb.GDBProtocolError):
                stub.register(15)

    def test_stop_validation_and_resume_ack(self):
        for reply in (b'OK', b'', b'S05junk', b'W00', b'X0b', b'Tzz',
                      b'T05garbage', b'T05thread;', b'T05thread:\x00'):
            with self.assertRaises(gdb.GDBError):
                gdb.Stub.require_stop(reply)
        self.assertEqual(gdb.Stub.require_stop(b'T05;thread:1;'), b'T05;thread:1;')
        stub, transport = self.stub(b'+')
        stub.resume(wait_ack=True)
        self.assertEqual(transport.sent, [frame(b'c')])
        stub, _ = self.stub(b'-')
        with self.assertRaises(gdb.GDBProtocolError):
            stub.resume(wait_ack=True)

    def test_context_manager_closes_socket(self):
        stub, transport = self.stub()
        with stub:
            pass
        self.assertTrue(transport.closed)
        with self.assertRaises(gdb.GDBError):
            stub.command(b'?')

    def test_sampler_resumes_after_register_failure(self):
        stub, transport = self.stub(frame(b'T02thread:1;'), frame(b'E01'))
        pcs, pairs = gdb.sample(stub, 0.1, 0.01, lambda: True)
        self.assertEqual((pcs, pairs), ({}, {}))
        self.assertEqual(transport.sent[-1], frame(b'c'))
        self.assertIn('E01', stub.last_error)

    def test_sampler_resumes_after_invalid_interrupt_reply(self):
        stub, transport = self.stub(frame(b'OK'))
        self.assertEqual(gdb.sample(stub, 0.1, 0.01, lambda: True), ({}, {}))
        self.assertEqual(transport.sent[-1], frame(b'c'))
        self.assertIn('stop reply', stub.last_error)

    def test_sampler_zero_samples_are_not_reported_as_one(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'profile.txt'
            with patch('gdb_sampler.subprocess.run') as run:
                run.return_value.stdout = ''
                gdb.symbolize('mock.elf', {}, {}, output)
            self.assertTrue(output.read_text().startswith('samples=0\n'))

    def test_connection_failure_retries_and_closed_send_are_explicit(self):
        with patch('gdb_sampler.socket.create_connection', side_effect=ConnectionRefusedError('mock refusal')) as connect:
            with self.assertRaises(gdb.GDBError):
                gdb.Stub(24689, attempts=1, connect_timeout=0.01)
            self.assertEqual(connect.call_count, 1)
            self.assertEqual(connect.call_args.args[0], ('127.0.0.1', 24689))
        stub, transport = self.stub()
        with patch.object(transport, 'sendall', side_effect=socket.timeout()):
            with self.assertRaises(gdb.GDBTimeout):
                stub.command(b'?')


class DiagnosticTests(unittest.TestCase):
    def stub(self, *replies):
        return gdb.Stub(1, sock=MockTransport(*(frame(r) for r in replies)))

    def test_range_validation_is_bounded_and_no_wrap(self):
        for address, length in ((-1, 1), (2 ** 32, 1), (0, 0),
                                (0xffffffff, 2), (0x1000, debug.MAX_READ + 1)):
            with self.assertRaises(ValueError):
                debug.validate_range(address, length)
        debug.validate_range(0xffffffff, 1)

    def test_memory_rejects_short_oversized_nonhex_unavailable_and_error(self):
        for reply in (b'1234', b'1234567890', b'xxxxxxxx', b'12 45678', b''):
            with self.subTest(reply=reply):
                with self.assertRaises(gdb.GDBError):
                    debug.read_memory(self.stub(reply), 0x1000, 4)
        with self.assertRaises(gdb.GDBRemoteError):
            debug.read_memory(self.stub(b'E0e'), 0x1000, 4)

    def test_memory_requests_only_explicit_range_and_respects_packet_size(self):
        stub = self.stub(b'00010203', b'0405')
        self.assertEqual(debug.read_memory(stub, 0x1000, 6, chunk_size=4), bytes(range(6)))
        self.assertEqual([x for x in stub.socket.sent if x.startswith(b'$')],
                         [frame(b'm1000,4'), frame(b'm1004,2')])
        for size in (0, debug.MAX_CHUNK + 1):
            with self.assertRaises(ValueError):
                debug.read_memory(stub, 0, 1, chunk_size=size)

    def test_checksum_matches_native_dump_fnv1a(self):
        self.assertEqual(debug.checksums(b'hello')['fnv1a32'], '4f9f2cab')
        self.assertEqual(debug.checksums(b'')['fnv1a32'], '811c9dc5')
        self.assertEqual(len(debug.checksums(b'hello')['sha256']), 64)

    def test_enumeration_pagination_and_malformed_ids(self):
        self.assertEqual(debug.list_threads(self.stub(b'm1,2', b'ma', b'l')), (['1', '2', 'a'], None))
        for replies in ((b'm1', b'm1'), (b'm0',), (b'm1,',), (b'OK',), (b'm1;bad',)):
            with self.subTest(replies=replies):
                with self.assertRaises(gdb.GDBProtocolError):
                    debug.list_threads(self.stub(*replies))
        self.assertIsNone(debug.list_threads(self.stub(b''))[0])

    def test_xml_chunks_and_unavailable(self):
        stub = self.stub(b'm<threads><thread ', b'lid="1" name="Thread 1"/></threads>')
        extra, error = debug.read_thread_xml(stub, {'qXfer:threads:read': True})
        self.assertIsNone(error)
        self.assertEqual(extra['1']['name'], 'Thread 1')
        self.assertIn(frame(b'qXfer:threads:read::11,400'), stub.socket.sent)
        self.assertEqual(debug.read_thread_xml(stub, {})[0], {})

    def test_xml_rejects_invalid_and_entities(self):
        for reply in (b'm', b'l<broken', b'l<wrong/>', b'l<!DOCTYPE threads><threads/>',
                      b'l<threads><thread id="1"/><thread id="1"/></threads>'):
            with self.subTest(reply=reply):
                with self.assertRaises(gdb.GDBProtocolError):
                    debug.read_thread_xml(self.stub(reply), {'qXfer:threads:read': True})

    def test_thread_and_xml_bounds(self):
        ids = ','.join(f'{i:x}' for i in range(1, debug.MAX_THREADS + 2)).encode()
        with self.assertRaises(gdb.GDBProtocolError):
            debug.list_threads(self.stub(b'm' + ids))
        with patch.object(debug, 'MAX_XML', 16):
            with self.assertRaises(gdb.GDBProtocolError):
                debug.read_thread_xml(self.stub(b'l<threads>too much text</threads>'),
                                      {'qXfer:threads:read': True})

    def test_bad_supported_packet_size(self):
        for value in (b'no', b'0', b'PacketSize+', b''):
            reply = value if value == b'PacketSize+' else b'PacketSize=' + value
            with self.assertRaises(gdb.GDBProtocolError):
                debug.capabilities(self.stub(reply))

    def test_current_thread_fallback_does_not_invent_id(self):
        self.assertEqual(debug.selected_thread(self.stub(b''), b'T050f:00000000;thread:2;'), '2')
        self.assertIsNone(debug.selected_thread(self.stub(b''), b'S05'))
        self.assertEqual(debug.selected_thread(self.stub(b'QCp1.2'), b'S05'), 'p1.2')

    def test_snapshot_resumes_after_capture_exception(self):
        transport = MockTransport(frame(b'T05thread:1;'), b'+')
        stub = gdb.Stub(1, sock=transport)
        record = {}
        with self.assertRaisesRegex(ValueError, 'capture failed'):
            with debug.guest_session(stub, snapshot=True, state='running', record=record):
                stub.deadline = time.monotonic() - 1
                raise ValueError('capture failed')
        self.assertEqual(transport.sent[-1], frame(b'c'))
        self.assertIn('acknowledged', record['resume'])

    def test_snapshot_resumes_even_if_pause_reply_is_malformed(self):
        transport = MockTransport(frame(b'OK'), b'+')
        stub = gdb.Stub(1, sock=transport)
        with self.assertRaises(gdb.GDBProtocolError):
            with debug.guest_session(stub, snapshot=True, state='running'):
                self.fail('Invalid pause must not yield capture')
        self.assertEqual(transport.sent[-1], frame(b'c'))

    def test_resume_failure_is_explicit(self):
        stub = gdb.Stub(1, sock=MockTransport(frame(b'T05'), b'-'))
        record = {}
        with self.assertRaises(gdb.GDBProtocolError):
            with debug.guest_session(stub, snapshot=True, state='running', record=record):
                pass
        self.assertEqual(record['resume'], 'failed; guest state unknown')

    def test_resume_failure_retains_original_capture_error(self):
        stub = gdb.Stub(1, sock=MockTransport(frame(b'T05'), b'-'))
        record = {}
        with self.assertRaises(gdb.GDBProtocolError):
            with debug.guest_session(stub, snapshot=True, state='running', record=record):
                raise ValueError('original read failed')
        self.assertEqual(record['capture_error']['message'], 'original read failed')
        self.assertIn('resume_error', record)

    def test_stopped_guest_has_no_explicit_continue(self):
        stub = self.stub(b'T05')
        with debug.guest_session(stub, snapshot=True, state='stopped') as (record, _):
            pass
        self.assertNotIn(frame(b'c'), stub.socket.sent)
        self.assertIn('resumes on disconnect', record['disconnect_behavior'])
        with self.assertRaises(ValueError):
            with debug.guest_session(stub, snapshot=False, state='running'):
                pass

    def test_private_write_is_exclusive_and_owner_only(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'capture.bin'
            debug.private_write(path, b'payload')
            self.assertEqual(path.read_bytes(), b'payload')
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            with self.assertRaises(FileExistsError):
                debug.private_write(path, b'replace')
            self.assertEqual(path.read_bytes(), b'payload')


class CLITests(unittest.TestCase):
    def run_cli(self, server, *args):
        output = io.StringIO()
        with redirect_stdout(output):
            status = debug.main(['--port', str(server.port), '--timeout', '1',
                                 '--deadline', '5', '--snapshot', '--initial-state', 'running', *args])
        return status, json.loads(output.getvalue())

    def test_inventory_reports_only_observed_fields_and_restores_selection(self):
        with MockServer() as server:
            status, result = self.run_cli(server, 'inventory')
        self.assertEqual(status, 0)
        self.assertEqual(result['status'], 'complete')
        self.assertEqual(result['inventory']['selected_thread_before'], '1')
        self.assertTrue(result['inventory']['selection_restored'])
        self.assertEqual([x['id'] for x in result['inventory']['threads']], ['1', '2'])
        first = result['inventory']['threads'][0]
        self.assertEqual(first['state']['status'], 'unavailable')
        self.assertIsNone(first['priority']['value'])
        self.assertEqual(first['name']['value'], 'Thread 1')
        self.assertEqual(first['registers']['pc']['hex'], '0x00001234')
        self.assertEqual(first['registers']['cpsr']['status'], 'unavailable')
        self.assertEqual(server.commands[-2:], [b'Hg1', b'c'])
        self.assertFalse(any(c.startswith(b'm') for c in server.commands))
        self.assertNotIn(b'p f', server.commands)

    def test_unsupported_thread_queries_and_selection_are_reported(self):
        def handler(command):
            if command in (b'qfThreadInfo', b'qSupported'):
                return b''
            return MockServer.reply(command)
        with MockServer(handler) as server:
            status, result = self.run_cli(server, 'inventory')
        self.assertEqual(status, 0)
        self.assertEqual(result['inventory']['status'], 'unavailable')

        def handler(command):
            return b'' if command.startswith(b'Hg') else MockServer.reply(command)
        with MockServer(handler) as server:
            status, result = self.run_cli(server, 'inventory')
        self.assertEqual(status, 0)
        self.assertEqual(result['inventory']['threads'][0]['register_status']['status'], 'unavailable')

    def test_dump_chunks_metadata_checksums_and_readback(self):
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / 'range.bin'
            with MockServer() as server:
                status, result = self.run_cli(server, 'dump', '--address', '0x1000',
                                              '--size', '150', '--binary', str(binary))
            self.assertEqual(status, 0)
            self.assertEqual(binary.read_bytes(), bytes(range(150)))
            metadata = json.loads(Path(str(binary) + '.json').read_text())
            self.assertEqual(metadata, result)
            self.assertEqual(result['checksums'], debug.checksums(bytes(range(150))))
            self.assertTrue(result['binary_readback_verified'])
            self.assertEqual([c for c in server.commands if c.startswith(b'm')],
                             [b'm1000,40', b'm1040,40', b'm1080,16'])

    def test_read_json_has_explicit_range_and_no_file_required(self):
        with MockServer() as server:
            status, result = self.run_cli(server, 'read', '--address', '0x1234', '--size', '4')
        self.assertEqual(status, 0)
        self.assertEqual(result['data_hex'], '34353637')
        self.assertEqual(result['range']['end_exclusive'], '0x00001238')

    def test_read_failure_resumes_and_creates_error_metadata_without_binary(self):
        def handler(command):
            return b'E0e' if command.startswith(b'm') else MockServer.reply(command)
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / 'range.bin'
            with MockServer(handler) as server:
                status, result = self.run_cli(server, 'dump', '--address', '0x1000',
                                              '--size', '4', '--binary', str(binary))
            self.assertEqual(status, 1)
            self.assertEqual(result['status'], 'error')
            self.assertIn('E0e', result['error']['message'])
            self.assertIn('acknowledged', result['session']['resume'])
            self.assertFalse(binary.exists())
            self.assertTrue(Path(str(binary) + '.json').exists())
            self.assertEqual(server.commands[-1], b'c')

    def test_bad_cli_range_and_existing_output_fail_before_connect(self):
        cases = [ ['--initial-state', 'running', 'inventory'],
                  ['--initial-state', 'stopped', '--timeout', 'nan', 'inventory'],
                  ['--initial-state', 'stopped', 'read', '--address', '0xffffffff', '--size', '2'],
                  ['--initial-state', 'stopped', 'read', '--address', '0', '--size', '4097'] ]
        with tempfile.TemporaryDirectory() as temporary:
            existing = Path(temporary) / 'existing.json'
            existing.write_text('preserve')
            cases.append(['--initial-state', 'stopped', '--output', str(existing), 'inventory'])
            for args in cases:
                with self.subTest(args=args), patch.object(gdb, 'Stub') as connect:
                    with self.assertRaises(SystemExit), redirect_stdout(io.StringIO()), \
                            unittest.mock.patch('sys.stderr', io.StringIO()):
                        debug.main(args)
                    connect.assert_not_called()
            self.assertEqual(existing.read_text(), 'preserve')

    def test_cli_connection_failure_retains_requested_range(self):
        with patch('gdb_sampler.socket.create_connection', side_effect=ConnectionRefusedError('mock refusal')):
            output = io.StringIO()
            with redirect_stdout(output):
                status = debug.main(['--initial-state', 'stopped', 'read', '--address', '0x1000', '--size', '4'])
        result = json.loads(output.getvalue())
        self.assertEqual(status, 1)
        self.assertEqual(result['status'], 'error')
        self.assertEqual(result['range']['address'], '0x00001000')

    def test_metadata_failure_never_reports_complete(self):
        with tempfile.TemporaryDirectory() as temporary:
            missing = Path(temporary) / 'missing' / 'metadata.json'
            with MockServer() as server:
                status, result = self.run_cli(server, '--output', str(missing), 'inventory')
        self.assertEqual(status, 1)
        self.assertEqual(result['status'], 'error')
        self.assertIn('metadata_write_error', result)


if __name__ == '__main__':
    unittest.main(verbosity=2)
