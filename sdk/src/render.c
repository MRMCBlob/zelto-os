// libzelto software renderer: paints the laid-out node tree into an ARGB8888
// buffer (the wl_shm buffer the app commits to its surface). A GLES/EGL render
// path through a glyph atlas is the documented target; shm software rendering is
// the MVP — it needs no client GPU context, which is robust under QEMU's virtio
// software path. See docs/contributing/sdk-internals.md.
#include <math.h>
#include <string.h>

#include "internal.h"

void z_canvas_set_clip(ZCanvas *c, int x0, int y0, int x1, int y1) {
    if (x0 < 0) { x0 = 0; }
    if (y0 < 0) { y0 = 0; }
    if (x1 > c->width) { x1 = c->width; }
    if (y1 > c->height) { y1 = c->height; }
    c->clip_x0 = x0;
    c->clip_y0 = y0;
    c->clip_x1 = x1;
    c->clip_y1 = y1;
}

void z_canvas_clear_clip(ZCanvas *c) {
    for (int y = c->clip_y0; y < c->clip_y1; y++) {
        memset(&c->pixels[y * c->stride_px + c->clip_x0], 0,
               (size_t)(c->clip_x1 - c->clip_x0) * 4);
    }
}

static void fill_round_rect(ZCanvas *c, float fx, float fy, float fw, float fh,
                            float radius, ZColor col) {
    // Rect geometry (used for the rounded-corner test) — independent of the clip.
    int rx0 = (int)(fx + 0.5f), ry0 = (int)(fy + 0.5f);
    int rx1 = (int)(fx + fw + 0.5f), ry1 = (int)(fy + fh + 0.5f);

    float r = radius;
    float rw = (float)(rx1 - rx0), rh = (float)(ry1 - ry0);
    if (r > rw / 2.0f) {
        r = rw / 2.0f;
    }
    if (r > rh / 2.0f) {
        r = rh / 2.0f;
    }

    // Iteration bounds = rect intersected with the active clip region.
    int x0 = rx0 < c->clip_x0 ? c->clip_x0 : rx0;
    int y0 = ry0 < c->clip_y0 ? c->clip_y0 : ry0;
    int x1 = rx1 > c->clip_x1 ? c->clip_x1 : rx1;
    int y1 = ry1 > c->clip_y1 ? c->clip_y1 : ry1;

    uint32_t src = 0xff000000u | ((uint32_t)col.r << 16) |
                   ((uint32_t)col.g << 8) | col.b;
    uint32_t sa = col.a;

    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            if (r > 0.5f) {
                // Skip pixels outside the rounded corners (rect-relative).
                float cx = -1.0f, cy = -1.0f;
                if (x - rx0 < r && y - ry0 < r) {
                    cx = (float)rx0 + r;
                    cy = (float)ry0 + r;
                } else if (x - rx0 < r && ry1 - 1 - y < r) {
                    cx = (float)rx0 + r;
                    cy = (float)ry1 - r;
                } else if (rx1 - 1 - x < r && y - ry0 < r) {
                    cx = (float)rx1 - r;
                    cy = (float)ry0 + r;
                } else if (rx1 - 1 - x < r && ry1 - 1 - y < r) {
                    cx = (float)rx1 - r;
                    cy = (float)ry1 - r;
                }
                if (cx >= 0.0f) {
                    float dx = (float)x + 0.5f - cx;
                    float dy = (float)y + 0.5f - cy;
                    if (dx * dx + dy * dy > r * r) {
                        continue;
                    }
                }
            }
            uint32_t *dst = &c->pixels[y * c->stride_px + x];
            if (sa == 0xff) {
                *dst = src;
            } else {
                // Source-over in PREMULTIPLIED-alpha space, PRESERVING the
                // destination alpha. wl_shm ARGB8888 surface buffers are
                // premultiplied (wlroots/wlr_scene composites them so), so a
                // partly-transparent fill must store premultiplied colour — else
                // the compositor reads colour/alpha that are out of step and a
                // 50%-white fill blows out to near-solid white. The buffer is
                // itself premultiplied (we wrote it), so dst.rgb already carries
                // its own alpha; blend straight src OVER premultiplied dst:
                //   out.a   = sa + da*(1-sa)
                //   out.rgb = src.rgb*sa + dst.rgb*(1-sa)     [premultiplied]
                // Over an opaque pixel (da=255) out.a=255 and this reduces to the
                // old formula, so every widget on a solid background is unchanged;
                // only painting over transparent now yields a correct translucent
                // surface the compositor blends over the app (the P19 dim scrim,
                // and any cross-surface translucency).
                uint32_t d = *dst;
                uint32_t da = (d >> 24) & 0xff;
                uint32_t dr = (d >> 16) & 0xff, dg = (d >> 8) & 0xff,
                         db = d & 0xff;
                uint32_t inv = 255u - sa;                 // 1 - src_alpha
                uint32_t oa = sa + da * inv / 255u;        // out alpha (0..255)
                uint32_t rr = (col.r * sa + dr * inv) / 255u;
                uint32_t gg = (col.g * sa + dg * inv) / 255u;
                uint32_t bb = (col.b * sa + db * inv) / 255u;
                *dst = (oa << 24) | (rr << 16) | (gg << 8) | bb;
            }
        }
    }
}

// True if pixel center (x+0.5, y+0.5) lies inside the rounded rectangle
// [rx0,rx1) x [ry0,ry1) with corner radius r. Shared by the image blit's
// rounded-corner mask (fill_round_rect inlines the same test for speed).
static bool rrect_inside(int x, int y, int rx0, int ry0, int rx1, int ry1,
                         float r) {
    if (r <= 0.5f) {
        return true;
    }
    float cx = -1.0f, cy = -1.0f;
    if (x - rx0 < r && y - ry0 < r) {
        cx = (float)rx0 + r; cy = (float)ry0 + r;
    } else if (x - rx0 < r && ry1 - 1 - y < r) {
        cx = (float)rx0 + r; cy = (float)ry1 - r;
    } else if (rx1 - 1 - x < r && y - ry0 < r) {
        cx = (float)rx1 - r; cy = (float)ry0 + r;
    } else if (rx1 - 1 - x < r && ry1 - 1 - y < r) {
        cx = (float)rx1 - r; cy = (float)ry1 - r;
    }
    if (cx < 0.0f) {
        return true;
    }
    float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
    return dx * dx + dy * dy <= r * r;
}

// Bilinearly sample a premultiplied-ARGB bitmap at (u,v) in pixel coordinates.
// Premultiplied is the correct space to interpolate in (no dark/edge halo).
static uint32_t sample_bilinear(const uint32_t *px, int w, int h, float u,
                                float v) {
    u -= 0.5f;
    v -= 0.5f;
    int x0 = (int)floorf(u), y0 = (int)floorf(v);
    float fx = u - (float)x0, fy = v - (float)y0;
    int x1 = x0 + 1, y1 = y0 + 1;
    if (x0 < 0) { x0 = 0; }
    if (y0 < 0) { y0 = 0; }
    if (x1 > w - 1) { x1 = w - 1; }
    if (y1 > h - 1) { y1 = h - 1; }
    if (x0 > w - 1) { x0 = w - 1; }
    if (y0 > h - 1) { y0 = h - 1; }
    uint32_t p00 = px[y0 * w + x0], p10 = px[y0 * w + x1];
    uint32_t p01 = px[y1 * w + x0], p11 = px[y1 * w + x1];
    float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy);
    float w01 = (1 - fx) * fy, w11 = fx * fy;
    float out[4];
    for (int c = 0; c < 4; c++) {
        int sh = c * 8;
        out[c] = ((p00 >> sh) & 0xff) * w00 + ((p10 >> sh) & 0xff) * w10 +
                 ((p01 >> sh) & 0xff) * w01 + ((p11 >> sh) & 0xff) * w11;
    }
    uint32_t r = 0;
    for (int c = 0; c < 4; c++) {
        int v8 = (int)(out[c] + 0.5f);
        if (v8 > 255) { v8 = 255; }
        r |= (uint32_t)v8 << (c * 8);
    }
    return r;
}

// Blit a cached image into a node's frame. Default: aspect-fit (letterboxed,
// centered). With img_cover (Cover()): aspect-fill — scale to cover the frame,
// center-cropping the overflow. Either way the paint is clipped to the active
// clip AND the node frame, masked to the node's rounded corners, composited
// source-over. Both source and destination are premultiplied ARGB, so the blend
// is out = src + dst*(1 - src_a).
static void blit_image(ZCanvas *c, ZView n) {
    const ZImage *img = z_image_get(n->img_path);
    if (!img || !img->ok || img->w <= 0 || img->h <= 0) {
        return;   // failed decode: draw nothing (caller supplies any fallback)
    }
    // Fit uses the SMALLER axis scale (whole image visible, bars); cover uses the
    // LARGER (frame fully covered, overflow cropped by the node-frame clamp below).
    float fw = n->w, fh = n->h;
    float sx = fw / (float)img->w, sy = fh / (float)img->h;
    float scale = n->img_cover ? (sx > sy ? sx : sy)
                               : (sx < sy ? sx : sy);
    float dw = (float)img->w * scale, dh = (float)img->h * scale;
    float ox = n->x + (fw - dw) / 2.0f, oy = n->y + (fh - dh) / 2.0f;

    // Rounded-corner mask geometry = the node frame (matches fill_round_rect).
    int rx0 = (int)(n->x + 0.5f), ry0 = (int)(n->y + 0.5f);
    int rx1 = (int)(n->x + fw + 0.5f), ry1 = (int)(n->y + fh + 0.5f);
    float rr = n->radius;
    float rw = (float)(rx1 - rx0), rh = (float)(ry1 - ry0);
    if (rr > rw / 2.0f) { rr = rw / 2.0f; }
    if (rr > rh / 2.0f) { rr = rh / 2.0f; }

    // Iterate the scaled rect, clamped to the node frame (cover crops the overflow
    // here; fit's letterboxed rect already sits inside it) and the active clip.
    int x0 = (int)ox, y0 = (int)oy;
    int x1 = (int)(ox + dw + 0.5f), y1 = (int)(oy + dh + 0.5f);
    if (x0 < rx0) { x0 = rx0; }
    if (y0 < ry0) { y0 = ry0; }
    if (x1 > rx1) { x1 = rx1; }
    if (y1 > ry1) { y1 = ry1; }
    if (x0 < c->clip_x0) { x0 = c->clip_x0; }
    if (y0 < c->clip_y0) { y0 = c->clip_y0; }
    if (x1 > c->clip_x1) { x1 = c->clip_x1; }
    if (y1 > c->clip_y1) { y1 = c->clip_y1; }

    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            if (!rrect_inside(x, y, rx0, ry0, rx1, ry1, rr)) {
                continue;
            }
            float u = ((float)x + 0.5f - ox) / scale;
            float v = ((float)y + 0.5f - oy) / scale;
            uint32_t s = sample_bilinear(img->px, img->w, img->h, u, v);
            uint32_t sa = (s >> 24) & 0xff;
            if (sa == 0) {
                continue;
            }
            uint32_t *dst = &c->pixels[y * c->stride_px + x];
            if (sa == 0xff) {
                *dst = s;
                continue;
            }
            uint32_t sr = (s >> 16) & 0xff, sg = (s >> 8) & 0xff, sb = s & 0xff;
            uint32_t d = *dst;
            uint32_t da = (d >> 24) & 0xff, dr = (d >> 16) & 0xff,
                     dg = (d >> 8) & 0xff, db = d & 0xff;
            uint32_t inv = 255u - sa;
            uint32_t oa = sa + da * inv / 255u;
            uint32_t rres = sr + dr * inv / 255u;
            uint32_t gres = sg + dg * inv / 255u;
            uint32_t bres = sb + db * inv / 255u;
            *dst = (oa << 24) | (rres << 16) | (gres << 8) | bres;
        }
    }
}

// Signed distance from (px,py) to the rounded rectangle [rx0,rx1) x [ry0,ry1)
// with corner radius r: negative inside, 0 on the edge, positive (the Euclidean
// distance) outside. Used to shade a soft drop shadow's falloff.
static float sdf_round_rect(float px, float py, float rx0, float ry0, float rx1,
                            float ry1, float r) {
    float cx = (rx0 + rx1) * 0.5f, cy = (ry0 + ry1) * 0.5f;
    float hw = (rx1 - rx0) * 0.5f, hh = (ry1 - ry0) * 0.5f;
    float qx = fabsf(px - cx) - (hw - r);
    float qy = fabsf(py - cy) - (hh - r);
    float ax = qx > 0.0f ? qx : 0.0f, ay = qy > 0.0f ? qy : 0.0f;
    float outside = sqrtf(ax * ax + ay * ay);
    float mx = qx > qy ? qx : qy;
    float inside = mx < 0.0f ? mx : 0.0f;
    return outside + inside - r;
}

// Paint a soft drop shadow behind a node's frame (Shadow()/elevation). The shadow
// is the node's rounded rect dropped slightly downward (light from above) and
// blurred over `e` px: for each pixel we take the signed distance to that shifted
// rect and fade the ink from full (inside) to zero (e px out) with a smoothstep.
// Pixels the node's own fill will overdraw are skipped, so a translucent card is
// never muddied from beneath and opaque cards waste no work — the visible result
// is the penumbra crescent around (mostly below) the surface. Black ink, source-
// over in premultiplied space preserving destination alpha (like fill_round_rect),
// so it composites correctly over an opaque surface, the scrim, or bare transparency
// (a floating overlay's halo over the app beneath).
static void paint_shadow(ZCanvas *c, ZView n) {
    float e = n->elevation;
    if (e < 0.5f) {
        return;
    }
    float dy = e * 0.42f;   // downward drop

    float nx0 = n->x, ny0 = n->y, nx1 = n->x + n->w, ny1 = n->y + n->h;
    float r = n->radius;
    float rw = nx1 - nx0, rh = ny1 - ny0;
    if (r > rw / 2.0f) { r = rw / 2.0f; }
    if (r > rh / 2.0f) { r = rh / 2.0f; }

    // The shadow shape: the node rect shifted down, with marginally softer corners.
    float sx0 = nx0, sy0 = ny0 + dy, sx1 = nx1, sy1 = ny1 + dy;
    float sr = r + 1.0f;

    ZColor sh = Z_COLOR_SHADOW;

    int x0 = (int)floorf(sx0 - e), y0 = (int)floorf(sy0 - e);
    int x1 = (int)ceilf(sx1 + e), y1 = (int)ceilf(sy1 + e);
    if (x0 < c->clip_x0) { x0 = c->clip_x0; }
    if (y0 < c->clip_y0) { y0 = c->clip_y0; }
    if (x1 > c->clip_x1) { x1 = c->clip_x1; }
    if (y1 > c->clip_y1) { y1 = c->clip_y1; }

    // The node's own fill rect (int, matching fill_round_rect) — pixels it will
    // cover are skipped here.
    int frx0 = (int)(nx0 + 0.5f), fry0 = (int)(ny0 + 0.5f);
    int frx1 = (int)(nx1 + 0.5f), fry1 = (int)(ny1 + 0.5f);

    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            if (x >= frx0 && x < frx1 && y >= fry0 && y < fry1 &&
                rrect_inside(x, y, frx0, fry0, frx1, fry1, r)) {
                continue;
            }
            float d = sdf_round_rect((float)x + 0.5f, (float)y + 0.5f, sx0, sy0,
                                     sx1, sy1, sr);
            if (d >= e) {
                continue;
            }
            float t = d < 0.0f ? 0.0f : d / e;
            float cov = 1.0f - t * t * (3.0f - 2.0f * t);   // smoothstep falloff
            uint32_t sa = (uint32_t)((float)sh.a * cov + 0.5f);
            if (sa == 0) {
                continue;
            }
            uint32_t *dst = &c->pixels[y * c->stride_px + x];
            uint32_t d0 = *dst;
            uint32_t da = (d0 >> 24) & 0xff, dr = (d0 >> 16) & 0xff,
                     dg = (d0 >> 8) & 0xff, db = d0 & 0xff;
            uint32_t inv = 255u - sa;                  // ink is black: src.rgb = 0
            uint32_t oa = sa + da * inv / 255u;
            uint32_t rr = dr * inv / 255u;
            uint32_t gg = dg * inv / 255u;
            uint32_t bb = db * inv / 255u;
            *dst = (oa << 24) | (rr << 16) | (gg << 8) | bb;
        }
    }
}

// Draw a rounded plate just outside a node's frame; the node's own fill paints
// over the interior immediately after, leaving a thin border = the focus ring.
static void stroke_focus_ring(ZCanvas *c, ZView n) {
    const ZColor ring = {0x2e, 0x9b, 0xff, 0xff};  // accent
    const float t = 3.0f;     // ring thickness
    const float g = 2.0f;     // gap from the frame
    float x = n->x - g - t, y = n->y - g - t;
    float w = n->w + 2.0f * (g + t), h = n->h + 2.0f * (g + t);
    float r = n->radius > 0.0f ? n->radius + g + t : 0.0f;
    fill_round_rect(c, x, y, w, h, r, ring);
}

// Blend a single coverage-weighted pixel of `col` over the destination, in the
// same premultiplied-preserving-dst-alpha space as fill_round_rect.
static void blend_coverage(ZCanvas *c, int x, int y, ZColor col, float cov) {
    if (x < c->clip_x0 || y < c->clip_y0 || x >= c->clip_x1 ||
        y >= c->clip_y1) {
        return;
    }
    uint32_t a = (uint32_t)((float)col.a * cov + 0.5f);
    if (a == 0) {
        return;
    }
    uint32_t *dst = &c->pixels[y * c->stride_px + x];
    uint32_t d = *dst;
    uint32_t da = (d >> 24) & 0xff, dr = (d >> 16) & 0xff, dg = (d >> 8) & 0xff,
             db = d & 0xff;
    uint32_t inv = 255u - a;
    uint32_t oa = a + da * inv / 255u;
    uint32_t rr = (col.r * a + dr * inv) / 255u;
    uint32_t gg = (col.g * a + dg * inv) / 255u;
    uint32_t bb = (col.b * a + db * inv) / 255u;
    *dst = (oa << 24) | (rr << 16) | (gg << 8) | bb;
}

// Rasterize one round-capped line segment of half-width `r`: for each pixel in
// the segment's bounding box, coverage falls from full inside to zero one px
// past the edge of the capsule (distance to the segment). Round caps come free
// from clamping the projection parameter to [0,1].
static void stroke_segment(ZCanvas *c, float ax, float ay, float bx, float by,
                           float r, ZColor col) {
    float minx = (ax < bx ? ax : bx) - r - 1.0f;
    float maxx = (ax > bx ? ax : bx) + r + 1.0f;
    float miny = (ay < by ? ay : by) - r - 1.0f;
    float maxy = (ay > by ? ay : by) + r + 1.0f;
    int x0 = (int)floorf(minx), y0 = (int)floorf(miny);
    int x1 = (int)ceilf(maxx), y1 = (int)ceilf(maxy);
    if (x0 < c->clip_x0) { x0 = c->clip_x0; }
    if (y0 < c->clip_y0) { y0 = c->clip_y0; }
    if (x1 > c->clip_x1) { x1 = c->clip_x1; }
    if (y1 > c->clip_y1) { y1 = c->clip_y1; }
    float dx = bx - ax, dy = by - ay;
    float len2 = dx * dx + dy * dy;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float px = (float)x + 0.5f - ax, py = (float)y + 0.5f - ay;
            float t = len2 > 0.0f ? (px * dx + py * dy) / len2 : 0.0f;
            if (t < 0.0f) { t = 0.0f; }
            if (t > 1.0f) { t = 1.0f; }
            float qx = px - t * dx, qy = py - t * dy;
            float dist = sqrtf(qx * qx + qy * qy);
            float cov = r - dist + 0.5f;   // 1px anti-aliased edge
            if (cov <= 0.0f) { continue; }
            if (cov > 1.0f) { cov = 1.0f; }
            blend_coverage(c, x, y, col, cov);
        }
    }
}

// Draw a polyline (Z_K_STROKE): points are in the unit box, mapped into the node
// frame; each consecutive pair is a round-capped segment, optionally closed.
static void paint_stroke(ZCanvas *c, ZView n) {
    const float *p = n->stroke_pts;
    int np = n->stroke_n;
    if (!p || np < 2) {
        return;
    }
    float sx = n->x, sy = n->y, sw = n->w, sh = n->h;
    float r = n->stroke_w * 0.5f;
    for (int i = 0; i + 1 < np; i++) {
        stroke_segment(c, sx + p[2 * i] * sw, sy + p[2 * i + 1] * sh,
                       sx + p[2 * i + 2] * sw, sy + p[2 * i + 3] * sh, r,
                       n->color);
    }
    if (n->stroke_closed && np > 2) {
        stroke_segment(c, sx + p[2 * (np - 1)] * sw,
                       sy + p[2 * (np - 1) + 1] * sh, sx + p[0] * sw,
                       sy + p[1] * sh, r, n->color);
    }
}

static void paint(ZCanvas *canvas, ZView n) {
    // A clipping node (scroll viewport) intersects the active clip with its frame
    // for its subtree, then restores it. Skip entirely if nothing is visible.
    int save_x0 = canvas->clip_x0, save_y0 = canvas->clip_y0;
    int save_x1 = canvas->clip_x1, save_y1 = canvas->clip_y1;
    if (n->clip) {
        // Intersect with the active clip (which may already be a damage rect),
        // never widen it — otherwise a partial repaint would paint outside its
        // damaged region over stale pixels.
        int cx0 = (int)n->x, cy0 = (int)n->y;
        int cx1 = (int)(n->x + n->w), cy1 = (int)(n->y + n->h);
        if (cx0 < save_x0) { cx0 = save_x0; }
        if (cy0 < save_y0) { cy0 = save_y0; }
        if (cx1 > save_x1) { cx1 = save_x1; }
        if (cy1 > save_y1) { cy1 = save_y1; }
        z_canvas_set_clip(canvas, cx0, cy0, cx1, cy1);
        if (canvas->clip_x1 <= canvas->clip_x0 ||
            canvas->clip_y1 <= canvas->clip_y0) {
            canvas->clip_x0 = save_x0;
            canvas->clip_y0 = save_y0;
            canvas->clip_x1 = save_x1;
            canvas->clip_y1 = save_y1;
            return;
        }
    }

    if (n->elevation > 0.5f) {
        paint_shadow(canvas, n);
    }
    if (n->focused && (n->has_bg || n->kind == Z_K_RECT)) {
        stroke_focus_ring(canvas, n);
    }
    if (n->has_bg) {
        fill_round_rect(canvas, n->x, n->y, n->w, n->h, n->radius, n->bg);
    }
    switch (n->kind) {
    case Z_K_RECT:
        fill_round_rect(canvas, n->x, n->y, n->w, n->h, n->radius, n->color);
        break;
    case Z_K_TEXT:
        if (n->text_shadow) {
            // A dark, slightly-dropped copy under the ink so a light label holds
            // legibility over a bright/busy backdrop (a home caption over art).
            z_text_draw(canvas, n->text, n->font_size, n->weight, z_scrim(0x9e),
                        n->x + n->padding + 1.0f, n->y + n->padding + 1.5f);
        }
        z_text_draw(canvas, n->text, n->font_size, n->weight, n->fg,
                    n->x + n->padding, n->y + n->padding);
        break;
    case Z_K_IMAGE:
        blit_image(canvas, n);
        break;
    case Z_K_STROKE:
        paint_stroke(canvas, n);
        break;
    case Z_K_STACK:
    case Z_K_SPACER:
    case Z_K_SCROLL:
        break;
    }
    for (int i = 0; i < n->n_children; i++) {
        paint(canvas, n->children[i]);
    }

    // Press feedback (P31): a light veil over the whole control (icon + label),
    // drawn last so it sits above the node's content. The app loop stamps
    // n->press (0..1) onto the node under the live press; the veil alpha scales
    // with it, so touch-down fades in and release fades out. Masked to the node's
    // own rounded corners (a bare tap frame with radius 0 gets a soft default so
    // the highlight reads as a rounded tap target, not a hard box).
    if (n->press > 0.003f) {
        ZColor veil = Z_COLOR_PRESS;
        veil.a = (uint8_t)((float)veil.a * (n->press < 1.0f ? n->press : 1.0f) +
                           0.5f);
        float pr = n->radius > 0.5f ? n->radius : 10.0f;
        fill_round_rect(canvas, n->x, n->y, n->w, n->h, pr, veil);
    }

    if (n->clip) {
        canvas->clip_x0 = save_x0;
        canvas->clip_y0 = save_y0;
        canvas->clip_x1 = save_x1;
        canvas->clip_y1 = save_y1;
    }
}

void z_render(ZCanvas *canvas, ZView root) { paint(canvas, root); }
