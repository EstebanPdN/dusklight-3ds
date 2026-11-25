#!/usr/bin/env python3
"""Multi-texture GPU texgen (gx_rigid_mt.v.pica) against the CPU vertex path.

Builds Source/platforms/3ds/tests/gx_rigid_texgen_test.cpp, which plans real
register banks with planRigidMultiVertex/finishCompactRigidUniforms and prints
the uniform block, compact attributes and the CPU route's final texture
coordinates. This script runs the shader source on them with host float32
arithmetic and compares every used unit (roles 1 and -1: TEX0, TEX1 and the
normal). Shader mutations must be detected. When picasso is installed the
shader is also assembled and passed through check_pica_shader.py (every
output component written exactly once on every path).

Also builds Source/platforms/3ds/tests/native_replay_plan_test.cpp: the lean
native shape replay planner (reused plan storage, cached projection, texgen
check without a transform plan, continuations from kept unit rows) against
the complete replay over random register banks, and the in-place
planRasterColor against its previous form.

Host floats only: PICA float24 rounding, rasterization and texture sampling
are Azahar/console checks.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT/'Source/platforms/3ds'
SHADER = NATIVE/'gx_rigid_mt.v.pica'
AXIS = {c: i for i, c in enumerate('xyzw')}
AXIS.update(s=0, t=1, p=2, q=3)


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def parse(source):
    """Shader body as a block tree: ('op', text) / ('if', cond, then, else) / ('for', body)."""
    consts = {name: [float(x) for x in values.split(',')]
              for name, values in re.findall(r'\.constf (\w+)\(([^)]+)\)', source)}
    outputs = {}
    for name, semantic in re.findall(r'^\.out (\w+) (\w+)', source, re.M):
        outputs[name] = semantic
    body = source.split('.proc main', 1)[1]
    lines = [x.split(';', 1)[0].strip() for x in body.splitlines()]
    lines = [x for x in lines if x]
    pos = 0

    def block(stop):
        nonlocal pos
        items = []
        while pos < len(lines):
            line = lines[pos]; pos += 1
            if line in stop:
                return items, line
            if line.startswith('ifc ') or line.startswith('ifu '):
                then, ended = block(('.else', '.end'))
                other = []
                if ended == '.else':
                    other, _ = block(('.end',))
                items.append(('if', line[4:].strip(), then, other))
            elif line.startswith('for '):
                loop, _ = block(('.end',))
                items.append(('for', loop))
            else:
                items.append(('op', line))
        return items, None
    tree, _ = block(('.end',))
    return consts, outputs, tree


class Machine:
    def __init__(self, consts, uniforms, vertex, light_count, bools=None):
        self.consts = consts; self.u = uniforms; self.light_count = light_count
        # Boolean uniforms (ifu bN): what the caller sets for this draw. The
        # scene draws leave them false; gx_pica.cpp sets b0 only for the rigid
        # fog redraw (submitGxRigidFogGeometry).
        self.b = list(bools or [False]*16)
        self.r = {f'r{i}': [0.]*4 for i in range(16)}
        self.v = {f'v{i}': list(map(float, vertex[i])) for i in range(6)}
        self.out = {}; self.writes = {}
        self.a0 = [0, 0]; self.aL = 0; self.cmp = [False, False]

    def read(self, token):
        negative = token.startswith('-')
        token = token.lstrip('-')
        m = re.fullmatch(r'(u\[([^\]]+)\]|\w+)(?:\.([xyzwstpq]+))?', token)
        name, index, swizzle = m.groups()
        if index is not None:
            if index.startswith('a0.x+'): at = self.a0[0]+int(index[5:])
            elif index.startswith('a0.y+'): at = self.a0[1]+int(index[5:])
            elif index.startswith('aL+'): at = self.aL+int(index[3:])
            else: at = int(index)
            assert 0 <= at < len(self.u), (token, at)
            values = self.u[at]
        elif name in self.r: values = self.r[name]
        elif name in self.v: values = self.v[name]
        else: values = self.consts[name]
        swizzle = swizzle or 'xyzw'
        swizzle = swizzle+swizzle[-1]*(4-len(swizzle))
        out = [values[AXIS[c]] for c in swizzle]
        return [-x for x in out] if negative else out

    def store(self, token, value, outputs):
        m = re.fullmatch(r'(\w+)(?:\.([xyzwstpq]+))?', token)
        name, mask = m.groups()
        mask = mask or 'xyzw'
        if name in outputs:
            target = self.out.setdefault(name, [0.]*4)
            for c in mask:
                key = (name, AXIS[c]); self.writes[key] = self.writes.get(key, 0)+1
        else:
            target = self.r[name]
        for c in mask:
            target[AXIS[c]] = f32(value[AXIS[c]])

    def condition(self, text):
        m = re.fullmatch(r'(!?)b(\d+)', text)
        if m:
            value = bool(self.b[int(m[2])])
            return not value if m[1] else value
        m = re.fullmatch(r'(!?)cmp\.([xy])', text)
        assert m, text
        value = self.cmp[0 if m[2] == 'x' else 1]
        return not value if m[1] else value

    def run(self, items, outputs):
        for item in items:
            if item[0] == 'if':
                self.run(item[2] if self.condition(item[1]) else item[3], outputs)
                continue
            if item[0] == 'for':
                for n in range(self.light_count):
                    self.aL = 4*n; self.run(item[1], outputs)
                self.aL = 0
                continue
            line = item[1]
            if line == 'end':
                return True
            op, rest = line.split(' ', 1)
            args = [x.strip() for x in rest.split(',')]
            if op == 'cmp':
                a, b = self.read(args[0]), self.read(args[3])
                tests = {'eq': lambda x, y: x == y, 'ne': lambda x, y: x != y, 'lt': lambda x, y: x < y,
                         'le': lambda x, y: x <= y, 'gt': lambda x, y: x > y, 'ge': lambda x, y: x >= y}
                self.cmp = [tests[args[1]](a[0], b[0]), tests[args[2]](a[1], b[1])]
                continue
            if op == 'mova':
                value = self.read(args[1])
                mask = args[0].split('.', 1)[1]
                for c in mask: self.a0[AXIS[c]] = int(value[AXIS[c]])
                continue
            a = self.read(args[1]); b = self.read(args[2]) if len(args) > 2 else None
            if op == 'mov': value = a
            elif op == 'add': value = [f32(x+y) for x, y in zip(a, b)]
            elif op == 'mul': value = [f32(x*y) for x, y in zip(a, b)]
            elif op in ('dp3', 'dp4'):
                total = 0.
                for i in range(int(op[-1])): total = f32(total+f32(a[i]*b[i]))
                value = [total]*4
            elif op == 'max': value = [max(x, y) for x, y in zip(a, b)]
            elif op == 'min': value = [min(x, y) for x, y in zip(a, b)]
            elif op == 'rcp': value = [f32(1/a[0])]*4
            elif op == 'rsq': value = [f32(1/math.sqrt(a[0]))]*4
            elif op == 'flr': value = [float(math.floor(x)) for x in a]
            else: raise AssertionError('unexpected instruction '+line)
            self.store(args[0], value, outputs)
        return False


def execute(source, case, vertex):
    consts, outputs, tree = parse(source)
    machine = Machine(consts, case['uniforms'], [vertex[f'v{i}'] for i in range(6)], case['lights'], case.get('bools'))
    assert machine.run(tree, outputs)
    # The console rule the build also checks on the binary: each output
    # component the program declares is written exactly once.
    for name in outputs:
        for c in range(4):
            assert machine.writes.get((name, c), 0) == 1, ('output written', name, c, machine.writes.get((name, c), 0))
    return machine.out


def compare(source, data):
    units = 0
    for case in data['cases']:
        for vertex in case['vertices']:
            out = execute(source, case, vertex)
            for n, want in enumerate(vertex['clip']):
                assert abs(out['outpos'][n]-want) <= 2e-4*max(1, abs(want)), (case['name'], 'clip', n, out['outpos'][n], want)
            for u, want in enumerate(vertex['expected']):
                got = out[['outtex', 'outtex1', 'outtex2'][u]]
                for c in range(2):
                    assert abs(got[c]-want[c]) <= 2e-5*max(1, abs(want[c])), (case['name'], 'unit', u, c, got[c], want[c])
                units += 1
    return units


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    out = parser.parse_args().output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    devkitpro = Path(os.environ.get('DEVKITPRO') or
                     next((p for p in (Path.home()/'devkitpro', Path('/opt/devkitpro')) if p.is_dir()), '/opt/devkitpro'))
    aurora = ROOT/'Source/extern/aurora/include'
    sources = [NATIVE/'tests/gx_rigid_texgen_test.cpp', NATIVE/'runtime/gx_vertex_decode.cpp', NATIVE/'runtime/gx_rigid_decode.cpp',
               NATIVE/'runtime/gx_vertex_transform.cpp', NATIVE/'runtime/gx_pica_math.cpp']
    replay_sources = [NATIVE/'tests/native_replay_plan_test.cpp', NATIVE/'runtime/gx_vertex_decode.cpp', NATIVE/'runtime/gx_rigid_decode.cpp',
                      NATIVE/'runtime/gx_vertex_transform.cpp', NATIVE/'runtime/gx_pica_math.cpp', NATIVE/'runtime/gx_raster_color.cpp']
    inputs = [*sources, *replay_sources[:1], replay_sources[-1], NATIVE/'runtime/gx_rigid_vertex.hpp', NATIVE/'runtime/gx_vertex_transform.hpp',
              NATIVE/'runtime/gx_raster_color.hpp', SHADER, Path(__file__).resolve()]
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    common = ['-std=c++20', '-DTARGET_PC=1', '-DDUSK_TARGET_3DS=1', '-I'+str(aurora), '-Wall', '-Wextra', '-Werror']
    receipt = {'status': 'running', 'input_sha256': hashes,
               'limits': 'Host float32 shader algebra against the CPU route; no PICA float24, raster or sampling.'}
    try:
        build = ['clang++', *common, '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                 *map(str, sources), '-o', str(out/'test')]
        log = subprocess.run(build, capture_output=True, text=True, timeout=180)
        (out/'build.log').write_text(log.stdout+log.stderr); log.check_returncode()
        run = subprocess.run([str(out/'test')], capture_output=True, text=True, timeout=120,
                             env={**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1',
                                  'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})
        (out/'cases.json').write_text(run.stdout); (out/'test.log').write_text(run.stderr)
        if run.returncode: raise AssertionError('case generator failed: '+run.stderr[-2000:])
        data = json.loads(run.stdout)
        replay_build = ['clang++', *common, '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        *map(str, replay_sources), '-o', str(out/'replay-plan')]
        log = subprocess.run(replay_build, capture_output=True, text=True, timeout=180)
        (out/'replay-plan-build.log').write_text(log.stdout+log.stderr); log.check_returncode()
        replay = subprocess.run([str(out/'replay-plan')], capture_output=True, text=True, timeout=300,
                                env={**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1',
                                     'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})
        (out/'replay-plan.log').write_text(replay.stdout+replay.stderr)
        if replay.returncode: raise AssertionError('lean replay planner parity failed: '+replay.stderr[-2000:])
        replay_parity = [line for line in replay.stderr.splitlines() if line.startswith('PASS')]
        if not replay_parity: raise AssertionError('lean replay planner parity printed no result')
        source = SHADER.read_text()
        units = compare(source, data)
        detected = []
        for name, old, new in [('normal-input', 'dp4 r6.x, u[85], r14', 'dp4 r6.x, u[85], r12'),
                               ('normal-role-compare', 'cmp u[13].w, eq, lt, r8', 'cmp u[13].w, eq, gt, r8'),
                               ('normal-t-row', 'dp4 r5.y, u[91], r14', 'dp4 r5.y, u[92], r14'),
                               ('normal-unit2-input', 'dp4 r7.y, u[83], r14', 'dp4 r7.y, u[83], r13'),
                               ('role1-row', 'dp4 r4.y, u[86], r13', 'dp4 r4.y, u[86], r12')]:
            assert source.count(old) == 1, name
            try:
                compare(source.replace(old, new), data)
            except AssertionError:
                detected.append(name)
            else:
                raise AssertionError('undetected shader mutation '+name)
        assembled = 'picasso not found'
        picasso = devkitpro/'tools/bin/picasso'
        if picasso.exists():
            shbin = out/'gx_rigid_mt.shbin'
            subprocess.run([str(picasso), '-o', str(shbin), str(SHADER)], check=True, capture_output=True, timeout=60)
            check = subprocess.run(['python3', '-I', str(ROOT/'Tools/3ds/check_pica_shader.py'), str(shbin)],
                                   capture_output=True, text=True, timeout=60)
            if check.returncode: raise AssertionError('check_pica_shader: '+check.stderr)
            data_bytes = shbin.read_bytes()
            assembled = {'instructions': struct.unpack_from('<I', data_bytes, 12+12)[0],
                         'sha256': hashlib.sha256(data_bytes).hexdigest(), 'output_rule': 'pass'}
        assert hashes == {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        receipt.update(status='pass', cases=[c['name'] for c in data['cases']],
                       vertices=sum(len(c['vertices']) for c in data['cases']), unit_coordinates_compared=units,
                       detected_mutants=detected, assembled=assembled, replay_parity=replay_parity)
    except (OSError, subprocess.SubprocessError, AssertionError, ValueError) as error:
        receipt.update(status='fail', error=str(error))
    (out/'results.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print(json.dumps(receipt, indent=2))
    return int(receipt['status'] != 'pass')


if __name__ == '__main__':
    raise SystemExit(main())
