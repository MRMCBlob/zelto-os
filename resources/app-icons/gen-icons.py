#!/usr/bin/env python3
# Author the raster (PNG) app icons that ship as-is on the device (the SVG icons
# are rasterized on-device instead). Pure Python stdlib only (zlib + struct) so
# it runs anywhere with no Pillow/cairo/network — the build host here has none.
#
# Each icon is a 512x512 straight-alpha RGBA PNG with a TRANSPARENT background:
# the launcher paints the app's colour tile behind it, so the emblem is drawn in
# white (like the SVG line icons) and composites over that tile. The Fetch icon
# deliberately includes a HALF-alpha halo disc so the renderer's premultiplied
# source-over path (the "50%-white blow-out" note in sdk/src/render.c) is
# exercised by a real translucent PNG, not only by antialiased edges.
#
#   python3 resources/app-icons/gen-icons.py
import math
import struct
import zlib

N = 512


def blank():
    # Straight-alpha float RGBA buffer.
    return [[0.0, 0.0, 0.0, 0.0] for _ in range(N * N)]


def over(dst, r, g, b, a):
    # Source-over in straight alpha (dst is [r,g,b,a] 0..1).
    da = dst[3]
    oa = a + da * (1 - a)
    if oa <= 0:
        return
    for i, s in enumerate((r, g, b)):
        dst[i] = (s * a + dst[i] * da * (1 - a)) / oa
    dst[3] = oa


def aa(cov):
    # Clamp a coverage/distance ramp to [0,1].
    return 0.0 if cov < 0 else (1.0 if cov > 1 else cov)


def disc(buf, cx, cy, radius, rgb, alpha):
    r, g, b = rgb
    x0, x1 = int(cx - radius - 1), int(cx + radius + 1)
    y0, y1 = int(cy - radius - 1), int(cy + radius + 1)
    for y in range(max(0, y0), min(N, y1)):
        for x in range(max(0, x0), min(N, x1)):
            d = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
            cov = aa(radius + 0.5 - d)
            if cov > 0:
                over(buf[y * N + x], r, g, b, alpha * cov)


def capsule(buf, ax, ay, bx, by, rad, rgb, alpha=1.0):
    r, g, b = rgb
    minx, maxx = min(ax, bx) - rad - 1, max(ax, bx) + rad + 1
    miny, maxy = min(ay, by) - rad - 1, max(ay, by) + rad + 1
    dx, dy = bx - ax, by - ay
    ln2 = dx * dx + dy * dy
    for y in range(max(0, int(miny)), min(N, int(maxy))):
        for x in range(max(0, int(minx)), min(N, int(maxx))):
            px, py = x + 0.5, y + 0.5
            t = 0.0 if ln2 < 1e-6 else ((px - ax) * dx + (py - ay) * dy) / ln2
            t = 0.0 if t < 0 else (1.0 if t > 1 else t)
            d = math.hypot(px - (ax + t * dx), py - (ay + t * dy))
            cov = aa(rad + 0.5 - d)
            if cov > 0:
                over(buf[y * N + x], r, g, b, alpha * cov)


def write_png(path, buf):
    raw = bytearray()
    for y in range(N):
        raw.append(0)  # filter: None
        for x in range(N):
            r, g, b, a = buf[y * N + x]
            raw += bytes(int(v * 255 + 0.5) for v in (r, g, b, a))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", N, N, 8, 6, 0, 0, 0)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", ihdr)
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)
    print("wrote", path)


def fetch_icon():
    # A download emblem: a translucent halo disc + an opaque white down-arrow
    # over a tray line. White so it reads on the launcher's coloured tile.
    buf = blank()
    W = (1.0, 1.0, 1.0)
    disc(buf, 256, 232, 150, W, 0.16)          # soft translucent halo (alpha check)
    capsule(buf, 256, 120, 256, 300, 26, W)    # arrow shaft
    capsule(buf, 256, 300, 176, 220, 26, W)    # arrow head, left
    capsule(buf, 256, 300, 336, 220, 26, W)    # arrow head, right
    capsule(buf, 150, 392, 362, 392, 26, W)    # tray / baseline
    return buf


if __name__ == "__main__":
    import os

    here = os.path.dirname(os.path.abspath(__file__))
    write_png(os.path.join(here, "os.zelto.fetch.png"), fetch_icon())
