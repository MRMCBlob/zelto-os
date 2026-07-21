// Shared System-UI vector glyphs.
//
// The status bar drew its own Wi-Fi arcs and padlock as file-statics, the volume
// HUD drew its own speaker, and the Control Center needs the same marks again on
// its round toggles. Three copies of one glyph drift: the bar's Wi-Fi and the
// Control Center's Wi-Fi have to be the SAME mark or the toggle stops reading as
// "the thing in the status bar". So they live here, once, sized by argument.
//
// Everything is drawn from the toolkit's two vector primitives — a rounded Rect
// and Stroke (a round-capped polyline in the unit box, scaled into whatever
// Frame you give it) — so there are no bitmaps to ship and every mark is crisp
// at any size. Points are copied by Stroke, so the static arrays are safe.
#ifndef ZELTO_SYSTEM_COMMON_GLYPHS_H
#define ZELTO_SYSTEM_COMMON_GLYPHS_H

#include <zelto/ui.h>

// Wi-Fi: two arcs over a dot. A coloured DOT (what the bar used before) says only
// "something is on" — it does not say what, and a green dot in a status bar reads
// as a recording indicator.
static inline ZView zelto_glyph_wifi(float size, ZColor c) {
    static const float outer[] = {0.06f, 0.42f, 0.20f, 0.28f, 0.38f, 0.21f,
                                  0.62f, 0.21f, 0.80f, 0.28f, 0.94f, 0.42f};
    static const float inner[] = {0.26f, 0.58f, 0.38f, 0.47f, 0.50f, 0.44f,
                                  0.62f, 0.47f, 0.74f, 0.58f};
    float h = size * 0.78f;
    float t = size / 9.0f;
    if (t < 1.5f) {
        t = 1.5f;
    }
    return Frame(size, h,
        ZStack(
            Frame(size, h,
                Stroke(.points = outer, .count = 6, .thickness = t, .color = c)),
            Frame(size, h,
                Stroke(.points = inner, .count = 5, .thickness = t, .color = c)),
            OffsetXY(0.0f, h * 0.32f,
                Frame(t * 1.75f, t * 1.75f,
                    Rect(.color = c, .radius = t * 0.875f))),
            .align = Z_ALIGN_CENTER));
}

// Cellular signal: four bars of rising height, bottom-aligned. `bars` (0..4) is
// how many are lit; the rest render in `off` so the mark keeps its silhouette at
// every level instead of shrinking — the same way a phone draws it.
static inline ZView zelto_glyph_cellular(float size, int bars, ZColor on,
                                         ZColor off) {
    float w = size * 0.22f;
    float gap = size * 0.10f;
    float r = w * 0.4f;
    ZStackOpts s = {.spacing = gap, .align = Z_ALIGN_TRAILING};
    for (int i = 0; i < 4; i++) {
        float h = size * (0.34f + 0.22f * (float)i);
        s.children[i] = Frame(w, h,
            Rect(.color = i < bars ? on : off, .radius = r));
    }
    return Frame(size, size, z_stack(Z_AXIS_HORIZONTAL, &s));
}

// Airplane mode: a plane silhouette, drawn as ONE closed polygon outline. A
// fuselage-plus-wings built from two crossed rectangles reads as a plus sign, not
// as a plane, which is why this is the one mark that needs a real outline.
static inline ZView zelto_glyph_airplane(float size, ZColor c) {
    static const float plane[] = {
        0.50f, 0.03f, 0.58f, 0.30f, 0.97f, 0.57f, 0.97f, 0.69f,
        0.58f, 0.53f, 0.56f, 0.79f, 0.71f, 0.91f, 0.71f, 0.98f,
        0.50f, 0.90f, 0.29f, 0.98f, 0.29f, 0.91f, 0.44f, 0.79f,
        0.42f, 0.53f, 0.03f, 0.69f, 0.03f, 0.57f, 0.42f, 0.30f,
    };
    float t = size / 13.0f;
    if (t < 1.5f) {
        t = 1.5f;
    }
    return Frame(size, size,
        Stroke(.points = plane, .count = 16, .thickness = t, .color = c,
               .closed = true));
}

// A speaker: body + cone. With `slash` a bar is struck through it (muted).
static inline ZView zelto_glyph_speaker(float size, ZColor c, bool slash) {
    static const float bar[] = {0.16f, 0.84f, 0.84f, 0.16f};
    float t = size / 9.0f;
    if (t < 1.5f) {
        t = 1.5f;
    }
    ZView cone = HStack(
        Frame(size * 0.28f, size * 0.50f, Rect(.color = c, .radius = size * 0.1f)),
        Frame(size * 0.50f, size * 0.89f, Rect(.color = c, .radius = size * 0.17f)),
        .spacing = 0, .align = Z_ALIGN_CENTER);
    if (!slash) {
        return Frame(size, size, ZStack(cone, .align = Z_ALIGN_CENTER));
    }
    return Frame(size, size,
        ZStack(cone,
               Frame(size, size,
                   Stroke(.points = bar, .count = 2, .thickness = t, .color = c)),
               .align = Z_ALIGN_CENTER));
}

// Brightness: a disc with four rays. Eight rays would need diagonals, and the
// toolkit has no rotation — four read as a sun perfectly well at this size.
static inline ZView zelto_glyph_sun(float size, ZColor c) {
    float d = size * 0.44f;
    float ray_l = size * 0.20f;
    float ray_t = size * 0.11f;
    float off = size * 0.40f;
    return Frame(size, size,
        ZStack(
            Frame(d, d, Rect(.color = c, .radius = d * 0.5f)),
            OffsetXY(0.0f, -off,
                Frame(ray_t, ray_l, Rect(.color = c, .radius = ray_t * 0.5f))),
            OffsetXY(0.0f, off,
                Frame(ray_t, ray_l, Rect(.color = c, .radius = ray_t * 0.5f))),
            OffsetXY(-off, 0.0f,
                Frame(ray_l, ray_t, Rect(.color = c, .radius = ray_t * 0.5f))),
            OffsetXY(off, 0.0f,
                Frame(ray_l, ray_t, Rect(.color = c, .radius = ray_t * 0.5f))),
            .align = Z_ALIGN_CENTER));
}

// A padlock: shackle over a body.
static inline ZView zelto_glyph_lock(float size, ZColor c) {
    float bw = size * 0.75f;
    return Frame(size, size,
        VStack(
            Frame(bw * 0.66f, size * 0.36f, Rect(.color = c, .radius = size * 0.2f)),
            Frame(bw, size * 0.52f, Rect(.color = c, .radius = size * 0.12f)),
            .spacing = 0, .align = Z_ALIGN_CENTER));
}

// Reduce Motion: a double chevron — the universal "fast" mark, which is exactly
// the thing this toggle turns down.
static inline ZView zelto_glyph_motion(float size, ZColor c) {
    static const float chev[] = {0.30f, 0.20f, 0.58f, 0.50f, 0.30f, 0.80f};
    static const float chev2[] = {0.58f, 0.20f, 0.86f, 0.50f, 0.58f, 0.80f};
    float t = size / 8.0f;
    if (t < 1.5f) {
        t = 1.5f;
    }
    return Frame(size, size,
        ZStack(
            Frame(size, size,
                Stroke(.points = chev, .count = 3, .thickness = t, .color = c)),
            Frame(size, size,
                Stroke(.points = chev2, .count = 3, .thickness = t, .color = c)),
            .align = Z_ALIGN_CENTER));
}

// Battery: a bordered cell whose interior fill tracks the charge, plus the
// terminal nub. `col` is the fill ink (the caller decides charging/low colour).
static inline ZView zelto_glyph_battery(int64_t pct, ZColor col, ZColor shell) {
    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }
    float inner = 20.0f;                       // usable fill width inside the cell
    float fill = inner * (float)pct / 100.0f;
    if (fill < 2.0f) {
        fill = 2.0f;                           // always a sliver so 1% is visible
    }
    return HStack(
        ZStack(
            Frame(24.0f, 12.0f, Rect(.color = shell, .radius = 3)),
            Frame(24.0f, 12.0f,
                Padding(2.0f,
                    HStack(Frame(fill, 8.0f, Rect(.color = col, .radius = 1)),
                           Spacer(), .spacing = 0))),
            .align = Z_ALIGN_CENTER),
        Frame(3.0f, 6.0f, Rect(.color = shell, .radius = 1)),   // nub
        .spacing = 1, .align = Z_ALIGN_CENTER);
}

#endif   // ZELTO_SYSTEM_COMMON_GLYPHS_H
