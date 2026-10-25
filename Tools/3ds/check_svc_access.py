#!/usr/bin/env python3
"""Fail when the ELF uses an SVC that the CXI exheader does not grant.

The console kills a process that calls an unlisted SVC; Azahar ignores the list.
Usage: check_svc_access.py ELF CXI
"""
import os, re, struct, subprocess, sys
from pathlib import Path

OBJDUMP = str(Path(os.environ.get('DEVKITARM', str(Path(os.environ.get('DEVKITPRO', '/opt/devkitpro')) / 'devkitARM'))) / 'bin/arm-none-eabi-objdump')

def used(elf):
    text = subprocess.run([OBJDUMP, '-d', elf], capture_output=True, text=True, check=True).stdout
    return {int(m, 16) for m in re.findall(r'\bsvc\s+0x([0-9a-f]+)', text)}

def granted(cxi):
    with open(cxi, 'rb') as stream: data = stream.read(0x200 + 0x400 + 0x400)
    caps = struct.unpack_from('<28I', data, 0x200 + 0x370)
    out = set()
    for cap in caps:
        if cap >> 27 == 0b11110:
            index = (cap >> 24) & 7
            out |= {index * 24 + bit for bit in range(24) if cap & (1 << bit)}
    return out

def main():
    elf, cxi = sys.argv[1:3]
    missing = sorted(used(elf) - granted(cxi))
    if missing:
        print('SVCs used but not granted: ' + ', '.join(hex(n) for n in missing))
        return 1
    print('SVC access OK')
    return 0

if __name__ == '__main__': sys.exit(main())
