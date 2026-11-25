"""Host interpreter for picasso vertex shaders (.shbin): runs one DVLE of the
binary over given inputs and uniforms and counts the instructions executed.

Why: the PICA's vertex work grows with the instructions each vertex runs, and
the emulator shows no GPU time. Running the assembled program itself (not a
re-implementation of its source) gives the instruction count of every path
and lets two programs be compared output for output on the same inputs.

Arithmetic follows the emulator's interpreter (float32; a product with a zero
factor is zero even against infinity; MAX/MIN in the form "a > b ? a : b").
The console computes with 24-bit floats, so outputs are proofs of equality
between two programs that run the same operations, not console values.

usage: python3 -I pica_vertex_interp.py file.shbin      (lists programs and sizes)
"""
import math
import struct
import sys

OUTPUT_NAMES = {0: 'position', 1: 'normalquat', 2: 'color', 3: 'texcoord0', 4: 'texcoord0w', 5: 'texcoord1',
                6: 'texcoord2', 8: 'view'}


def f32(x):
    """Round a Python float to float32 (overflow gives an infinity, as the FPU does)."""
    try:
        return struct.unpack('<f', struct.pack('<f', x))[0]
    except OverflowError:
        return math.copysign(math.inf, x)


def bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def float24(raw):
    """picasso stores constants as 24-bit floats (1 sign, 7 exponent, 16 mantissa)."""
    raw &= 0xffffff
    sign = (raw >> 23) << 31
    if not raw & 0x7fffff:
        return struct.unpack('<f', struct.pack('<I', sign))[0]
    exponent = (raw >> 16) & 0x7f
    exponent = 255 if exponent == 0x7f else exponent + 64
    return struct.unpack('<f', struct.pack('<I', sign | ((raw & 0xffff) << 7) | (exponent << 23)))[0]


class Program:
    """One DVLE: entry point, constants, outputs and named uniforms."""
    def __init__(self, index, entry, end, constants, outputs, uniforms):
        self.index, self.entry, self.end = index, entry, end
        self.constants, self.outputs, self.uniforms = constants, outputs, uniforms


class Shbin:
    def __init__(self, path):
        data = open(path, 'rb').read()
        if data[:4] != b'DVLB':
            raise ValueError('%s: not a DVLB file' % path)
        count = struct.unpack_from('<I', data, 4)[0]
        offsets = struct.unpack_from('<%dI' % count, data, 8)
        dvlp = 8 + 4 * count
        if data[dvlp:dvlp + 4] != b'DVLP':
            raise ValueError('%s: DVLP block missing' % path)
        code_offset, code_words, desc_offset, desc_count = struct.unpack_from('<IIII', data, dvlp + 8)
        self.code = struct.unpack_from('<%dI' % code_words, data, dvlp + code_offset)
        self.descs = [struct.unpack_from('<I', data, dvlp + desc_offset + 8 * i)[0] for i in range(desc_count)]
        self.programs = []
        for index, offset in enumerate(offsets):
            if data[offset:offset + 4] != b'DVLE':
                raise ValueError('%s: DVLE block missing' % path)
            entry, end = struct.unpack_from('<II', data, offset + 8)
            const_offset, const_count = struct.unpack_from('<II', data, offset + 0x18)
            out_offset, out_count = struct.unpack_from('<II', data, offset + 0x28)
            uni_offset, uni_count = struct.unpack_from('<II', data, offset + 0x30)
            sym_offset, _sym_size = struct.unpack_from('<II', data, offset + 0x38)
            constants = []
            for i in range(const_count):
                at = offset + const_offset + 20 * i
                kind, register = struct.unpack_from('<HH', data, at)
                if kind == 2:
                    words = struct.unpack_from('<4I', data, at + 4)
                    constants.append(('f', register, tuple(float24(w) for w in words)))
                elif kind == 1:
                    constants.append(('i', register, tuple(data[at + 4:at + 8])))
                else:
                    constants.append(('b', register, data[at + 4] & 1))
            outputs = []
            for i in range(out_count):
                kind, register, mask, _ = struct.unpack_from('<HHHH', data, offset + out_offset + 8 * i)
                outputs.append((OUTPUT_NAMES.get(kind, 'type%d' % kind), register, mask))
            uniforms = {}
            for i in range(uni_count):
                symbol, start, stop = struct.unpack_from('<IHH', data, offset + uni_offset + 8 * i)
                name_at = offset + sym_offset + symbol
                name = data[name_at:data.index(b'\0', name_at)].decode()
                uniforms[name] = (start, stop)
            self.programs.append(Program(index, entry, end, constants, outputs, uniforms))


class Result:
    def __init__(self, outputs, executed, by_opcode):
        self.outputs, self.executed, self.by_opcode = outputs, executed, by_opcode


NAMES = {0x00: 'add', 0x01: 'dp3', 0x02: 'dp4', 0x03: 'dph', 0x08: 'mul', 0x09: 'sge', 0x0a: 'slt', 0x0b: 'flr',
         0x0c: 'max', 0x0d: 'min', 0x0e: 'rcp', 0x0f: 'rsq', 0x12: 'mova', 0x13: 'mov', 0x05: 'ex2', 0x06: 'lg2',
         0x20: 'break', 0x21: 'nop', 0x22: 'end', 0x23: 'breakc', 0x24: 'call', 0x25: 'callc', 0x26: 'callu',
         0x27: 'ifu', 0x28: 'ifc', 0x29: 'loop', 0x2e: 'cmp', 0x2f: 'cmp'}


def _mul(a, b):
    r = f32(a * b) if not (math.isinf(a) and b == 0 or math.isinf(b) and a == 0) else None
    if r is None or (math.isnan(r) and not math.isnan(a) and not math.isnan(b)):
        return 0.0
    return r


def _compare(a, b, op):
    return (a == b, a != b, a < b, a <= b, a > b, a >= b, True, True)[op]


def run(shbin, program, inputs, floats, ints=((0, 0, 0, 0),) * 4, bools=0, limit=100000):
    """Run one vertex. inputs: 16 four-float lists (v0..v15); floats: 96 rows
    (the program's constants are loaded over them, as Citro3D does on bind);
    ints: four (x, y, z, w) loop uniforms; bools: bit n = b n.
    Returns the output registers written (o0..o15) and the count executed."""
    code, descs = shbin.code, shbin.descs
    c = [list(map(f32, row)) for row in floats]
    i_uniforms = [list(v) for v in ints]
    # The constant table names registers by index (c0-c95, i0-i3, b0-b15);
    # the uniform table counts from 0x10 (libctru shaderInstanceInit).
    for kind, register, value in program.constants:
        if kind == 'f':
            c[register] = list(value)
        elif kind == 'i':
            i_uniforms[register] = list(value)
        elif value:
            bools |= 1 << register
        else:
            bools &= ~(1 << register)
    v = [list(map(f32, x)) for x in inputs]
    r = [[0.0] * 4 for _ in range(16)]
    o = {}
    a0 = [0, 0]
    aL = 0
    flags = [False, False]
    stack = []  # (final, return, repeat, increment, loop_start)
    pc = program.entry
    executed = 0
    by_opcode = {}

    def source(index, desc, which, relative):
        shift = (5, 14, 23)[which]
        negate = desc >> (shift - 1) & 1
        selector = desc >> shift & 0xff
        if index < 0x10:
            vec = v[index]
        elif index < 0x20:
            vec = r[index - 0x10]
        else:
            at = index - 0x20 + relative
            vec = c[at] if 0 <= at < 96 else [0.0] * 4
        out = [vec[(selector >> (6 - 2 * k)) & 3] for k in range(4)]
        return [-x for x in out] if negate else out

    def write(dest, desc, values):
        mask = desc & 0xf
        if dest < 0x10:
            target = o.setdefault(dest, [None] * 4)
        else:
            target = r[dest - 0x10]
        for k in range(4):
            if mask >> (3 - k) & 1:
                target[k] = values[k]

    def condition(word):
        refx, refy, op = word >> 25 & 1, word >> 24 & 1, word >> 22 & 3
        x, y = flags[0] == bool(refx), flags[1] == bool(refy)
        return (x or y, x and y, x, y)[op]

    while True:
        while stack and pc == stack[-1][0]:
            final, back, repeat, increment, start = stack[-1]
            aL += increment
            if repeat == 0:
                pc = back
                stack.pop()
            else:
                stack[-1] = (final, back, repeat - 1, increment, start)
                pc = start
        if executed > limit:
            raise RuntimeError('instruction limit reached')
        word = code[pc]
        op = word >> 26
        executed += 1
        name = NAMES.get(op, 'op%02x' % op)
        by_opcode[name] = by_opcode.get(name, 0) + 1
        nxt = pc + 1
        if op == 0x22:
            break
        if op == 0x21:
            pc = nxt
            continue
        if op in (0x2e, 0x2f):
            desc = descs[word & 0x7f]
            relative = (0, a0[0], a0[1], aL)[word >> 19 & 3]
            s1 = source(word >> 12 & 0x7f, desc, 0, relative)
            s2 = source(word >> 7 & 0x1f, desc, 1, 0)
            flags[0] = _compare(s1[0], s2[0], word >> 24 & 7)
            flags[1] = _compare(s1[1], s2[1], word >> 21 & 7)
            pc = nxt
            continue
        if op in (0x28, 0x27):  # ifc, ifu
            dst, num = word >> 10 & 0xfff, word & 0xff
            taken = condition(word) if op == 0x28 else bool(bools >> (word >> 22 & 0xf) & 1)
            if taken:
                stack.append((dst, dst + num, 0, 0, pc + 1))
                pc = pc + 1
            else:
                stack.append((dst + num, dst + num, 0, 0, dst))
                pc = dst
            continue
        if op == 0x29:  # loop
            dst = word >> 10 & 0xfff
            x, y, z, _ = i_uniforms[word >> 22 & 3]
            aL = y
            stack.append((dst + 1, dst + 1, x, z, pc + 1))
            pc = pc + 1
            continue
        if op in (0x24, 0x25, 0x26):
            dst, num = word >> 10 & 0xfff, word & 0xff
            go = True if op == 0x24 else condition(word) if op == 0x25 else bool(bools >> (word >> 22 & 0xf) & 1)
            if go:
                stack.append((dst + num, pc + 1, 0, 0, dst))
                pc = dst
            else:
                pc = nxt
            continue
        if op >= 0x30 or op in (0x18, 0x19, 0x1a, 0x1b, 0x20, 0x23, 0x2c, 0x2d):
            raise NotImplementedError('opcode %02x at %d' % (op, pc))
        desc = descs[word & 0x7f]
        dest = word >> 21 & 0x1f
        relative = (0, a0[0], a0[1], aL)[word >> 19 & 3]
        s1 = source(word >> 12 & 0x7f, desc, 0, relative)
        s2 = source(word >> 7 & 0x1f, desc, 1, 0)
        if op == 0x00:
            res = [f32(a + b) for a, b in zip(s1, s2)]
        elif op == 0x08:
            res = [_mul(a, b) for a, b in zip(s1, s2)]
        elif op in (0x01, 0x02, 0x03):
            n = 3 if op == 0x01 else 4
            left = s1[:3] + [1.0] if op == 0x03 else s1
            dot = 0.0
            for k in range(n):
                dot = f32(dot + _mul(left[k], s2[k]))
            res = [dot] * 4
        elif op == 0x0c:
            res = [a if a > b else b for a, b in zip(s1, s2)]
        elif op == 0x0d:
            res = [a if a < b else b for a, b in zip(s1, s2)]
        elif op == 0x09:
            res = [1.0 if a >= b else 0.0 for a, b in zip(s1, s2)]
        elif op == 0x0a:
            res = [1.0 if a < b else 0.0 for a, b in zip(s1, s2)]
        elif op == 0x0b:
            res = [float(math.floor(a)) if math.isfinite(a) else a for a in s1]
        elif op == 0x0e:
            x = s1[0]
            res = [f32(1.0 / x) if x != 0 else math.copysign(math.inf, x)] * 4
        elif op == 0x0f:
            x = s1[0]
            if x < 0 or math.isnan(x):
                y = math.nan
            elif x == 0:
                y = math.inf
            else:
                y = f32(1.0 / f32(math.sqrt(x)))
            res = [y] * 4
        elif op == 0x05:
            res = [f32(2.0 ** s1[0]) if s1[0] < 128 else math.inf] * 4
        elif op == 0x06:
            res = [f32(math.log2(s1[0])) if s1[0] > 0 else -math.inf] * 4
        elif op == 0x12:
            mask = desc & 0xf
            if mask & 8:
                a0[0] = int(s1[0])
            if mask & 4:
                a0[1] = int(s1[1])
            pc = nxt
            continue
        elif op == 0x13:
            res = s1
        else:
            raise NotImplementedError('opcode %02x at %d' % (op, pc))
        write(dest, desc, res)
        pc = nxt
    return Result(o, executed, by_opcode)


def program_named(shbin, entry_label=None, index=None):
    return shbin.programs[index if index is not None else 0]


if __name__ == '__main__':
    s = Shbin(sys.argv[1])
    print('%s: %d instruction words, %d operand descriptors, %d programs' % (sys.argv[1], len(s.code), len(s.descs), len(s.programs)))
    for p in s.programs:
        consts = ', '.join('%s%d=%s' % (k, reg, val) for k, reg, val in p.constants)
        print('  program %d: entry %d end %d; outputs %s; uniforms %s; constants %s' % (
            p.index, p.entry, p.end, [(n, reg) for n, reg, _ in p.outputs],
            {k: (a - 0x10, b - 0x10) for k, (a, b) in p.uniforms.items()}, consts))
