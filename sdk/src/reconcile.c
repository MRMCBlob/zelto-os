// libzelto reconcile: the retained-tree diff. Each build produces a fresh view
// tree (in the alternate arena); this pass walks it against the previous build's
// tree and records the pixel regions whose appearance or geometry changed. The
// app loop then repaints only those regions, so unchanged subtrees keep their
// already-painted pixels in the retained shm buffer.
// See docs/contributing/sdk-internals.md ("View tree vs. scene graph").
#include <string.h>

#include "internal.h"

// Painted content can spill a few px outside a node's frame (focus ring +
// glyph antialiasing), so damage rects are inflated by this margin.
#define Z_DAMAGE_MARGIN 8

void z_damage_reset(ZDamage *d) {
    d->count = 0;
    d->full = false;
}

void z_damage_add(ZDamage *d, ZIRect r) {
    if (d->full) {
        return;
    }
    if (r.x1 <= r.x0 || r.y1 <= r.y0) {
        return;
    }
    if (d->count >= Z_MAX_DAMAGE) {
        // Too fragmented to track cheaply; fall back to a full repaint.
        d->full = true;
        return;
    }
    d->rects[d->count++] = r;
}

void z_damage_merge(ZDamage *dst, const ZDamage *src) {
    if (src->full) {
        dst->full = true;
        return;
    }
    for (int i = 0; i < src->count; i++) {
        z_damage_add(dst, src->rects[i]);
    }
}

static ZIRect node_rect(ZView n) {
    ZIRect r = {
        .x0 = (int)n->x - Z_DAMAGE_MARGIN,
        .y0 = (int)n->y - Z_DAMAGE_MARGIN,
        .x1 = (int)(n->x + n->w) + Z_DAMAGE_MARGIN,
        .y1 = (int)(n->y + n->h) + Z_DAMAGE_MARGIN,
    };
    return r;
}

static bool color_eq(ZColor a, ZColor b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

// Does the node's painted output differ between the two builds? Compares
// geometry (a move/resize is visible) and every visual property.
static bool node_changed(ZView a, ZView b) {
    if (a->x != b->x || a->y != b->y || a->w != b->w || a->h != b->h) {
        return true;
    }
    if (a->has_bg != b->has_bg || a->radius != b->radius ||
        a->focused != b->focused || a->font_size != b->font_size) {
        return true;
    }
    if (a->has_bg && !color_eq(a->bg, b->bg)) {
        return true;
    }
    if (!color_eq(a->color, b->color) || !color_eq(a->fg, b->fg)) {
        return true;
    }
    const char *ta = a->text ? a->text : "";
    const char *tb = b->text ? b->text : "";
    if (strcmp(ta, tb) != 0) {
        return true;
    }
    return false;
}

static void walk(ZView old_n, ZView new_n, ZDamage *d) {
    // Structural divergence: can't map old pixels onto new layout cheaply.
    if (old_n->kind != new_n->kind || old_n->n_children != new_n->n_children) {
        d->full = true;
        return;
    }
    if (node_changed(old_n, new_n)) {
        // Damage both the old and new footprints (the node may have moved).
        z_damage_add(d, node_rect(old_n));
        z_damage_add(d, node_rect(new_n));
    }
    for (int i = 0; i < new_n->n_children && !d->full; i++) {
        walk(old_n->children[i], new_n->children[i], d);
    }
}

void z_reconcile(ZView old_root, ZView new_root, ZDamage *out) {
    z_damage_reset(out);
    if (!old_root || !new_root) {
        out->full = true;
        return;
    }
    walk(old_root, new_root, out);
}
