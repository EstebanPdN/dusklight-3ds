#!/usr/bin/env python3
"""Validate this diagnostic CIA's structure and hashes, not signatures or hardware.

CIA, TMD, ticket, NCCH, ExHeader and ExeFS layouts follow the makerom source
headers (cia.h, tmd.h, tik.h, ncch.h, exheader.h and exefs.h). This deliberately
accepts only the single-content, unencrypted diagnostic package profile.
"""

import argparse
import hashlib
import json
from pathlib import Path

TITLE_ID = 0x000400000D051000
VERSION = 1
SHORT_TITLE = 'Dusklight Diagnostic 0.1'
LONG_TITLE = 'Native platform test - game not playable'


def _require(condition, message):
    if not condition:
        raise ValueError(message)


def _part(data, offset, size, label):
    _require(0 <= offset <= len(data) and 0 <= size <= len(data) - offset,
             f'{label}: truncated or out-of-bounds region')
    return data[offset:offset + size]


def _number(data, offset, size, order='little'):
    return int.from_bytes(_part(data, offset, size, 'integer'), order)


def _sha(data):
    return hashlib.sha256(data).digest()


def _hash(data, expected, label):
    _require(_sha(data) == expected, f'{label}: SHA-256 mismatch')


def _align(value):
    return (value + 63) & ~63


def _signed_body(data, label):
    # makerom writes RSA-2048/SHA-256. Parsing the type is not authentication.
    _require(_number(data, 0, 4, 'big') == 0x00010004,
             f'{label}: expected RSA-2048/SHA-256 signature layout')
    return _part(data, 0x140, len(data) - 0x140, label)


def _smdh(data, short_title=SHORT_TITLE, long_title=LONG_TITLE):
    _require(len(data) == 0x36C0, 'SMDH: incorrect size')
    _require(data[:4] == b'SMDH' and _number(data, 4, 2) == 0,
             'SMDH: incorrect magic or format version')
    flags = _number(data, 0x2028, 4)
    _require(flags & 0x1000, 'SMDH: New3DS-exclusive flag missing')
    _require(flags & 1, 'SMDH: HOME Menu visibility flag missing')
    # Each of the 16 localized title records is 0x200 bytes; English is slot 1.
    def title(offset, size):
        try:
            return _part(data, offset, size, 'SMDH title').decode('utf-16le').split('\0')[0]
        except UnicodeDecodeError as error:
            raise ValueError('SMDH: malformed UTF-16 title') from error
    short = title(0x208, 0x80)
    long = title(0x288, 0x100)
    _require(short == short_title and long == long_title,
             'SMDH: English diagnostic-only labels do not match')
    for slot in range(16):
        offset = 8 + slot * 0x200
        localized_short = title(offset, 0x80)
        localized_long = title(offset + 0x80, 0x100)
        if localized_short or localized_long:
            _require(localized_short == short_title and localized_long == long_title,
                     f'SMDH: language slot {slot} has inconsistent diagnostic labels')
    return {'short_title': short, 'long_title': long, 'flags': f'0x{flags:08x}',
            'new3ds_exclusive': True}


def _ncch(data, smdh, version=VERSION):
    _part(data, 0, 0x200, 'NCCH header')
    _require(data[0x100:0x104] == b'NCCH', 'NCCH: incorrect magic')
    _require(_number(data, 0x112, 2) == 2, 'NCCH: expected format version 2')
    _require(data[0x18E] == 0, 'NCCH: expected 512-byte media units')
    unit = 512
    _require(_number(data, 0x104, 4) * unit == len(data), 'NCCH: declared size mismatch')
    _require(data[0x18F] & 4, 'NCCH: encrypted content is unsupported')
    _require(data[0x18F] & 2, 'NCCH: NoMountRomFs flag missing')
    _require(data[0x18D] & 3 == 2, 'NCCH: expected executable without RomFS')
    for offset, label in [(0x108, 'partition'), (0x118, 'program')]:
        _require(_number(data, offset, 8) == TITLE_ID, f'NCCH: {label} TitleID mismatch')
    _require(not any(data[0x1B0:0x1BC]) and not any(data[0x1E0:0x200]),
             'NCCH: diagnostic package must have no RomFS')
    exhdr_size = _number(data, 0x180, 4)
    _require(exhdr_size == 0x400, 'NCCH: expected 0x400-byte ExHeader')
    exhdr = _part(data, 0x200, exhdr_size, 'ExHeader')
    _hash(exhdr, data[0x160:0x180], 'ExHeader')
    _part(data, 0x600, 0x400, 'access descriptor')
    _require(_number(exhdr, 0x0E, 2) == version, 'ExHeader: remaster version mismatch')
    _require(_number(exhdr, 0x200, 8) == TITLE_ID, 'ExHeader: program TitleID mismatch')
    _require(exhdr[0x20C] & 3 == 3, 'ExHeader: CPU 804MHz and L2 cache must be enabled')
    _require(exhdr[0x20D] & 15 == 1, 'ExHeader: SystemModeExt must be 124MB')
    text_address = _number(exhdr, 0x10, 4)
    text_pages = _number(exhdr, 0x14, 4)
    text_size = _number(exhdr, 0x18, 4)
    _require(text_address > 0 and 0 < text_size <= text_pages * 0x1000,
             'ExHeader: text segment is empty or exceeds page allocation')
    exefs_offset = _number(data, 0x1A0, 4) * unit
    exefs_size = _number(data, 0x1A4, 4) * unit
    hash_size = _number(data, 0x1A8, 4) * unit
    _require(exefs_offset >= 0xA00 and exefs_size >= 512,
             'ExeFS: missing or overlaps executable headers')
    _require(hash_size == 512, 'ExeFS: expected one header hash media unit')
    exefs = _part(data, exefs_offset, exefs_size, 'ExeFS')
    _hash(exefs[:hash_size], data[0x1C0:0x1E0], 'ExeFS header')
    entries = {}
    intervals = []
    # makerom uses eight entries and reversed fileHashes[7 - index].
    for index in range(8):
        start = index * 16
        raw_name = exefs[start:start + 8]
        offset = _number(exefs, start + 8, 4)
        size = _number(exefs, start + 12, 4)
        if not any(raw_name):
            _require(offset == 0 and size == 0, 'ExeFS: unnamed file entry')
            continue
        try:
            name = raw_name.split(b'\0')[0].decode('ascii')
        except UnicodeDecodeError as error:
            raise ValueError('ExeFS: invalid filename') from error
        _require(name not in entries, f'ExeFS: duplicate {name} entry')
        _require(size > 0, f'ExeFS: {name} is empty')
        content = _part(exefs, 512 + offset, size, f'ExeFS {name}')
        _require(all(offset + size <= left or offset >= right for left, right in intervals),
                 f'ExeFS: overlapping {name} entry')
        intervals.append((offset, offset + size))
        hash_offset = 0x100 + (7 - index) * 32
        _hash(content, exefs[hash_offset:hash_offset + 32], f'ExeFS {name}')
        entries[name] = {'bytes': size, 'sha256': _sha(content).hex()}
        if name == 'icon':
            _require(content == smdh, 'ExeFS: icon differs from supplied SMDH')
    _require(set(entries) == {'.code', 'icon', 'banner'},
             'ExeFS: expected only .code, icon and banner')
    logo_offset = _number(data, 0x198, 4) * unit
    logo_size = _number(data, 0x19C, 4) * unit
    if logo_size:
        _hash(_part(data, logo_offset, logo_size, 'NCCH logo'), data[0x130:0x150], 'NCCH logo')
    return {'bytes': len(data), 'sha256': _sha(data).hex(), 'romfs_present': False,
            'exheader_sha256': _sha(exhdr).hex(), 'system_mode_ext': '124MB',
            'cpu_mhz': 804, 'l2_cache': True, 'text_bytes': text_size,
            'code_compressed': bool(exhdr[0x0D] & 1), 'exefs': entries}


def verify(cia: Path, cxi: Path, smdh: Path, *, iteration: int = 1,
           short_title=None, long_title=LONG_TITLE) -> dict:
    """Raise ValueError on a malformed/mismatched package; return JSON-safe evidence.

    A pass checks bytes and profile only. It does not validate signatures,
    installation, CPU execution, full code decompression, or physical hardware.
    """
    _require(isinstance(iteration, int) and 1 <= iteration <= 1023, 'Diagnostic iteration must be 1..1023 (16-bit title version)')
    cia_data, cxi_data, icon_data = (Path(path).read_bytes() for path in (cia, cxi, smdh))
    smdh_result = _smdh(icon_data, short_title or f'Dusklight Diagnostic 0.{iteration}', long_title)
    ncch_result = _ncch(cxi_data, icon_data, iteration)
    _part(cia_data, 0, 0x2020, 'CIA header')
    _require(_number(cia_data, 0, 4) == 0x2020, 'CIA: incorrect header size')
    _require(_number(cia_data, 4, 4) == 0, 'CIA: unexpected container type/version')
    cert_size, tik_size, tmd_size, meta_size = (_number(cia_data, offset, 4)
                                             for offset in (8, 12, 16, 20))
    content_size = _number(cia_data, 24, 8)
    _require(cia_data[32:0x2020] == b'\x80' + bytes(0x1FFF),
             'CIA: expected only content index 0')
    _require(cert_size > 0, 'CIA: certificate section missing')
    cert_offset = _align(0x2020)
    _part(cia_data, cert_offset, cert_size, 'CIA certificates')
    tik_offset = _align(cert_offset + cert_size)
    tmd_offset = _align(tik_offset + tik_size)
    content_offset = _align(tmd_offset + tmd_size)
    content = _part(cia_data, content_offset, content_size, 'CIA content')
    _require(content == cxi_data, 'CIA: content differs from supplied CXI')
    end = content_offset + content_size
    if meta_size:
        metadata = _part(cia_data, _align(end), meta_size, 'CIA metadata')
        _require(meta_size == 0x400 + len(icon_data), 'CIA: unexpected metadata size')
        _require(metadata[0x400:] == icon_data, 'CIA: metadata icon differs from supplied SMDH')
        end = _align(end) + meta_size
    _require(len(cia_data) == end, 'CIA: trailing or missing bytes')
    ticket = _signed_body(_part(cia_data, tik_offset, tik_size, 'CIA ticket'), 'Ticket')
    tmd = _signed_body(_part(cia_data, tmd_offset, tmd_size, 'CIA TMD'), 'TMD')
    _require(_number(ticket, 0x9C, 8, 'big') == TITLE_ID, 'Ticket: TitleID mismatch')
    _require(_number(ticket, 0xA6, 2, 'big') == iteration, 'Ticket: title version mismatch')
    _require(_number(tmd, 0x4C, 8, 'big') == TITLE_ID, 'TMD: TitleID mismatch')
    _require(_number(tmd, 0x9C, 2, 'big') == iteration, 'TMD: title version mismatch')
    _require(_number(tmd, 0x9E, 2, 'big') == 1 and _number(tmd, 0xA0, 2, 'big') == 0,
             'TMD: expected one boot content at index 0')
    records = _part(tmd, 0xC4, 64 * 36, 'TMD content-info records')
    chunks = _part(tmd, 0x9C4, 48, 'TMD content chunk')
    _require(len(tmd) == 0x9F4, 'TMD: unexpected size')
    _hash(records, tmd[0xA4:0xC4], 'TMD info-record table')
    _require(_number(records, 0, 2, 'big') == 0 and _number(records, 2, 2, 'big') == 1,
             'TMD: first info record does not cover the single content')
    _require(not any(records[36:]), 'TMD: unexpected additional info records')
    _hash(chunks, records[4:36], 'TMD content-chunk table')
    _require(_number(chunks, 0, 4, 'big') == 0 and _number(chunks, 4, 2, 'big') == 0,
             'TMD: content ID/index mismatch')
    _require(_number(chunks, 6, 2, 'big') == 0, 'TMD: content flags must be unencrypted/nonshared')
    _require(_number(chunks, 8, 8, 'big') == content_size, 'TMD: content size mismatch')
    _hash(content, chunks[16:48], 'TMD content')
    return {'validation': 'structure-and-hashes-only', 'signature_validation': False,
            'physical_console_test': False, 'gameplay_verified': False,
            'title_id': f'{TITLE_ID:016x}', 'title_version': iteration,
            'cia_bytes': len(cia_data), 'cia_sha256': _sha(cia_data).hex(),
            'content_sha256_verified': True, 'tmd_table_hashes_verified': True,
            'ncch': ncch_result, 'smdh': smdh_result}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('cia', type=Path)
    parser.add_argument('cxi', type=Path)
    parser.add_argument('smdh', type=Path)
    parser.add_argument('--iteration', type=int, default=1)
    args = parser.parse_args()
    print(json.dumps(verify(args.cia, args.cxi, args.smdh, iteration=args.iteration), indent=2))
