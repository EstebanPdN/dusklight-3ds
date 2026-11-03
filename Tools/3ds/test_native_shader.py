#!/usr/bin/env python3
"""Check the assembled native ABI64 shader contract; never interpret shader source.

Lighting, fog, interpolation and sampler correctness require the parent-owned
GPU scenario. This receipt intentionally makes no algebra or pixel claim.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def binary_abi(path):
    data = path.read_bytes()
    assert data[:4] == b'DVLB'
    count, offset = struct.unpack_from('<II', data, 4)
    assert count == 1 and data[offset:offset + 4] == b'DVLE'
    dvlp = 12
    assert data[dvlp:dvlp + 4] == b'DVLP'
    instructions = struct.unpack_from('<I', data, dvlp + 12)[0]
    assert 0 < instructions <= 512, instructions
    input_mask = struct.unpack_from('<H', data, offset + 16)[0]
    assert input_mask == 0x1ff, input_mask
    uniform_offset, uniform_count, symbol_offset = struct.unpack_from('<III', data, offset + 48)
    uniforms = {}
    for n in range(uniform_count):
        symbol, start, end = struct.unpack_from('<IHH', data, offset + uniform_offset + 8 * n)
        at = offset + symbol_offset + symbol
        name = data[at:data.index(b'\0', at)].decode()
        uniforms[name] = (start, end)
    # DVLE float register IDs are biased by 0x10; libctru removes this bias.
    assert uniforms['u'] == (0x10, 0x10 + 89), uniforms
    assert uniforms['lightLoop'] == (0x70, 0x70), uniforms
    constant_offset, constant_count = struct.unpack_from('<II', data, offset + 24)
    constants = [struct.unpack_from('<HH4I', data, offset + constant_offset + 20 * n)
                 for n in range(constant_count)]
    registers = {(x[0], x[1]) for x in constants}
    assert len(registers) == len(constants), constants
    assert all(kind == 2 and 90 <= register < 96 for kind, register in registers), constants
    output_offset, output_count = struct.unpack_from('<II', data, offset + 40)
    outputs = [struct.unpack_from('<HHB', data, offset + output_offset + 8 * n)
               for n in range(output_count)]
    required_outputs = {(0, 0): 15, (2, 1): 15, (3, 2): 3, (4, 2): 4, (5, 3): 3, (6, 4): 3}
    actual_outputs = {(kind, register): mask for kind, register, mask in outputs}
    assert all(actual_outputs.get(key, 0) & mask == mask for key, mask in required_outputs.items()), outputs
    return {'instructions': instructions, 'input_mask': input_mask, 'uniform_entries': uniforms,
            'constants': constants, 'outputs': outputs,
            'binary_sha256': hashlib.sha256(data).hexdigest()}


def write_receipt(output, binaries, filename='receipt.json'):
    abi = {name: binary_abi(path) for name, path in binaries.items()}
    first = next(iter(abi.values()))
    for row in abi.values():
        assert row['constants'] == first['constants'] and row['outputs'] == first['outputs']
    result = {'status': 'pass', 'assembled_abi': abi,
              'scope': 'Assembled ABI/resource bounds only. GPU lighting, original-vertex fog/range, '
                       'carrier interpolation, projective sampling and physical behavior remain unverified.'}
    output.mkdir(parents=True, exist_ok=True)
    (output / filename).write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--shader-binary', type=Path, required=True)
    parser.add_argument('--rigid-binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    write_receipt(args.output, {'weighted': args.shader_binary, 'rigid': args.rigid_binary}, 'shader-receipt.json')


if __name__ == '__main__':
    main()
