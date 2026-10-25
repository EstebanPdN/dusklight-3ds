#!/usr/bin/env python3
"""Bounded emulator smoke test; private ISO read-only, workspace-only writes."""
import argparse
from contextlib import contextmanager
import datetime
import hashlib
import json
import math
import os
import re
import select
from pathlib import Path
import shutil
import signal
import stat
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import capture_to_png  # noqa: E402
import azahar_debug  # noqa: E402
import gdb_sampler  # noqa: E402
import twilight_pack  # noqa: E402
from azahar_run_report import inspect_run, STOP_FAILURE  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_AZAHAR = Path.home() / 'Applications/azahar/azahar-macos-arm64-2126.2/Azahar.app/Contents/MacOS/azahar'
RENDERERS = {'software': 0, 'opengl': 1, 'vulkan': 2}
# Azahar 2126.2 src/audio_core/sink_details.h and common/settings.h.
AUDIO_ENGINES = {'auto': 0, 'null': 1, 'cubeb': 2, 'openal': 3, 'sdl2': 4}
AUDIO_EMULATION = {'hle': 0, 'lle': 1, 'lle-multithreaded': 2}
SCRIPT_FILES = {'input_script': 'input-script.txt', 'capture_frames': 'capture-frames.txt',
                'dump_frames': 'dump-frames.txt', 'depth_audit_frames': 'depth-audit-frames.txt',
                'depth_retained_frames': 'depth-retained-frames.txt'}

# ARM32 LE: Header = magic[8], u32 version, CARDStat (0x6c bytes, align 4).
# CARDStat: fileName[32] at 0, u32 length at 0x20; final u32 at 0x68.
# Source/platforms/3ds/runtime/card.cpp and extern/aurora/include/dolphin/card.h.
NATIVE_CARD_HEADER = 12 + 0x6c
NATIVE_CARD_CAPACITY = 1024 * 1024
NATIVE_CARD_MAX = NATIVE_CARD_HEADER + NATIVE_CARD_CAPACITY
CARD_PRIVATE_DIRS = ('Diagnostics', 'Saves', 'Build')


@contextmanager
def card_private_parent(path, *, create=False):
    """Pin project-private ancestors; never follow a symlink or enter a repo."""
    root = ROOT.resolve()
    path = Path(os.path.abspath(path))
    try:
        relative = path.relative_to(root)
    except ValueError:
        raise ValueError('Native card paths must remain project-private') from None
    if len(relative.parts) < 2 or relative.parts[0] not in CARD_PRIVATE_DIRS or '.git' in relative.parts:
        raise ValueError('Native card paths require Diagnostics, Saves or Build outside Git')
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
    fd = os.open(root, flags)
    try:
        for part in relative.parts[:-1]:
            try:
                os.stat('.git', dir_fd=fd, follow_symlinks=False)
            except FileNotFoundError:
                pass
            else:
                raise ValueError('Native card paths must not enter a Git repository')
            if create:
                try:
                    os.mkdir(part, mode=0o700, dir_fd=fd)
                except FileExistsError:
                    pass
            child = os.open(part, flags, dir_fd=fd)
            os.close(fd)
            fd = child
        try:
            os.stat('.git', dir_fd=fd, follow_symlinks=False)
        except FileNotFoundError:
            pass
        else:
            raise ValueError('Native card paths must not enter a Git repository')
        yield fd, relative.name, path
    finally:
        os.close(fd)


def card_identity(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def inspect_data_bundle(path, *, expected=None):
    """Validate private packed assets without touching the original ISO.

    Full payload verification occurs before launch. Rehash the same file after
    owned-process cleanup; a read-only symlink alone is not immutability proof.
    """
    with card_private_parent(path) as (parent, name, absolute):
        fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=parent)
        with os.fdopen(fd, 'rb') as source:
            before = os.fstat(source.fileno())
            if not stat.S_ISREG(before.st_mode) or not 128 <= before.st_size <= 0x7fffffff:
                raise ValueError('Data bundle must be a bounded regular file')
            if expected is None:
                bundle = twilight_pack.Bundle(source)
                for _, offset, size, digest in bundle.ranges:
                    if twilight_pack.digest_region(source, offset, size) != digest:
                        raise ValueError('Data bundle payload checksum mismatch')
            digest = twilight_pack.digest_region(source, 0, before.st_size).hex()
            if (card_identity(before) != card_identity(os.fstat(source.fileno())) or
                    card_identity(before) != card_identity(os.stat(name, dir_fd=parent, follow_symlinks=False))):
                raise ValueError('Data bundle changed during verification')
    record = {'path': str(absolute), 'identity': list(card_identity(before)),
              'bytes': before.st_size, 'sha256': digest}
    if expected is not None and record != expected:
        raise ValueError('Data bundle changed during emulator execution')
    return record


def read_card_snapshot(path):
    """One bounded descriptor snapshot; reject observed mutation/replacement.

    No locks are imposed on user data. A change after this snapshot is taken
    cannot change the immutable bytes seeded into the new run.
    """
    with card_private_parent(path) as (parent, name, path):
        fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=parent)
        with os.fdopen(fd, 'rb') as source:
            before = os.fstat(source.fileno())
            if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1:
                raise ValueError('Native card must be a regular file with one link')
            if not 0 <= before.st_size <= NATIVE_CARD_MAX:
                raise ValueError('Native card size exceeds the bounded ARM32 container envelope')
            data = source.read(NATIVE_CARD_MAX + 1)
            after = os.fstat(source.fileno())
            current = os.stat(name, dir_fd=parent, follow_symlinks=False)
            if (card_identity(before) != card_identity(after) or
                    card_identity(before) != card_identity(current) or len(data) != before.st_size):
                raise ValueError('Native card changed during snapshot')
    return data, {'path': str(path), 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                  'mtime_ns': before.st_mtime_ns, 'device': before.st_dev, 'inode': before.st_ino}


def validate_native_card(data):
    """Match native valid()/load(); do not interpret original game save data."""
    if len(data) < NATIVE_CARD_HEADER or data[:8] != b'DSVCARD1':
        raise ValueError('Native card requires a DSVCARD1 ARM32 header; no raw GCN/GCI conversion')
    if int.from_bytes(data[8:12], 'little') != 1:
        raise ValueError('Native card version must be 1')
    length = int.from_bytes(data[12 + 0x20:12 + 0x24], 'little')
    if not 0 < length <= NATIVE_CARD_CAPACITY or length % 8192:
        raise ValueError('Native card payload must be 1..128 blocks of 8192 bytes')
    if b'\0' not in data[12:12 + 32] or len(data) != NATIVE_CARD_HEADER + length:
        raise ValueError('Native card filename termination or payload length mismatch')


def seed_native_card(sd, data, expected):
    """Verified private temporary + atomic no-clobber link, before launch only."""
    destination = sd / 'saves/native-card0.bin'
    with card_private_parent(sd / 'saves') as (sd_fd, name, unused):
        os.mkdir(name, mode=0o700, dir_fd=sd_fd)  # Never reuse an old save directory.
        directory = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=sd_fd)
    temporary = '.native-card0.seed'
    created = False
    try:
        fd = os.open(temporary, os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                     0o600, dir_fd=directory)
        created = True
        with os.fdopen(fd, 'w+b') as output:
            if output.write(data) != len(data):
                raise OSError('Short native card seed write')
            output.flush()
            os.fsync(output.fileno())
            output.seek(0)
            copied = output.read(NATIVE_CARD_MAX + 1)
            if copied != data or hashlib.sha256(copied).hexdigest() != expected['sha256']:
                raise OSError('Native card seed failed copy verification')
        # rename/replace would overwrite; link publishes a complete file or fails.
        os.link(temporary, destination.name, src_dir_fd=directory, dst_dir_fd=directory,
                follow_symlinks=False)
        os.unlink(temporary, dir_fd=directory)
        created = False
        os.fsync(directory)
        info = os.stat(destination.name, dir_fd=directory, follow_symlinks=False)
        return {'path': str(destination), 'bytes': len(copied),
                'sha256': hashlib.sha256(copied).hexdigest(), 'mtime_ns': info.st_mtime_ns}
    finally:
        try:
            if created:
                os.unlink(temporary, dir_fd=directory)
        finally:
            os.close(directory)


def final_native_card(path):
    """Observe the post-stop output; it need not have been written by the game."""
    record = {'path': str(path), 'sha256': None}
    try:
        with card_private_parent(path) as (parent, name, unused):
            info = os.stat(name, dir_fd=parent, follow_symlinks=False)
            record.update(bytes=info.st_size, mtime_ns=info.st_mtime_ns)
        data, snapshot = read_card_snapshot(path)
        record.update(snapshot, status='present')
    except FileNotFoundError:
        return {**record, 'status': 'absent'}
    except (ValueError, OSError) as exc:
        return {**record, 'status': 'unavailable', 'error': error_record(exc)}
    try:
        validate_native_card(data)
        record['native_structure_valid'] = True
    except ValueError as exc:
        record['native_structure_valid'] = False
        record['validation_error'] = str(exc)
    return record


def read_dsp_component(path):
    """Bounded image-envelope validation, not firmware/hardware acceptance."""
    path = path.resolve()
    if not path.is_relative_to(ROOT) or not path.is_file() or not 0x300 <= path.stat().st_size <= 1024*1024:
        raise ValueError('DSP component must be a project file of 768 bytes..1 MiB')
    with path.open('rb') as source:
        data = source.read(1024*1024+1)
    if not 0x300 <= len(data) <= 1024*1024 or data[0x100:0x104] != b'DSP1':
        raise ValueError('DSP component has no bounded DSP1 image header')
    declared = int.from_bytes(data[0x104:0x108], 'little')
    if not 0x300 <= declared <= len(data):
        raise ValueError('DSP component declared size exceeds the supplied image')
    return data, {'path': str(path), 'bytes': len(data), 'declared_bytes': declared,
                  'sha256': hashlib.sha256(data).hexdigest(),
                  'validation': 'bounded DSP1 envelope only; HLE does not execute firmware'}


class RunInterrupted(Exception):
    def __init__(self, signum):
        self.signum = signum
        super().__init__(f'Runner interrupted by {signal.Signals(signum).name}')


@contextmanager
def interruption_handlers():
    # A second signal must not interrupt cleanup or receipt persistence.
    interrupted = False
    state = {'cleaning_up': False, 'signal': None}

    def stop(signum, unused_frame):
        nonlocal interrupted
        state['signal'] = signum
        if not interrupted and not state['cleaning_up']:
            interrupted = True
            raise RunInterrupted(signum)

    previous = {sig: signal.signal(sig, stop) for sig in
                (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
    try:
        yield state
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)


class DetachedProcess:
    """Double-fork for launchd adoption, with an ownership gate before exec.

    start() is separate so the caller retains cleanup ownership if launch fails.
    A PID is never signalled without its original process-group/birth identity.
    On macOS a kernel exit watch ties polling to the original process lifetime.
    """
    def __init__(self):
        self.pid = None
        self.returncode = None
        self.identity = None
        self.watcher = None

    def _identity(self):
        result = subprocess.run(['ps', '-p', str(self.pid), '-o', 'pgid=,lstart='],
                                capture_output=True, text=True, timeout=2)
        return result.stdout.strip() if result.returncode == 0 else ''

    def start(self, command, cwd, env, log_path):
        read = write = gate_read = gate_write = None
        first = None
        try:
            read, write = os.pipe()
            gate_read, gate_write = os.pipe()
            first = os.fork()
            if first == 0:
                try:
                    os.close(read)
                    os.close(gate_write)
                    os.setsid()
                    if os.fork() == 0:
                        os.write(write, str(os.getpid()).encode() + b'\n')
                        # EOF cancels an interrupted/failed parent launch.
                        if os.read(gate_read, 1) != b'1':
                            os._exit(127)
                        os.close(gate_read)
                        log = os.open(log_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
                        os.dup2(log, 1)
                        os.dup2(log, 2)
                        os.close(log)
                        os.chdir(cwd)
                        os.execve(command[0], command, env)
                except BaseException as exc:
                    try:
                        os.write(write, ('launch error: ' + str(exc)).encode()[:1024])
                    except OSError:
                        pass
                finally:
                    os._exit(127)
            os.close(write)
            write = None
            os.close(gate_read)
            gate_read = None
            deadline = time.monotonic() + 5
            payload = b''
            while b'\n' not in payload:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not select.select([read], [], [], remaining)[0]:
                    raise TimeoutError('Detached emulator PID handshake timed out')
                chunk = os.read(read, 1024)
                if not chunk:
                    raise RuntimeError('Detached emulator launch failed: ' + payload.decode(errors='replace'))
                payload += chunk
                if len(payload) > 1024:
                    raise RuntimeError('Invalid detached emulator PID handshake')
            pid_text, extra = payload.split(b'\n', 1)
            if not pid_text.isdigit() or int(pid_text) <= 1 or extra:
                raise RuntimeError('Invalid detached emulator PID handshake')
            self.pid = int(pid_text)
            self.identity = self._identity()
            if not self.identity or int(self.identity.split()[0]) != first:
                self.identity = None
                raise RuntimeError('Cannot establish detached emulator ownership')
            if hasattr(select, 'kqueue'):
                self.watcher = select.kqueue()
                self.watcher.control([select.kevent(self.pid, filter=select.KQ_FILTER_PROC,
                                      flags=select.KQ_EV_ADD, fflags=select.KQ_NOTE_EXIT)], 0, 0)
            # Reap the short-lived session leader before releasing the emulator.
            while os.waitpid(first, os.WNOHANG)[0] == 0:
                if time.monotonic() >= deadline:
                    raise TimeoutError('Detached launcher did not exit')
                time.sleep(0.01)
            first = None
            os.write(gate_write, b'1')
            os.close(gate_write)
            gate_write = None
            # The close-on-exec pipe returns EOF on success; errors remain explicit.
            remaining = max(0, deadline - time.monotonic())
            if not select.select([read], [], [], remaining)[0]:
                raise TimeoutError('Detached emulator exec handshake timed out')
            error = os.read(read, 1024)
            if error:
                raise RuntimeError(error.decode(errors='replace'))
        finally:
            for fd in (read, write, gate_read, gate_write):
                if fd is not None:
                    try:
                        os.close(fd)
                    except OSError:
                        pass
            if first is not None and first > 0:
                # Never block receipt cleanup on a broken launcher.
                try:
                    os.waitpid(first, os.WNOHANG)
                except ChildProcessError:
                    pass

    def poll(self):
        if self.returncode is not None:
            return self.returncode
        if self.pid is None or self.pid <= 1 or self.identity is None:
            return 'not launched'
        exited = (bool(self.watcher.control([], 1, 0)) if self.watcher is not None
                  else self._identity() != self.identity)
        if exited:
            self.returncode = 'exited (reaped by launchd; exit status unavailable)'
        return self.returncode

    def terminate(self, grace=3):
        for sig in (signal.SIGTERM, signal.SIGKILL):
            if self.poll() is not None:
                return
            if self._identity() != self.identity:
                self.returncode = 'exited (process identity changed; not signalled)'
                return
            try:
                os.kill(self.pid, sig)
            except ProcessLookupError:
                self.returncode = 'exited (reaped by launchd; exit status unavailable)'
                return
            deadline = time.monotonic() + grace
            while time.monotonic() < deadline:
                if self.poll() is not None:
                    return
                time.sleep(0.1)
        raise TimeoutError('Owned emulator still visible after bounded TERM/KILL cleanup')

    def close(self):
        if self.watcher is not None:
            self.watcher.close()
            self.watcher = None


def error_record(exc):
    return {'type': type(exc).__name__, 'message': str(exc)}


def capture_inventory(stub, output, frame, budget):
    """One bounded pause/resume on the caller's existing running connection."""
    record = {'schema': 'dusklight.azahar-debug.v1', 'action': 'inventory',
              'status': 'incomplete', 'trigger_frame': frame, 'privacy': 'local_only',
              'origin': 'Azahar guest GDB evidence; not physical New3DS performance',
              'started_at': azahar_debug.timestamp(), 'session': {},
              'capture_deadline_seconds': budget}
    start = time.monotonic()
    stub.deadline = start + budget
    body_error = None
    failure = None
    try:
        with azahar_debug.guest_session(stub, snapshot=True, state='running',
                                       record=record['session']) as (_, stop):
            try:
                cap = azahar_debug.capabilities(stub)
                record['capabilities'] = cap
                record['inventory'] = azahar_debug.inventory(stub, stop, cap['features'])
            except BaseException as exc:
                body_error = exc
                raise
        record['status'] = 'complete'
    except BaseException as exc:
        # guest_session records resume errors, but its finally can raise them.
        # Keep the original capture error/interrupt as the primary failure.
        failure = body_error if body_error is not None else exc
        record['status'] = 'error'
        record['error'] = error_record(failure)
    finally:
        stub.deadline = None
        record['finished_at'] = azahar_debug.timestamp()
        record['elapsed_seconds'] = round(time.monotonic() - start, 6)
        try:
            azahar_debug.private_write(output, (json.dumps(record, indent=2) + '\n').encode())
        except BaseException:
            if failure is not None:
                raise failure
            raise
    if failure is not None and not isinstance(failure, (gdb_sampler.GDBError, OSError, EOFError, ValueError)):
        raise failure
    return record


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--azahar', type=Path, default=DEFAULT_AZAHAR)
    parser.add_argument('--renderer', choices=sorted(RENDERERS), default='vulkan')
    parser.add_argument('--unthrottled', action='store_true', help='Disable the emulator speed limit; guest ticks are unchanged')
    parser.add_argument('--dump-chord-on-a', action='store_true',
                        help='Isolated HID test: map keyboard A to all three physical L/R/A buttons')
    parser.add_argument('--input-script', type=Path, help='Private native input script copied to the isolated SD')
    parser.add_argument('--capture-frames', help='Comma-separated frame numbers to capture from the main framebuffer')
    parser.add_argument('--dump-frames', help='Comma-separated frame numbers whose game arena is dumped')
    parser.add_argument('--depth-audit-frames', help='Comma-separated frames for private previous/current depth and queued-query evidence')
    parser.add_argument('--depth-retained-frames', help='Subset of depth audit frames for retained independent A/B readback ledger')
    parser.add_argument('--exit-frame', type=int, help='Native game loop exits cleanly after this frame')
    parser.add_argument('--calibrate', action='store_true', help='Report guest ticks for loops of known instruction counts')
    parser.add_argument('--audio-engine', choices=sorted(AUDIO_ENGINES), default='null', help='Azahar output engine; null keeps DSP emulation silent')
    parser.add_argument('--audio-emulation', choices=sorted(AUDIO_EMULATION), default='hle', help='DSP emulation; HLE bypasses firmware execution')
    parser.add_argument('--dsp-component', type=Path, help='Bounded private/project DSP1 component copied into isolated SD /3ds/dspfirm.cdc')
    parser.add_argument('--audio-pcm', nargs=2, type=int, metavar=('START_BLOCK', 'BLOCK_COUNT'),
                        help='Capture final mixer PCM in isolated SD; zero-based fills, 1..327 blocks, not game frames')
    parser.add_argument('--no-sandbox', action='store_true', help='Diagnosis only: run without the write-restricting profile')
    parser.add_argument('--debug-after-frame', type=int, help='Capture one private thread/register JSON inventory after this logged frame')
    parser.add_argument('--profile-after-frame', type=int, help='Start guest PC sampling once this frame is logged')
    parser.add_argument('--profile-seconds', type=float, default=30.0)
    parser.add_argument('--profile-interval', type=float, default=0.003)
    parser.add_argument('--profile-elf', type=Path, help='Unstripped ELF matching the image, for symbols')
    parser.add_argument('--gdb-port', type=int, default=24689)
    parser.add_argument('--rpc', action='store_true', help='Enable local-only Azahar RPC for process/screenshot/performance inspection')
    parser.add_argument('--rgb565-rgba8-reference', action='store_true', help='Same-binary diagnostic A/B: retain the former RGBA8 GPU storage for GX RGB565')
    parser.add_argument('--texture-half-allowlist', type=Path,
                        help='Reviewed source SHA256 tokens for selective authored mip1; no wildcard, <=64 sources')
    parser.add_argument('--alpha-interval-experiment', action='store_true',
                        help='Explicit isolated SD opt-in for native alpha intervals; requires prior native controls')
    parser.add_argument('--signed-q-experiment', action='store_true',
                        help='Default-off direct projective texture0 diagnostic; retains finite signed divisors')
    parser.add_argument('--fused-color-copy', action='store_true', help='Same-binary A/B: enable the fused EFB->PICA colour copy via SD config')
    parser.add_argument('--profile-class', choices=['0','1','2','3'], help='Restrict detailed draw timers to one class (2 = orthographic 2D, 3 = perspective)')
    parser.add_argument('--gpu-capture', choices=['0','1'], help='Same-binary A/B for the GPU-resident EFB capture (default on)')
    parser.add_argument('--gpu-capture-orient', type=int, help='GPU capture orientation bits (calibration)')
    parser.add_argument('--gpu-capture-check', action='store_true', help='Compare GPU capture with the CPU reference for 3 copies')
    parser.add_argument('--texture-scenery', action='store_true', help='Enable room-scenery texture halving (SD opt-in)')
    parser.add_argument('--profile-all-frames', action='store_true', help='A/B switch: retain detailed GX timers on all guest frames')
    parser.add_argument('--name', required=True)
    parser.add_argument('--seconds', type=int, default=120)
    parser.add_argument('--long-gameplay', action='store_true',
                        help='Explicit bounded story/soak run: permit up to 3600 seconds instead of 600')
    assets = parser.add_mutually_exclusive_group()
    assets.add_argument('--private-disc', action='store_true')
    assets.add_argument('--data-bundle', type=Path, help='Verified private zelda.twilight; no ISO staged or read')
    parser.add_argument('--initial-card', type=Path,
                        help='Explicit DSVCARD1 snapshot from project Diagnostics/Saves/Build; requires --private-disc or --data-bundle')
    parser.add_argument('--boot-stage', help='Explicit native development entry: STAGE,ROOM,POINT,LAYER')
    parser.add_argument('--gui-empty', action='store_true', help='Open isolated GUI, then load the expected image through its native file dialog')
    parser.add_argument('--log-filter', default='*:Info Debug.Emulated:Debug', help='Isolated emulator log filter, e.g. *:Info Debug.Emulated:Debug')
    parser.add_argument('--instant-debug-log',action='store_true',help='Flush each emulator log event; functional diagnosis only')
    movie = parser.add_mutually_exclusive_group()
    movie.add_argument('--movie-record', action='store_true', help='Record actual emulator HID polls to this run evidence directory')
    movie.add_argument('--movie-play', type=Path, help='Replay a private CTM input fixture from this project')
    parser.add_argument('--image', type=Path, default=ROOT / 'Build/new3ds-native/dusklight-diagnostic.cxi')
    parser.add_argument('--evidence-root', type=Path, default=ROOT / 'Diagnostics/implementation-43/emulator')
    args = parser.parse_args(argv)
    # The child runs in its isolated SD workspace, not the caller's cwd.
    args.azahar = args.azahar.resolve()
    has_data = args.private_disc or args.data_bundle is not None
    bundle_record = None
    if args.data_bundle is not None:
        try:
            bundle_record = inspect_data_bundle(args.data_bundle)
        except (OSError, ValueError) as exc:
            parser.error(str(exc))
    debugging = args.profile_after_frame is not None or args.debug_after_frame is not None
    # Guest exit detection reads the log while the emulator is still running.
    args.instant_debug_log = args.instant_debug_log or bool(args.exit_frame) or debugging
    if args.alpha_interval_experiment and not has_data:
        parser.error('Alpha interval game experiment requires --private-disc or --data-bundle')
    if args.signed_q_experiment and not has_data:
        parser.error('Signed-q game experiment requires --private-disc or --data-bundle')
    if args.profile_after_frame is not None and not args.profile_elf:
        parser.error('Profiling requires --profile-elf')
    duration_limit = 3600 if args.long_gameplay else 600
    if not args.name.replace('-', '').replace('_', '').isalnum() or not 1 <= args.seconds <= duration_limit:
        parser.error(f'Use a simple run name and duration 1..{duration_limit} seconds')
    if (not 1 <= args.gdb_port <= 65535 or
            any(frame is not None and frame < 0 for frame in (args.profile_after_frame, args.debug_after_frame)) or
            not math.isfinite(args.profile_seconds) or not 0 < args.profile_seconds <= 300 or
            not math.isfinite(args.profile_interval) or not 0 < args.profile_interval <= 60):
        parser.error('Use bounded GDB port, nonnegative trigger frames, profile duration (0,300] and interval (0,60]')
    if args.depth_retained_frames:
        try:
            retained = [int(n) for n in args.depth_retained_frames.split(',')]
            audits = [int(n) for n in (args.depth_audit_frames or '').split(',') if n]
            if not has_data or len(retained)>16 or len(set(retained))!=len(retained) or any(n<=0 or n not in audits or (args.exit_frame is not None and n>=args.exit_frame) for n in retained):
                raise ValueError()
        except ValueError:
            parser.error('Retained-depth frames require private-disc or data-bundle and a unique bounded subset of depth-audit-frames')
    initial_card_data = None
    initial_card_record = None
    if args.initial_card is not None:
        if not has_data:
            parser.error('--initial-card requires --private-disc or --data-bundle')
        try:
            initial_card_data, input_record = read_card_snapshot(args.initial_card)
            validate_native_card(initial_card_data)
        except (ValueError, OSError) as exc:
            parser.error(str(exc))
        initial_card_record = {'status': 'validated_snapshot', 'input': input_record,
                               'initial': None, 'final': None,
                               'scope': 'native container snapshot only; gameplay save/reload not accepted'}
    component_data = None
    component_record = None
    if args.dsp_component:
        if not has_data:
            parser.error('--dsp-component requires isolated game-data SD setup')
        try:
            component_data, component_record = read_dsp_component(args.dsp_component)
        except (ValueError, OSError) as exc:
            parser.error(str(exc))
    half_record = None
    half_config = None
    if args.texture_half_allowlist is not None:
        source = args.texture_half_allowlist.resolve()
        if not has_data or not source.is_relative_to(ROOT) or not source.is_file():
            parser.error('--texture-half-allowlist requires private-disc or data-bundle and a project file')
        with source.open('rb') as stream:
            raw = stream.read(8193)
        try:
            tokens = raw.decode('ascii').split()
            if len(raw) > 8192 or not 1 <= len(tokens) <= 64 or any(not re.fullmatch('[0-9a-fA-F]{64}', t) for t in tokens):
                raise ValueError()
            tokens = [t.lower() for t in tokens]
            if len(set(tokens)) != len(tokens): raise ValueError()
        except (UnicodeError, ValueError):
            parser.error('Texture half policy requires 1..64 unique SHA256 hex tokens in <=8192 bytes')
        half_config = ('\n'.join(tokens)+'\n').encode('ascii')
        half_record = {'source': str(source), 'source_sha256': hashlib.sha256(raw).hexdigest(),
                       'config_sha256': hashlib.sha256(half_config).hexdigest(), 'fingerprints': tokens,
                       'scope': 'selective authored mip1, original CPU assets retained; role audit required'}
    pcm_record = None
    if args.audio_pcm is not None:
        start_block, block_count = args.audio_pcm
        if not has_data or not 0 <= start_block <= 0xffffffff or not 1 <= block_count <= 327:
            parser.error('--audio-pcm requires private-disc or data-bundle, START_BLOCK 0..4294967295 and BLOCK_COUNT 1..327')
        pcm_record = {'start_block': start_block, 'block_count': block_count,
                      'scope': 'pre-device final mixer; blocks are fills, not game frames',
                      'export_requires_joined_shutdown': True}
    audio_record = {'requested_engine': args.audio_engine, 'output_type': AUDIO_ENGINES[args.audio_engine],
                    'emulation': args.audio_emulation, 'component': component_record,
                    'pcm_request': pcm_record,
                    'audible_output_verified': False}
    stage_text=None
    if args.boot_stage:
        parts=args.boot_stage.split(',')
        try:
            if len(parts)!=4 or not re.fullmatch(r'[A-Za-z0-9_]{1,8}',parts[0]):raise ValueError()
            room,point,layer=map(int,parts[1:])
            if not has_data or not(0<=room<=63 and -32768<=point<=32767 and -1<=layer<=15):raise ValueError()
        except ValueError:parser.error('Invalid boot stage; requires private-disc or data-bundle and bounded STAGE,ROOM,POINT,LAYER')
        stage_text=f'{parts[0]} {room} {point} {layer}\n'
    # One emulator at a time: several Azahar instances exhaust the owner's RAM.
    running = subprocess.run(['pgrep', '-f', '[Aa]zahar.app/Contents/MacOS/azahar'], capture_output=True, text=True).stdout.split()
    if running:
        parser.error('Another Azahar instance is running (PID ' + ', '.join(running) + '); run one at a time')
    runtime = ROOT / 'Build/azahar-runs' / args.name
    out = args.evidence_root.resolve() / args.name
    if initial_card_record is None:
        runtime.mkdir(parents=True, exist_ok=False)
        out.mkdir(parents=True, exist_ok=False)
    else:
        # Seeded runs also pin private output ancestors and refuse old run roots.
        out = Path(os.path.abspath(args.evidence_root)) / args.name
        for directory in (runtime, out):
            with card_private_parent(directory, create=True) as (parent, name, unused):
                os.mkdir(name, mode=0o700, dir_fd=parent)
    # macOS GUI startup may change cwd. Explicit pre-existing XDG directories
    # avoid falling back to the user's real Application Support directory.
    user = runtime / 'xdg-data/azahar-emu'
    config = runtime / 'xdg-config/azahar-emu'
    for directory in (user, config, runtime / 'xdg-cache/azahar-emu'):
        directory.mkdir(parents=True)
    (config / 'qt-config.ini').write_text(
        '[System]\nis_new_3ds=true\nis_new_3ds\\default=false\n'
        '[UI]\nfirstStart=false\ncalloutFlags=4294967295\nconfirmClose=false\nconfirmClose\\default=false\n'
        '[Audio]\noutput_type=' + str(AUDIO_ENGINES[args.audio_engine]) + '\noutput_type\\default=false\n'
        'audio_emulation=' + str(AUDIO_EMULATION[args.audio_emulation]) + '\naudio_emulation\\default=false\n'
        '[Miscellaneous]\ncheck_for_update_on_start=false\ncheck_for_update_on_start\\default=false\n'
        'log_filter=' + args.log_filter + '\nlog_filter\\default=false\n'
        '[Debugging]\ninstant_debug_log=' + str(args.instant_debug_log).lower() + '\ninstant_debug_log\\default=false\n'
        + 'enable_rpc_server=' + str(args.rpc).lower() + '\nenable_rpc_server\\default=false\n'
        + ('use_gdbstub=true\nuse_gdbstub\\default=false\ngdbstub_port=' + str(args.gdb_port) +
           '\ngdbstub_port\\default=false\n' if debugging else '') +
        '[Renderer]\ngraphics_api=' + str(RENDERERS[args.renderer]) + '\ngraphics_api\\default=false\n'
        'frame_limit=' + ('0' if args.unthrottled else '100') + '\nframe_limit\\default=false\n'
        'use_vsync=false\nuse_vsync\\default=false\n'
        '[WebService]\nenable_telemetry=false\n')
    if args.dump_chord_on_a:
        with (config / 'qt-config.ini').open('a') as settings:
            settings.write('[Controls]\nprofile=0\nprofile\\default=false\nprofiles\\size=1\n'
                           'profiles\\1\\name=Application dump test\nprofiles\\1\\name\\default=false\n')
            for button in ('a', 'l', 'r'):
                settings.write(f'profiles\\1\\button_{button}="code:65,engine:keyboard"\n'
                               f'profiles\\1\\button_{button}\\default=false\n')
    source_stat = (ROOT / 'twilight.iso').stat() if args.private_disc else None
    if has_data:
        sd = user / ('sdmc/3ds/DuskLight' if bundle_record else 'sdmc/3ds/dusklight')
        sd.mkdir(parents=True)
        if bundle_record:
            (sd / 'zelda.twilight').symlink_to(bundle_record['path'])
            # On case-sensitive hosts, mirror FAT's case-insensitive directory
            # lookup for legacy save/config paths without staging an ISO.
            legacy = user / 'sdmc/3ds/dusklight'
            if not legacy.exists():
                legacy.symlink_to('DuskLight', target_is_directory=True)
        else:
            (sd / 'twilight.iso').symlink_to(ROOT / 'twilight.iso')
        if component_data is not None:
            destination = sd.parent / 'dspfirm.cdc'
            azahar_debug.private_write(destination, component_data)
            if hashlib.sha256(destination.read_bytes()).hexdigest() != component_record['sha256']:
                raise OSError('Isolated DSP component failed copy verification')
        if stage_text:(sd / 'boot-stage.txt').write_text(stage_text)
        if half_config is not None:
            azahar_debug.private_write(sd / 'texture-half-sha256.txt', half_config)
        if pcm_record is not None:
            azahar_debug.private_write(sd / 'audio-pcm.txt',
                                       f'{pcm_record["start_block"]} {pcm_record["block_count"]}\n'.encode('ascii'))
        if args.input_script:shutil.copyfile(args.input_script, sd / SCRIPT_FILES['input_script'])
        for option in ('capture_frames', 'dump_frames', 'depth_audit_frames', 'depth_retained_frames'):
            if getattr(args, option):
                frames = [int(value) for value in getattr(args, option).split(',')]
                (sd / SCRIPT_FILES[option]).write_text(''.join(f'{frame}\n' for frame in frames))
        if args.exit_frame:(sd / 'exit-frame.txt').write_text(f'{args.exit_frame}\n')
        if args.calibrate:(sd / 'calibrate.txt').write_text('1\n')
        if args.rgb565_rgba8_reference:(sd / 'rgb565-rgba8-reference.txt').write_text('1\n')
        if args.alpha_interval_experiment:(sd / 'alpha-interval.txt').write_bytes(b'enabled=1\n')
        if args.signed_q_experiment:(sd / 'signed-q.txt').write_bytes(b'enabled=1\n')
        if args.profile_class:(sd / 'profile-class.txt').write_text(args.profile_class + '\n')
        if args.gpu_capture:(sd / 'gpu-capture.txt').write_text(args.gpu_capture + '\n')
        if args.gpu_capture_orient is not None:(sd / 'gpu-capture-orient.txt').write_text(f'{args.gpu_capture_orient}\n')
        if args.gpu_capture_check:(sd / 'gpu-capture-check.txt').write_text('1\n')
        if args.texture_scenery:(sd / 'texture-scenery.txt').write_text('1\n')
        if args.fused_color_copy:(sd / 'fused-color-copy.txt').write_bytes(b'enabled=1\n')
        if args.profile_all_frames:(sd / 'profile-all-frames.txt').write_text('1\n')
    profile = out / 'sandbox.sb'
    # Azahar creates an input/RPC UDP socket at startup. Permit binding only;
    # incoming/outgoing traffic remains denied, as do writes to the original ISO.
    profile.write_text('(version 1)\n(allow default)\n(deny network*)\n(allow network-bind)\n' +
                       ('(allow network-inbound (local ip "localhost:*"))\n' if debugging or args.rpc else '') +
                       ('(allow network-outbound (remote ip "localhost:*"))\n' if args.rpc else '') +
                       '(deny file-write*)\n(allow file-write* (subpath ' + json.dumps(str(runtime)) +
                       ') (subpath ' + json.dumps(str(out)) + ') (literal "/dev/null"))\n')
    image = args.image.resolve()
    image_hash = hashlib.sha256(image.read_bytes()).hexdigest()
    command = ([] if args.no_sandbox else ['/usr/bin/sandbox-exec', '-f', str(profile)]) + [str(args.azahar)]
    movie_path = None
    if args.movie_record:
        movie_path = out / 'recorded-input.ctm'
        command.extend(['-r', str(movie_path), '-a', 'DUSKLIGHT local input diagnosis'])
    elif args.movie_play:
        movie_path = args.movie_play.resolve()
        if not movie_path.is_relative_to(ROOT) or not movie_path.is_file():
            parser.error('Movie playback requires an existing private project file')
        command.extend(['-p', str(movie_path)])
    if not args.gui_empty:
        command.extend(['-w', str(image)])
    start = time.monotonic()
    guest_log = user / 'log/azahar_log.txt'
    process = DetachedProcess()
    stub = None
    guest_exit = guest_failure = timed_out = False
    final_log_scanned = shutdown_complete_reported = False
    host_samples, converted, dumps = [], [], []
    cleanup_errors = []
    primary_error = None
    profile_result = {'requested': args.profile_after_frame is not None,
                      'status': 'pending' if args.profile_after_frame is not None else 'not_requested',
                      'last_error': None}
    debug_result = {'requested': args.debug_after_frame is not None,
                    'status': 'pending' if args.debug_after_frame is not None else 'not_requested',
                    'file': 'debug-inventory.json' if args.debug_after_frame is not None else None}
    launch = {'started_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'pid': None, 'command': command, 'expected_image': str(image),
              'expected_image_sha256': image_hash, 'boot_stage': args.boot_stage, 'audio': audio_record,
              'gui_empty': args.gui_empty, 'rgb565_rgba8_reference': args.rgb565_rgba8_reference, 'texture_half_policy': half_record, 'instant_debug_log': args.instant_debug_log,
              'dump_chord_on_a': args.dump_chord_on_a,
              'movie_mode': 'record' if args.movie_record else 'play' if args.movie_play else None,
              'movie_path': str(movie_path) if movie_path else None,
              'image_loaded': 'Requires separate guest/UI evidence',
              'private_iso': 'read-only original symlink' if args.private_disc else 'absent test',
              'data_bundle': bundle_record,
              'initial_card': initial_card_record,
              'ISO_before': {'size': source_stat.st_size, 'mtime_ns': source_stat.st_mtime_ns} if source_stat else None}

    def finalize_step(label, action):
        try:
            return action()
        except BaseException as exc:
            cleanup_errors.append({'step': label, **error_record(exc)})
            return None

    with interruption_handlers() as interruption:
        try:
            (out / 'launch.json').write_text(json.dumps(launch, indent=2) + '\n')
            if initial_card_data is not None:
                initial_card_record['initial'] = seed_native_card(sd, initial_card_data, input_record)
                initial_card_record['status'] = 'seeded'
                (out / 'launch.json').write_text(json.dumps(launch, indent=2) + '\n')
            process.start(command, runtime, {**os.environ, 'CFFIXED_USER_HOME': str(runtime),
                                             'XDG_DATA_HOME': str(runtime / 'xdg-data'),
                                             'XDG_CONFIG_HOME': str(runtime / 'xdg-config'),
                                             'XDG_CACHE_HOME': str(runtime / 'xdg-cache')},
                          out / 'startup.log')
            launch['pid'] = process.pid
            launch['pid_identity'] = process.identity
            (out / 'pid').write_text(str(process.pid))
            (out / 'launch.json').write_text(json.dumps(launch, indent=2) + '\n')
            print('Azahar PID', process.pid, 'run', args.name, flush=True)
            deadline = start + args.seconds
            if debugging:
                # Exactly one client: the newly launched stub starts stopped.
                stub = gdb_sampler.Stub(args.gdb_port, timeout=3,
                                        connect_timeout=max(0.001, min(10, deadline - time.monotonic())))
                stub.deadline = deadline
                stub.require_stop(stub.command(b'?'))
                stub.resume(wait_ack=True)
                stub.deadline = None
            while process.poll() is None and time.monotonic() < deadline:
                time.sleep(min(1, max(0, deadline - time.monotonic())))
                if not host_samples or time.monotonic() - start - host_samples[-1]['elapsed_seconds'] >= 2:
                    fields = subprocess.run(['ps', '-p', str(process.pid), '-o', 'rss=,pcpu='],
                                            capture_output=True, text=True, timeout=2).stdout.split()
                    if len(fields) == 2:
                        host_samples.append({'elapsed_seconds': round(time.monotonic() - start, 3),
                                             'rss_kib': int(fields[0]), 'cpu_percent': float(fields[1])})
                log = guest_log.read_bytes() if guest_log.exists() else b''
                if STOP_FAILURE.search(log):
                    guest_failure = True
                    break
                frames = [int(n) for n in re.findall(rb'Original native frame (\d+) complete', log)]
                last_frame = max(frames, default=-1)
                # Inventory first if both triggers become due in the same poll.
                if (stub and debug_result['status'] == 'pending' and
                        last_frame >= args.debug_after_frame):
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        break
                    debug_result['status'] = 'capturing'
                    record = capture_inventory(stub, out / 'debug-inventory.json',
                                               args.debug_after_frame, min(30, remaining))
                    debug_result.update(status=record['status'], session=record['session'])
                    if record['status'] != 'complete':
                        debug_result['error'] = record['error']
                        raise gdb_sampler.GDBError('Thread inventory failed: ' + record['error']['message'])
                if (stub and profile_result['status'] == 'pending' and
                        last_frame >= args.profile_after_frame):
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        break
                    profile_result['status'] = 'sampling'
                    # All sampling operations share the run deadline; on an
                    # expired budget, final cleanup closes the owned connection.
                    stub.deadline = deadline
                    try:
                        pcs, pairs = gdb_sampler.sample(stub, min(args.profile_seconds, remaining),
                                                        args.profile_interval,
                                                        lambda: process.poll() is None)
                    finally:
                        profile_result['last_error'] = stub.last_error
                        stub.deadline = None
                    profile_result['samples'] = sum(pcs.values())
                    profile_result['status'] = 'error' if stub.last_error or not pcs else 'complete'
                    gdb_sampler.symbolize(args.profile_elf, pcs, pairs, out / 'pc-profile.txt')
                    print('PC samples', sum(pcs.values()), flush=True)
                    if stub.last_error:
                        raise gdb_sampler.GDBError('PC sampling failed: ' + stub.last_error)
                    if not pcs:
                        raise gdb_sampler.GDBError('PC sampling produced no observed PC samples')
                if args.exit_frame and b'Native developer exit after frame' in log:
                    guest_exit = True
                    time.sleep(min(2, max(0, deadline - time.monotonic())))
                    break
            timed_out = time.monotonic() >= deadline and not guest_exit and not guest_failure
        except BaseException as exc:
            primary_error = exc
            if profile_result['status'] == 'sampling':
                profile_result['status'] = 'error'
                profile_result['error'] = error_record(exc)
            if debug_result['status'] == 'capturing':
                debug_result['status'] = 'error'
                debug_result['error'] = error_record(exc)
            raise
        finally:
            interruption['cleaning_up'] = True
            if stub is not None:
                profile_result['last_error'] = stub.last_error
                finalize_step('gdb_close', stub.close)
            finalize_step('process_terminate', process.terminate)
            finalize_step('process_watch_close', process.close)

            def check_iso():
                if source_stat:
                    after = (ROOT / 'twilight.iso').stat()
                    if (source_stat.st_size, source_stat.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                        raise RuntimeError('Unexpected private ISO mutation')
            finalize_step('iso_stat_check', check_iso)
            if bundle_record is not None:
                finalize_step('bundle_integrity_check', lambda: inspect_data_bundle(args.data_bundle, expected=bundle_record))
            if initial_card_record is not None:
                initial_card_record['final'] = (
                    final_native_card(sd / 'saves/native-card0.bin') if process.returncode is not None
                    else {'status': 'unavailable', 'reason': 'owned process stop not established'})
                final = initial_card_record['final']
                final['matches_seed_snapshot'] = (final.get('sha256') == input_record['sha256']
                                                  if final.get('sha256') else None)
            def copy_guest_log():
                nonlocal guest_exit, guest_failure, final_log_scanned, shutdown_complete_reported
                if guest_log.exists():
                    shutil.copyfile(guest_log, out / 'guest.log')
                    # The guest can fault after announcing an exit request,
                    # including during the grace interval or final process stop.
                    # Classify the actual preserved log after owned cleanup.
                    final_log = (out / 'guest.log').read_bytes()
                    guest_failure = guest_failure or bool(STOP_FAILURE.search(final_log))
                    if args.exit_frame:
                        guest_exit = guest_exit or bool(re.search(
                            rb'Native developer exit after frame ' + str(args.exit_frame).encode() + rb'\b', final_log))
                    shutdown_complete_reported = bool(re.search(
                        rb'Native shutdown complete: workers=0 stackBytes=0 waitingQueues=0 graphics=stopped\b',
                        final_log))
                    final_log_scanned = True
            finalize_step('guest_log_copy', copy_guest_log)

            def convert_captures():
                capture_dir = user / 'sdmc/3ds/dusklight/captures'
                for capture in sorted(capture_dir.glob('*.rgba')):
                    target = out / (capture.stem + '.png')

                    def convert(capture=capture, target=target):
                        capture_to_png.convert(capture, target)
                        converted.append(target.name)
                    finalize_step('capture_convert:' + capture.name, convert)
            finalize_step('capture_listing', convert_captures)
            dump_dir = user / 'sdmc/3ds/dusklight/dumps'

            def list_dumps():
                dumps.extend(sorted(str(path.relative_to(runtime)) for path in dump_dir.iterdir()
                                    if path.is_file() and path.suffix in ('.bin', '.dstx'))
                             if dump_dir.is_dir() else [])
            finalize_step('dump_listing', list_dumps)
            receipt = {'origin': f'Azahar {args.azahar.parent.parent.parent.name} emulator ({args.renderer}), not physical New3DS',
                       'pid': process.pid, 'pid_identity': process.identity,
                       'guest_exit_frame_reached': guest_exit, 'captures': converted, 'dumps_in_runtime': dumps,
                       'guest_exit_requested': guest_exit,
                       'guest_shutdown_complete_reported': shutdown_complete_reported,
                       'guest_final_log_scanned': final_log_scanned,
                       'dump_verification': 'listing only; existence does not verify bytes or guest checksums',
                       'guest_fatal_detected': guest_failure, 'rpc_enabled': args.rpc,
                       'returncode': process.returncode, 'bounded_stop': timed_out,
                       'elapsed_seconds': round(time.monotonic() - start, 3),
                       'input': str(image), 'cxi_sha256': image_hash, 'boot_stage': args.boot_stage,
                       'gui_empty': args.gui_empty, 'rgb565_rgba8_reference': args.rgb565_rgba8_reference,
                       'dump_chord_on_a': args.dump_chord_on_a,
                       'data_bundle': bundle_record,
                       'initial_card': initial_card_record,
                       'texture_half_policy': half_record,
                       'alpha_interval_experiment': args.alpha_interval_experiment,
                       'signed_q_experiment': args.signed_q_experiment,
                       'movie_mode': launch['movie_mode'], 'movie_path': launch['movie_path'],
                       'guest_completion': 'Requires separate guest evidence; process exit alone is insufficient',
                       'private_iso': 'read-only symlink' if args.private_disc else 'absent test',
                       'network': 'traffic denied; socket bind permitted',
                       'profile': profile_result, 'debug_inventory': debug_result, 'audio': audio_record,
                       'runner_status': 'error' if primary_error or cleanup_errors else 'complete',
                       'runner_error': error_record(primary_error) if primary_error else None,
                       'signal': interruption['signal'], 'cleanup_errors': cleanup_errors,
                       'hardware_performance_validated': False, 'gameplay_validated': False}
            finalize_step('host_samples_write', lambda: (out / 'host-process-samples.json').write_text(
                json.dumps(host_samples, indent=2) + '\n'))
            receipt['runner_status'] = 'error' if primary_error or cleanup_errors else 'complete'
            finalize_step('process_receipt_write', lambda: (out / 'process.json').write_text(
                json.dumps(receipt, indent=2) + '\n'))

            def analysis():
                checked = inspect_run(out, args.exit_frame,
                                      [int(n) for n in (args.capture_frames or '').split(',') if n])
                (out / 'analysis.json').write_text(json.dumps(checked, indent=2) + '\n')
            finalize_step('analysis_write', analysis)
            # Persist errors discovered by analysis/receipt writes when possible.
            receipt['runner_status'] = 'error' if primary_error or cleanup_errors else 'complete'
            finalize_step('final_receipt_write', lambda: (out / 'process.json').write_text(
                json.dumps(receipt, indent=2) + '\n'))
            finalize_step('receipt_stdout', lambda: print(json.dumps(receipt, indent=2)))
            if primary_error is None and cleanup_errors:
                raise RuntimeError('Runner finalization failed: ' + json.dumps(cleanup_errors))
    # Preserve historic exit behavior; inspect analysis.json for guest acceptance.
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except RunInterrupted as exc:
        raise SystemExit(128 + exc.signum)
