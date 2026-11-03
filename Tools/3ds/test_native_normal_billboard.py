#!/usr/bin/env python3
"""SHP billboard admission preserves source streams and ordered matrix groups.

The native parameter harness separately checks the original billboard matrix
helpers and normal texgen recipes. This check does not claim GPU equivalence.
"""
import struct
import j3d_native_geometry as geometry
from test_j3d_native_geometry import model


def billboard_source(mode):
    source = bytearray(model(shape_mode=mode))
    at = 32
    while source[at:at + 4] != b'SHP1':
        at += geometry.u32(source, at + 4)
    shape = memoryview(source)[at:at + geometry.u32(source, at + 4)]
    arrays = geometry.Arrays(geometry.sections(source)['VTX1'])
    init = geometry.u32(shape, 12)
    desc = geometry.descriptor(shape, geometry.u32(shape, 24) + geometry.u16(shape, init + 4), arrays)
    stride = sum(1 if attr == 0 else arrays.layout(attr) if kind == 1 else kind - 1 for attr, kind in desc)
    for group in range(2):
        # Single-matrix shapes use the authored useMtxIndex, not the multi table.
        struct.pack_into('>H', shape, geometry.u32(shape, 36) + group * 8, group + 1)
        draw = geometry.u32(shape, 40) + group * 8
        packet = geometry.u32(shape, 32) + geometry.u32(shape, draw + 4)
        for vertex in range(geometry.u16(shape, packet + 1)):
            shape[packet + 3 + vertex * stride] = 0
    return source


def main():
    baseline, _, metadata = geometry.convert(billboard_source(0))
    assert [entry['draw_matrix_palette'] for entry in metadata['meshes']] == [[1], [2]]
    for mode in (1, 2):
        document, _, manifest = geometry.convert(billboard_source(mode))
        assert document == baseline
        assert [(entry['source_shape'], entry['source_group'], entry['source_mode'], entry['draw_matrix_palette'])
                for entry in manifest['meshes']] == [(0, 0, mode, [1]), (0, 1, mode, [2])]
        assert [entry['source_attributes'] for entry in manifest['meshes']] == [[0, 9, 10, 11, 13, 14]] * 2
    try:
        geometry.convert(billboard_source(4))
    except geometry.Unsupported:
        pass
    else:
        raise AssertionError('Unknown billboard mode admitted')
    print('PASS source billboard modes1/2 preserve geometry, descriptors and matrix-group order (offline only)')


if __name__ == '__main__':
    main()
