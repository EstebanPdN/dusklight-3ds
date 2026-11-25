#!/usr/bin/env python3
"""Fail the build when C++ exception state can be shared between threads.

devkitARM's libstdc++ finds each thread's exception state (the exceptions in
catch blocks and, on the ARM EH ABI, those running cleanups) with a pthread
key. pthread_key_create returns ENOSYS there, so libstdc++ uses one static
block for the whole process, and two threads that unwind at the same time pop
and rethrow each other's exceptions. The worker barrier at exit unwinds every
game worker at once: abort() from unwind_phase2 on the console (Alpha 13, 15).

runtime/eh_globals.cpp gives every thread its own block, and the image links
libstdc++'s callers to it with -Wl,--wrap=__cxa_get_globals(_fast). This
reads the linked image (ELF32, ARM code) and checks that:
- both wrappers are in it;
- no branch and no stored address reaches the original accessors, which
  return the shared block;
- the functions that push, pop and rethrow (__cxa_begin_catch, __cxa_rethrow,
  __cxa_begin_cleanup, __gnu_end_cleanup) call the wrapper.

usage: check_eh_globals.py <image.elf>
"""
import struct
import sys

ORIGINALS = ('__cxa_get_globals', '__cxa_get_globals_fast')
WRAPPERS = ('__wrap___cxa_get_globals', '__wrap___cxa_get_globals_fast')
MUST_CALL_WRAPPER = ('__cxa_begin_catch', '__cxa_rethrow', '__cxa_begin_cleanup', '__gnu_end_cleanup')
SHF_ALLOC, SHF_EXECINSTR = 0x2, 0x4
STT_FUNC = 2


def read_elf(data):
    if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 1:
        raise ValueError('not a little-endian ELF32 image')
    shoff, = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x2e)
    sections = []
    for index in range(shnum):
        name, kind, flags, addr, offset, size, link, info, align, entsize = struct.unpack_from(
            '<IIIIIIIIII', data, shoff + index * shentsize)
        sections.append(dict(name=name, kind=kind, flags=flags, addr=addr, offset=offset, size=size, link=link))
    names = sections[shstrndx]
    for section in sections:
        start = names['offset'] + section['name']
        section['name'] = data[start:data.index(b'\0', start)].decode()
    symtab = next((s for s in sections if s['kind'] == 2), None)
    if symtab is None:
        raise ValueError('no symbol table (stripped image)')
    strtab = sections[symtab['link']]
    symbols = []
    for at in range(symtab['offset'], symtab['offset'] + symtab['size'], 16):
        name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', data, at)
        start = strtab['offset'] + name
        symbols.append((data[start:data.index(b'\0', start)].decode(), value, size, info & 0xf))
    return sections, symbols


def arm_branch_target(word, address):
    """Target of an ARM B/BL/BLX(immediate) at address, else None."""
    if (word >> 25) & 0x7 != 0x5:
        return None
    offset = word & 0xffffff
    if offset & 0x800000:
        offset -= 0x1000000
    target = address + 8 + offset * 4
    if word >> 28 == 0xf:  # BLX immediate: H bit adds a halfword, target is Thumb
        target += ((word >> 24) & 1) * 2
    return target & 0xffffffff


def check(path):
    data = open(path, 'rb').read()
    sections, symbols = read_elf(data)
    defined = {}
    functions = []
    for name, value, size, kind in symbols:
        if value and name:
            defined.setdefault(name, value & ~1)
        if kind == STT_FUNC and value and size:
            functions.append((value & ~1, size, name))
    functions.sort()
    problems = []
    for name in WRAPPERS:
        if name not in defined:
            problems.append(f'{name} is not in the image (runtime/eh_globals.cpp and -Wl,--wrap missing)')
    originals = {defined[n]: n for n in ORIGINALS if n in defined}
    wrappers = {defined[n]: n for n in WRAPPERS if n in defined}
    original_ranges = [(start, start + size) for start, size, name in functions if name in ORIGINALS]

    def owner(address):
        low, high = 0, len(functions)
        while low < high:
            middle = (low + high) // 2
            if functions[middle][0] <= address:
                low = middle + 1
            else:
                high = middle
        if low:
            start, size, name = functions[low - 1]
            if address < start + size:
                return name
        return f'0x{address:08x}'

    wrapper_callers = set()
    original_calls = []
    stored = []
    for section in sections:
        if not section['flags'] & SHF_ALLOC or section['kind'] == 8:  # skip NOBITS
            continue
        base, size, offset = section['addr'], section['size'] & ~3, section['offset']
        words = struct.unpack_from(f'<{size // 4}I', data, offset)
        executable = section['flags'] & SHF_EXECINSTR
        for index, word in enumerate(words):
            address = base + index * 4
            if word & ~1 in originals and section['name'] not in ('.ARM.exidx', '.ARM.extab'):
                stored.append((address, originals[word & ~1]))
            if not executable:
                continue
            target = arm_branch_target(word, address)
            if target is None:
                continue
            if target in originals and not any(lo <= address < hi for lo, hi in original_ranges):
                original_calls.append((address, originals[target]))
            elif target in wrappers:
                wrapper_callers.add(owner(address))
    for address, name in original_calls:
        problems.append(f'{owner(address)} at 0x{address:08x} branches to {name}, the shared exception state')
    for address, name in stored:
        problems.append(f'the address of {name} is stored at 0x{address:08x} ({owner(address)})')
    if wrappers:
        for name in MUST_CALL_WRAPPER:
            if name in defined and name not in wrapper_callers:
                problems.append(f'{name} does not call {WRAPPERS[0]}')
    return problems, sorted(wrapper_callers)


def main():
    if len(sys.argv) != 2:
        print('usage: check_eh_globals.py <image.elf>', file=sys.stderr)
        return 2
    try:
        problems, callers = check(sys.argv[1])
    except (OSError, ValueError, struct.error) as error:
        print(f'check_eh_globals.py: error: {error}', file=sys.stderr)
        return 2
    for problem in problems:
        print(f'check_eh_globals.py: error: {problem}', file=sys.stderr)
    if problems:
        print('check_eh_globals.py: error: two threads unwinding at once would share one exception state '
              '(see runtime/eh_globals.cpp).', file=sys.stderr)
        return 1
    print(f'check_eh_globals.py: exception state per thread; {len(callers)} libstdc++ functions use it '
          f'({", ".join(callers)})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
