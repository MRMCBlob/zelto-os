// libzelto scrolling: the Scroll container, the virtualised List, the Offset
// transform, and the persistent ZScroll cell. Scroll/List clip a content child
// to the viewport and translate it by a retained offset that the pointer wheel
// and pan drags move (with fling momentum from animation.c). List builds only
// the rows in (and just around) the viewport, keying each so the reconciler
// reuses them across scrolls. See docs/ui/components/{scroll,list}.md.
#include <stdlib.h>

#include "internal.h"

extern ZArena *z_build_arena;

// --- retained scroll cell (call-order allocation, like z_animated_value) ---
static ZScroll *next_scroll_cell(ZApp *app) {
    ZUI *ui = z_app_ui(app);
    ZScreen *s = ui->cur;
    int i = s->scroll_cursor++;
    if (i >= Z_MAX_CELLS) {
        i = Z_MAX_CELLS - 1;
    }
    ZScroll *sc = &s->scrolls[i];
    if (!sc->used) {
        sc->used = true;
        sc->offset = sc->velocity = sc->raw = 0.0f;
        sc->flinging = sc->settling = false;
        sc->painted_offset = -1.0f;
        // Deterministic past-limit freeze-frame for the screenshot harness (P33):
        // ZELTO_SCROLL_OVERPULL=<px> pins the FIRST scroll cell at a rubber-banded
        // over-pull off the TOP (content dragged down, resisting) on a still frame,
        // so the rubber-band curve is shot-verifiable without injected drag input.
        // Env-gated, so zero production effect. (Only the first cell, so a nested
        // scroll elsewhere is unaffected.)
        if (i == 0) {
            const char *op = getenv("ZELTO_SCROLL_OVERPULL");
            if (op && op[0]) {
                float px = (float)atof(op);
                sc->raw = -px;
                sc->offset = z_rubber_band(-px, 400.0f);
                sc->dragging = true;   // hold the over-pull so layout won't clamp it
            }
            // ZELTO_SCROLL_TO=<px> parks the first scroll cell at an ordinary
            // scroll position, clamped by layout like any other. The over-pull
            // hook above cannot do this job: it rubber-bands, so it saturates
            // after a few hundred px and the BOTTOM of a long screen (the last
            // group of a settings list, the final row of a form) has no way to be
            // photographed at all. Env-gated, so zero production effect.
            const char *to = getenv("ZELTO_SCROLL_TO");
            if (to && to[0]) {
                float px = (float)atof(to);
                sc->raw = sc->offset = px > 0.0f ? px : 0.0f;
            }
        }
        if (s->scroll_count <= i) {
            s->scroll_count = i + 1;
        }
    }
    sc->app = app;
    return sc;
}

ZScroll *z_scroll(ZApp *app) { return next_scroll_cell(app); }

void z_scroll_to(ZScroll *sc, float x, float y, bool animated) {
    (void)x;
    (void)animated;  // MVP: immediate; spring-to is Planned.
    sc->offset = y < 0.0f ? 0.0f : y;
    sc->raw = sc->offset;
    sc->velocity = 0.0f;
    sc->flinging = false;
    sc->settling = false;
    sc->dragging = false;
    if (sc->app) {
        z_invalidate(sc->app);
    }
}

// --- input (called by app.c) ----------------------------------------------
static float scroll_max(const ZScroll *sc) {
    float m = sc->content_h - sc->viewport_h;
    return m > 0.0f ? m : 0.0f;
}

// Map a raw (un-damped) drag position to the applied offset: inside [0,max] it is
// the identity; past either bound it resists with the shared rubber-band curve, so
// an over-pull moves a shrinking amount instead of stopping dead at the edge.
static float rubber_offset(const ZScroll *sc, float raw) {
    float max = scroll_max(sc);
    float dim = sc->viewport_h > 1.0f ? sc->viewport_h : 400.0f;
    if (raw < 0.0f) {
        return z_rubber_band(raw, dim);          // above the top
    }
    if (raw > max) {
        return max + z_rubber_band(raw - max, dim);  // below the bottom
    }
    return raw;
}

void z_scroll_begin_drag(ZScroll *sc) {
    sc->flinging = false;
    sc->settling = false;
    sc->dragging = true;
    sc->velocity = 0.0f;
    sc->raw = sc->offset;   // seed the un-damped position from where we are
}

void z_scroll_drag_by(ZScroll *sc, float dy) {
    // Accumulate the raw drag and apply the rubber-banded mapping. The wheel path
    // (which never calls begin_drag) keeps raw in step with offset so a wheel notch
    // past the edge also resists rather than jerking.
    sc->raw += dy;
    sc->offset = rubber_offset(sc, sc->raw);
    if (sc->app) {
        z_invalidate(sc->app);
    }
}

void z_scroll_end_drag(ZScroll *sc, float velocity_y) {
    float max = scroll_max(sc);
    sc->dragging = false;
    // Released while over-pulled: settle elastically back to the nearest bound
    // (rubber-band snap-back), ignoring any fling velocity into the wall.
    if (sc->raw < 0.0f || sc->raw > max) {
        sc->settling = true;
        sc->settle_target = sc->raw < 0.0f ? 0.0f : max;
        sc->flinging = false;
        sc->velocity = 0.0f;
    } else {
        sc->velocity = velocity_y;
        sc->flinging = (velocity_y < -40.0f || velocity_y > 40.0f);
    }
    if (sc->app) {
        z_invalidate(sc->app);
    }
}

// --- Offset ---------------------------------------------------------------
ZView Offset(ZAnimated *x, float y, ZView view) {
    view->off_x += x ? z_animated_get(x) : 0.0f;
    view->off_y += y;
    return view;
}

ZView OffsetXY(float x, float y, ZView view) {
    view->off_x += x;
    view->off_y += y;
    return view;
}

ZView OffsetXYAnimated(ZAnimated *x, ZAnimated *y, ZView view) {
    view->off_x += x ? z_animated_get(x) : 0.0f;
    view->off_y += y ? z_animated_get(y) : 0.0f;
    return view;
}

// --- Scroll container -----------------------------------------------------
static ZView new_node(ZKind kind) {
    struct ZNode *n = z_arena_alloc(z_build_arena, sizeof(*n));
    n->kind = kind;
    n->fg = Z_COLOR_TEXT;
    n->font_size = (float)Z_FONT_BODY;
    return n;
}

ZView z_scroll_view(ZApp *app, const ZScrollOpts *opts) {
    (void)opts->axis;  // vertical only in the MVP (horizontal is Planned)
    ZView n = new_node(Z_K_SCROLL);
    n->clip = true;
    n->scroll = next_scroll_cell(app);
    if (opts->children[0]) {
        n->children[n->n_children++] = opts->children[0];
    }
    return n;
}

// --- List (virtualised) ---------------------------------------------------
ZView z_list(ZApp *app, const ZListOpts *opts) {
    ZScroll *sc = next_scroll_cell(app);
    // z_row_h(), not 44 and not Z_ROW_H: 44 is Apple's row height in POINTS and
    // this number is in screen units (see Z_PT in ui.h — the same confusion P43
    // found in the type scale and P44 in the safe areas), and Z_ROW_H is only
    // the touch-target FLOOR. A List virtualises on this number, so a row that
    // is too short for its own text at a large text size does not merely clip —
    // it puts the wrong rows on screen.
    float rh = opts->row_height > 1.0f ? opts->row_height : z_row_h(app);
    int count = opts->count < 0 ? 0 : opts->count;

    ZView scroll = new_node(Z_K_SCROLL);
    scroll->clip = true;
    scroll->scroll = sc;

    // The content node spans the full virtual height; its children are absolutely
    // positioned by layout_y so only the visible slice needs to exist.
    ZView content = new_node(Z_K_STACK);
    content->axis = Z_AXIS_VERTICAL;
    content->abs_children = true;
    content->content_h = (float)count * rh;

    // Visible range from the (persistent) offset and the prior frame's viewport.
    // viewport_h is 0 on the very first build, so fall back to a generous slice.
    float vp = sc->viewport_h > 1.0f ? sc->viewport_h : 2048.0f;
    int first = (int)((sc->offset) / rh) - 1;
    int last = (int)((sc->offset + vp) / rh) + 1;
    if (first < 0) {
        first = 0;
    }
    if (last > count) {
        last = count;
    }

    for (int i = first; i < last && content->n_children < Z_MAX_CHILDREN; i++) {
        const void *item = (const char *)opts->data + (size_t)i * opts->stride;
        ZView row = opts->row(app, item, i);
        row->layout_y = (float)i * rh;
        row->fixed_h = rh;
        row->key = opts->key ? opts->key(item, i) : (uint64_t)(i + 1);
        content->children[content->n_children++] = row;
    }

    scroll->children[scroll->n_children++] = content;
    return scroll;
}
