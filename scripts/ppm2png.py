#!/usr/bin/env python3
"""Convert binary PPM (P6) to PNG. Stdlib only - no PIL in the dev container."""
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, 'rb') as handle:
        data = handle.read()
    fields = []
    index = 0
    while len(fields) < 4:
        while index < len(data) and data[index:index + 1].isspace():
            index += 1
        if data[index:index + 1] == b'#':
            while index < len(data) and data[index:index + 1] != b'\n':
                index += 1
            continue
        start = index
        while index < len(data) and not data[index:index + 1].isspace():
            index += 1
        fields.append(data[start:index])
    index += 1
    width, height = int(fields[1]), int(fields[2])
    return width, height, data[index:index + width * height * 3]


def write_png(path, width, height, rgb):
    raw = b''.join(b'\x00' + rgb[y * width * 3:(y + 1) * width * 3]
                   for y in range(height))

    def chunk(tag, payload):
        return (struct.pack('>I', len(payload)) + tag + payload +
                struct.pack('>I', zlib.crc32(tag + payload) & 0xffffffff))

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6))
    png += chunk(b'IEND', b'')
    with open(path, 'wb') as handle:
        handle.write(png)


def main(source, target):
    width, height, rgb = read_ppm(source)
    write_png(target, width, height, rgb)
    total = width * height
    mean = sum(rgb[0::3]) / total
    black = sum(1 for i in range(0, len(rgb), 3)
                if rgb[i] < 8 and rgb[i + 1] < 8 and rgb[i + 2] < 8)
    print('%s -> %s %dx%d mean_R=%.1f black_pixels=%.1f%%'
          % (source, target, width, height, mean, 100.0 * black / total))
    # Rough brightness of horizontal bands, to locate a black video region.
    for band in range(8):
        y0, y1 = band * height // 8, (band + 1) * height // 8
        chunk_sum = sum(rgb[(y0 * width) * 3:(y1 * width) * 3:3])
        print('  band %d (y %4d-%4d) mean_R=%.1f'
              % (band, y0, y1, chunk_sum / max(1, (y1 - y0) * width)))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
