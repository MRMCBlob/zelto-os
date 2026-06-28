// libzelto software renderer: paints the laid-out node tree into an ARGB8888
// buffer (the wl_shm buffer the app commits to its surface). A GLES/EGL render
// path through a glyph atlas is the documented target; shm software rendering is
// the MVP — it needs no client GPU context, which is robust under QEMU's virtio
// software path. See docs/contributing/sdk-internals.md.
#include "internal.h"

static void fill_round_rect(ZCanvas *c, float fx, float fy, float fw, float fh,
                            float radius, ZColor col) {
    int x0 = (int)(fx + 0.5f), y0 = (int)(fy + 0.5f);
    int x1 = (int)(fx + fw + 0.5f), y1 = (int)(fy + fh + 0.5f);
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > c->width) {
        x1 = c->width;
    }
    if (y1 > c->height) {
        y1 = c->height;
    }
    float r = radius;
    float rw = (float)(x1 - x0), rh = (float)(y1 - y0);
    if (r > rw / 2.0f) {
        r = rw / 2.0f;
    }
    if (r > rh / 2.0f) {
        r = rh / 2.0f;
    }

    uint32_t src = 0xff000000u | ((uint32_t)col.r << 16) |
                   ((uint32_t)col.g << 8) | col.b;
    uint32_t sa = col.a;

    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            if (r > 0.5f) {
                // Skip pixels outside the rounded corners.
                float cx = -1.0f, cy = -1.0f;
                if (x - x0 < r && y - y0 < r) {
                    cx = (float)x0 + r;
                    cy = (float)y0 + r;
                } else if (x - x0 < r && y1 - 1 - y < r) {
                    cx = (float)x0 + r;
                    cy = (float)y1 - r;
                } else if (x1 - 1 - x < r && y - y0 < r) {
                    cx = (float)x1 - r;
                    cy = (float)y0 + r;
                } else if (x1 - 1 - x < r && y1 - 1 - y < r) {
                    cx = (float)x1 - r;
                    cy = (float)y1 - r;
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
                uint32_t d = *dst;
                uint32_t dr = (d >> 16) & 0xff, dg = (d >> 8) & 0xff,
                         db = d & 0xff;
                uint32_t rr = (col.r * sa + dr * (255 - sa)) / 255u;
                uint32_t gg = (col.g * sa + dg * (255 - sa)) / 255u;
                uint32_t bb = (col.b * sa + db * (255 - sa)) / 255u;
                *dst = 0xff000000u | (rr << 16) | (gg << 8) | bb;
            }
        }
    }
}

static void paint(ZCanvas *canvas, ZView n) {
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
        break;
    }
    for (int i = 0; i < n->n_children; i++) {
        paint(canvas, n->children[i]);
    }
}

void z_render(ZCanvas *canvas, ZView root) { paint(canvas, root); }
