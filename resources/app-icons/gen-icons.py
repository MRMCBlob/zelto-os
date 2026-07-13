#!/usr/bin/env python3
# The Zelto app-icon system.
#
# Every shipped app gets a real icon, and they are drawn as ONE FAMILY rather than
# ten separate pictures. The system half is fixed for all of them:
#
#   TILE      a 512x512 squircle (|x|^4 + |y|^4 = r^4, r = 0.2237*512 — Apple's
#             icon-grid proportion, so the shape holds at any size), filled with a
#             vertical two-stop gradient of the app's hue (lighter at the top, as
#             if lit from above), with a soft specular band across the top edge and
#             a hairline rim. That is what makes ten icons look like siblings.
#   GRID      the mark is composed inside a centred 320x320 box (62.5% of the
#             tile), then OPTICALLY adjusted per shape — a circle has to overshoot
#             a square to look the same size, so the numbers below are eyeballed,
#             not derived.
#   INK       white by default. Two icons (Notes, JS) invert to dark ink on a light
#             tile, because their real-world referents are light.
#
# The app half is hand-composed per icon (see each *_icon function): the marks are
# not generated from a rule, they are drawn.
#
# The tile is baked into the PNG (not drawn by the launcher) so an icon is a single
# self-contained asset — the same file works in the launcher, the app drawer, the
# task switcher, the share sheet and the Store.
#
# Pure Python stdlib only (zlib + struct): no Pillow, no cairo, no network.
#   python3 resources/app-icons/gen-icons.py
import math
import struct
import zlib

N = 512
CORNER = 0.2237 * N          # the icon-grid corner radius
CORNER_N = 4.0               # squircle exponent (continuous curvature)

W = (1.0, 1.0, 1.0)
INK = (0.06, 0.06, 0.07)     # dark ink, for a light tile


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


def hexrgb(s):
    s = s.lstrip("#")
    return tuple(int(s[i:i + 2], 16) / 255.0 for i in (0, 2, 4))


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


# --- shapes ---------------------------------------------------------------

def disc(buf, cx, cy, radius, rgb, alpha=1.0):
    r, g, b = rgb
    x0, x1 = int(cx - radius - 1), int(cx + radius + 2)
    y0, y1 = int(cy - radius - 1), int(cy + radius + 2)
    for y in range(max(0, y0), min(N, y1)):
        for x in range(max(0, x0), min(N, x1)):
            d = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
            cov = aa(radius + 0.5 - d)
            if cov > 0:
                over(buf[y * N + x], r, g, b, alpha * cov)


def ring(buf, cx, cy, radius, thickness, rgb, alpha=1.0, a0=None, a1=None):
    """An annulus, optionally only over the arc [a0, a1] radians."""
    r, g, b = rgb
    half = thickness / 2.0
    x0, x1 = int(cx - radius - half - 1), int(cx + radius + half + 2)
    y0, y1 = int(cy - radius - half - 1), int(cy + radius + half + 2)
    for y in range(max(0, y0), min(N, y1)):
        for x in range(max(0, x0), min(N, x1)):
            px, py = x + 0.5 - cx, y + 0.5 - cy
            d = math.hypot(px, py)
            cov = aa(half + 0.5 - abs(d - radius))
            if cov <= 0:
                continue
            if a0 is not None:
                ang = math.atan2(py, px)
                if ang < 0:
                    ang += 2 * math.pi
                lo, hi = a0 % (2 * math.pi), a1 % (2 * math.pi)
                inside = (lo <= ang <= hi) if lo <= hi else (ang >= lo or ang <= hi)
                if not inside:
                    continue
            over(buf[y * N + x], r, g, b, alpha * cov)


def capsule(buf, ax, ay, bx, by, rad, rgb, alpha=1.0):
    r, g, b = rgb
    minx, maxx = min(ax, bx) - rad - 1, max(ax, bx) + rad + 2
    miny, maxy = min(ay, by) - rad - 1, max(ay, by) + rad + 2
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


def sq_cov(px, py, x, y, w, h, r, n=CORNER_N):
    """Coverage of a superellipse-cornered rect at a pixel centre."""
    if r <= 0.5:
        inside = x <= px <= x + w and y <= py <= y + h
        return 1.0 if inside else 0.0
    r = min(r, w / 2.0, h / 2.0)
    dx = dy = 0.0
    if px < x + r:
        dx = x + r - px
    elif px > x + w - r:
        dx = px - (x + w - r)
    if py < y + r:
        dy = y + r - py
    elif py > y + h - r:
        dy = py - (y + h - r)
    if px < x or px > x + w or py < y or py > y + h:
        return 0.0
    if dx <= 0 or dy <= 0:
        return 1.0
    u, v = dx / r, dy / r
    e = u ** n + v ** n
    g = (n / r) * math.hypot(u ** (n - 1), v ** (n - 1))
    if g < 1e-9:
        return 1.0 if e <= 1 else 0.0
    return aa((1.0 - e) / g + 0.5)


def rrect(buf, x, y, w, h, r, rgb, alpha=1.0):
    """A rounded rect with the SAME continuous corner as the tile."""
    cr, cg, cb = rgb
    for py in range(max(0, int(y - 1)), min(N, int(y + h + 2))):
        for px in range(max(0, int(x - 1)), min(N, int(x + w + 2))):
            cov = sq_cov(px + 0.5, py + 0.5, x, y, w, h, r)
            if cov > 0:
                over(buf[py * N + px], cr, cg, cb, alpha * cov)


def tile(top, bottom, ink_is_dark=False):
    """The shared squircle tile: vertical gradient + top specular + rim."""
    buf = blank()
    for y in range(N):
        t = y / (N - 1.0)
        # Ease the ramp so most of the change happens in the top half — a linear
        # ramp reads as a flat wash, this reads as light falling on a surface.
        tt = t ** 0.85
        r, g, b = mix(top, bottom, tt)
        for x in range(N):
            cov = sq_cov(x + 0.5, y + 0.5, 0, 0, N, N, CORNER)
            if cov > 0:
                over(buf[y * N + x], r, g, b, cov)

    # Specular: a soft light band hugging the top edge (a highlight, not a shine).
    for y in range(0, int(N * 0.42)):
        f = 1.0 - (y / (N * 0.42))
        a = 0.10 * (f ** 2.2)
        if a < 0.002:
            continue
        for x in range(N):
            cov = sq_cov(x + 0.5, y + 0.5, 0, 0, N, N, CORNER)
            if cov > 0:
                over(buf[y * N + x], 1.0, 1.0, 1.0, a * cov)

    # Rim: a hairline just inside the edge, so the icon has a defined boundary
    # against both the wallpaper and a dark home screen.
    rim = (0.0, 0.0, 0.0) if ink_is_dark else (1.0, 1.0, 1.0)
    rim_a = 0.10 if ink_is_dark else 0.16
    for y in range(N):
        for x in range(N):
            outer = sq_cov(x + 0.5, y + 0.5, 0, 0, N, N, CORNER)
            inner = sq_cov(x + 0.5, y + 0.5, 2.5, 2.5, N - 5, N - 5, CORNER - 2.5)
            band = outer - inner
            if band > 0.01:
                over(buf[y * N + x], rim[0], rim[1], rim[2], rim_a * band)
    return buf


# --- the marks (hand-composed, one per app) -------------------------------

def cards_icon():
    # Three cards fanned back-to-front, the front one lifted: a deck.
    buf = tile(hexrgb("#7D7BFF"), hexrgb("#4340C4"))
    rrect(buf, 150, 128, 212, 250, 26, W, 0.30)     # back card
    rrect(buf, 132, 152, 248, 250, 28, W, 0.55)     # middle card
    rrect(buf, 112, 180, 288, 210, 30, W, 1.0)      # front card
    # Two content lines on the front card (what makes it a CARD, not a rectangle).
    capsule(buf, 150, 250, 300, 250, 11, hexrgb("#4340C4"), 0.55)
    capsule(buf, 150, 300, 250, 300, 11, hexrgb("#4340C4"), 0.35)
    return buf


def fetch_icon():
    # A download: an arrow driven into a tray. The halo disc is deliberately
    # half-alpha — it is the one asset that exercises the renderer's premultiplied
    # source-over path with a real translucent PNG (see sdk/src/render.c).
    buf = tile(hexrgb("#4EC3F0"), hexrgb("#0A84C8"))
    disc(buf, 256, 226, 132, W, 0.14)
    capsule(buf, 256, 118, 256, 286, 26, W)         # shaft
    capsule(buf, 256, 292, 178, 214, 26, W)         # head, left
    capsule(buf, 256, 292, 334, 214, 26, W)         # head, right
    capsule(buf, 146, 384, 366, 384, 24, W)         # tray floor
    capsule(buf, 146, 330, 146, 384, 24, W)         # tray wall, left
    capsule(buf, 366, 330, 366, 384, 24, W)         # tray wall, right
    return buf


def notes_icon():
    # A ruled page — dark ink on a light (paper) tile, the one place the family
    # inverts, because a note IS a light thing.
    buf = tile(hexrgb("#FFE066"), hexrgb("#F5B301"), ink_is_dark=True)
    rrect(buf, 116, 108, 280, 300, 26, (1.0, 1.0, 1.0), 0.96)
    for i, ln in enumerate((172, 222, 272, 322)):
        w = 200 if i < 3 else 130
        capsule(buf, 156, ln, 156 + w, ln, 10, INK, 0.72)
    return buf


def notepad_icon():
    # A page, and a pencil writing on it: the editable counterpart to Notes. The
    # pencil is drawn OVER the page with a tile-coloured gap around it, so the two
    # objects separate instead of merging into one white blob.
    buf = tile(hexrgb("#FFB84D"), hexrgb("#E07000"))
    rrect(buf, 116, 106, 240, 300, 24, W, 0.95)
    capsule(buf, 152, 168, 300, 168, 9, hexrgb("#E07000"), 0.45)
    capsule(buf, 152, 214, 300, 214, 9, hexrgb("#E07000"), 0.45)
    capsule(buf, 152, 260, 244, 260, 9, hexrgb("#E07000"), 0.45)
    # The pencil: a diagonal shaft from lower-left to upper-right, with a knock-out
    # gap beneath it, then the ferrule band and the sharpened graphite tip.
    capsule(buf, 236, 402, 404, 234, 34, hexrgb("#E58A16"))   # the gap (tile hue)
    capsule(buf, 244, 394, 396, 242, 24, W)                   # shaft
    capsule(buf, 356, 282, 386, 252, 24, W, 0.62)             # ferrule band
    capsule(buf, 232, 406, 250, 388, 10, INK, 0.85)           # graphite tip
    return buf


def settings_icon():
    # A gear: eight teeth on a ring, hollow hub. Graphite, so it reads as SYSTEM
    # rather than as one more app.
    buf = tile(hexrgb("#8E8E93"), hexrgb("#48484A"))
    for i in range(8):
        a = i * math.pi / 4.0
        cx, cy = 256 + 118 * math.cos(a), 256 + 118 * math.sin(a)
        capsule(buf, 256 + 74 * math.cos(a), 256 + 74 * math.sin(a),
                cx, cy, 30, W)
    ring(buf, 256, 256, 96, 60, W)
    disc(buf, 256, 256, 44, hexrgb("#5C5C60"))
    return buf


def store_icon():
    # A shopping bag. The handle is a narrow arc RISING OUT of the bag's top edge —
    # a wide one plus a squat body reads as a padlock, which is the wrong idea
    # entirely for a store.
    buf = tile(hexrgb("#D07BF7"), hexrgb("#8944AB"))
    ring(buf, 256, 214, 52, 20, W, 1.0, math.pi, 2 * math.pi)   # handle
    rrect(buf, 128, 208, 256, 208, 28, W)                       # bag body (wide, low)
    # Where the handle meets the bag: two short stubs, so it is attached, not floating.
    capsule(buf, 204, 208, 204, 236, 10, hexrgb("#8944AB"), 0.40)
    capsule(buf, 308, 208, 308, 236, 10, hexrgb("#8944AB"), 0.40)
    return buf


def pinger_icon():
    # A bell (it posts notifications): dome, clapper, and two ring arcs to say the
    # thing is actually going off.
    buf = tile(hexrgb("#FF6F63"), hexrgb("#D02F27"))
    ring(buf, 256, 268, 96, 30, W, 1.0, math.pi, 2 * math.pi)   # dome
    capsule(buf, 160, 268, 160, 330, 15, W)                     # skirt, left
    capsule(buf, 352, 268, 352, 330, 15, W)                     # skirt, right
    capsule(buf, 142, 338, 370, 338, 16, W)                     # rim
    disc(buf, 256, 380, 26, W)                                  # clapper
    disc(buf, 256, 158, 20, W)                                  # crown
    ring(buf, 256, 268, 152, 14, W, 0.45, math.pi * 1.15, math.pi * 1.35)
    ring(buf, 256, 268, 152, 14, W, 0.45, math.pi * 1.65, math.pi * 1.85)
    return buf


def jsdemo_icon():
    # Dark ink on the canonical script yellow. A `</>` — the universal mark for
    # "this is code" — drawn as three strokes. (Curly braces built from arcs came
    # out as noise at this size; two chevrons and a slash survive the downscale to
    # a 56px home tile, which is the only size that actually matters.)
    buf = tile(hexrgb("#F7E14A"), hexrgb("#E0C500"), ink_is_dark=True)
    t = 26
    capsule(buf, 196, 176, 116, 256, t, INK)     # `<` upper
    capsule(buf, 116, 256, 196, 336, t, INK)     # `<` lower
    capsule(buf, 316, 176, 396, 256, t, INK)     # `>` upper
    capsule(buf, 396, 256, 316, 336, t, INK)     # `>` lower
    capsule(buf, 288, 146, 224, 366, t, INK)     # the slash
    return buf


def widget_icon():
    # A bento of tiles: one wide, two small — literally what a widget is.
    buf = tile(hexrgb("#5CE07E"), hexrgb("#1F9E45"))
    rrect(buf, 116, 116, 280, 118, 26, W)
    rrect(buf, 116, 262, 118, 134, 26, W, 0.85)
    rrect(buf, 278, 262, 118, 134, 26, W, 0.55)
    return buf


def greeter_icon():
    # A speech bubble — the sample app that says hello.
    buf = tile(hexrgb("#4DA3FF"), hexrgb("#0060DF"))
    rrect(buf, 106, 122, 300, 234, 54, W)
    capsule(buf, 176, 344, 150, 412, 22, W)      # tail
    for x in (196, 256, 316):                    # the three dots ("typing")
        disc(buf, x, 238, 20, hexrgb("#0060DF"), 0.55)
    return buf


def rows_icon():
    # The "Rows" sample (id os.zelto.app): a list — a leading dot and a line, three
    # times over. It is a demo of the list primitive, so the icon IS a list.
    buf = tile(hexrgb("#7FE3D4"), hexrgb("#12968A"))
    for i, y in enumerate((166, 256, 346)):
        disc(buf, 152, y, 22, W, 1.0 - i * 0.18)
        capsule(buf, 204, y, 372 - i * 44, y, 17, W, 1.0 - i * 0.18)
    return buf


def placeholder_icon():
    # The fallback for an app that ships no icon: a neutral tile with a quiet
    # dotted grid. Deliberately characterless — it should read as "no icon yet",
    # never as a real icon.
    buf = tile(hexrgb("#4A4A4E"), hexrgb("#2C2C2E"))
    for gy in range(3):
        for gx in range(3):
            disc(buf, 176 + gx * 80, 176 + gy * 80, 15, W, 0.34)
    return buf


ICONS = {
    "os.zelto.cards": cards_icon,
    "os.zelto.fetch": fetch_icon,
    "os.zelto.notes": notes_icon,
    "os.zelto.notepad": notepad_icon,
    "os.zelto.settings": settings_icon,
    "os.zelto.store": store_icon,
    "os.zelto.pinger": pinger_icon,
    "os.zelto.jsdemo": jsdemo_icon,
    "os.zelto.widget": widget_icon,
    "os.zelto.greeter": greeter_icon,
    "os.zelto.app": rows_icon,          # samples/hello, shown as "Rows"
    "Placeholder": placeholder_icon,
}


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


if __name__ == "__main__":
    import os
    import sys

    here = os.path.dirname(os.path.abspath(__file__))
    only = sys.argv[1:] or list(ICONS)
    for name in only:
        write_png(os.path.join(here, name + ".png"), ICONS[name]())
