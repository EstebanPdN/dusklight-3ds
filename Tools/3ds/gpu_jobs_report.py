#!/usr/bin/env python3
"""Where the console PICA spends a picture, from an extracted application dump.

Reads a directory written by `verify_application_dump.py <dump.dpk> --extract`
(gpu-jobs.bin, ram-14000000-*.bin, vram.bin, index.txt). The job ring holds the
words of the last GSP jobs with their submission and completion ticks
(ctru_gxqueue.c: one job in flight at a time, so a job's time is the GPU's).
The command lists of the last complete picture (from its clear to its display
transfer) are still in RAM: each one is decoded register by register, its draws
listed with their index count, unique vertices, textures and raster state, and,
for draws of the rigid 32-byte records, the triangles inside the clip volume
and their screen area (vertices transformed with the uniforms the list sent).

With sdmc:/3ds/DuskLight/gpu-draw-times.txt each command list holds one draw,
so the time of each draw is measured: --draws lists them, most expensive first,
and a least-squares fit gives the cost per index and per screen of area.

Run with `python3 -I`; standard library only. Private dump data stays local.
"""
import argparse
import struct
import sys
from pathlib import Path

TICKS_PER_SECOND = 268111856.0
LINEAR_VA, FCRAM_PA, VRAM_PA = 0x14000000, 0x20000000, 0x18000000
KIND = {0: 'dma', 1: 'list', 2: 'fill', 3: 'transfer', 4: 'copy', 5: 'flush'}
TEXTURE_FORMATS = {0: 'RGBA8', 1: 'RGB8', 2: 'RGBA5551', 3: 'RGB565', 4: 'RGBA4', 5: 'IA8', 6: 'HILO8', 7: 'I8',
                   8: 'A8', 9: 'IA4', 10: 'I4', 11: 'A4', 12: 'ETC1', 13: 'ETC1A4'}


class Memory:
    def __init__(self, directory):
        rams = sorted(directory.glob('ram-14000000-*.bin'))
        if not rams:
            raise SystemExit(f'{directory}: no ram-14000000-*.bin (extract the dump with verify_application_dump.py --extract)')
        self.linear = rams[0].read_bytes()
        vram = directory / 'vram.bin'
        self.vram = vram.read_bytes() if vram.exists() else b''

    def read(self, physical, fmt):
        if FCRAM_PA <= physical < FCRAM_PA + len(self.linear):
            return struct.unpack_from(fmt, self.linear, physical - FCRAM_PA)
        if VRAM_PA <= physical < VRAM_PA + len(self.vram):
            return struct.unpack_from(fmt, self.vram, physical - VRAM_PA)
        raise ValueError(f'address {physical:08x} not in the dump')

    def words(self, virtual, count):
        return struct.unpack_from(f'<{count}I', self.linear, virtual - LINEAR_VA)


def jobs(path):
    data = path.read_bytes()
    out = []
    for n in range(len(data) // 56):
        w = struct.unpack_from('<14I', data, n * 56)
        submitted, completed = w[8] | (w[9] << 32), w[10] | (w[11] << 32)
        out.append({'type': w[0] & 0xff, 'words': w, 'submitted': submitted, 'completed': completed,
                    'ms': (completed - submitted) * 1000 / TICKS_PER_SECOND if completed else None})
    return out


def last_picture(ring):
    """The jobs from the last clear (fill) that has a display transfer after it."""
    for start in reversed([n for n, j in enumerate(ring) if j['type'] == 2]):
        ends = [n for n in range(start, len(ring)) if ring[n]['type'] == 3]
        if ends and all(j['ms'] is not None for j in ring[start:ends[0] + 1]):
            return ring[start:ends[0] + 1]
    return []


def commands(words):
    """(register, value, byte mask) of every write of a PICA command list."""
    n, out = 0, []
    while n + 1 < len(words):
        parameter, header = words[n], words[n + 1]
        n += 2
        register, mask, extra, consecutive = header & 0xffff, (header >> 16) & 0xf, (header >> 20) & 0xff, header >> 31
        values = [parameter, *words[n:n + extra]]
        n += extra + (extra & 1)
        for k, value in enumerate(values):
            out.append((register + k if consecutive else register, value, mask))
    return out


def f32(word):
    return struct.unpack('<f', struct.pack('<I', word))[0]


class State:
    """Registers and float uniforms as the lists leave them (carried across lists)."""

    def __init__(self):
        self.registers = {}
        self.uniforms = [[0.0] * 4 for _ in range(96)]
        self.uniform_index = 0
        self.pending = []

    def write(self, register, value, mask):
        if mask != 0xf and register in self.registers:
            bits = sum(0xff << (8 * b) for b in range(4) if mask >> b & 1)
            value = (self.registers[register] & ~bits) | (value & bits)
        if register == 0x2c0:
            self.uniform_index, self.pending = value & 0xff, []
        elif 0x2c1 <= register <= 0x2c8:
            self.pending.append(value)
            if len(self.pending) == 4:  # f32 mode, components w z y x
                w, z, y, x = (f32(v) for v in self.pending)
                if self.uniform_index < 96:
                    self.uniforms[self.uniform_index] = [x, y, z, w]
                self.uniform_index += 1
                self.pending = []
        self.registers[register] = value
        return register in (0x22e, 0x22f) and value & 1

    def get(self, register):
        return self.registers.get(register, 0)


def textures(state):
    out = []
    enabled = state.get(0x80)
    for unit, (dim, param, address, kind, lod) in enumerate(((0x82, 0x83, 0x85, 0x8e, 0x84), (0x92, 0x93, 0x95, 0x96, 0x94),
                                                             (0x9a, 0x9b, 0x9d, 0x9e, 0x9c))):
        if not enabled >> unit & 1:
            continue
        size = state.get(dim)
        physical = state.get(address) << 3
        where = 'vram' if VRAM_PA <= physical < VRAM_PA + 0x600000 else 'fcram'
        out.append(f'{TEXTURE_FORMATS.get(state.get(kind) & 0xf, "?")} {(size >> 16) & 0x7ff}x{size & 0x7ff}'
                   f' mip{(state.get(lod) >> 16) & 0xf} {where}')
    return out


def rigid_geometry(memory, state, count):
    """Clip-volume triangles, unique vertices and screen area of a draw of the
    32-byte rigid records (position float xyz at 0, position matrix row at 26),
    or None for any other layout."""
    if (state.get(0x201) & 0xf) != 0xb or state.get(0x2bb) != 0x345210 or (state.get(0x204) & 0xffffff) != 0x543210:
        return None
    stride = (state.get(0x205) >> 16) & 0xff
    if stride != 32:
        return None
    base = state.get(0x200) << 3
    vertices = base + (state.get(0x203) & 0xfffffff)
    index_config = state.get(0x227)
    indices = memory.read(base + (index_config & 0xfffffff), f'<{count}H' if index_config >> 31 else f'<{count}B')
    u = state.uniforms
    projection = u[6:10]
    clamped = u[10][3] > 0
    cache = {}

    def clip(index):
        if index not in cache:
            x, y, z, row = memory.read(vertices + index * stride, '<3f14xb5x')
            m = u[row:row + 3] if 0 <= row <= 93 else [[0.0] * 4] * 3
            eye = [m[k][0] * x + m[k][1] * y + m[k][2] * z + m[k][3] for k in range(3)] + [1.0]
            cache[index] = [sum(projection[k][q] * eye[q] for q in range(4)) for k in range(4)]
        return cache[index]

    tests = [lambda c: c[0] > c[3], lambda c: c[0] < -c[3], lambda c: c[1] > c[3], lambda c: c[1] < -c[3]]
    if not clamped:
        tests.append(lambda c: c[2] > 0)
    inside = area = 0.0
    for t in range(0, count - 2, 3):
        corners = [clip(indices[t + q]) for q in range(3)]
        if any(all(test(c) for c in corners) for test in tests):
            continue
        inside += 1
        if all(c[3] > 1e-6 for c in corners):
            p = [((c[0] / c[3] + 1) * 120, (c[1] / c[3] + 1) * 200) for c in corners]
            raw = abs((p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (p[2][0] - p[0][0]) * (p[1][1] - p[0][1])) / 2
            xs = [min(max(q[0], 0), 240) for q in p]
            ys = [min(max(q[1], 0), 400) for q in p]
            area += min(raw, (max(xs) - min(xs)) * (max(ys) - min(ys)))
        else:
            area += 48000  # crosses the camera plane: counted as half a screen
    return {'inside': int(inside), 'unique': len(cache), 'screens': area / 96000}


def describe_list(memory, state, job):
    address, size = job['words'][1], job['words'][2]
    draws = []
    for register, value, mask in commands(memory.words(address, size // 4)):
        if not state.write(register, value, mask):
            continue
        count = state.get(0x228)
        depth = state.get(0x107)
        draw = {'indices': count, 'textures': textures(state), 'depth_test': depth & 1, 'depth_write': depth >> 12 & 1,
                'colour_mask': depth >> 8 & 0xf, 'alpha_test': state.get(0x104) & 1, 'lights': (state.get(0x2b1) & 0xff) + 1,
                'colour_buffer': state.get(0x11d) << 3}
        try:
            draw['rigid'] = rigid_geometry(memory, state, count)
        except (ValueError, struct.error):
            draw['rigid'] = None
        draws.append(draw)
    return draws


def least_squares(rows, targets):
    k = len(rows[0])
    m = [[sum(r[i] * r[j] for r in rows) for j in range(k)] + [sum(r[i] * t for r, t in zip(rows, targets))] for i in range(k)]
    for i in range(k):
        pivot = max(range(i, k), key=lambda r: abs(m[r][i]))
        m[i], m[pivot] = m[pivot], m[i]
        if abs(m[i][i]) < 1e-12:
            return None
        for r in range(k):
            if r != i:
                f = m[r][i] / m[i][i]
                m[r] = [a - f * b for a, b in zip(m[r], m[i])]
    return [m[i][k] / m[i][i] for i in range(k)]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('extracted', type=Path, help='directory written by verify_application_dump.py --extract')
    parser.add_argument('--draws', type=int, default=25, help='draws to list, most expensive first (one-draw lists only)')
    args = parser.parse_args()
    memory = Memory(args.extracted)
    ring = jobs(args.extracted / 'gpu-jobs.bin')
    picture = last_picture(ring)
    if not picture:
        raise SystemExit('no complete picture (clear to display transfer) in the job ring')
    index = (args.extracted / 'index.txt').read_text(errors='replace')
    scene = next((line.split('=', 1)[1] for line in index.splitlines() if line.startswith('scene=')), '?')
    room = next((line.split('=', 1)[1] for line in index.splitlines() if line.startswith('room=')), '?')
    span = (picture[-1]['completed'] - picture[0]['submitted']) * 1000 / TICKS_PER_SECOND
    busy = sum(j['ms'] for j in picture)
    print(f'{scene} room {room}: last picture {len(picture)} GPU jobs, busy {busy:.2f} ms of {span:.2f} ms '
          f'(clear to display transfer), {len(ring)} jobs in the ring')
    state = State()
    lists = [j for j in picture if j['type'] == 1]
    for job in lists:  # first pass: the state the picture's first list inherits
        try:
            describe_list(memory, state, job)
        except (ValueError, struct.error):
            pass
    timed = []
    by_kind = {}
    previous = None
    for job in picture:
        gap = (job['submitted'] - previous) * 1000 / TICKS_PER_SECOND if previous else 0.0
        previous = job['completed']
        kind = KIND.get(job['type'], str(job['type']))
        by_kind[kind] = by_kind.get(kind, 0.0) + job['ms']
        if job['type'] != 1:
            print(f'  {kind:8} {job["ms"]:7.2f} ms  idle before {gap:5.2f}')
            continue
        try:
            draws = describe_list(memory, state, job)
        except (ValueError, struct.error) as error:
            print(f'  list     {job["ms"]:7.2f} ms  idle before {gap:5.2f}  (not decoded: {error})')
            continue
        indices = sum(d['indices'] for d in draws)
        rigid = [d['rigid'] for d in draws if d['rigid']]
        inside = sum(r['inside'] for r in rigid)
        screens = sum(r['screens'] for r in rigid)
        print(f'  list     {job["ms"]:7.2f} ms  idle before {gap:5.2f}  {job["words"][2]:6d} bytes, {len(draws):3d} draws, '
              f'{indices:6d} indices ({indices / 3 - inside if rigid else 0:6.0f} triangles outside the clip volume), '
              f'{screens:5.2f} screens of area')
        if len(draws) == 1:
            timed.append((job['ms'], draws[0]))
    print('  by kind: ' + ', '.join(f'{k} {v:.2f} ms' for k, v in sorted(by_kind.items(), key=lambda kv: -kv[1])))
    if len(timed) < 8:
        print('Fewer than 8 one-draw lists: run with sdmc:/3ds/DuskLight/gpu-draw-times.txt for the time of each draw.')
        return
    print(f'\nOne-draw lists: {len(timed)}, {sum(t for t, _ in timed):.2f} ms. Most expensive:')
    for ms, d in sorted(timed, key=lambda item: -item[0])[:args.draws]:
        r = d['rigid']
        geometry = (f'unique {r["unique"]:5d} inside {r["inside"]:5d} tris, {r["screens"]:4.2f} screens' if r else 'not a rigid record draw')
        print(f'  {ms:6.2f} ms  {d["indices"]:5d} indices  {geometry}  z{d["depth_test"]}{d["depth_write"]} '
              f'alpha-test {d["alpha_test"]} lights {d["lights"]}  ' + '; '.join(d['textures']))
    rows = [(d['indices'] / 1000, d['rigid']['screens'], 1.0) for _, d in timed if d['rigid']]
    targets = [ms for ms, d in timed if d['rigid']]
    fit = least_squares(rows, targets) if len(rows) >= 8 else None
    if fit:
        print(f'\nFit over {len(rows)} rigid draws: {fit[0]:.3f} ms per 1000 indices, {fit[1]:.3f} ms per screen of area, '
              f'{fit[2]:.3f} ms per draw')


if __name__ == '__main__':
    sys.exit(main())
