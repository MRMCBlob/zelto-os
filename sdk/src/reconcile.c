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
    // A drop shadow (Shadow()/elevation) spills its blur (~e) plus a downward
    // drop (~0.42e) beyond the frame, so an elevated node damages a wider box —
    // else a partial repaint would leave stale penumbra when it moves/changes.
    int m = Z_DAMAGE_MARGIN;
    if (n->elevation > 0.5f) {
        int em = (int)(n->elevation * 1.5f) + 2;
        if (em > m) { m = em; }
    }
    ZIRect r = {
        .x0 = (int)n->x - m,
        .y0 = (int)n->y - m,
        .x1 = (int)(n->x + n->w) + m,
        .y1 = (int)(n->y + n->h) + m,
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
        a->focused != b->focused || a->font_size != b->font_size ||
        a->elevation != b->elevation || a->text_shadow != b->text_shadow) {
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
    const char *ia = a->img_path ? a->img_path : "";
    const char *ib = b->img_path ? b->img_path : "";
    if (strcmp(ia, ib) != 0) {
        return true;
    }
    if (a->img_cover != b->img_cover) {
        return true;
    }
    if (a->stroke_n != b->stroke_n || a->stroke_w != b->stroke_w ||
        a->stroke_closed != b->stroke_closed) {
        return true;
    }
    return false;
}

// Exact node frame (no AA margin) as an integer rect.
static ZIRect frame_rect(ZView n) {
    ZIRect r = {(int)n->x, (int)n->y, (int)(n->x + n->w), (int)(n->y + n->h)};
    return r;
}

// Intersect r with the viewport rect v (so list-row damage never escapes the
// scroll area and repaints the navbar / surrounding UI).
static ZIRect intersect(ZIRect r, ZIRect v) {
    ZIRect o = r;
    if (o.x0 < v.x0) { o.x0 = v.x0; }
    if (o.y0 < v.y0) { o.y0 = v.y0; }
    if (o.x1 > v.x1) { o.x1 = v.x1; }
    if (o.y1 > v.y1) { o.y1 = v.y1; }
    return o;
}

static void walk(ZView old_n, ZView new_n, ZDamage *d);

// A scroll viewport: keep the diff bounded to the viewport instead of falling
// back to a full repaint when virtualisation changes the visible row set.
static void reconcile_scroll(ZView old_n, ZView new_n, ZDamage *d) {
    ZIRect vp = frame_rect(new_n);

    // The viewport itself moved or resized (e.g. a screen transition): repaint
    // the old and new viewport areas; the content underneath all shifted.
    if (old_n->x != new_n->x || old_n->y != new_n->y ||
        old_n->w != new_n->w || old_n->h != new_n->h) {
        z_damage_add(d, node_rect(old_n));
        z_damage_add(d, node_rect(new_n));
        return;
    }
    if (old_n->n_children == 0 || new_n->n_children == 0) {
        z_damage_add(d, vp);
        return;
    }

    ZView oc = old_n->children[0], nc = new_n->children[0];
    // Content scrolled this frame (offset changed -> content origin moved): the
    // whole viewport's pixels shifted, so damage exactly the viewport.
    if (oc->y != nc->y) {
        z_damage_add(d, vp);
        return;
    }

    // Offset unchanged: keyed diff of the visible rows. Entering/leaving/changed
    // rows damage (clipped to the viewport); reused, unchanged rows cost nothing.
    for (int i = 0; i < nc->n_children; i++) {
        ZView nr = nc->children[i];
        ZView match = NULL;
        for (int j = 0; j < oc->n_children; j++) {
            if (oc->children[j]->key == nr->key) {
                match = oc->children[j];
                break;
            }
        }
        if (!match) {
            z_damage_add(d, intersect(node_rect(nr), vp));   // entered
        } else {
            walk(match, nr, d);                              // same key: diff in place
        }
    }
    for (int j = 0; j < oc->n_children; j++) {
        ZView orow = oc->children[j];
        bool still = false;
        for (int i = 0; i < nc->n_children; i++) {
            if (nc->children[i]->key == orow->key) {
                still = true;
                break;
            }
        }
        if (!still) {
            z_damage_add(d, intersect(node_rect(orow), vp));  // left
        }
    }
}

static void walk(ZView old_n, ZView new_n, ZDamage *d) {
    // Structural divergence: can't map old pixels onto new layout cheaply.
    if (old_n->kind != new_n->kind) {
        d->full = true;
        return;
    }
    if (new_n->kind == Z_K_SCROLL) {
        reconcile_scroll(old_n, new_n, d);
        return;
    }
    if (old_n->n_children != new_n->n_children) {
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
