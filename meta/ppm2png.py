#!/usr/bin/env python3
"""Tiny binary-PPM (P6) -> PNG converter, no third-party deps.

Used by meta/run-qemu.sh to turn a QEMU `screendump` frame into a viewable PNG
when neither netpbm nor ImageMagick is installed.

Usage: ppm2png.py <in.ppm> <out.png>
"""
import sys
import zlib
import struct


def read_ppm(path):
    data = open(path, "rb").read()
    if data[:2] != b"P6":
        raise SystemExit("not a binary PPM (P6)")
    i = 2
    fields = []
    while len(fields) < 3:
        while data[i] in b" \t\n\r":
            i += 1
        if data[i:i + 1] == b"#":           # comment line
            while data[i] not in b"\n":
                i += 1
            continue
        j = i
        while data[j] not in b" \t\n\r":
            j += 1
        fields.append(int(data[i:j]))
        i = j
    width, height, _maxval = fields
    i += 1                                   # single whitespace after maxval
    return width, height, data[i:]


def write_png(path, width, height, rgb):
    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)                        # filter type 0 (None)
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xffffffff))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: ppm2png.py <in.ppm> <out.png>")
    w, h, rgb = read_ppm(sys.argv[1])
    write_png(sys.argv[2], w, h, rgb)


if __name__ == "__main__":
    main()
