#!/usr/bin/env python3
"""Vertex program variants (features::Id::vertexVariants) against the general
multi-texture program, output bit for output bit.

A variant must run, for every plan the runtime gives it, the operations the
general program runs for that plan, in the same order on the same values.
This runner checks it by execution:
1. assembles gx_rigid_mt.v.pica and its variants into one binary, as the
   build does (the files and their order are read from CMakeLists.txt), and
   checks it: the output rule (check_pica_shader.py), 512 code words,
   128 operand descriptors, the same uniform and constant tables in every
   program (a variant switch keeps the uniform rows);
2. builds Source/platforms/3ds/tests/vertex_variant_test.cpp, which gives
   the key (runtime/gx_vertex_variant.hpp) and the variant the runtime
   chooses for each plan;
3. runs the general program and the chosen variant (pica_vertex_interp.py)
   on the same rows and vertices: a sweep of plans of every kind, and the
   plans recorded in the game (vertex-plans.bin files given as arguments,
   SD file vertex-plans.txt, gx_pica.cpp). Every output bit must match.
It also prints the instructions per vertex of each variant against the
general program.

usage: test_vertex_variants.py [--plans N] [vertex-plans.bin ...]
"""
import argparse
import os
from pathlib import Path
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / 'Source/platforms/3ds'
sys.path.insert(0, str(Path(__file__).resolve().parent))
import pica_vertex_interp as pica  # noqa: E402

RECORD = struct.Struct('<4I376f576s')  # gx_pica.cpp VertexPlanRecord
FOG_ONLY = 1 << 24                     # vertexKey::fogOnly


def program_files():
    """The .v.pica files of gx_rigid_mt.shbin, in the order of the build's picasso call."""
    text = (NATIVE / 'CMakeLists.txt').read_text()
    match = re.search(r'gx_rigid_mt\.shbin\.unchecked"([^\n]*)', text)
    if not match:
        raise SystemExit('CMakeLists.txt: no picasso command for gx_rigid_mt.shbin')
    names = re.findall(r'/([\w]+\.v\.pica)"', match.group(1))
    if not names or names[0] != 'gx_rigid_mt.v.pica':
        raise SystemExit('CMakeLists.txt: gx_rigid_mt.v.pica must be the first program of gx_rigid_mt.shbin')
    return [NATIVE / n for n in names]


def assemble(work):
    picasso = Path(os.environ.get('DEVKITPRO', str(Path.home() / 'devkitpro'))) / 'tools/bin/picasso'
    out = work / 'gx_rigid_mt.shbin'
    subprocess.run([str(picasso), '-o', str(out)] + [str(p) for p in program_files()], check=True, timeout=60)
    check = subprocess.run([sys.executable, '-I', str(ROOT / 'Tools/3ds/check_pica_shader.py'), str(out)],
                           capture_output=True, text=True)
    if check.returncode:
        raise SystemExit('output rule: ' + check.stderr)
    shbin = pica.Shbin(str(out))
    problems = []
    if len(shbin.code) > 512:
        problems.append('%d code words (the vertex unit holds 512)' % len(shbin.code))
    if len(shbin.descs) > 128:
        problems.append('%d operand descriptors (128)' % len(shbin.descs))
    first = shbin.programs[0]
    for p in shbin.programs[1:]:
        if p.uniforms != first.uniforms:
            problems.append('program %d: uniform table differs from the general program' % p.index)
        if sorted(p.constants) != sorted(first.constants):
            problems.append('program %d: constants differ from the general program' % p.index)
        if p.outputs != first.outputs:
            problems.append('program %d: outputs differ from the general program' % p.index)
    if problems:
        raise SystemExit('binary: ' + '; '.join(problems))
    return shbin


def build_helper(work):
    out = work / 'vertex-variant-test'
    aurora = ROOT / 'Source/extern/aurora/include'
    command = ['clang++', '-std=c++20', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1', '-I' + str(aurora), '-Wall', '-Wextra',
               '-Werror', '-O1', '-g', '-fsanitize=address,undefined', str(NATIVE / 'tests/vertex_variant_test.cpp'), '-o', str(out)]
    subprocess.run(command, check=True, timeout=180)
    return out


def choose(helper, plans_path):
    lines = subprocess.run([str(helper), str(plans_path)], check=True, capture_output=True, text=True, timeout=120).stdout.split()
    return [(int(lines[i], 16), int(lines[i + 1], 16), int(lines[i + 2])) for i in range(0, len(lines), 3)]


def read_plans(path):
    data = Path(path).read_bytes()
    magic, version, count, size = struct.unpack_from('<4I', data)
    if magic != 0x52505644 or version != 1 or size != RECORD.size:
        raise SystemExit('%s: not a vertex-plans.bin of version 1' % path)
    plans = []
    for n in range(count):
        fields = RECORD.unpack_from(data, 16 + n * RECORD.size)
        key, lights, stride, vertex_count = fields[:4]
        rows = [list(fields[4 + 4 * r:8 + 4 * r]) for r in range(94)]
        plans.append(dict(key=key, lights=lights, stride=stride, vertices=decode(fields[380], stride, vertex_count), rows=rows))
    return plans


def write_plans(path, plans):
    with open(path, 'wb') as f:
        f.write(struct.pack('<4I', 0x52505644, 1, len(plans), RECORD.size))
        for p in plans:
            flat = [x for row in p['rows'] for x in row]
            f.write(RECORD.pack(p['key'], p['lights'], p['stride'], len(p['raw']) // p['stride'], *flat, p['raw'].ljust(576, b'\0')))


def decode(raw, stride, count):
    """Shader inputs of compact records as the PICA's loader gives them
    (gx_geometry_pica.cpp layouts 1 and 2; missing components 0, w 1)."""
    out = []
    for n in range(count):
        at = n * stride
        pos = struct.unpack_from('<3f', raw, at)
        normal = struct.unpack_from('<3h', raw, at + 12)
        tex0 = struct.unpack_from('<2h', raw, at + 18)
        wide = stride == 36
        tex1 = struct.unpack_from('<4h' if wide else '<2h', raw, at + 22)
        matrix = struct.unpack_from('<2b', raw, at + (30 if wide else 26))
        color = struct.unpack_from('<4B', raw, at + (32 if wide else 28))
        v = [[0.0, 0.0, 0.0, 1.0] for _ in range(16)]
        v[0] = [*pos, 1.0]
        v[1] = [*map(float, normal), 1.0]
        v[2] = [*map(float, tex0), 0.0, 1.0]
        v[3] = [*map(float, color)]
        v[4] = [*map(float, matrix), 0.0, 1.0]
        v[5] = [*map(float, tex1)] if wide else [*map(float, tex1), 0.0, 1.0]
        out.append(v)
    return out


def record_bytes(rng, stride, rows_with_matrices):
    """Random compact records: positions near the origin, some behind the camera, zero normals now and then."""
    raw = b''
    for n in range(8):
        pos = [rng.uniform(-400, 400), rng.uniform(-400, 400), rng.uniform(-900, 300)]
        normal = [0, 0, 0] if n == 7 else [rng.randint(-16384, 16384) for _ in range(3)]
        tex = [rng.randint(-32768, 32767) for _ in range(6 if stride == 36 else 4)]
        position_row, normal_row = rng.choice(rows_with_matrices)
        color = [rng.randint(0, 255) for _ in range(4)]
        raw += struct.pack('<3f3h', *pos, *normal) + struct.pack('<%dh' % len(tex), *tex) + \
            struct.pack('<2b4B', position_row, normal_row, *color)
    return raw


# The plan kinds of the variants (roles of units 0-2, fog, range, carrier),
# so that the sweep reaches each of them often; the rest of a plan is random.
FAMILIES = [((1, 0, 0), 1, 1, 1), ((1, 0.5, 0), 1, 1, 0), ((2, 1, 0.5), 1, 1, 0), ((2, 1, 1), 0, 0, 0)]


def sweep_plan(rng):
    """Rows of a random plan of any kind, laid out as buildRigidMultiVertex and
    finishCompactRigidUniforms lay them out (gx_rigid_vertex.hpp)."""
    u = [[rng.uniform(-2, 2) for _ in range(4)] for _ in range(94)]
    family = rng.choice(FAMILIES) if rng.random() < 0.6 else None
    dark = rng.random() < (0.2 if family else 0.5)
    lights = rng.randint(1, 4)
    # Palette: one or two matrices of six rows from row 0 or after the lights.
    matrices = [(0, 3)]
    for r in range(3):
        u[r] = [rng.uniform(-1.5, 1.5) for _ in range(3)] + [rng.uniform(-500, 500)]
        u[3 + r] = [rng.uniform(-1.5, 1.5) for _ in range(3)] + [0.0]
    if dark and rng.random() < 0.5:
        u[3] = u[4] = u[5] = [0.0] * 4  # normalsUnused
    base = 19 + 4 * lights
    if rng.random() < 0.3 and base + 6 <= 81:
        for r in range(6):
            u[base + r] = [rng.uniform(-1.5, 1.5) for _ in range(3)] + [rng.uniform(-500, 500) if r < 3 else 0.0]
        matrices.append((base, base + 3))
    u[6] = [rng.uniform(0.8, 1.5), 0, rng.uniform(-0.1, 0.1), 0]
    u[7] = [0, rng.uniform(0.8, 1.5), rng.uniform(-0.1, 0.1), 0]
    u[8] = [0, 0, rng.uniform(-0.6, -0.4), rng.uniform(-20, -5)]
    u[9] = [0, 0, -1, 0]
    u[10] = [1.0, 1.0, 1.0, 0.0] if dark and rng.random() < 0.5 else [rng.randint(0, 255) / 255 for _ in range(3)] + [0.0]
    u[10][3] = 1.0 if rng.random() < 0.15 else 0.0
    fog = family[1] if family else rng.choice((0, 0, 1, 1, 2))
    u[11] = [rng.uniform(1, 400), rng.uniform(1.01, 3), rng.uniform(-1, 1), rng.choice((1 / 16384, 1 / 4096, 1.0))]
    u[12] = [float(fog), float(fog == 1 and rng.random() < 0.3), float(rng.choice((0, 0, 1, 2))), 0.0]
    if family:
        u[12][1], u[12][2] = float(family[2]), float(family[3])
    if u[12][1]:
        u[13][:2] = [rng.uniform(-1.5, -0.5), rng.uniform(-0.2, 0.2)]
        for i in range(5):
            u[14 + i][:3] = [rng.uniform(1, 1.4) for _ in range(3)]
    for n in range(lights):
        b = 19 + 4 * n
        u[b] = [rng.uniform(-900, 900) for _ in range(3)] + [rng.uniform(0, 1)]
        u[b + 1] = [0.0, 0.0, 0.0, rng.uniform(0, 1)] if dark else [rng.uniform(0, 1) for _ in range(3)] + [1.0]
        u[b + 2] = [rng.uniform(-1, 1) for _ in range(3)] + [rng.uniform(-1, 1)]
        u[b + 3] = [rng.uniform(0.5, 2), rng.uniform(0, 0.01), rng.uniform(0, 0.0001), rng.uniform(-1, 1)]
    roles = [rng.choice((0, 1, 1, 1, -1, 0.5)) for _ in range(3)]
    if rng.random() < 0.15:
        roles[0] = 2.0
    if family:
        roles = [float(r) for r in family[0]]
    u[13][2], u[13][3], u[18][3] = roles
    u[14][3] = rng.choice((0.0, 1.0))
    u[15][3] = rng.choice((-1.0, 1.0, rng.uniform(0, 1)))
    u[16][3] = 1 / 255
    u[17][3] = rng.choice((0.0, 0.0, 1.0)) if not family or rng.random() < 0.1 else 0.0
    u[12][3] = 1.0 if roles.count(-1) and not dark and rng.random() < 0.3 else 0.0
    if family and rng.random() < 0.9:
        u[10][3] = 0.0
    for unit, role in enumerate(roles):
        b = 89 - 4 * unit
        if role == 2:
            u[b] = [rng.uniform(-0.01, 0.01) for _ in range(3)] + [0.5]
            u[b + 1] = [rng.uniform(-0.01, 0.01) for _ in range(3)] + [0.5]
            u[b + 2] = [0.0, 0.0, -0.01, rng.uniform(1, 3)]
            u[b + 3] = [rng.uniform(0.5, 1), rng.uniform(0.5, 1), rng.choice((0.0, 1.0)), rng.choice((0.0, 1 / 64))]
            if family:
                u[b + 3][2] = 0.0
    u[93] = [rng.uniform(0, 1) for _ in range(4)]
    stride = 36 if rng.random() < 0.1 else 32
    key = FOG_ONLY if fog and rng.random() < 0.05 else 0
    raw = record_bytes(rng, stride, matrices)
    if rng.random() < 0.1:
        # A light exactly at the first vertex (d^2 = 0: the loop skips it).
        pos = struct.unpack_from('<3f', raw, 0)
        row = struct.unpack_from('<2b', raw, 30 if stride == 36 else 26)[0]
        m = [[pica.f32(x) for x in r] for r in u[row:row + 3]]
        u[19][:3] = [pica.f32(pica.f32(pica.f32(pica.f32(m[r][0] * pos[0]) + pica.f32(m[r][1] * pos[1])) +
                      pica.f32(m[r][2] * pos[2])) + m[r][3]) for r in range(3)]
    return dict(key=key, lights=lights, stride=stride, rows=u, raw=raw)


def run_one(shbin, program, plan, vertex):
    lights = plan['lights']
    bools = 1 if plan['key'] & FOG_ONLY else 0
    return pica.run(shbin, program, vertex, plan['rows'] + [[0.0] * 4, [0.0] * 4],
                    ints=((lights - 1, 0, 4, 0), (0, 0, 0, 0), (0, 0, 0, 0), (0, 0, 0, 0)), bools=bools)


def same_bits(a, b):
    if set(a) != set(b):
        return False
    return all(pica.bits(x) == pica.bits(y) for r in a for x, y in zip(a[r], b[r]))


def compare(shbin, plans, choices, label, totals, seen=None):
    failures = 0
    for plan, (recorded, key, variant) in zip(plans, choices):
        if seen is not None and variant and len(seen.setdefault(variant, [])) < 40:
            seen[variant].append(plan)
        if label == 'recorded' and recorded != key:
            print('%s: the host key %08x differs from the recorded key %08x' % (label, key, recorded))
            failures += 1
            continue
        t = totals.setdefault(variant, [0, 0, 0])
        for vertex in plan['vertices']:
            general = run_one(shbin, shbin.programs[0], plan, vertex)
            chosen = run_one(shbin, shbin.programs[variant], plan, vertex) if variant else general
            t[0] += 1; t[1] += general.executed; t[2] += chosen.executed
            if not same_bits(general.outputs, chosen.outputs):
                failures += 1
                if failures <= 5:
                    print('%s: key %08x variant %d: outputs differ\n  general %s\n  variant %s' %
                          (label, key, variant, general.outputs, chosen.outputs))
    return failures


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument('--plans', type=int, default=1500, help='plans of the random sweep')
    parser.add_argument('--seed', type=int, default=1120)
    parser.add_argument('recorded', nargs='*')
    args = parser.parse_args(argv)
    work = Path(tempfile.mkdtemp(prefix='vertex-variants-'))
    try:
        shbin = assemble(work)
        helper = build_helper(work)
        table = subprocess.run([str(helper), '--table'], check=True, capture_output=True, text=True).stdout.splitlines()
        if len(table) != len(shbin.programs) - 1:
            raise SystemExit('the runtime table names %d variants, the binary holds %d' % (len(table), len(shbin.programs) - 1))
        names = {0: 'general'}
        for line in table:
            index, _mask, _value, name = line.split(None, 3)
            names[int(index)] = name
        print('binary: %d programs, %d code words of 512, %d operand descriptors of 128' %
              (len(shbin.programs), len(shbin.code), len(shbin.descs)))
        rng = random.Random(args.seed)
        sweep = [sweep_plan(rng) for _ in range(args.plans)]
        for p in sweep:
            p['vertices'] = decode(p['raw'], p['stride'], len(p['raw']) // p['stride'])
        write_plans(work / 'sweep.bin', sweep)
        totals = {}
        seen = {}
        failures = compare(shbin, sweep, choose(helper, work / 'sweep.bin'), 'sweep', totals, seen)
        # The comparison must be able to fail: each variant's plans drawn by
        # the next variant differ somewhere.
        for variant, plans in seen.items():
            other = shbin.programs[variant % (len(shbin.programs) - 1) + 1]
            if all(same_bits(run_one(shbin, shbin.programs[0], p, v).outputs, run_one(shbin, other, p, v).outputs)
                   for p in plans for v in p['vertices']):
                print('self-check: %s on the plans of %s gave no difference' % (other.index, names[variant]))
                failures += 1
        recorded_vertices = 0
        for path in args.recorded:
            plans = read_plans(path)
            before = sum(t[0] for t in totals.values())
            failures += compare(shbin, plans, choose(helper, path), 'recorded', totals)
            recorded_vertices += sum(t[0] for t in totals.values()) - before
        print('%-34s %8s %10s %10s' % ('program', 'vertices', 'general', 'chosen'))
        for variant in sorted(totals):
            n, g, c = totals[variant]
            print('%-34s %8d %10.1f %10.1f' % (names.get(variant, '?'), n, g / n, c / n))
        missing = [names[v] for v in names if v and v not in totals]
        if missing:
            print('no plan reached: ' + ', '.join(missing))
            failures += len(missing)
        print('recorded vertices compared: %d' % recorded_vertices)
        print('FAILED: %d' % failures if failures else 'all outputs identical')
        return 1 if failures else 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
