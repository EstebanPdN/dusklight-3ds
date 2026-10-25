#!/usr/bin/env python3
"""Convert native DSKCAP01 framebuffer captures to PNG in LCD orientation."""
import argparse
from pathlib import Path
import struct
import zlib


def read_capture(path):
    data = path.read_bytes()
    magic, version, width, height = struct.unpack_from('<4I', data)
    if (magic, version) != (0x43534b44, 0x31305041) or len(data) != 16 + width * height * 4:
        raise ValueError(f'{path} is not a complete DSKCAP01 capture')
    return width, height, memoryview(data)[16:]


def to_lcd_rgb(width, height, pixels):
    """GPU rows are LCD columns from left to right; each row runs bottom to top."""
    out = bytearray(width * height * 3)
    for column in range(height):
        row = pixels[column * width * 4:(column + 1) * width * 4]
        for i in range(width):
            a, b, g, r = row[i * 4:i * 4 + 4]
            y = width - 1 - i
            at = (y * height + column) * 3
            out[at:at + 3] = bytes((r, g, b))
    return height, width, bytes(out)


def write_png(path, width, height, rgb):
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    raw = b''.join(b'\0' + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def convert(source, target):
    width, height, rgb = to_lcd_rgb(*read_capture(source))
    write_png(target, width, height, rgb)
    return width, height


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('captures', type=Path, nargs='+')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for source in args.captures:
        target = args.output / (source.stem + '.png')
        print(target, *convert(source, target))


if __name__ == '__main__':
    main()
