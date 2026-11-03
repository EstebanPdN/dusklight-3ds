#!/usr/bin/env python3
"""Pure-Python ordered 65/384/1024-chunk source conversion and high-slot checks.

Run after integration: python3 Tools/3ds/test_native_large_model.py
No compiler, original game input, output files or emulator is required.
"""
import struct
import zlib

import j3d_native_geometry as geometry
from j3d_native_model import assemble
from native_mesh_pack import compile_pack
from native_model_plan import compile_plan, MAX_FILE
from test_j3d_native_geometry import append, finish, half, word
from test_native_model_plan import fixture, rebuild


def source_fixture(groups):
    blocks = geometry.sections(fixture())
    draw = bytearray(20)
    half(draw, 8, groups + 1)
    word(draw, 12, append(draw, 4, bytes(groups + 1)))
    word(draw, 16, append(draw, 2, struct.pack('>' + 'H' * (groups + 1),
                                            0, *(1 + n % 2 for n in range(groups)))))
    blocks['DRW1'] = finish(draw, b'DRW1')
    shape = bytearray(44)
    half(shape, 8, 1)
    init = bytearray(40)
    init[0] = 3
    half(init, 2, groups)
    word(shape, 12, append(shape, 4, init))
    word(shape, 16, append(shape, 2, bytes(2)))
    attributes = [(0, 1), (9, 3), (10, 2), (11, 2), (13, 3), (14, 1), (255, 0)]
    word(shape, 24, append(shape, 4, b''.join(struct.pack('>II', *a) for a in attributes)))
    word(shape, 28, append(shape, 2, b''.join(struct.pack('>HH', 0, n + 1) for n in range(groups))))
    display, draws = bytearray(), []
    for group in range(groups):
        start = len(display)
        display.extend(struct.pack('>BH', 0x98, 4))
        for i in range(4):
            display.extend(struct.pack('>BHBBH2f', 3 if i in (1, 2) else 0, i, 0, i, i,
                                       i + .25, group + .5))
        draws.append((len(display) - start, start))
    word(shape, 32, append(shape, 32, display))
    word(shape, 36, append(shape, 4, b''.join(struct.pack('>HHI', 0, 2, 2 * n) for n in range(groups))))
    word(shape, 40, append(shape, 4, b''.join(struct.pack('>II', *d) for d in draws)))
    blocks['SHP1'] = finish(shape, b'SHP1')
    return rebuild(list(blocks.values()))


def rejects(action, message):
    try:
        action()
    except ValueError as error:
        assert message in str(error), str(error)
    else:
        raise AssertionError('Out-of-bound native model admitted')


def check_source(groups):
    source = source_fixture(groups)
    document, meshes, receipt = geometry.convert(source)
    plan, packed, association = assemble(source, allow_normalized_arithmetic=True, allow_sdk_sampler_model=True)
    positions = [(-2., 0., 0.), (2., 0., 0.), (2., 2., 0.), (-2., 2., 0.)]
    uv = [(-.5, 0.), (1., 0.), (1., 1.25), (-.5, 1.25)]
    colors = [(255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255), (255, 255, 255, 255)]
    assert len(document['meshes']) == len(receipt['meshes']) == len(association['source_correspondence']) == groups
    for group, binding in enumerate(receipt['meshes']):
        assert binding['draw_matrix_palette'] == [0, group + 1]
        assert binding['source_shape'] == 0 and binding['source_group'] == group
        identifier, nv, ni, vo, io, vb, ib, crc = struct.unpack_from('<8I', meshes, 32 + group * 32)
        assert (identifier, nv, ni, vb, ib) == (group + 1, 4, 6, 256, 12)
        indices = struct.unpack_from('<6H', meshes, io)
        assert indices == (0, 1, 2, 2, 1, 3), 'Source strip winding/order or local index base changed'
        assert zlib.crc32(meshes[vo:vo + vb] + meshes[io:io + ib]) == crc
        expected = []
        for i in indices:
            expected.append(struct.pack('<12f16B', *positions[i], 0., 0., 1., *uv[i], i + .25, group + .5,
                                        0., 0., *colors[i], *([255] * 4),
                                        3 if i in (1, 2) else 0, 0, 0, 0, 255, 0, 0, 0))
        assert [meshes[vo + i * 64:vo + (i + 1) * 64] for i in indices] == expected, 'Lost/reordered source attributes'
        assert association['source_correspondence'][group] == {
            'part': group, 'source_joint': 0, 'source_material': 0, 'source_shape': 0, 'source_group': group}
        # Every group has a distinct source draw-matrix identity, even though
        # several draw matrices happen to refer to the same original joint.
        assert struct.unpack_from('<3I', packed, 48 + group * 64) == (group + 1, 1, group)
        uniform_at = 48 + groups * 64 + group * 80
        assert struct.unpack_from('<7I', packed, uniform_at) == (1, 1, 2, 1, group + 2, 0, 0)
    assert association['unused_native_meshes'] == []
    assert not association['rendering_ready']
    assert struct.unpack_from('<I', packed, 44)[0] == zlib.crc32(packed[:44] + packed[48:])
    if groups == 1024:
        assert len(packed) == MAX_FILE
        too_many_meshes = {**document, 'meshes': document['meshes'] + [{**document['meshes'][0], 'id': 1025}]}
        rejects(lambda: compile_pack(too_many_meshes), 'mesh count')
        rejects(lambda: compile_plan({**plan, 'parts': plan['parts'] + [plan['parts'][0]]}), 'counts')
        rejects(lambda: compile_plan({**plan, 'uniforms': plan['uniforms'] + [plan['uniforms'][0]]}), 'counts')


def main():
    for groups in (65, 384, 1024):
        check_source(groups)
    rejects(lambda: geometry.convert(source_fixture(1025)), 'more than1024mesh chunks')
    print('PASS ordered 65/384/1024-chunk source triangles/attributes, local indices, high uniform identities and overflow refusal')


if __name__ == '__main__':
    main()
