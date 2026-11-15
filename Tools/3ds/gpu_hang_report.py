"""Report a gpu-hang-NN.bin written by the console GPU guard (gpu_canary_ctru.cpp).

usage: python3 -I gpu_hang_report.py gpu-hang-01.bin [--vertices N]

Chunks: ENTR the silent job's GSP words, REGS the shadow PICA registers (0x300
words), UNIF/UNIW the float uniforms, CMDS the silent command list (decode it
with pica_list_decode.py), JOBS the last GSP jobs (14 words each: eight GSP
words, submitted and completed ticks, event, counted-complete flag), INDX/VERT
the indices and vertices of the draw that ran alone, when one did."""
import struct, sys
def chunks(data):
    out, i = {}, 0
    while i + 8 <= len(data):
        tag = data[i:i+4].decode('ascii', 'replace'); n = struct.unpack('<I', data[i+4:i+8])[0]
        out[tag] = data[i+8:i+8+n]; i += 8 + n
    return out
def f24(x):
    s, e, m = x >> 23 & 1, x >> 16 & 0x7f, x & 0xffff
    if e == 0 and m == 0: return 0.0
    return (-1) ** s * (1 + m / 65536) * 2 ** (e - 63)
def main():
    c = chunks(open(sys.argv[1], 'rb').read())
    limit = int(sys.argv[sys.argv.index('--vertices') + 1]) if '--vertices' in sys.argv else 12
    print('job', ' '.join('%08x' % w for w in struct.unpack('<8I', c['ENTR'])))
    regs = struct.unpack('<%dI' % (len(c['REGS']) // 4), c['REGS'])
    if 'JOBS' in c:
        jobs = c['JOBS']; count = len(jobs) // 56
        print('last GPU jobs (oldest first; type 1 list, 2 fill, 3 display transfer, 4 texture copy):')
        for i in range(count):
            w = struct.unpack_from('<14I', jobs, i * 56)
            submitted, completed = w[8] | w[9] << 32, w[10] | w[11] << 32
            took = '%.2f ms' % ((completed - submitted) / 268111.856) if completed else 'never signalled'
            print('  type %d  %s  %s%s' % (w[0] & 0xff, ' '.join('%08x' % x for x in w[1:7]), took,
                                         '  (counted complete, no event)' if w[13] else ''))
    fmt, hi = regs[0x201], regs[0x202]
    count = (hi >> 28) + 1
    kinds = [((fmt if a < 8 else hi) >> (4 * (a % 8))) & 0xf for a in range(count)]
    sizes = {0: 1, 1: 1, 2: 2, 3: 4}; codes = {0: 'b', 1: 'B', 2: 'h', 3: 'f'}
    stride = regs[0x205] >> 16 & 0xff
    print('vertex format %08x/%08x: %d attributes, stride %d, indices %d' % (fmt, hi, count, stride, regs[0x228]))
    print('units %x  ' % (regs[0x80] & 7) + '  '.join('tex%d %dx%d type %d param %08x lod %08x' % (u, regs[d] >> 16, regs[d] & 0xffff, regs[t], regs[p], regs[l])
          for u, (d, p, l, t) in enumerate(((0x82, 0x83, 0x84, 0x8e), (0x92, 0x93, 0x94, 0x96), (0x9a, 0x9b, 0x9c, 0x9e))) if regs[0x80] >> u & 1))
    if 'UNIF' in c:
        words = struct.unpack('<384I', c['UNIF']); stored = c.get('UNIW', bytes(96))
        for i in range(96):
            if not stored[i]: continue
            w = words[4*i:4*i+4]
            if stored[i] == 4: v = [struct.unpack('<f', struct.pack('<I', x))[0] for x in reversed(w)]
            else:
                raw = (w[0] << 64) | (w[1] << 32) | w[2]; v = [f24((raw >> (24 * k)) & 0xffffff) for k in (0, 1, 2, 3)]
            print('  u[%2d] = %s' % (i, ' '.join('%12.5g' % x for x in v)))
    if 'INDX' in c:
        idx = struct.unpack('<%dH' % (len(c['INDX']) // 2), c['INDX'])
        print('indices %d, highest %d, first %s' % (len(idx), max(idx), idx[:12]))
    if 'VERT' in c and stride:
        v = c['VERT']; n = len(v) // stride; print('vertices', n)
        lo = [1e30] * 3; hi3 = [-1e30] * 3; bad = 0
        for i in range(n):
            at = i * stride; row = []
            for k in kinds:
                size, comps = sizes[k & 3], (k >> 2) + 1
                at = (at + size - 1) // size * size
                row.append(struct.unpack_from('<%d%s' % (comps, codes[k & 3]), v, at)); at += size * comps
            pos = row[0][:3]
            if any(x != x or abs(x) > 1e30 for x in pos): bad += 1
            else:
                for a in range(3): lo[a] = min(lo[a], pos[a]); hi3[a] = max(hi3[a], pos[a])
            if i < limit: print('  %4d %s' % (i, row))
        print('position bounds', lo, hi3, 'non-finite', bad)
if __name__ == '__main__': main()
