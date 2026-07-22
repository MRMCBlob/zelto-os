#!/usr/bin/env python3
"""Count ZCOMP_BG (#0a858c) pixels in a frame, and say where they are.

Teal in a captured frame is the compositor's clear colour showing through a
client surface with alpha 0 — an unpainted region (compositor/src/server.c
ZCOMP_BG). It is not a colour any Zelto surface paints, so any of it in a
finished frame means part of that frame is missing.

    meta/teal.py out/frame.png [out/frame-2.png ...]

Prints, per frame, the pixel count and the bounding box of the teal, so a run of
frames from one boot (run-qemu.sh FRAMES=N) can be read as evidence: teal only in
the first frame is a first-paint race that resolves; teal in a random subset of
frames taken seconds apart, of a screen that is not changing, is the capture
itself tearing.
"""
import sys
import zlib
import struct

TEAL = (0x0A, 0x85, 0x8C)
TOL = 6      # the renderer's rounding, not a colour range


def read_png(path):
    """Minimal PNG reader: 8-bit RGB/RGBA, non-interlaced. Returns (w, h, rows)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")
    pos, idat, w, h, depth, ctype = 8, b"", 0, 0, 0, 0
    while pos < len(data):
        (ln,) = struct.unpack(">I", data[pos:pos + 4])
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
        pos += 12 + ln
    if depth != 8 or ctype not in (2, 6):
        raise SystemExit(f"{path}: need 8-bit RGB/RGBA, got depth={depth} type={ctype}")
    nch = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * nch
    out, prev = [], bytearray(stride)
    p = 0
    for _ in range(h):
        ft = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - nch] if i >= nch else 0
            b = prev[i]
            c = prev[i - nch] if i >= nch else 0
            if ft == 1:
                line[i] = (line[i] + a) & 0xFF
            elif ft == 2:
                line[i] = (line[i] + b) & 0xFF
            elif ft == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif ft == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out.append(bytes(line))
        prev = line
    return w, h, out, nch


def read_ppm(path):
    """Binary P6, which is what QEMU's screendump writes. Preferred over the
    converted PNG: pnmtopng emits a PALETTE image for a frame with few colours,
    and a palette PNG is a different decode entirely — the first attempt at this
    read the .png and simply refused every frame."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"P6":
        raise SystemExit(f"{path}: not a binary PPM")
    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos] != 0x0A:
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1                       # the single whitespace after maxval
    w, h, maxv = fields
    if maxv != 255:
        raise SystemExit(f"{path}: maxval {maxv}, expected 255")
    stride = w * 3
    rows = [data[pos + y * stride:pos + (y + 1) * stride] for y in range(h)]
    return w, h, rows, 3


def scan(path):
    if path.endswith(".ppm"):
        w, h, rows, nch = read_ppm(path)
    else:
        w, h, rows, nch = read_png(path)
    n = 0
    x0, y0, x1, y1 = w, h, -1, -1
    for y, row in enumerate(rows):
        for x in range(w):
            i = x * nch
            if (abs(row[i] - TEAL[0]) <= TOL and abs(row[i + 1] - TEAL[1]) <= TOL
                    and abs(row[i + 2] - TEAL[2]) <= TOL):
                n += 1
                if x < x0:
                    x0 = x
                if y < y0:
                    y0 = y
                if x > x1:
                    x1 = x
                if y > y1:
                    y1 = y
    pct = 100.0 * n / float(w * h) if w and h else 0.0
    if n == 0:
        print(f"{path}: {w}x{h}  NO teal")
    else:
        print(f"{path}: {w}x{h}  teal {n} px ({pct:.3f}%)  "
              f"box x{x0}..{x1} y{y0}..{y1}")
    return n


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    total = 0
    clean = []
    for a in sys.argv[1:]:
        n = scan(a)
        total += n
        if n == 0:
            clean.append(a)
    # Which frame to believe, and a NON-ZERO EXIT when none of them can be (P47).
    # This used to exit 0 unconditionally, which made it a thing a person read
    # rather than a thing a script could act on — and "a person read the frame"
    # is exactly the step that let a torn capture be mistaken for a layout bug
    # for two phases. A caller that wants the old behaviour reads the printout;
    # a caller that wants an answer reads the status.
    if clean:
        print(f"clean: {clean[0]}" + (f"  ({len(clean)} of {len(sys.argv) - 1})"
                                      if len(sys.argv) > 2 else ""))
        sys.exit(0)
    print(f"NO CLEAN FRAME among {len(sys.argv) - 1}: every capture has "
          f"compositor background showing through. Take more (FRAMES=N).")
    sys.exit(1)
