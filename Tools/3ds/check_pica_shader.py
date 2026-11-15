#!/usr/bin/env python3
"""Build gate for picasso vertex shaders (.shbin): every component of every
output register the shader declares must be written exactly once on every
path to END.

Why: on a New 3DS the PICA200 never finished a vertex of gx_rigid.v.pica,
whose texcoord0 register was written only in x, y and z (2026-11-13/14, five
console runs). 3dbrew (GPU/Pitfalls) states the rule: each configured output
component has to be written exactly once or the PICA freezes; a second write
is ignored when it comes late and can stop the GPU when it comes early.
Azahar runs such shaders as written, so only this check catches them before
a console does.

usage: check_pica_shader.py file.shbin [more.shbin ...]   (exit 1 on a finding)
"""
import struct
import sys

FLOW_IF = (0x27, 0x28)          # ifu, ifc
FLOW_CALL = (0x24, 0x25, 0x26)  # call, callc, callu
FLOW_JUMP = (0x2c, 0x2d)        # jmpc, jmpu
COMPONENTS = 'xyzw'


def load(path):
    data = open(path, 'rb').read()
    if data[:4] != b'DVLB':
        raise ValueError('not a DVLB file')
    count = struct.unpack_from('<I', data, 4)[0]
    offsets = struct.unpack_from('<%dI' % count, data, 8)
    dvlp = 8 + 4 * count
    if data[dvlp:dvlp + 4] != b'DVLP':
        raise ValueError('DVLP block missing')
    code_offset, code_words, desc_offset, desc_count = struct.unpack_from('<IIII', data, dvlp + 8)
    code = struct.unpack_from('<%dI' % code_words, data, dvlp + code_offset)
    descs = [struct.unpack_from('<I', data, dvlp + desc_offset + 8 * i)[0] for i in range(desc_count)]
    programs = []
    for offset in offsets:
        if data[offset:offset + 4] != b'DVLE':
            raise ValueError('DVLE block missing')
        entry, end = struct.unpack_from('<II', data, offset + 8)
        out_offset, out_count = struct.unpack_from('<II', data, offset + 0x28)
        registers = set()
        for i in range(out_count):
            _kind, register, _mask, _unused = struct.unpack_from('<HHHH', data, offset + out_offset + 8 * i)
            registers.add(register)
        programs.append((entry, end, sorted(registers)))
    return code, descs, programs


def output_writes(code, descs, entry):
    """Per output register, three component masks over all paths from entry to END:
    written on every path, written on some path, written more than once on some path."""
    ends = []

    def merge(a, b):
        if a is None: return b
        if b is None: return a
        out = {}
        for r in set(a) | set(b):
            x, y = a.get(r, (0, 0, 0)), b.get(r, (0, 0, 0))
            out[r] = (x[0] & y[0], x[1] | y[1], x[2] | y[2])
        return out

    def walk(pc, stop, state, repeated=False):
        while pc < stop:
            if pc >= len(code):
                raise ValueError('execution runs past the code at %d' % pc)
            word = code[pc]; op = word >> 26
            if op == 0x22:                                   # end
                ends.append(dict(state)); return None
            if op in FLOW_IF:
                dst, num = word >> 10 & 0xfff, word & 0xff
                state = merge(walk(pc + 1, dst, dict(state), repeated), walk(dst, dst + num, dict(state), repeated))
                if state is None: return None
                pc = dst + num; continue
            if op == 0x29:                                   # loop: the body runs one or more times
                dst = word >> 10 & 0xfff
                state = walk(pc + 1, dst + 1, state, True)
                if state is None: return None
                pc = dst + 1; continue
            if op in FLOW_CALL:
                dst, num = word >> 10 & 0xfff, word & 0xff
                if op == 0x24:
                    state = walk(dst, dst + num, state, repeated)
                else:                                        # conditional call: may or may not run
                    state = merge(dict(state), walk(dst, dst + num, dict(state), repeated))
                if state is None: return None
                pc += 1; continue
            if op in FLOW_JUMP:
                raise ValueError('jmpc/jmpu at %d: unstructured flow is not analysed' % pc)
            if op >= 0x30:                                   # mad / madi
                dest, desc = word >> 24 & 0x1f, descs[word & 0x1f]
            elif op <= 0x0f or op == 0x13 or 0x18 <= op <= 0x1b:
                dest, desc = word >> 21 & 0x1f, descs[word & 0x7f]
            else:
                pc += 1; continue                            # cmp, mova, nop, break, emit...
            if dest < 0x10:
                mask = desc & 0xf
                every, some, twice = state.get(dest, (0, 0, 0))
                twice |= some & mask
                if repeated: twice |= mask                   # inside a loop body
                state[dest] = (every | mask, some | mask, twice)
            pc += 1
        return state

    walk(entry, len(code), {})
    if not ends:
        raise ValueError('no END instruction is reached')
    result = None
    for state in ends:
        result = merge(result, state)
    return result


def components(mask):
    return ''.join(c for i, c in enumerate(COMPONENTS) if mask >> (3 - i) & 1)


def check(path):
    code, descs, programs = load(path)
    findings = []
    for index, (entry, _end, registers) in enumerate(programs):
        writes = output_writes(code, descs, entry)
        for register in registers:
            every, _some, twice = writes.get(register, (0, 0, 0))
            if every != 0xf:
                findings.append('program %d: output register o%d is not written in .%s on every path to END'
                                % (index, register, components(~every & 0xf)))
            if twice:
                findings.append('program %d: output register o%d is written more than once in .%s on some path'
                                % (index, register, components(twice)))
    return findings


def main(paths):
    failed = False
    for path in paths:
        try:
            findings = check(path)
        except (OSError, ValueError, struct.error) as error:
            findings = ['cannot analyse: %s' % error]
        for finding in findings:
            failed = True
            print('%s: error: %s (the console PICA takes each output component exactly once)' % (path, finding), file=sys.stderr)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
