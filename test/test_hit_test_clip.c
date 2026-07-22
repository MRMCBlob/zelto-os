// test_hit_test_clip — a node is tappable exactly where it is painted.
//
// WHAT THIS PINS. libzelto paints a node wherever layout put it and masks a
// subtree at exactly one kind of node: one with `clip` set (a Scroll viewport,
// or an explicit Clip()). Hit testing has to answer for the SAME region. Until
// P44 it did not: the walk descended only into children of nodes whose own
// frame contained the point, so the touchable region was the intersection of
// every ancestor's frame — neither the painted region nor the clip stack.
//
// Two mismatches came out of that, and they point in opposite directions, which
// is why neither was obvious from the code:
//
//   1. VISIBLE, NOT TAPPABLE (the big one). A node painted outside a
//      NON-clipping ancestor — an OffsetXY'd home icon mid-drag, a screen
//      sliding through a transition, the last child of an overfull stack — was
//      dead to touch, because the walk turned back at the ancestor.
//   2. TAPPABLE, NOT VISIBLE (the small one). Clip(radius) masks the corners
//      away; the walk tested a rectangle, so the corner nubs of a rounded
//      viewport stayed live where nothing is drawn.
//
// AND ONE CLAIM THIS TEST REFUTES, deliberately kept as an assertion rather
// than deleted: a row scrolled out of a viewport was NOT tappable at the empty
// coordinates above it. The old ancestor-frame walk turned back at the viewport
// (whose frame is the visible box), so the scrolled-out case was already safe —
// by accident, via the same rule that caused bug 1. Case C below states that as
// a regression lock: it must stay false now that the accident is gone and the
// clip stack does the job on purpose.
//
// WHY THE TREES ARE BUILT BY HAND. The node builders in sdk/src/view.c drag in
// the text field, the clipboard and the app loop; the geometry under test is
// layout.c alone. So the test allocates ZNodes directly and sets the fields the
// builders set — Frame -> fixed_w/h, Clip -> clip + clip_radius, OnTap ->
// on_tap, OffsetXY -> off_x/off_y — and includes layout.c into this TU with the
// two measure-time externs stubbed. That keeps the uniform one-file compile
// recipe in test/README.md.
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "internal.h"

#include "framework/ztest.h"

// measure() calls these for Z_K_TEXT / Z_K_IMAGE nodes. The trees here are
// rectangles, so they are never reached; they exist to link.
float z_text_measure(ZText *t, const char *s, float size, ZWeight weight,
                     float *ascent, float *descent) {
    (void)t; (void)s; (void)size; (void)weight;
    if (ascent) { *ascent = size; }
    if (descent) { *descent = 0.0f; }
    return 0.0f;
}
bool z_image_intrinsic(const char *path, int *w, int *h) {
    (void)path; (void)w; (void)h;
    return false;
}

#include "layout.c"

// --- tiny tree builder ------------------------------------------------------
#define MAX_NODES 32
static struct ZNode g_nodes[MAX_NODES];
static int g_used;

static ZView nd(ZKind kind) {
    struct ZNode *n = &g_nodes[g_used++];
    memset(n, 0, sizeof(*n));
    n->kind = kind;
    return n;
}
static ZView add(ZView parent, ZView child) {
    parent->children[parent->n_children++] = child;
    return parent;
}
static ZView vstack(void) {
    ZView n = nd(Z_K_STACK);
    n->axis = Z_AXIS_VERTICAL;
    return n;
}
static ZView zstack(void) {
    ZView n = nd(Z_K_STACK);
    n->axis = Z_AXIS_DEPTH;
    return n;
}
static ZView rect(float w, float h) {          // Frame(w, h, Rect())
    ZView n = nd(Z_K_RECT);
    n->fixed_w = w;
    n->fixed_h = h;
    return n;
}
static ZView spacer(void) {
    ZView n = nd(Z_K_SPACER);
    n->grow = 1.0f;
    return n;
}
static ZView frame(ZView n, float w, float h) { n->fixed_w = w; n->fixed_h = h; return n; }
static ZView offset(ZView n, float dx, float dy) { n->off_x = dx; n->off_y = dy; return n; }
static ZView clip(ZView n, float radius) { n->clip = true; n->clip_radius = radius; return n; }

static void tap_noop(ZApp *app, void *state) { (void)app; (void)state; }
static ZView tappable(ZView n) { n->on_tap = tap_noop; return n; }

static void pan_noop(ZApp *app, void *state, const ZPanEvent *e) {
    (void)app; (void)state; (void)e;
}
static ZView pannable(ZView n) { n->on_pan = pan_noop; return n; }

static void reset(void) { g_used = 0; }

// The surface every case is laid out on: the phone, in screen units.
#define SCREEN_W 720.0f
#define SCREEN_H 1440.0f

static ZView tap_at(ZView root, double x, double y) {
    return z_hit_test(root, x, y, Z_HIT_TAP);
}

int main(void) {
    // --- A. painted outside a NON-clipping parent: visible => tappable -------
    // A 200x200 card in a 720x200 box, pushed 300px DOWN by OffsetXY. Nothing
    // clips it, so it paints at y 300..500 — well past the box's bottom edge,
    // over the empty space below. This is the drag-ghost / mid-transition case.
    reset();
    {
        ZView card = tappable(offset(rect(200.0f, 200.0f), 0.0f, 300.0f));
        ZView box = frame(add(vstack(), card), SCREEN_W, 200.0f);
        ZView root = add(add(vstack(), box), spacer());
        z_layout(root, SCREEN_W, SCREEN_H, NULL);

        // The card really is where we think it is (guard the guard: if layout
        // ever stops honouring off_y, the tap assertions below would pass for
        // the wrong reason).
        ASSERT_NEAR(300.0f, card->y, 0.5f);
        ASSERT_NEAR(200.0f, card->h, 0.5f);

        // THE BUG: a tap where the card is drawn must reach the card.
        EXPECT_TRUE(tap_at(root, 100.0, 400.0) == card);
        // ... and where it is NOT drawn (its unoffset slot) must not.
        EXPECT_TRUE(tap_at(root, 100.0, 100.0) == NULL);
    }

    // --- B. the same card inside a Clip(): masked => not tappable ------------
    reset();
    {
        ZView card = tappable(offset(rect(200.0f, 200.0f), 0.0f, 300.0f));
        ZView box = clip(frame(add(vstack(), card), SCREEN_W, 200.0f), 0.0f);
        ZView root = add(add(vstack(), box), spacer());
        z_layout(root, SCREEN_W, SCREEN_H, NULL);

        ASSERT_NEAR(300.0f, card->y, 0.5f);
        // Painted region is (300..500) INTERSECT (0..200) = nothing.
        EXPECT_TRUE(tap_at(root, 100.0, 400.0) == NULL);
        EXPECT_TRUE(tap_at(root, 100.0, 100.0) == NULL);
    }

    // --- C. scrolled out of a viewport: not tappable above it ---------------
    // The shape a Scroll produces: a clipping viewport at y 400..800 whose
    // content is translated up, carrying a row off the top edge to y 100..176.
    // A tap in the empty space above the viewport must find nothing, and a tap
    // inside the viewport where the row is NOT drawn must find nothing either.
    reset();
    {
        ZView row = tappable(offset(rect(SCREEN_W, 76.0f), 0.0f, -300.0f));
        ZView viewport = clip(frame(add(vstack(), row), SCREEN_W, 400.0f), 0.0f);
        ZView top_gap = rect(SCREEN_W, 400.0f);
        ZView root = add(add(add(vstack(), top_gap), viewport), spacer());
        z_layout(root, SCREEN_W, SCREEN_H, NULL);

        ASSERT_NEAR(400.0f, viewport->y, 0.5f);
        ASSERT_NEAR(100.0f, row->y, 0.5f);      // scrolled clean off the top

        EXPECT_TRUE(tap_at(root, 100.0, 150.0) == NULL);   // above the viewport
        EXPECT_TRUE(tap_at(root, 100.0, 600.0) == NULL);   // inside, row is gone
    }

    // --- D. Clip(radius): the masked-away corner is not tappable ------------
    // A 200x200 tap target clipped to a 64px radius, at the surface origin. The
    // top-left corner pixel is outside the mask: nothing is drawn there.
    reset();
    {
        ZView card = tappable(clip(rect(200.0f, 200.0f), 64.0f));
        ZView root = add(add(vstack(), card), spacer());
        z_layout(root, SCREEN_W, SCREEN_H, NULL);

        ASSERT_NEAR(0.0f, card->x, 0.5f);
        ASSERT_NEAR(0.0f, card->y, 0.5f);

        EXPECT_TRUE(tap_at(root, 4.0, 4.0) == NULL);        // cut-away corner
        EXPECT_TRUE(tap_at(root, 100.0, 100.0) == card);    // the middle
        EXPECT_TRUE(tap_at(root, 100.0, 4.0) == card);      // straight top edge
        EXPECT_TRUE(tap_at(root, 4.0, 100.0) == card);      // straight left edge
    }

    // --- E. ordinary trees still resolve front-to-back, deepest-first -------
    // Losing the ancestor-frame cull must not change who wins an overlap: the
    // LAST child of a ZStack paints on top, so it takes the tap.
    reset();
    {
        ZView back = tappable(rect(400.0f, 400.0f));
        ZView front = tappable(rect(200.0f, 200.0f));
        ZView stack = frame(add(add(zstack(), back), front), 400.0f, 400.0f);
        ZView root = add(add(vstack(), stack), spacer());
        z_layout(root, SCREEN_W, SCREEN_H, NULL);

        // ZStack centres each child at its own size: front covers the middle.
        EXPECT_TRUE(tap_at(root, 200.0, 200.0) == front);
        EXPECT_TRUE(tap_at(root, 20.0, 20.0) == back);
        EXPECT_TRUE(tap_at(root, 500.0, 500.0) == NULL);
    }

    // --- F. the surface bounds everything -----------------------------------
    // A node slid off the display is unreachable however large its frame.
    reset();
    {
        ZView card = tappable(offset(rect(200.0f, 200.0f), -400.0f, 0.0f));
        ZView root = add(add(vstack(), card), spacer());
        z_layout(root, SCREEN_W, SCREEN_H, NULL);

        ASSERT_NEAR(-400.0f, card->x, 0.5f);
        EXPECT_TRUE(tap_at(root, -300.0, 100.0) == NULL);
    }

    // --- G. the other three walkers share the rule ---------------------------
    // Scroll targets, pan targets and long-press targets are found by the same
    // walk with a different predicate, so the fix is not tap-only. A pan handler
    // painted outside its parent must be pannable where it is drawn.
    reset();
    {
        ZView card = pannable(offset(rect(200.0f, 200.0f), 0.0f, 300.0f));
        ZView box = frame(add(vstack(), card), SCREEN_W, 200.0f);
        ZView root = add(add(vstack(), box), spacer());
        z_layout(root, SCREEN_W, SCREEN_H, NULL);

        EXPECT_TRUE(z_hit_test(root, 100.0, 400.0, Z_HIT_PAN) == card);
        EXPECT_TRUE(z_hit_test(root, 100.0, 100.0, Z_HIT_PAN) == NULL);
        EXPECT_TRUE(z_hit_test(root, 100.0, 400.0, Z_HIT_TAP) == NULL);  // no on_tap
    }

    return zt_result();
}
