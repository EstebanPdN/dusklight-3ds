#!/usr/bin/env python3
"""Private, bounded GameCube USA r0 -> DuskLight indexed asset container v1."""
import argparse
import bisect
import hashlib
import json
import os
from pathlib import Path
import signal
import stat
import struct
import sys
import tempfile
import zlib

MAGIC = b'DLTWPK01'
HEADER = struct.Struct('>8sIIQQIIQQII32s32s')
RECORD = struct.Struct('>QQQ32s8s')
MAX_RANGES = 65556
CHUNK = 1024 * 1024
JSON_MODE = '--progress-json' in sys.argv


def emit(event, phase, completed=0, total=0, message='', **extra):
    obj = dict(event=event, phase=phase, completed=int(completed), total=int(total),
               message=message[:512], **extra)
    print(json.dumps(obj, ensure_ascii=True) if JSON_MODE else
          f'{event}: {phase} {completed}/{total} {message}', flush=True)


def require(test, message):
    if not test:
        raise ValueError(message)


def extent(limit, offset, size):
    return 0 <= offset <= limit and 0 <= size <= limit - offset


def exact(f, offset, size):
    f.seek(offset)
    data = f.read(size)
    require(len(data) == size, 'Truncated input')
    return data


def identity(f):
    s = os.fstat(f.fileno())
    require(stat.S_ISREG(s.st_mode), 'Input must be a regular file')
    return (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns)


def digest_region(f, offset, size, progress=None):
    h = hashlib.sha256()
    f.seek(offset)
    done = 0
    next_event = 0
    while done < size:
        data = f.read(min(CHUNK, size - done))
        require(bool(data), 'Truncated payload')
        h.update(data)
        done += len(data)
        if progress and (done >= next_event or done == size):
            emit('progress', progress, done, size, 'Hashing input')
            next_event = done + 32 * CHUNK
    return h.digest()


def layout(read, length):
    """Validate the complete original FST, return retained extents and file records."""
    require(0x440 <= length <= 0x7fffffff, 'Unsupported logical disc size')
    hdr = read(0, 0x440)
    require(hdr[:8] == b'GZ2E01\0\0' and hdr[0x18:0x20] == b'\0\0\0\0\xc2\x33\x9f\x3d',
            'Requires GameCube USA revision 0 (GZ2E01)')
    dol, fst, fs = struct.unpack_from('>III', hdr, 0x420)
    require(dol >= 0x440 and fst >= 0x440 and 12 <= fs <= 4 * CHUNK and
            extent(length, dol, 256) and extent(length, fst, fs), 'Invalid DOL/FST bounds')
    ranges = [(0, 0x440), (dol, 256), (fst, fs)]
    exe = read(dol, 256)
    for i in range(18):
        off = struct.unpack_from('>I', exe, i * 4)[0]
        size = struct.unpack_from('>I', exe, 0x90 + i * 4)[0]
        if size:
            require(off >= 256 and extent(length, dol + off, size), 'Invalid DOL section')
            ranges.append((dol + off, size))
    data = read(fst, fs)
    root, parent, count = struct.unpack_from('>III', data)
    require(root == 0x01000000 and parent == 0 and 1 <= count <= 65536 and count <= fs // 12,
            'Invalid FST root/count')
    strings = count * 12
    stack = [(0, count, b'', set())]
    files = []
    for i in range(1, count):
        while len(stack) > 1 and i == stack[-1][1]:
            stack.pop()
        typname, off, size = struct.unpack_from('>III', data, i * 12)
        typ, name = typname >> 24, typname & 0xffffff
        require(typ <= 1 and strings + name < fs, 'Invalid FST type/name')
        start = strings + name
        end = data.find(b'\0', start, min(fs, start + 256))
        require(end > start, 'Unterminated/empty FST name')
        base = data[start:end]
        require(base not in (b'.', b'..') and all(c >= 32 and c != 127 and c not in (47, 92) for c in base),
                'Unsafe FST basename')
        folded = base.lower()
        require(folded not in stack[-1][3], 'Ambiguous case-insensitive FST sibling')
        stack[-1][3].add(folded)
        path = stack[-1][2] + b'/' + base
        require(len(path) <= 1023, 'FST path exceeds native limit')
        if typ:
            require(off == stack[-1][0] and i < size <= stack[-1][1] and len(stack) < 64,
                    'Invalid FST nesting')
            stack.append((i, size, path, set()))
        else:
            require(extent(length, off, size), 'Invalid FST file extent')
            files.append((i, path, off, size))
            if size:
                ranges.append((off, size))
    merged = []
    for off, size in sorted(ranges):
        if merged and off <= merged[-1][0] + merged[-1][1]:
            lo, old = merged[-1]
            merged[-1] = (lo, max(lo + old, off + size) - lo)
        else:
            merged.append((off, size))
    require(1 <= len(merged) <= MAX_RANGES, 'Too many retained ranges')
    return merged, files


def header_bytes(logical, physical, count, index_crc, source_hash):
    args = (MAGIC, 1, 128, logical, physical, count, 64, 128, 128 + count * 64,
            index_crc, 0, source_hash, bytes(32))
    hdr = bytearray(HEADER.pack(*args))
    struct.pack_into('>I', hdr, 60, zlib.crc32(hdr))
    return hdr


class Bundle:
    def __init__(self, f):
        self.f = f
        physical = identity(f)[2]
        hdr = bytearray(exact(f, 0, 128))
        (magic, version, hs, logical, ps, count, rs, io, do, ic, hc,
         self.source_hash, reserved) = HEADER.unpack(hdr)
        struct.pack_into('>I', hdr, 60, 0)
        require(magic == MAGIC and version == 1 and hs == 128 and rs == 64 and not any(reserved),
                'Unknown bundle magic/version/flags')
        require(zlib.crc32(hdr) == hc, 'Header checksum mismatch')
        require(0x440 <= logical <= 0x7fffffff and ps == physical and ps <= 0x7fffffff and
                1 <= count <= MAX_RANGES and io == 128 and do == 128 + count * 64 and do <= ps,
                'Invalid bundle bounds/count')
        index = exact(f, io, count * 64)
        require(zlib.crc32(index) == ic, 'Index checksum mismatch')
        self.logical = logical
        self.ranges = []
        end, cursor = 0, do
        for lo, po, size, sha, pad in RECORD.iter_unpack(index):
            require(size > 0 and lo >= end and extent(logical, lo, size) and po == cursor and
                    extent(ps, po, size) and not any(pad), 'Invalid/overlapping bundle range')
            self.ranges.append((lo, po, size, sha))
            end, cursor = lo + size, po + size
        require(cursor == ps, 'Trailing or missing bundle bytes')
        self.starts = [r[0] for r in self.ranges]
        # Complete FST/DOL coverage; no padded or synthesized bytes.
        self.required, self.files = layout(self.read, logical)
        for off, size in self.required:
            require(self.covers(off, size), 'Required payload range is unmapped')

    def covers(self, off, size):
        if not extent(self.logical, off, size):
            return False
        first = max(0, bisect.bisect_right(self.starts, off) - 1)
        for i in range(first, len(self.ranges)):
            lo, po, n, sha = self.ranges[i]
            if lo <= off < lo + n:
                take = min(size, lo + n - off)
                off += take
                size -= take
                if not size:
                    return True
            elif lo > off:
                return False
        return size == 0

    def read(self, off, size):
        require(extent(self.logical, off, size), 'Logical read out of bounds')
        if not size:
            return b''
        pieces = []
        first = max(0, bisect.bisect_right(self.starts, off) - 1)
        for i in range(first, len(self.ranges)):
            lo, po, n, _ = self.ranges[i]
            if lo <= off < lo + n:
                count = min(size, lo + n - off)
                pieces.append(exact(self.f, po + off - lo, count))
                off += count
                size -= count
                if not size:
                    return b''.join(pieces)
            elif lo > off:
                break
        raise ValueError('Unmapped logical disc range')

    def verify(self):
        total = sum(r[2] for r in self.ranges)
        done = 0
        for lo, po, size, sha in self.ranges:
            require(digest_region(self.f, po, size) == sha, 'Payload SHA-256 mismatch')
            done += size
            emit('progress', 'verify', done, total, 'Verified retained payload')
        return digest_region(self.f, 0, identity(self.f)[2]).hex()


def publish(staging, output):
    """Atomic no-replace publication, including Mac destinations without hard links."""
    if sys.platform == 'darwin':
        import ctypes
        libc = ctypes.CDLL('/usr/lib/libSystem.B.dylib', use_errno=True)
        rename = libc.renamex_np
        rename.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
        rename.restype = ctypes.c_int
        # RENAME_EXCL, from the installed macOS SDK sys/stdio.h.
        if rename(os.fsencode(staging), os.fsencode(output), 0x00000004):
            code = ctypes.get_errno()
            raise OSError(code, os.strerror(code))
    else:
        os.link(staging, output)
        os.unlink(staging)


def cancelled(_signum, _frame):
    raise KeyboardInterrupt('Cancelled')


class Parser(argparse.ArgumentParser):
    def error(self, message):
        raise ValueError(message)


def run():
    parser = Parser(description=__doc__)
    parser.add_argument('--input', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--verify', type=Path)
    parser.add_argument('--progress-json', action='store_true')
    args = parser.parse_args()
    require((args.verify is not None and args.input is None and args.output is None) or
            (args.verify is None and args.input is not None and args.output is not None),
            'Use --input ISO --output zelda.twilight OR --verify zelda.twilight')
    chosen_output = args.verify if args.verify else args.output
    require(len(json.dumps(str(chosen_output.absolute()), ensure_ascii=True)) <= 3000,
            'Output path exceeds bounded progress protocol limit')
    if args.verify:
        output = args.verify.absolute()
        with output.open('rb') as f:
            before = identity(f)
            sha = Bundle(f).verify()
            require(identity(f) == before, 'Bundle changed during verification')
        emit('complete', 'complete', before[2], before[2], 'Container verified',
             output=str(output), bytes=before[2], sha256=sha)
        return
    source, output = args.input.absolute(), args.output.absolute()
    require(not os.path.lexists(output), 'Output already exists; refusing overwrite')
    require(source.resolve() != output.resolve(), 'Input and output must differ')
    staging = None
    try:
        with source.open('rb') as src:
            before = identity(src)
            emit('progress', 'inspect', 0, 1, 'Validating complete disc index')
            ranges, files = layout(lambda o, n: exact(src, o, n), before[2])
            source_hash = digest_region(src, 0, before[2], 'source-hash')
            data_start = 128 + len(ranges) * 64
            total = sum(n for _, n in ranges)
            physical = data_start + total
            require(physical <= 0x7fffffff, 'Container exceeds native physical offset limit')
            fd, staging = tempfile.mkstemp(prefix='.zelda.twilight-', suffix='.partial', dir=output.parent)
            with os.fdopen(fd, 'w+b') as dst:
                dst.seek(data_start)
                index = bytearray()
                done = 0
                next_event = 0
                for lo, size in ranges:
                    po = dst.tell()
                    src.seek(lo)
                    h = hashlib.sha256()
                    remain = size
                    while remain:
                        chunk = src.read(min(CHUNK, remain))
                        require(bool(chunk), 'Truncated source while packing')
                        require(dst.write(chunk) == len(chunk), 'Short output write')
                        h.update(chunk)
                        remain -= len(chunk)
                        done += len(chunk)
                        if done >= next_event or done == total:
                            emit('progress', 'pack', done, total, 'Packing original asset bytes')
                            next_event = done + 32 * CHUNK
                    index += RECORD.pack(lo, po, size, h.digest(), bytes(8))
                dst.seek(0)
                dst.write(header_bytes(before[2], physical, len(ranges), zlib.crc32(index), source_hash))
                dst.write(index)
                dst.flush()
                bundle = Bundle(dst)
                sha = bundle.verify()
                # Independently compare each original FST file, not only merged ranges.
                for number, (entry, path, lo, size) in enumerate(files, 1):
                    original, packed = hashlib.sha256(), hashlib.sha256()
                    for offset in range(0, size, CHUNK):
                        n = min(CHUNK, size - offset)
                        original.update(exact(src, lo + offset, n))
                        packed.update(bundle.read(lo + offset, n))
                    require(original.digest() == packed.digest(), 'FST file readback mismatch')
                    if number % 128 == 0 or number == len(files):
                        emit('progress', 'readback', number, len(files), 'Verified original FST files')
                require(digest_region(src, 0, before[2], 'source-hash') == source_hash and identity(src) == before,
                        'Source changed during extraction')
                require(os.stat(source).st_ino == before[1] and os.stat(source).st_dev == before[0],
                        'Source path identity changed')
                os.fsync(dst.fileno())
            # Publication refuses an output created concurrently.
            # Publication is the cancellation commit point (a few syscalls).
            # Keep signals blocked until process exit so a queued cancellation
            # cannot turn an already committed verified output into failure.
            signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM, signal.SIGINT})
            publish(staging, output)
            staging = None
            emit('complete', 'complete', physical, physical, 'Container verified and published',
                 output=str(output), bytes=physical, sha256=sha)
    finally:
        if staging is not None:
            os.unlink(staging)


def main():
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, cancelled)
    try:
        run()
        return 0
    except KeyboardInterrupt:
        emit('error', 'error', message='Cancelled; owned staging output removed')
        return 130
    except (OSError, ValueError, OverflowError, struct.error, MemoryError) as error:
        emit('error', 'error', message=str(error))
        return 1


if __name__ == '__main__':
    sys.exit(main())
