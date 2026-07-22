// libzelto layout: a single-pass, constraint-light stack/flex layout. Children
// report a desired size (measure), then each parent distributes free main-axis
// space to flexible children and aligns the cross axis (arrange).
// See docs/guides/layout.md and docs/contributing/sdk-internals.md.
#include <errno.h>   // program_invocation_short_name (glibc), for the hit-test stats
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "internal.h"

static float maxf(float a, float b) { return a > b ? a : b; }

// Compute each node's desired (content + padding) size into node->w / node->h.
static void measure(ZView n, ZText *text) {
    for (int i = 0; i < n->n_children; i++) {
        measure(n->children[i], text);
    }

    float pad2 = 2.0f * n->padding;
    float dw = 0.0f, dh = 0.0f;

    switch (n->kind) {
    case Z_K_RECT:
    case Z_K_SPACER:
    case Z_K_STROKE:
        // A vector mark has no intrinsic size; it fills whatever Frame() gives it.
        dw = n->fixed_w;
        dh = n->fixed_h;
        break;
    case Z_K_SCROLL:
        // A viewport sizes to its parent (fills); content size is the child's.
        dw = 0.0f;
        dh = 0.0f;
        break;
    case Z_K_TEXT: {
        float ascent = 0.0f, descent = 0.0f;
        dw = z_text_measure(text, n->text, n->font_size, n->weight, &ascent,
                            &descent);
        dh = ascent + descent;
        n->text_w = dw;   // what it will paint; arrange() may narrow n->w below it
        break;
    }
    case Z_K_IMAGE: {
        // Intrinsic pixel size when no fixed frame is given; a Frame() around
        // the Image (the usual case for an app icon) overrides both below.
        int iw = 0, ih = 0;
        if (n->img_path && z_image_intrinsic(n->img_path, &iw, &ih)) {
            dw = (float)iw;
            dh = (float)ih;
        }
        break;
    }
    case Z_K_STACK: {
        if (n->abs_children) {
            // Virtualised list content: full virtual height, fills width.
            dw = 0.0f;
            dh = n->content_h;
            break;
        }
        if (n->axis == Z_AXIS_DEPTH) {
            for (int i = 0; i < n->n_children; i++) {
                dw = maxf(dw, n->children[i]->w);
                dh = maxf(dh, n->children[i]->h);
            }
        } else {
            bool horiz = (n->axis == Z_AXIS_HORIZONTAL);
            float main = 0.0f, cross = 0.0f;
            for (int i = 0; i < n->n_children; i++) {
                ZView c = n->children[i];
                main += horiz ? c->w : c->h;
                cross = maxf(cross, horiz ? c->h : c->w);
            }
            if (n->n_children > 1) {
                main += n->spacing * (float)(n->n_children - 1);
            }
            dw = horiz ? main : cross;
            dh = horiz ? cross : main;
        }
        break;
    }
    }

    if (n->fixed_w > 0.0f) {
        dw = n->fixed_w;
    }
    if (n->fixed_h > 0.0f) {
        dh = n->fixed_h;
    }
    n->w = dw + pad2;
    n->h = dh + pad2;
}

static float align_offset(ZAlign align, float free) {
    switch (align) {
    case Z_ALIGN_CENTER:
        return free / 2.0f;
    case Z_ALIGN_TRAILING:
        return free;
    case Z_ALIGN_LEADING:
    default:
        return 0.0f;
    }
}

// Assign final frames top-down. Reads children's desired sizes (from measure)
// before overwriting them.
static void arrange(ZView n, float x, float y, float w, float h) {
    // Offset (drag / screen transition) shifts this node and its whole subtree.
    x += n->off_x;
    y += n->off_y;
    n->x = x;
    n->y = y;
    n->w = w;
    n->h = h;

    float pad = n->padding;
    float ix = x + pad, iy = y + pad;
    float iw = w - 2.0f * pad, ih = h - 2.0f * pad;

    // Scroll viewport: lay the content out at its natural height, translated up
    // by the offset, and record the metrics the fling/clamp logic reads back.
    if (n->kind == Z_K_SCROLL) {
        if (n->n_children == 0) {
            return;
        }
        ZView c = n->children[0];
        float content_h = c->content_h > 0.0f ? c->content_h : c->h;
        if (n->scroll) {
            n->scroll->viewport_h = h;
            n->scroll->content_h = content_h;
            float max = content_h - h;
            if (max < 0.0f) {
                max = 0.0f;
            }
            // Keep the RESTING offset valid (content may have shrunk since the last
            // frame), but leave an intentional over-pull alone: while a drag holds
            // the offset past a bound, or a release settles it back, the rubber-band
            // (scroll.c) owns the out-of-range value — clamping here would cancel it.
            if (!n->scroll->dragging && !n->scroll->settling) {
                if (n->scroll->offset > max) {
                    n->scroll->offset = max;
                }
                if (n->scroll->offset < 0.0f) {
                    n->scroll->offset = 0.0f;
                }
            }
        }
        float off = n->scroll ? n->scroll->offset : 0.0f;
        arrange(c, x, y - off, w, content_h);
        return;
    }

    if (n->kind != Z_K_STACK || n->n_children == 0) {
        return;
    }

    // Absolutely positioned children (list rows): place each at its layout_y.
    if (n->abs_children) {
        for (int i = 0; i < n->n_children; i++) {
            ZView c = n->children[i];
            float cw = c->fixed_w > 0.0f ? c->fixed_w : iw;
            float ch = c->fixed_h > 0.0f ? c->fixed_h : c->h;
            arrange(c, ix, y + c->layout_y, cw, ch);
        }
        return;
    }

    if (n->axis == Z_AXIS_DEPTH) {
        for (int i = 0; i < n->n_children; i++) {
            ZView c = n->children[i];
            float cw = c->w > 0.0f ? c->w : iw;
            float ch = c->h > 0.0f ? c->h : ih;
            if (c->fill) {
                cw = iw;
                ch = ih;
            }
            arrange(c, ix + (iw - cw) / 2.0f, iy + (ih - ch) / 2.0f, cw, ch);
        }
        return;
    }

    bool horiz = (n->axis == Z_AXIS_HORIZONTAL);
    float inner_main = horiz ? iw : ih;
    float inner_cross = horiz ? ih : iw;

    // A Share() child contributes NOTHING to the measured total, which is what
    // leaves the whole axis (less spacing) as freedom for the weights to divide.
    // Grow, by contrast, only ever divides what its siblings' content left over.
    float total_main = 0.0f, total_grow = 0.0f;
    for (int i = 0; i < n->n_children; i++) {
        ZView c = n->children[i];
        total_main += c->grow_share ? 0.0f : (horiz ? c->w : c->h);
        total_grow += c->grow;
    }
    if (n->n_children > 1) {
        total_main += n->spacing * (float)(n->n_children - 1);
    }
    float freedom = inner_main - total_main;
    if (freedom < 0.0f) {
        freedom = 0.0f;
    }

    float pos = horiz ? ix : iy;
    for (int i = 0; i < n->n_children; i++) {
        ZView c = n->children[i];
        float c_main = c->grow_share ? 0.0f : (horiz ? c->w : c->h);
        if (total_grow > 0.0f) {
            c_main += freedom * c->grow / total_grow;
        }
        // Cross axis: size to content, or fill the parent. Nested stacks fill
        // the cross axis (unless given a fixed cross size); leaves that left
        // their cross dimension unset (0) fill too.
        float c_cross = horiz ? c->h : c->w;
        bool fixed_cross = horiz ? (c->fixed_h > 0.0f) : (c->fixed_w > 0.0f);
        if ((c->kind == Z_K_STACK && !fixed_cross) || c->kind == Z_K_SCROLL ||
            c_cross <= 0.0f || c_cross > inner_cross) {
            c_cross = inner_cross;
        }
        if (c->fill) {
            c_main = inner_main;
            c_cross = inner_cross;
        }
        float cross_off = align_offset(n->align, inner_cross - c_cross);

        if (horiz) {
            arrange(c, pos, iy + cross_off, c_main, c_cross);
        } else {
            arrange(c, ix + cross_off, pos, c_cross, c_main);
        }
        pos += c_main + n->spacing;
    }
}

void z_layout(ZView root, float w, float h, ZText *text) {
    measure(root, text);
    arrange(root, 0.0f, 0.0f, w, h);
}

// --- line breaking --------------------------------------------------------
//
// WHY THIS IS A BUILD-TIME SPLIT AND NOT A LAYOUT PASS. Layout here is one
// bottom-up intrinsic-size pass (measure) followed by one top-down assignment
// (arrange). A Text therefore reports the width of ONE line, and prose wider
// than its column runs off the right edge — which is what the P43 type rescale
// did to every caption in Settings, and what every prose surface added since
// would have inherited. Real reflow means a node's height depending on the width
// its parent grants it: a measure-under-constraint protocol, i.e. rewriting the
// contract every kind of node implements, to fix the one kind that has a
// non-rectangular intrinsic size.
//
// The cheaper answer is exact and is what shipped: the width of a prose column
// is known when the tree is BUILT (it is an inset of the screen, or of a card),
// so a builder can ask the real shaper where the breaks fall and emit one Text
// per line. WrapText() is that builder. The measurement is the same shaper the
// renderer uses, so the result is not an estimate; the layout pass stays one
// pass; and a caller who genuinely does not know its width until arrange time
// still cannot wrap — which is a real limit, and the honest place to reconsider
// the constraint protocol if a surface ever needs it.
//
// The algorithm is greedy, which is what every phone does. The one guarantee
// worth stating: NO line exceeds max_w. A word too long to fit alone is broken
// mid-word rather than allowed to overflow, because a caption that silently
// leaves the screen is the bug this exists to end.
int z_wrap_lines(const char *text, float max_w, ZWrapMeasure measure, void *ud,
                 ZWrapLine *out, int max_lines, const char **rest) {
    if (rest) {
        *rest = text ? text : "";
    }
    if (!text || !out || max_lines <= 0) {
        return 0;
    }
    int n = 0;
    const char *p = text;
    while (n < max_lines) {
        // A break consumes its own whitespace; a space carried onto the next
        // line reads as an indent nobody asked for.
        while (*p == ' ') {
            p++;
        }
        if (!*p) {
            break;
        }
        if (*p == '\n') {
            // An empty line, kept: it is the paragraph break the author wrote.
            out[n].s = p;
            out[n].len = 0;
            n++;
            p++;
            continue;
        }
        const char *line = p;
        const char *best = NULL;     // end of the longest prefix that fits
        const char *scan = p;
        while (*scan && *scan != '\n') {
            const char *word_end = scan;
            while (*word_end && *word_end != ' ' && *word_end != '\n') {
                word_end++;
            }
            int len = (int)(word_end - line);
            if (measure && max_w > 0.0f && measure(ud, line, len) > max_w) {
                if (!best) {
                    // One word, too wide on its own: keep as many characters as
                    // fit, at least one so the loop always makes progress.
                    int keep = 1;
                    for (int i = 2; i <= len; i++) {
                        if (measure(ud, line, i) > max_w) {
                            break;
                        }
                        keep = i;
                    }
                    best = line + keep;
                }
                break;               // this word starts the next line
            }
            best = word_end;
            scan = word_end;
            while (*scan == ' ') {
                scan++;
            }
        }
        if (!best) {
            best = scan;             // no measure function: the whole line
        }
        int len = (int)(best - line);
        while (len > 0 && line[len - 1] == ' ') {
            len--;                   // trim the trailing space off the slice
        }
        out[n].s = line;
        out[n].len = len;
        n++;
        p = best;
        while (*p == ' ') {
            p++;
        }
        if (*p == '\n') {
            p++;                     // a break the author wrote, now consumed
        }
    }
    // Whatever is left, so the caller can continue rather than lose it. Trailing
    // spaces are the breaker's to consume, not the caller's to re-encounter.
    while (*p == ' ') {
        p++;
    }
    if (rest) {
        *rest = p;
    }
    return n;
}

// --- hit testing ----------------------------------------------------------
//
// A NODE IS TAPPABLE EXACTLY WHERE IT IS PAINTED. That sounds too obvious to
// write down, and it is the invariant this file got wrong for thirteen phases.
//
// The renderer masks a subtree at exactly one kind of node: one with `clip` set
// — a Scroll viewport, or an explicit Clip(). Everywhere else a child paints
// wherever arrange() put it, INCLUDING outside its parent's frame, which is not
// an edge case: OffsetXY moves a dragged home icon out of its cell, a screen
// transition slides a whole page across the surface, and an overfull stack runs
// its last child past the parent's edge. All of that is visible on screen.
//
// The old walk descended only into children of nodes whose own frame contained
// the point, so the hit region was the intersection of EVERY ancestor's frame —
// a much smaller region than the painted one, and a different region than the
// clip stack. Two mismatches came out of that, in opposite directions:
//
//   - Visible, not tappable. A node painted outside a NON-clipping ancestor was
//     dead to touch, because the walk turned back at that ancestor. The bigger
//     of the two bugs, and the one the shape of the code hid: the ancestor test
//     reads like a clip and is not one.
//   - Tappable, not visible. Clip(radius) masks the corners away; the walk tested
//     a rectangle, so the four corner nubs of a rounded viewport stayed live.
//
// So the walk carries the clip stack the renderer carries — a rectangle plus the
// same ZRoundClip list, pushed and popped at the same nodes, capped by the same
// Z_MAX_ROUND_CLIPS so an over-deep tree degrades identically in both — and
// culls on THAT, never on a plain ancestor's frame. A node answers for a point
// when its own frame contains it and the live clip admits it.
//
// Losing the ancestor test as a cull costs a full walk of the tree instead of
// one descent. That is fine here and worth saying why rather than leaving it to
// be rediscovered: a Zelto tree is tens of nodes at a depth under ten (stacks
// cap at Z_MAX_CHILDREN, List virtualises to the visible slice), and this runs
// once per pointer event, not per frame. Clip nodes still cull whole subtrees,
// which is what keeps a long scrolled list cheap.

static bool point_in(ZView n, double x, double y) {
    return x >= n->x && x < n->x + n->w && y >= n->y && y < n->y + n->h;
}

// The live clip: the rectangle every enclosing clip node has intersected, plus
// the rounded masks those of them that were Clip(radius) contributed.
typedef struct HitClip {
    float x0, y0, x1, y1;
    ZRoundClip rc[Z_MAX_ROUND_CLIPS];
    int n_rc;
} HitClip;

// Inside one rounded clip? The same geometry render.c's corner_coverage()
// integrates for antialiasing — the corner box, the n=4 superellipse, and the
// true circle once the radius has eaten both half-extents — reduced to the
// inside/outside question, which is all a finger needs. The two agree to within
// the half-pixel of coverage that AA exists to express.
static bool in_round_clip(const ZRoundClip *rc, double px, double py) {
    float r = rc->r;
    if (r <= 0.5f) {
        return true;
    }
    float dx = 0.0f, dy = 0.0f;
    if (px < (float)rc->x0 + r) {
        dx = (float)rc->x0 + r - (float)px;
    } else if (px > (float)rc->x1 - r) {
        dx = (float)px - ((float)rc->x1 - r);
    }
    if (py < (float)rc->y0 + r) {
        dy = (float)rc->y0 + r - (float)py;
    } else if (py > (float)rc->y1 - r) {
        dy = (float)py - ((float)rc->y1 - r);
    }
    if (dx <= 0.0f || dy <= 0.0f) {
        return true;   // on a straight edge band, not in a corner
    }
    float u = dx / r, v = dy / r;
    float u2 = u * u, v2 = v * v;
    float w = (float)(rc->x1 - rc->x0), h = (float)(rc->y1 - rc->y0);
    bool round_all = (r * 2.0f >= w - 0.5f) && (r * 2.0f >= h - 0.5f);
    float e = round_all ? (u2 + v2) : (u2 * u2 + v2 * v2);
    return e <= 1.0f;
}

static bool clip_admits(const HitClip *c, double x, double y) {
    if (x < c->x0 || x >= c->x1 || y < c->y0 || y >= c->y1) {
        return false;
    }
    for (int i = 0; i < c->n_rc; i++) {
        if (!in_round_clip(&c->rc[i], x, y)) {
            return false;
        }
    }
    return true;
}

static bool wants(ZView n, ZHitWant want) {
    switch (want) {
    case Z_HIT_TAP:
        return n->on_tap != NULL || n->on_tap_data != NULL;
    case Z_HIT_SCROLL:
        return n->kind == Z_K_SCROLL && n->scroll != NULL;
    case Z_HIT_PAN:
        return n->on_pan != NULL || n->on_pan_data != NULL;
    case Z_HIT_LONG_PRESS:
        return n->on_long_press != NULL;
    }
    return false;
}

// Nodes entered by the walk in progress (P45 instrumentation; see below).
unsigned long z_hit_nodes;

static ZView hit_walk(ZView n, double x, double y, HitClip *clip, ZHitWant want) {
    if (!n) {
        return NULL;
    }
    z_hit_nodes++;
    HitClip saved;
    bool pushed = false;
    if (n->clip) {
        saved = *clip;
        pushed = true;
        if (n->x > clip->x0) { clip->x0 = n->x; }
        if (n->y > clip->y0) { clip->y0 = n->y; }
        if (n->x + n->w < clip->x1) { clip->x1 = n->x + n->w; }
        if (n->y + n->h < clip->y1) { clip->y1 = n->y + n->h; }
        // Same cap, same silent-degrade-to-square as the renderer: hit and paint
        // must agree even in the pathological case. (render.c warns once.)
        if (n->clip_radius > 0.5f && clip->n_rc < Z_MAX_ROUND_CLIPS) {
            ZRoundClip *rc = &clip->rc[clip->n_rc++];
            rc->x0 = (int)(n->x + 0.5f);
            rc->y0 = (int)(n->y + 0.5f);
            rc->x1 = (int)(n->x + n->w + 0.5f);
            rc->y1 = (int)(n->y + n->h + 0.5f);
            float r = n->clip_radius;
            float hw = (float)(rc->x1 - rc->x0) / 2.0f;
            float hh = (float)(rc->y1 - rc->y0) / 2.0f;
            if (r > hw) { r = hw; }
            if (r > hh) { r = hh; }
            rc->r = r;
        }
        // Nothing this subtree paints can reach the point: cull it whole.
        if (!clip_admits(clip, x, y)) {
            *clip = saved;
            return NULL;
        }
    }

    // Children paint last-on-top; search them front-to-back.
    ZView found = NULL;
    for (int i = n->n_children - 1; i >= 0 && !found; i--) {
        found = hit_walk(n->children[i], x, y, clip, want);
    }
    if (!found && wants(n, want) && point_in(n, x, y) && clip_admits(clip, x, y)) {
        found = n;
    }
    if (pushed) {
        *clip = saved;
    }
    return found;
}

// --- instrumentation ------------------------------------------------------
// P44 dropped the ancestor-frame cull (it was the bug) and justified the full
// walk with "trees are tens of nodes at a depth under ten, and this runs once
// per pointer event, not per frame". The first half is checkable and the second
// half is WRONG: stamp_press() in app.c re-hit-tests at the frozen press point
// on EVERY BUILD while the press spring is live, so during a press this runs at
// frame rate, not event rate. That is the case worth measuring, so the cost is
// measurable rather than asserted. ZELTO_HIT_STATS=1 prints the worst walk seen.
//
// MEASURED (P45, simulator at 720x1440, ZELTO_HIT_STATS=1 with a held press so
// stamp_press runs the walk every build):
//     launcher, home carousel — the largest tree in the OS: WORST 124 NODES,
//     WORST 6.1us per walk.
// So P44's "tens of nodes" was low by about 4x, and its "not per frame" was
// simply wrong. Both corrections leave the conclusion standing, which is why the
// cull stays out: 6.1us against a 16,667us frame at 60Hz is 0.04% of the budget,
// on the worst tree, in the worst mode (per frame rather than per event). A cull
// would buy back four hundredths of one percent and reintroduce the class of bug
// that made a visible node untappable for thirteen phases. The number is written
// down here so the next phase can re-measure instead of re-arguing.
//
// The env check is hoisted so that when the instrumentation is OFF the two
// clock_gettime calls are skipped as well — an "off" measurement that still
// costs two syscalls on a per-frame path is not off.
static bool hit_stats_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("ZELTO_HIT_STATS");
        on = (e && *e && *e != '0') ? 1 : 0;
    }
    return on == 1;
}

static void hit_stats_report(double us) {
    static unsigned long worst_nodes;
    static double worst_us;
    static unsigned long calls;
    calls++;
    if (z_hit_nodes > worst_nodes) {
        worst_nodes = z_hit_nodes;
    }
    if (us > worst_us) {
        worst_us = us;
    }
    // Report on a cadence: a per-call line would itself dominate the cost.
    //
    // NAMED, because the whole System UI boots at once and every surface writes
    // to the same serial log — an anonymous "worst 124 nodes" cannot say WHICH
    // tree that was, which is the only interesting part of the number. P45 meant
    // to add this and the edit silently did not apply, so its measurement had to
    // be attributed by running one surface at a time. program_invocation_short_name
    // is the binary's name (_GNU_SOURCE is on for the whole build), which is
    // exactly the granularity wanted here: one process, one surface.
    if (calls % 32 == 0) {
        fprintf(stderr,
                "zelto: hit-test stats [%s]: %lu calls, worst %lu nodes, "
                "worst %.1fus, this %lu nodes/%.1fus\n",
                program_invocation_short_name, calls, worst_nodes, worst_us,
                z_hit_nodes, us);
    }
}

ZView z_hit_test(ZView root, double x, double y, ZHitWant want) {
    if (!root) {
        return NULL;
    }
    bool stats = hit_stats_on();
    struct timespec t0 = {0, 0};
    if (stats) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        z_hit_nodes = 0;
    }
    // The surface bounds everything: a node scrolled or slid off the display is
    // not reachable, whatever its frame says.
    HitClip clip = {
        .x0 = root->x, .y0 = root->y,
        .x1 = root->x + root->w, .y1 = root->y + root->h,
        .n_rc = 0,
    };
    ZView r = clip_admits(&clip, x, y) ? hit_walk(root, x, y, &clip, want) : NULL;
    if (stats) {
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        hit_stats_report((double)(t1.tv_sec - t0.tv_sec) * 1e6
                         + (double)(t1.tv_nsec - t0.tv_nsec) / 1e3);
    }
    return r;
}
