#!/usr/bin/env python3
"""Bounded local guest diagnostics over Azahar's GDB stub. No emulator launch.

All addresses are explicit 32-bit *guest virtual* addresses in the main launched
application, never host addresses. No process enumeration, arbitrary scans,
pointer chasing, register/memory writes, or system-process attachment is used.
"""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import time
import xml.etree.ElementTree as ET

import gdb_sampler as gdb


MAX_READ = 16 * 1024 * 1024
MAX_INLINE = 4096
MAX_CHUNK = 1024
MAX_THREADS = 256
MAX_XML = 256 * 1024
REGISTERS = [(i, 'r' + str(i)) for i in range(13)] + [
    (13, 'sp'), (14, 'lr'), (15, 'pc'), (25, 'cpsr')]
THREAD_ID = re.compile(r'(?:[1-9a-fA-F][0-9a-fA-F]{0,7}|p[0-9a-fA-F]{1,8}\.[0-9a-fA-F]{1,8})\Z')


def timestamp():
    return datetime.now(timezone.utc).isoformat()


def unavailable(reason):
    return {'status': 'unavailable', 'value': None, 'reason': reason}


def thread_id(value):
    if not THREAD_ID.fullmatch(value):
        raise gdb.GDBProtocolError('Malformed guest thread ID')
    return value.lower()


def optional_query(stub, command):
    """Only unsupported/remote errors are optional; corruption/timeouts are fatal."""
    try:
        reply = stub.command(command)
    except gdb.GDBRemoteError as exc:
        return None, str(exc)
    if not reply:
        return None, 'Unsupported query (empty RSP reply)'
    return reply, None


def capabilities(stub):
    reply, error = optional_query(stub, b'qSupported')
    result = {'status': 'unavailable' if error else 'available', 'features': {}}
    if error:
        result['reason'] = error
        return result
    try:
        text = reply.decode('ascii')
    except UnicodeDecodeError as exc:
        raise gdb.GDBProtocolError('Non-ASCII qSupported reply') from exc
    for feature in text.split(';'):
        if not feature:
            continue
        if '=' in feature:
            key, value = feature.split('=', 1)
        elif feature.endswith(('+', '-')):
            key, value = feature[:-1], feature[-1] == '+'
        else:
            key, value = feature, True
        result['features'][key] = value
    size = result['features'].get('PacketSize')
    if size is not None:
        if not isinstance(size, str) or not re.fullmatch(r'[0-9a-fA-F]+', size) or int(size, 16) < 32:
            raise gdb.GDBProtocolError('Invalid advertised PacketSize')
    return result


def read_thread_xml(stub, features):
    if features.get('qXfer:threads:read') is not True:
        return {}, 'Thread XML not advertised'
    data = bytearray()
    for _ in range(MAX_XML // 1024 + 1):
        reply, error = optional_query(stub, b'qXfer:threads:read::%x,400' % len(data))
        if error:
            return {}, error
        if reply[:1] not in (b'm', b'l') or (reply[:1] == b'm' and len(reply) == 1):
            raise gdb.GDBProtocolError('Malformed qXfer thread reply')
        data.extend(reply[1:])
        if len(data) > MAX_XML:
            raise gdb.GDBProtocolError('Thread XML exceeds limit')
        if reply[:1] == b'l':
            break
    else:
        raise gdb.GDBProtocolError('Too many thread XML chunks')
    # The stub produces plain XML. Never expand peer-supplied DTD/entities.
    if b'<!DOCTYPE' in data.upper() or b'<!ENTITY' in data.upper():
        raise gdb.GDBProtocolError('DTD/entities are not allowed in thread XML')
    try:
        root = ET.fromstring(bytes(data))
    except ET.ParseError as exc:
        raise gdb.GDBProtocolError('Malformed thread XML') from exc
    if root.tag != 'threads':
        raise gdb.GDBProtocolError('Unexpected thread XML root')
    result = {}
    for entry in root:
        if entry.tag != 'thread':
            continue
        ident = thread_id(entry.get('id', ''))
        if ident in result or len(result) >= MAX_THREADS:
            raise gdb.GDBProtocolError('Duplicate/excessive thread XML entries')
        result[ident] = {key: value for key, value in entry.attrib.items() if key != 'id'}
    return result, None


def list_threads(stub):
    ids = []
    command = b'qfThreadInfo'
    for _ in range(MAX_THREADS + 1):
        reply, error = optional_query(stub, command)
        if error:
            return None, error
        if reply == b'l':
            return ids, None
        if not reply.startswith(b'm'):
            raise gdb.GDBProtocolError('Malformed thread enumeration reply')
        try:
            page = [thread_id(item) for item in reply[1:].decode('ascii').split(',')]
        except UnicodeDecodeError as exc:
            raise gdb.GDBProtocolError('Non-ASCII thread enumeration') from exc
        for ident in page:
            if ident in ids or len(ids) >= MAX_THREADS:
                raise gdb.GDBProtocolError('Duplicate/excessive guest thread IDs')
            ids.append(ident)
        command = b'qsThreadInfo'
    raise gdb.GDBProtocolError('Thread enumeration did not terminate')


def selected_thread(stub, stop_reply):
    reply, error = optional_query(stub, b'qC')
    if error is None:
        try:
            if not reply.startswith(b'QC'):
                raise gdb.GDBProtocolError('Malformed current-thread reply')
            return thread_id(reply[2:].decode('ascii'))
        except UnicodeDecodeError as exc:
            raise gdb.GDBProtocolError('Non-ASCII current-thread ID') from exc
    match = re.search(rb'(?:^|;)thread:([^;]+);?', stop_reply[3:])
    if match:
        return thread_id(match[1].decode('ascii'))
    return None


def select_thread(stub, ident):
    reply = stub.command(b'Hg' + thread_id(ident).encode('ascii'))
    if not reply:
        raise gdb.GDBUnsupportedError('Guest thread selection is unsupported')
    if reply != b'OK':
        raise gdb.GDBProtocolError('Guest thread selection unsupported or malformed')


def inventory(stub, stop_reply, features):
    ids, error = list_threads(stub)
    extra, xml_error = read_thread_xml(stub, features)
    if ids is None and extra:
        ids, error = list(extra), None
    if ids is None:
        return {'status': 'unavailable', 'reason': error, 'threads': []}
    original = selected_thread(stub, stop_reply)
    result = {'status': 'available', 'selected_thread_before': original,
              'thread_xml': unavailable(xml_error) if xml_error else {'status': 'available'},
              'register_layout': 'ARM little-endian r0-r15 and CPSR (RSP number 25)',
              'selection_restored': False, 'threads': []}
    changed_selection = False
    try:
        for ident in ids:
            info = extra.get(ident, {})
            entry = {'id': ident, 'stub_attributes': info,
                     'name': {'status': 'available', 'value': info['name'],
                              'source': 'stub-reported label; may be generated'} if 'name' in info
                             else unavailable('No thread name supplied by stub'),
                     'state': unavailable('Not supplied by supported GDB queries'),
                     'priority': unavailable('Not supplied by supported GDB queries'),
                     'wait_reason': unavailable('Not supplied by supported GDB queries'),
                     'registers': {}}
            result['threads'].append(entry)
            try:
                select_thread(stub, ident)
                changed_selection = True
            except (gdb.GDBRemoteError, gdb.GDBUnsupportedError) as exc:
                entry['register_status'] = unavailable(str(exc))
                continue
            for number, name in REGISTERS:
                try:
                    value = stub.register(number)
                    entry['registers'][name] = unavailable('Register not exposed by stub') if value is None else {
                        'status': 'available', 'rsp_number': number,
                        'value': value, 'hex': f'0x{value:08x}'}
                except gdb.GDBRemoteError as exc:
                    entry['registers'][name] = unavailable(str(exc))
    finally:
        if original is not None and changed_selection:
            select_thread(stub, original)
            result['selection_restored'] = True
        else:
            result['selection_restore_reason'] = ('No thread selection succeeded' if not changed_selection
                                                   else 'Original selected thread unavailable')
    return result


def validate_range(address, length, limit=MAX_READ):
    if not 0 <= address < 2 ** 32 or not 0 < length <= min(limit, MAX_READ):
        raise ValueError(f'Explicit guest address and size required (maximum {min(limit, MAX_READ)} bytes)')
    if address + length > 2 ** 32:
        raise ValueError('Guest range wraps the 32-bit address space')


def read_memory(stub, address, length, *, chunk_size=MAX_CHUNK, packet_size=None):
    validate_range(address, length)
    if not 1 <= chunk_size <= MAX_CHUNK:
        raise ValueError('Memory chunks must be 1..1024 bytes')
    if packet_size is not None:
        if packet_size < 32:
            raise ValueError('PacketSize too small')
        chunk_size = min(chunk_size, packet_size // 2)
    data = bytearray()
    while len(data) < length:
        count = min(chunk_size, length - len(data))
        reply = stub.command(b'm%x,%x' % (address + len(data), count))
        if not reply:
            raise gdb.GDBError('Guest memory reads unsupported')
        if len(reply) != count * 2 or not re.fullmatch(rb'[0-9a-fA-F]+', reply):
            raise gdb.GDBProtocolError('Memory reply is short, oversized, unavailable or non-hex')
        data.extend(bytes.fromhex(reply.decode('ascii')))
    return bytes(data)


def checksums(data):
    fnv = 2166136261
    for value in data:
        fnv = ((fnv ^ value) * 16777619) & 0xffffffff
    return {'sha256': hashlib.sha256(data).hexdigest(), 'fnv1a32': f'{fnv:08x}'}


def private_write(path, data):
    """Exclusive owner-only artifact; never overwrite earlier evidence."""
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    # On a write failure leave the incomplete artifact as evidence; the caller
    # reports error and never calls it a complete dump.
    with os.fdopen(fd, 'wb') as output:
        output.write(data)
        output.flush()
        os.fsync(output.fileno())
    if Path(path).read_bytes() != data:
        raise OSError('Diagnostic artifact readback differs from written bytes')


@contextmanager
def guest_session(stub, *, snapshot, state, record=None):
    """Select/pause main app. Resume in finally only on explicit running snapshot.

    RSP cannot discover the prior run state reliably. Caller must supply it.
    The pinned stub resumes on disconnect, so a CLI cannot preserve a preexisting
    stop after closing; this is reported rather than promising false restoration.
    """
    if record is None:
        record = {}
    record.update({'snapshot': snapshot, 'declared_initial_state': state,
              'pause_method': '? (select main application and stop)',
              'resume': 'not_requested',
              'disconnect_behavior': 'stub-dependent; Azahar 2126.2 resumes on disconnect'})
    if state not in ('running', 'stopped'):
        raise ValueError('Initial state must be explicitly running or stopped')
    if state == 'running' and not snapshot:
        raise ValueError('A running guest requires --snapshot for a bounded pause/resume')
    try:
        stop = stub.require_stop(stub.command(b'?'))
        record['stop_reply'] = stop.decode('ascii')
        yield record, stop
    except BaseException as exc:
        record['capture_error'] = {'type': type(exc).__name__, 'message': str(exc)}
        raise
    finally:
        if snapshot and state == 'running':
            # Cleanup gets a fresh per-operation budget if the capture budget expired.
            stub.deadline = None
            try:
                stub.resume(wait_ack=True)
                record['resume'] = 'continue_acknowledged; execution not independently observed'
            except (gdb.GDBError, OSError, EOFError) as exc:
                record['resume'] = 'failed; guest state unknown'
                record['resume_error'] = str(exc)
                raise


def integer(text):
    try:
        return int(text, 16 if text.lower().startswith('0x') else 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError('Use decimal or 0x-prefixed hexadecimal') from exc


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument('--port', type=int, default=24689, help='127.0.0.1 only (default: 24689)')
    result.add_argument('--timeout', type=float, default=3, help='Per-operation timeout, seconds')
    result.add_argument('--deadline', type=float, default=30, help='Whole capture budget, seconds (plus bounded resume)')
    result.add_argument('--snapshot', action='store_true', help='Pause main app; finally resume if declared running')
    result.add_argument('--initial-state', choices=('running', 'stopped'), required=True,
                        help='Explicit caller knowledge; cannot be inferred from the stub')
    result.add_argument('--output', type=Path, help='Exclusive JSON metadata output (also printed to stdout)')
    commands = result.add_subparsers(dest='action', required=True)
    commands.add_parser('inventory', help='Guest threads and ARM core registers; no memory reads')
    for name in ('read', 'dump'):
        cmd = commands.add_parser(name, help='Read one explicit guest virtual range')
        cmd.add_argument('--address', type=integer, required=True)
        cmd.add_argument('--size', type=integer, required=True,
                         help='Bytes; read maximum 4096, dump maximum 16 MiB')
        cmd.add_argument('--chunk-size', type=integer, default=MAX_CHUNK, help='1..1024 bytes per request')
        if name == 'dump':
            cmd.add_argument('--binary', type=Path, required=True, help='Exclusive raw binary dump path')
    return result


def main(argv=None):
    cli = parser()
    args = cli.parse_args(argv)
    if not 1 <= args.port <= 65535 or not math.isfinite(args.timeout) or not 0 < args.timeout <= 60:
        cli.error('Port must be 1..65535 and timeout must be finite, positive and <=60s')
    if not math.isfinite(args.deadline) or not 0 < args.deadline <= 300:
        cli.error('Capture deadline must be finite, positive and <=300s')
    if args.initial_state == 'running' and not args.snapshot:
        cli.error('A running guest requires --snapshot')
    if args.action != 'inventory':
        try:
            validate_range(args.address, args.size, MAX_INLINE if args.action == 'read' else MAX_READ)
            if not 1 <= args.chunk_size <= MAX_CHUNK:
                raise ValueError('Chunk size must be 1..1024 bytes')
        except ValueError as exc:
            cli.error(str(exc))
    if args.action == 'dump' and args.output is None:
        args.output = Path(str(args.binary) + '.json')
    for path in (args.output, getattr(args, 'binary', None)):
        if path is not None and path.exists():
            cli.error(f'Output already exists: {path}')
    if args.action == 'dump' and args.output.resolve() == args.binary.resolve():
        cli.error('JSON metadata and binary paths must differ')
    record = {'schema': 'dusklight.azahar-debug.v1', 'status': 'incomplete',
              'started_at': timestamp(), 'action': args.action,
              'origin': 'Azahar guest GDB evidence; not physical New3DS performance',
              'endpoint': f'127.0.0.1:{args.port}', 'address_space': 'main application guest virtual ARM32',
              'privacy': 'local_only', 'capture_deadline_seconds': args.deadline}
    if args.action != 'inventory':
        record['range'] = {'address': f'0x{args.address:08x}', 'size': args.size,
                           'end_exclusive': f'0x{args.address + args.size:08x}'}
        record['requested_chunk_bytes'] = args.chunk_size
    start = time.monotonic()
    stub = None
    data = None
    try:
        stub = gdb.Stub(args.port, timeout=args.timeout,
                        connect_timeout=min(args.timeout, args.deadline), attempts=1)
        stub.deadline = start + args.deadline
        # Set this before entry so errors during pause also retain state metadata.
        record['session'] = {}
        with guest_session(stub, snapshot=args.snapshot, state=args.initial_state,
                           record=record['session']) as (session, stop):
            cap = capabilities(stub)
            record['capabilities'] = cap
            features = cap['features']
            if args.action == 'inventory':
                record['inventory'] = inventory(stub, stop, features)
            else:
                size = features.get('PacketSize')
                data = read_memory(stub, args.address, args.size, chunk_size=args.chunk_size,
                                   packet_size=int(size, 16) if size is not None else None)
        # Disk work occurs after the bounded guest pause/resume window.
        if data is not None:
            record['checksums'] = checksums(data)
        if args.action == 'dump':
            private_write(args.binary, data)
            record['binary'] = str(args.binary.resolve())
            record['binary_readback_verified'] = True
        elif args.action == 'read':
            record['data_hex'] = data.hex()
        record['status'] = 'complete'
    except (gdb.GDBError, OSError, EOFError, ValueError) as exc:
        record['status'] = 'error'
        record['error'] = {'type': type(exc).__name__, 'message': str(exc)}
    except KeyboardInterrupt:
        record['status'] = 'error'
        record['error'] = {'type': 'KeyboardInterrupt', 'message': 'Capture interrupted by caller'}
    finally:
        if stub is not None:
            stub.close()
        record['finished_at'] = timestamp()
        record['elapsed_seconds'] = round(time.monotonic() - start, 6)
    encoded = (json.dumps(record, indent=2) + '\n').encode('utf-8')
    if args.output:
        try:
            private_write(args.output, encoded)
        except OSError as exc:
            record['status'] = 'error'
            record['metadata_write_error'] = str(exc)
    print(json.dumps(record, indent=2))
    return 0 if record['status'] == 'complete' else 1


if __name__ == '__main__':
    raise SystemExit(main())
