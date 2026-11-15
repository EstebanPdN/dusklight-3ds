"""Disassemble a picasso .shbin (DVLB) vertex shader: instructions with resolved
operand descriptors. usage: python3 -I pica_shader_disasm.py file.shbin"""
import struct, sys
OPS = {0x00:'add',0x01:'dp3',0x02:'dp4',0x03:'dph',0x04:'dst',0x05:'ex2',0x06:'lg2',0x07:'litp',0x08:'mul',0x09:'sge',0x0a:'slt',
       0x0b:'flr',0x0c:'max',0x0d:'min',0x0e:'rcp',0x0f:'rsq',0x12:'mova',0x13:'mov',0x18:'dphi',0x19:'dsti',0x1a:'sgei',0x1b:'slti',
       0x20:'break',0x21:'nop',0x22:'end',0x23:'breakc',0x24:'call',0x25:'callc',0x26:'callu',0x27:'ifu',0x28:'ifc',0x29:'loop',
       0x2a:'emit',0x2b:'setemit',0x2c:'jmpc',0x2d:'jmpu',0x2e:'cmp',0x2f:'cmp'}
UNARY = {'ex2','lg2','litp','flr','rcp','rsq','mova','mov'}
def load(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'DVLB'
    n = struct.unpack('<I', d[4:8])[0]; dvlp = 8 + 4 * n
    assert d[dvlp:dvlp+4] == b'DVLP'
    code_off, code_n, desc_off, desc_n = struct.unpack('<IIII', d[dvlp+8:dvlp+24])
    code = struct.unpack('<%dI' % code_n, d[dvlp+code_off:dvlp+code_off+4*code_n])
    descs = [struct.unpack('<I', d[dvlp+desc_off+8*i:dvlp+desc_off+8*i+4])[0] for i in range(desc_n)]
    return code, descs
def reg(r, wide):
    if r < 0x10: return 'v%d' % r
    if r < 0x20: return 'r%d' % (r - 0x10)
    return 'c%d' % (r - 0x20)
def dst(r):
    return 'o%d' % r if r < 0x10 else 'r%d' % (r - 0x10)
def swz(sel):
    return ''.join('xyzw'[(sel >> (6 - 2 * i)) & 3] for i in range(4))
def src(r, d, which, idx=0, wide=False):
    shift = (5, 14, 23)[which]
    neg = '-' if d >> (shift - 1) & 1 else ''
    s = swz(d >> shift & 0xff)
    name = reg(r, wide)
    if idx: name = 'c[%s+%d]' % (('a0.x', 'a0.y', 'aL')[idx - 1], r - 0x20)
    return neg + name + '.' + s
def mask(d):
    return ''.join(c for i, c in enumerate('xyzw') if d >> (3 - i) & 1)
def disasm(code, descs):
    out = []
    for pc, w in enumerate(code):
        op = w >> 26
        if op >= 0x30:
            name = 'madi' if op < 0x38 else 'mad'
            d = descs[w & 0x1f]
            out.append((pc, name, 'mad %08x' % w)); continue
        name = OPS.get(op, 'op%02x' % op)
        if name in ('nop', 'end', 'break', 'emit'):
            out.append((pc, name, name)); continue
        if name in ('ifc', 'ifu', 'loop', 'call', 'callc', 'callu', 'jmpc', 'jmpu', 'breakc'):
            out.append((pc, name, '%s dst=%d num=%d cond=%x refx=%d refy=%d id=%d' % (name, w >> 10 & 0xfff, w & 0xff, w >> 22 & 3, w >> 25 & 1, w >> 24 & 1, w >> 22 & 0xf))); continue
        d = descs[w & 0x7f]
        if name == 'cmp':
            s1 = w >> 12 & 0x7f; s2 = w >> 7 & 0x1f; idx = w >> 19 & 3
            cx = ('eq', 'ne', 'lt', 'le', 'gt', 'ge', '??', '??')[w >> 24 & 7]; cy = ('eq', 'ne', 'lt', 'le', 'gt', 'ge', '??', '??')[w >> 21 & 7]
            out.append((pc, name, 'cmp %s, %s, %s, %s' % (src(s1, d, 0, idx, True), cx, cy, src(s2, d, 1)))); continue
        dest = w >> 21 & 0x1f; idx = w >> 19 & 3
        if name in ('dphi', 'dsti', 'sgei', 'slti'):
            s1 = w >> 14 & 0x1f; s2 = w >> 7 & 0x7f
            out.append((pc, name, '%s %s.%s, %s, %s' % (name, dst(dest), mask(d), src(s1, d, 0), src(s2, d, 1, idx, True)))); continue
        s1 = w >> 12 & 0x7f; s2 = w >> 7 & 0x1f
        if name == 'mova':
            out.append((pc, name, 'mova a0.%s, %s' % (mask(d), src(s1, d, 0, idx, True)))); continue
        if name in UNARY:
            out.append((pc, name, '%s %s.%s, %s' % (name, dst(dest), mask(d), src(s1, d, 0, idx, True)))); continue
        out.append((pc, name, '%s %s.%s, %s, %s' % (name, dst(dest), mask(d), src(s1, d, 0, idx, True), src(s2, d, 1))))
    return out
if __name__ == '__main__':
    code, descs = load(sys.argv[1])
    for pc, name, text in disasm(code, descs): print('%3d  %s' % (pc, text))
