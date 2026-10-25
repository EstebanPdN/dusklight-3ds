#!/usr/bin/env python3
"""Bounded guest thread-context observations; Azahar RSP is not a CPU profiler."""
import bisect
import collections
import json
import math
import os
from pathlib import Path
import re
import socket
import subprocess
import time

NM = Path(os.environ.get('DEVKITARM', Path.home() / 'devkitpro/devkitARM')) / 'bin/arm-none-eabi-nm'
NM_TIMEOUT = 10.0


class GDBError(Exception):
    """A remote diagnostic failed; no payload should be used as evidence."""


class GDBProtocolError(GDBError):
    pass


class GDBTimeout(GDBError, TimeoutError):
    pass


class GDBUnsupportedError(GDBError):
    pass


class GDBRemoteError(GDBError):
    def __init__(self, reply):
        self.reply = reply
        super().__init__('GDB stub returned ' + reply.decode('ascii', errors='replace'))


def packet(data):
    """Encode an RSP payload; the checksum covers the escaped wire bytes."""
    wire = bytearray()
    for value in data:
        if value in b'$#}*':
            wire.extend((ord('}'), value ^ 0x20))
        else:
            wire.append(value)
    return b'$' + wire + b'#' + ('%02x' % (sum(wire) & 0xff)).encode()


def decode_payload(wire, limit):
    decoded = bytearray()
    i = 0
    while i < len(wire):
        value = wire[i]
        i += 1
        if value == ord('}'):
            if i == len(wire):
                raise GDBProtocolError('Truncated RSP escape')
            value = wire[i] ^ 0x20
            i += 1
        elif value == ord('*'):
            if not decoded or i == len(wire):
                raise GDBProtocolError('Invalid RSP run-length encoding')
            count = wire[i] - 29
            if not 3 <= count <= 97 or wire[i] in b'$#+-':
                raise GDBProtocolError('Invalid RSP repeat count')
            i += 1
            if len(decoded) + count > limit:
                raise GDBProtocolError('Expanded RSP payload exceeds limit')
            decoded.extend([decoded[-1]] * count)
            continue
        elif value == ord('$'):
            raise GDBProtocolError('Unescaped packet start within RSP payload')
        decoded.append(value)
        if len(decoded) > limit:
            raise GDBProtocolError('RSP payload exceeds limit')
    return bytes(decoded)


class Stub:
    """Single-client, ACK-mode localhost RSP transport (not thread safe).

    Existing Stub(port), command/resume/interrupt/register APIs are retained.
    Errors now fail explicitly instead of silently decoding corrupt packets.
    Each operation has an absolute timeout, even when a peer trickles bytes.
    """
    def __init__(self, port, attempts=400, *, timeout=3.0, connect_timeout=10.0,
                 sock=None, max_payload=65536):
        if not 0 < timeout <= 300 or not 0 < connect_timeout <= 300:
            raise ValueError('Timeouts must be positive and at most 300 seconds')
        if not 1 <= max_payload <= 1024 * 1024 or attempts < 1:
            raise ValueError('Invalid packet limit or connection attempts')
        self.timeout = timeout
        self.max_payload = max_payload
        self.buffer = b''
        self.socket = sock
        self.last_error = None
        self.last_reply = None
        self.deadline = None  # Optional absolute deadline for a whole diagnostic.
        if sock is not None:
            return
        if not 1 <= port <= 65535:
            raise ValueError('Invalid localhost GDB port')
        deadline = time.monotonic() + connect_timeout
        error = None
        for attempt in range(attempts):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            try:
                self.socket = socket.create_connection(('127.0.0.1', port),
                                                       timeout=min(timeout, remaining))
                break
            except OSError as exc:
                error = exc
                if attempt + 1 < attempts:
                    time.sleep(min(0.25, max(0, deadline - time.monotonic())))
        if self.socket is None:
            raise GDBError('Local GDB stub did not accept a connection') from error

    def close(self):
        if self.socket is not None:
            self.socket.close()
            self.socket = None

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.close()

    def _remaining(self, deadline):
        if self.deadline is not None:
            deadline = min(deadline, self.deadline)
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise GDBTimeout('GDB operation timed out (possibly partial packet)')
        if self.socket is None:
            raise GDBError('GDB transport is closed')
        self.socket.settimeout(remaining)

    def _send(self, data, deadline):
        self._remaining(deadline)
        try:
            self.socket.sendall(data)
        except socket.timeout as exc:
            raise GDBTimeout('GDB send timed out') from exc

    def _receive(self, deadline):
        self._remaining(deadline)
        try:
            chunk = self.socket.recv(4096)
        except socket.timeout as exc:
            raise GDBTimeout('GDB reply timed out (possibly partial packet)') from exc
        if not chunk:
            raise EOFError('GDB disconnected before a complete reply')
        self.buffer += chunk

    def _event(self, deadline):
        while True:
            if not self.buffer:
                self._receive(deadline)
            first = self.buffer[:1]
            if first in (b'+', b'-'):
                self.buffer = self.buffer[1:]
                return first, b''
            if first != b'$':
                raise GDBProtocolError('Unexpected byte outside RSP packet')
            end = self.buffer.find(b'#', 1)
            if (end < 0 and len(self.buffer) > self.max_payload + 1) or end > self.max_payload + 1:
                raise GDBProtocolError('RSP wire payload exceeds limit')
            if end < 0 or len(self.buffer) < end + 3:
                self._receive(deadline)
                continue
            wire, checksum = self.buffer[1:end], self.buffer[end + 1:end + 3]
            self.buffer = self.buffer[end + 3:]
            if not re.fullmatch(rb'[0-9a-fA-F]{2}', checksum):
                self._send(b'-', deadline)
                raise GDBProtocolError('Non-hex RSP checksum')
            if (sum(wire) & 0xff) != int(checksum, 16):
                self._send(b'-', deadline)
                raise GDBProtocolError('RSP checksum mismatch')
            data = decode_payload(wire, self.max_payload)
            self._send(b'+', deadline)
            return b'$', data

    def read_packet(self, *, deadline=None):
        deadline = deadline if deadline is not None else time.monotonic() + self.timeout
        while True:
            kind, data = self._event(deadline)
            if kind == b'-':
                raise GDBProtocolError('GDB rejected command checksum (NACK)')
            if kind == b'$':
                return data

    def command(self, data):
        deadline = time.monotonic() + self.timeout
        if len(data) > self.max_payload:
            raise ValueError('Command exceeds RSP payload limit')
        self._send(packet(data), deadline)
        reply = self.read_packet(deadline=deadline)
        self.last_reply = reply
        if re.fullmatch(rb'E[0-9a-fA-F]{2}(?:;.*)?', reply):
            raise GDBRemoteError(reply)
        return reply

    def resume(self, *, wait_ack=False):
        # Continue has no immediate packet reply in all-stop RSP. Its ACK is
        # consumed by the next read; do not wait for another stop here.
        deadline = time.monotonic() + self.timeout
        self._send(packet(b'c'), deadline)
        if wait_ack:
            kind, data = self._event(deadline)
            if kind != b'+':
                raise GDBProtocolError('Continue was not acknowledged; guest state is unknown')

    def interrupt(self):
        deadline = time.monotonic() + self.timeout
        self._send(b'\x03', deadline)
        reply = self.read_packet(deadline=deadline)
        self.require_stop(reply)
        return reply

    @staticmethod
    def require_stop(reply):
        if re.fullmatch(rb'S[0-9a-fA-F]{2}', reply):
            return reply
        if re.fullmatch(rb'T[0-9a-fA-F]{2}[ -~]*', reply):
            # Some Azahar versions emit a leading ';' before thread: when no
            # expedited registers are present. Other fields remain key:value.
            fields = reply[3:].removeprefix(b';').removesuffix(b';')
            if not fields or all(re.fullmatch(rb'[A-Za-z0-9_.-]+:[^;]*', field)
                                 for field in fields.split(b';')):
                return reply
        if reply[:1] in (b'W', b'X'):
            raise GDBError('Guest has exited: ' + reply.decode('ascii', errors='replace'))
        raise GDBProtocolError('Expected guest stop reply')

    def register(self, number, bits=32):
        if not 0 <= number <= 255 or bits not in (32, 64):
            raise ValueError('Invalid ARM register number or width')
        reply = self.command(b'p%x' % number)
        if not reply:
            return None
        if len(reply) != bits // 4:
            raise GDBProtocolError('Invalid register reply width')
        if re.fullmatch(rb'x+', reply):
            return None
        if not re.fullmatch(rb'[0-9a-fA-F]+', reply):
            raise GDBProtocolError('Invalid register reply width or encoding')
        return int.from_bytes(bytes.fromhex(reply.decode('ascii')), 'little')


class SampleCounts(collections.Counter):
    """Counter-compatible observations carrying quality through symbolize's API."""


def stopped_thread(reply):
    """Return the stop's selected register thread, never infer an active CPU thread.

    Azahar 2126.2 Ctrl-C preserves current_thread. qC is unsupported and Hg0
    also preserves selection, so neither can discover the executing thread.
    """
    Stub.require_stop(reply)
    fields = reply[3:].strip(b';').split(b';') if reply[:1] == b'T' else []
    ids = [field[7:] for field in fields if field.startswith(b'thread:')]
    if not ids:
        raise GDBUnsupportedError('Stop did not identify its selected guest thread')
    if len(ids) != 1 or not re.fullmatch(rb'[0-9a-fA-F]{1,8}', ids[0]) or not 0 < int(ids[0], 16) <= 0x7fffffff:
        raise GDBProtocolError('Invalid or duplicate stopped guest thread ID')
    return b'%x' % int(ids[0], 16)


def sample(stub, seconds, interval, alive):
    """Observe selected saved contexts, resume all threads, retain the Counter API.

    The pinned stub supplies no pre-stop CPU identity or execution counter.
    Context variation is only a diagnostic, not proof of statistical fidelity.
    Caller must already own a known-running, initialized main-app GDB session.
    """
    pcs, pairs = SampleCounts(), collections.Counter()
    if not math.isfinite(seconds) or not math.isfinite(interval) or seconds < 0 or interval <= 0:
        raise ValueError('Sampling duration must be nonnegative and interval positive')
    start = time.monotonic()
    previous_deadline = stub.deadline
    deadline = start + seconds
    if previous_deadline is not None:
        deadline = min(deadline, previous_deadline)
    quality = {'schema': 'dusklight.azahar-sample-quality.v1',
               'statistical_profile_valid': False,
               'context_source': 'stop-reported selected thread saved context; not active CPU identity',
               'execution_progress': 'unavailable: no instruction/tick counter in pinned RSP',
               'requested_seconds': seconds, 'interval_seconds': interval,
               'attempts': 0, 'observations': 0, 'context_changes': 0,
               'longest_identical_pc_run': 0, 'selected_threads': {},
               'null_lr_observations': 0, 'unavailable_lr_observations': 0,
               'resume_acknowledged': 0, 'resume_errors': [], 'last_error': None}
    pcs.sample_quality = quality
    stub.sample_quality = quality
    stub.last_error = None
    previous = None
    pc_run = 0
    try:
        while time.monotonic() < deadline and alive():
            stub.deadline = deadline
            quality['attempts'] += 1
            error = None
            try:
                stop = stub.interrupt()
                ident = stopped_thread(stop)
                reply = stub.command(b'Hg' + ident)
                if not reply:
                    raise GDBUnsupportedError('Explicit guest register thread selection unsupported')
                if reply != b'OK':
                    raise GDBProtocolError('Explicit guest register thread selection failed')
                pc, lr = stub.register(15), stub.register(14)
                sp, cpsr = stub.register(13), stub.register(25)
                if pc is None:
                    raise GDBUnsupportedError('Selected guest PC unavailable')
                context = (ident, pc, lr, sp, cpsr)
            except (GDBError, EOFError, ValueError, OSError) as exc:
                error = str(exc)
            finally:
                # A fresh per-operation cleanup budget is allowed after the
                # capture deadline, but never extend a caller's whole-run bound.
                stub.deadline = previous_deadline
                try:
                    # Hg selects registers; Hc independently controls resume.
                    reply = stub.command(b'Hc-1')
                    if reply != b'OK':
                        raise GDBProtocolError('All-thread continue selection failed')
                except (GDBError, EOFError, ValueError, OSError) as exc:
                    quality['resume_errors'].append(str(exc))
                try:
                    stub.resume(wait_ack=True)
                    quality['resume_acknowledged'] += 1
                except (GDBError, EOFError, ValueError, OSError) as exc:
                    quality['resume_errors'].append(str(exc))
                if quality['resume_errors'] and error is None:
                    error = quality['resume_errors'][-1]
            if error is not None:
                stub.last_error = error
                break
            pcs[pc] += 1
            if lr is not None:
                pairs[(pc, lr & ~1)] += 1
            quality['observations'] += 1
            quality['null_lr_observations'] += lr == 0
            quality['unavailable_lr_observations'] += lr is None
            record = {'thread': ident.decode('ascii'), 'stop_reply': stop.decode('ascii'),
                      'pc': pc, 'lr': lr, 'sp': sp, 'cpsr': cpsr}
            if quality['observations'] == 1:
                quality['first_context'] = record
            quality['last_context'] = record
            thread = ident.decode('ascii')
            quality['selected_threads'][thread] = quality['selected_threads'].get(thread, 0) + 1
            if previous is not None and context != previous:
                quality['context_changes'] += 1
            pc_run = pc_run + 1 if previous is not None and pc == previous[1] else 1
            quality['longest_identical_pc_run'] = max(quality['longest_identical_pc_run'], pc_run)
            previous = context
            time.sleep(min(interval, max(0, deadline - time.monotonic())))
    finally:
        stub.deadline = previous_deadline
        total = sum(pcs.values())
        quality.update(unique_pcs=len(pcs), unique_pc_lr_pairs=len(pairs),
                       identical_pc=bool(total and len(pcs) == 1),
                       dominant_pc_fraction=max(pcs.values(), default=0) / max(total, 1),
                       elapsed_seconds=round(time.monotonic() - start, 6))
        quality['diagnostic'] = ('identical_pc; possible stale/sleeping context or tight loop'
                                 if quality['identical_pc'] else 'selected-context observations only')
        if total >= 3 and quality['identical_pc'] and stub.last_error is None:
            stub.last_error = 'Identical PC observations; active CPU context and execution progress unproven'
        quality['last_error'] = stub.last_error
    return pcs, pairs


def symbolize(elf, pcs, pairs, output, top=80):
    symbols = []
    symbol_error = None
    try:
        # run(timeout=...) kills and reaps the nm child before raising.
        result = subprocess.run([str(NM), '-C', '-n', '-S', str(elf)],
                                capture_output=True, text=True, timeout=NM_TIMEOUT)
        if result.returncode != 0:
            symbol_error = f'nm failed (exit {result.returncode}); raw addresses retained'
        else:
            for line in result.stdout.splitlines():
                parts = line.split(maxsplit=3)
                if (len(parts) == 4 and parts[2] in ('t', 'T', 'w', 'W')
                        and re.fullmatch(r'[0-9a-fA-F]+', parts[0])
                        and re.fullmatch(r'[0-9a-fA-F]+', parts[1])):
                    address, size = int(parts[0], 16), int(parts[1], 16)
                    if address and size:
                        symbols.append((address, size, parts[3]))
    except (OSError, subprocess.TimeoutExpired) as exc:
        symbol_error = f'nm {type(exc).__name__}; raw addresses retained'
    symbols.sort()
    addresses = [s[0] for s in symbols]

    def name(address):
        i = bisect.bisect_right(addresses, address) - 1
        # Never assign null, gaps or data LR values to a preceding text symbol.
        return symbols[i][2] if i >= 0 and address < symbols[i][0] + symbols[i][1] else hex(address)
    functions, callers = collections.Counter(), collections.Counter()
    for pc, count in pcs.items():
        functions[name(pc)] += count
    for (pc, lr), count in pairs.items():
        callers[(name(pc), name(lr))] += count
    total = sum(pcs.values())
    denominator = max(total, 1)
    quality = dict(getattr(pcs, 'sample_quality', {}))
    quality.update(statistical_profile_valid=False, symbolization_error=symbol_error,
                   observations=total, unique_pcs=len(pcs),
                   identical_pc=bool(total and len(pcs) == 1),
                   dominant_pc_fraction=max(pcs.values(), default=0) / denominator)
    with open(output, 'w') as out:
        out.write(f'samples={total}\n# Selected saved-thread context observations; NOT statistical CPU self time\n')
        out.write('# Active CPU identity and execution progress are unproven by pinned Azahar RSP\n')
        if quality['identical_pc']:
            out.write('# IDENTICAL PC: possible stale/sleeping context or tight loop; reject hotspot inference\n')
        if symbol_error:
            out.write('# ' + symbol_error + '\n')
        out.write('# observation frequency by symbol\n')
        for function, count in functions.most_common(top):
            out.write(f'{100.0 * count / denominator:6.2f}% {count:6d} {function[:160]}\n')
        out.write('\n# function <- LR symbol (LR is not a validated caller)\n')
        for (function, caller), count in callers.most_common(top):
            out.write(f'{100.0 * count / denominator:6.2f}% {count:6d} {function[:90]}  <-  {caller[:90]}\n')
    with open(Path(output).with_suffix('.samples'), 'w') as out:
        out.writelines(f'{pc:08x} {lr:08x} {count}\n' for (pc, lr), count in sorted(pairs.items()))
    with open(Path(output).with_suffix('.quality.json'), 'w') as out:
        json.dump(quality, out, indent=2)
        out.write('\n')
