#!/usr/bin/env python3
"""Compare two PNGs (or summarise one) with nothing but the Python stdlib.

This WSL has no PIL and no ImageMagick, so the screenshot catalogue has no way
to be checked other than by eye -- and eyeballing is exactly how three shots in
the P41 catalogue came to be photographing nothing at all. A press-feedback shot
in particular is a faint WHITE veil over a small rounded box: it is perfectly
possible for the veil to be missing, or 40px off target, and for the screenshot
to still look like a plausible screenshot. So measure it.

Usage:
    meta/pngdiff.py A.png                 # mean RGB + per-region means
    meta/pngdiff.py A.png B.png           # mean delta + the brightest 8 regions
    meta/pngdiff.py A.png B.png --box X Y W H     # delta restricted to a box
    meta/pngdiff.py A.png B.png --expect-box X Y W H [--min-delta 2.0]
        exit 0 only if the change is REAL (mean |delta| inside the box exceeds
        --min-delta) and LOCAL (the box is the strongest region on the screen).

The last form is the one a harness wants: it answers "did pressing at (x,y)
actually light up the thing at (x,y)", which is the question a screenshot
cannot answer.
"""
import struct
import sys
import zlib


def read_png(path):
    """Decode a non-interlaced 8-bit RGB/RGBA PNG to (w, h, rows-of-RGB-bytes)."""
    with open(path, 'rb') as fp:
        data = fp.read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'{path}: not a PNG')
    pos = 8
    idat = bytearray()
    w = h = depth = color = 0
    while pos < len(data):
        (length,) = struct.unpack('>I', data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b'IHDR':
            w, h, depth, color, _comp, _filt, interlace = struct.unpack('>IIBBBBB', body)
            if depth != 8 or color not in (2, 6) or interlace:
                raise ValueError(f'{path}: need 8-bit non-interlaced RGB/RGBA, '
                                 f'got depth={depth} color={color} interlace={interlace}')
        elif ctype == b'IDAT':
            idat += body
        elif ctype == b'IEND':
            break
    chan = 3 if color == 2 else 4
    raw = zlib.decompress(bytes(idat))
    stride = w * chan
    out = bytearray(w * h * 3)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        ftype = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        # Undo the PNG per-scanline filter (spec 9.2).
        if ftype == 1:
            for i in range(chan, stride):
                line[i] = (line[i] + line[i - chan]) & 0xFF
        elif ftype == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:
            for i in range(stride):
                a = line[i - chan] if i >= chan else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:
            for i in range(stride):
                a = line[i - chan] if i >= chan else 0
                b = prev[i]
                c = prev[i - chan] if i >= chan else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        elif ftype != 0:
            raise ValueError(f'{path}: bad filter {ftype} on row {y}')
        prev = line
        if chan == 3:
            out[y * w * 3:(y + 1) * w * 3] = line
        else:
            row = out
            base = y * w * 3
            for x in range(w):
                row[base + x * 3:base + x * 3 + 3] = line[x * 4:x * 4 + 3]
    return w, h, bytes(out)


def mean_rgb(w, h, px, box=None):
    x0, y0, bw, bh = box if box else (0, 0, w, h)
    x1, y1 = min(x0 + bw, w), min(y0 + bh, h)
    x0, y0 = max(x0, 0), max(y0, 0)
    n = (x1 - x0) * (y1 - y0)
    if n <= 0:
        return (0.0, 0.0, 0.0)
    r = g = b = 0
    for y in range(y0, y1):
        base = y * w * 3
        for x in range(x0, x1):
            i = base + x * 3
            r += px[i]; g += px[i + 1]; b += px[i + 2]
    return (r / n, g / n, b / n)


def mean_abs_delta(w, h, a, b, box=None):
    x0, y0, bw, bh = box if box else (0, 0, w, h)
    x1, y1 = min(x0 + bw, w), min(y0 + bh, h)
    x0, y0 = max(x0, 0), max(y0, 0)
    n = (x1 - x0) * (y1 - y0)
    if n <= 0:
        return 0.0
    tot = 0
    for y in range(y0, y1):
        base = y * w * 3
        for x in range(x0, x1):
            i = base + x * 3
            tot += (abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1])
                    + abs(a[i + 2] - b[i + 2]))
    return tot / (n * 3)


# Coarse tile grid used to locate WHERE two frames differ.
TILE = 40


def tile_deltas(w, h, a, b):
    out = []
    for ty in range(0, h, TILE):
        for tx in range(0, w, TILE):
            d = mean_abs_delta(w, h, a, b, (tx, ty, TILE, TILE))
            if d > 0:
                out.append((d, tx, ty))
    out.sort(reverse=True)
    return out


def main(argv):
    args = [a for a in argv[1:] if not a.startswith('--')]
    def opt(name, count):
        if name not in argv:
            return None
        i = argv.index(name)
        return [float(v) for v in argv[i + 1:i + 1 + count]]

    box = opt('--box', 4)
    expect = opt('--expect-box', 4)
    min_delta = (opt('--min-delta', 1) or [2.0])[0]
    if box:
        box = tuple(int(v) for v in box)
    if expect:
        expect = tuple(int(v) for v in expect)

    if not args:
        print(__doc__)
        return 2

    w, h, a = read_png(args[0])
    if len(args) == 1:
        r, g, bl = mean_rgb(w, h, a, box)
        print(f'{args[0]}: {w}x{h}  mean RGB = {r:.2f} {g:.2f} {bl:.2f}')
        return 0

    w2, h2, b = read_png(args[1])
    if (w, h) != (w2, h2):
        print(f'!! size mismatch {w}x{h} vs {w2}x{h2}')
        return 1

    whole = mean_abs_delta(w, h, a, b)
    print(f'{args[0]} vs {args[1]}: {w}x{h}  mean |delta| = {whole:.3f}')

    target = expect or box
    if target:
        d = mean_abs_delta(w, h, a, b, target)
        print(f'  box {target}: mean |delta| = {d:.3f}')

    tiles = tile_deltas(w, h, a, b)
    print(f'  changed {TILE}px tiles: {len(tiles)}; strongest:')
    for d, tx, ty in tiles[:8]:
        print(f'    ({tx:4d},{ty:4d})  {d:.2f}')

    if expect:
        d = mean_abs_delta(w, h, a, b, expect)
        if d < min_delta:
            print(f'!! FAIL: change inside {expect} is {d:.3f} < {min_delta} '
                  f'-- the shot did not capture what it claims to')
            return 1
        # And it must be the DOMINANT change, or we photographed something else
        # (a clock tick, a battery drain) and merely happened to overlap.
        ex0, ey0, ew, eh = expect
        for td, tx, ty in tiles[:3]:
            inside = (ex0 - TILE < tx < ex0 + ew and ey0 - TILE < ty < ey0 + eh)
            if inside:
                break
        else:
            print(f'!! FAIL: the strongest changes are OUTSIDE {expect} '
                  f'-- the frame moved for some other reason')
            return 1
        print(f'  OK: change of {d:.3f} localised to {expect}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
