// libzelto software renderer: paints the laid-out node tree into an ARGB8888
// buffer (the wl_shm buffer the app commits to its surface). A GLES/EGL render
// path through a glyph atlas is the documented target; shm software rendering is
// the MVP — it needs no client GPU context, which is robust under QEMU's virtio
// software path. See docs/contributing/sdk-internals.md.
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
        z_text_draw(canvas, n->text, n->font_size, n->fg, n->x + n->padding,
                    n->y + n->padding);
        break;
    case Z_K_STACK:
    case Z_K_SPACER:
    case Z_K_SCROLL:
        break;
    }
    for (int i = 0; i < n->n_children; i++) {
        paint(canvas, n->children[i]);
    }

    if (n->clip) {
        canvas->clip_x0 = save_x0;
        canvas->clip_y0 = save_y0;
        canvas->clip_x1 = save_x1;
        canvas->clip_y1 = save_y1;
    }
}

void z_render(ZCanvas *canvas, ZView root) { paint(canvas, root); }
