"""Instructions per vertex of the rigid vertex programs, path by path.

Builds the uniform rows the plans give (runtime/gx_rigid_vertex.hpp,
buildRigidMultiVertex and finishCompactRigidUniforms) for each kind of plan,
runs the assembled program on the host (pica_vertex_interp.py) and prints the
instructions one vertex executes. With a census of the draws (the log lines
"Native vertex plans", gx_pica.cpp) it gives the instructions per picture.

usage: python3 -I vertex_variant_cost.py <gx_rigid_mt.shbin> [census log ...]
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pica_vertex_interp as pica  # noqa: E402


def plan_rows(lights='none', vertex_color=False, vertex_alpha=False, ambient_vertex=False,
              roles=(1, 0, 0), projected_mode=0, warp=0.0, fog=0, fog_range=False, fog_carrier=0,
              depth_clamp=False, paired=False):
    """Uniform rows 0..93 of a multi-texture plan of the given kind (one matrix)."""
    u = [[0.0] * 4 for _ in range(96)]
    # Position and normal matrix of the single bone (rows 0-2, 3-5).
    u[0] = [1, 0, 0, 10]; u[1] = [0, 1, 0, -5]; u[2] = [0, 0, 1, -300]
    u[3] = [1, 0, 0, 0]; u[4] = [0, 1, 0, 0]; u[5] = [0, 0, 1, 0]
    if lights in ('none', 'ambient'):
        u[3] = u[4] = u[5] = [0.0] * 4  # normalsUnused: the normal rows may be absent
    # Perspective projection (camera space -> clip space, GX depth).
    u[6] = [1.2, 0, 0, 0]; u[7] = [0, 1.6, 0, 0]; u[8] = [0, 0, -0.5, -10]; u[9] = [0, 0, -1, 0]
    u[10] = [1, 1, 1, 0] if lights == 'none' else [0.25, 0.2, 0.3, 0]
    if depth_clamp:
        u[10][3] = 1
    u[11] = [0.0, 0.0, 0.0, 1 / 16384]
    u[12] = [float(fog), float(fog_range), float(fog_carrier), 1.0 if paired else 0.0]
    if fog == 1:
        u[11][:3] = [3.0, 1.5, 0.1]
    elif fog == 2:
        u[11][:3] = [1.0, 1.5, -1.0]
    if fog_range:
        u[13][:2] = [-1.0, 0.0]
        for i in range(5):
            u[14 + i][:3] = [1.0 + 0.1 * i, 1.05 + 0.1 * i, 1.1 + 0.1 * i]
    n = 19
    count = 0
    if lights in ('none', 'ambient'):
        u[n] = [0, 0, 0, 0]; u[n + 1] = [0, 0, 0, 0]; u[n + 2] = [0, 0, 0, 0]; u[n + 3] = [1, 0, 0, 0]
        count = 1
    else:
        for i in range(lights):
            b = 19 + 4 * i
            u[b] = [100.0 * (i + 1), 50.0, -200.0, 1.0]
            u[b + 1] = [0.8, 0.7, 0.6, 1.0]
            u[b + 2] = [0.0, 0.0, 1.0, 0.0]
            u[b + 3] = [1.0, 0.001, 0.00001, 0.0]
            count += 1
    u[13][2], u[13][3], u[18][3] = roles
    u[14][3] = 1.0 if vertex_color else 0.0
    u[15][3] = 1.0 if vertex_alpha else -1.0
    u[16][3] = 1 / 255
    u[17][3] = 1.0 if ambient_vertex and lights not in ('none',) else 0.0
    for unit, role in enumerate(roles):
        base = 89 - 4 * unit
        if role == 1:
            u[base] = [0, 0, 1 / 1024, 0]; u[base + 1] = [1 / 1024, 0, 0, 0]
            u[base + 2] = [0, 0, 0, 1]; u[base + 3] = [0, -1 / 1024, 0, 0]
        elif role == -1:
            u[base] = [0.5, 0, 0, 0.5]; u[base + 2] = [0, -0.5, 0, 0.5]
        elif role == 2:
            u[base] = [0.01, 0, 0.002, 0.5]; u[base + 1] = [0, 0.01, 0.001, 0.5]; u[base + 2] = [0, 0, -0.01, 3]
            u[base + 3] = [1.0, 1.0, float(projected_mode), warp]
    u[93] = [0.9, 0.8, 0.7, 1.0]
    return u, count


def vertex(normal=(0, 16384, 0)):
    return [[1.5, 2.0, 3.0, 1.0], [float(normal[0]), float(normal[1]), float(normal[2]), 1.0],
            [512.0, 256.0, 0.0, 1.0], [200.0, 100.0, 50.0, 255.0], [0.0, 3.0, 0.0, 1.0],
            [128.0, 64.0, 0.0, 1.0]] + [[0.0, 0.0, 0.0, 1.0]] * 10


def count(shbin, program, rows, lights, b0=False):
    result = pica.run(shbin, program, vertex(), rows, ints=((lights - 1, 0, 4, 0), (0,) * 4, (0,) * 4, (0,) * 4),
                      bools=1 if b0 else 0)
    return result.executed


CASES = [
    ('unlit, 1 texture', dict(lights='none')),
    ('unlit, 1 texture, vertex colour', dict(lights='none', vertex_color=True, vertex_alpha=True)),
    ('unlit, 2 textures', dict(lights='none', roles=(1, 1, 0))),
    ('unlit, 3 textures', dict(lights='none', roles=(1, 1, 1))),
    ('unlit, no texture', dict(lights='none', roles=(0, 0, 0))),
    ('ambient only, 1 texture', dict(lights='ambient')),
    ('lit 1 light, 1 texture', dict(lights=1)),
    ('lit 2 lights, 1 texture', dict(lights=2)),
    ('lit 3 lights, 1 texture', dict(lights=3)),
    ('lit 4 lights, 1 texture', dict(lights=4)),
    ('lit 2 lights, 2 textures', dict(lights=2, roles=(1, 1, 0))),
    ('lit 2 lights, env map (role -1)', dict(lights=2, roles=(1, -1, 0))),
    ('lit 2 lights, 1 texture, linear fog (ramp unit)', dict(lights=2, roles=(1, 0.5, 0), fog=1)),
    ('lit 2 lights, 1 texture, linear fog in alpha', dict(lights=2, fog=1, fog_carrier=1)),
    ('lit 2 lights, 1 texture, range fog (ramp unit)', dict(lights=2, roles=(1, 0.5, 0), fog=1, fog_range=True)),
    ('lit 2 lights, 1 texture, constant fog ramp', dict(lights=2, roles=(1, 0.5, 0), fog=2)),
    ('unlit, 1 texture, linear fog (ramp unit)', dict(lights='none', roles=(1, 0.5, 0), fog=1)),
    ('unlit, projected unit 0 (divide here)', dict(lights='none', roles=(2, 1, 0))),
    ('unlit, projected unit 0 (PICA divides)', dict(lights='none', roles=(2, 1, 0), projected_mode=1)),
    ('unlit, depth clamp (sky)', dict(lights='none', depth_clamp=True)),
]


def main(argv):
    shbin = pica.Shbin(argv[1])
    general = shbin.programs[0]
    print('Instructions per vertex, %s program %d (%d words):' % (os.path.basename(argv[1]), general.index, len(shbin.code)))
    for label, kwargs in CASES:
        rows, lights = plan_rows(**kwargs)
        print('  %-50s %4d' % (label, count(shbin, general, rows, lights)))
    rows, lights = plan_rows(lights=2)
    print('  %-50s %4d' % ('fog-only redraw (b0)', count(shbin, general, rows, lights, b0=True)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
