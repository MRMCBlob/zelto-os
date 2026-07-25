// test_round_clip_cap — what happens past Z_MAX_ROUND_CLIPS, in BOTH walks.
//
// THE TWO CLAIMS UNDER TEST, neither of which had ever been run.
//
// P44 capped the rounded-clip stack at Z_MAX_ROUND_CLIPS (4) and made the
// overflow warn once per process, because a soft-fail invisible in a screenshot
// gets diagnosed as the wrong radius token. But NOTHING IN THE OS BUILDS FIVE
// NESTED ROUNDED CLIPS — the deepest chain the system UI has is three — so "it
// warns" was a code-review claim about a branch no run had entered. The brief
// that found it was explicit: anything meant to fail must be negative-tested.
//
// P44 also asserted that hit-testing carries "the same push/pop nodes, the same
// Z_MAX_ROUND_CLIPS cap, so hit and paint degrade identically". That is the more
// interesting claim, and it was equally untested. If it were false the failure
// would be the P44 bug class returning: a corner you can see but cannot tap, or
// one you can tap but cannot see.
//
// HOW THE DEGRADE IS MADE OBSERVABLE. The cap drops the ROUNDED half of the
// innermost mask and keeps the rectangular half, so the innermost shape silently
// gets SQUARE corners. So the probe is a point in the innermost clip's corner —
// outside its rounded mask, inside its rectangle:
//
//     nesting <= 4  -> the rounding applies -> corner is masked -> NOT painted,
//                      NOT tappable
//     nesting == 5  -> the 5th rounding is dropped -> corner is square ->
//                      PAINTED, and tappable
//
// Reading the actual PIXEL for the paint side (rather than trusting the code) is
// what makes "identically" a measurement instead of a restatement: the same
// probe point is put to the renderer and to the hit walk, and they must agree.
//
// The outer clips are given a radius of 1.0 — over the 0.5 threshold, so they
// still occupy a slot on the stack, but with a corner too small to mask the
// probe point. That isolates the innermost clip as the only shape whose rounding
// the probe can see, so depth is the only variable between the two cases.
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

#include "framework/ztest.h"

// layout.c's measure() calls these for Z_K_TEXT / Z_K_IMAGE. The trees here are
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
// render.c's paint() calls these for the kinds this test never builds.
void z_text_draw(ZCanvas *canvas, const char *s, float size, ZWeight weight,
                 ZColor color, float pen_x, float pen_y) {
    (void)canvas; (void)s; (void)size; (void)weight; (void)color;
    (void)pen_x; (void)pen_y;
}
const ZImage *z_image_get(const char *path) { (void)path; return NULL; }

#include "layout.c"
#include "render.c"
// The palette became a runtime lookup in P54 (z_token), so render.c's
// Z_COLOR_SHADOW / Z_COLOR_ACCENT / Z_COLOR_PRESS are calls now rather than
// static inlines. This file compiles the renderer standalone rather than
// linking libzelto, so it has to bring the palette with it.
#include "theme.c"

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
static void reset(void) { g_used = 0; }

static void tap_noop(ZApp *app, void *state) { (void)app; (void)state; }

// The tree is laid out on a surface exactly the size of the box, NOT on the
// phone: z_layout() arranges the root to FILL whatever surface it is given
// (a fixed size on the root is overridden), and a depth stack then centres its
// child — so laying this out on 720x1440 would put the shape at (160, 520),
// outside the scratch canvas the probe reads.
// The innermost shape: a box at a known place with a big corner radius.
#define BOX 400.0f
#define INNER_R 100.0f

// Build `depth` nested clip nodes around one tappable, filled leaf. Every node
// is the same BOXxBOX frame at the origin, so the rectangles coincide and only
// the CORNER radii differ. Returns the root; *leaf receives the painted node.
static ZView build(int depth, ZView *leaf) {
    reset();
    ZView l = nd(Z_K_RECT);
    l->fixed_w = BOX;
    l->fixed_h = BOX;
    l->color = z_rgba(0xff, 0xff, 0xff, 0xff);   // opaque: a painted pixel is easy to read
    l->on_tap = tap_noop;
    *leaf = l;

    ZView child = l;
    for (int i = 0; i < depth; i++) {
        ZView c = nd(Z_K_STACK);
        c->axis = Z_AXIS_DEPTH;
        c->fixed_w = BOX;
        c->fixed_h = BOX;
        c->clip = true;
        // The INNERMOST clip (built first) is the one whose rounding the probe
        // point can see; every clip outside it is effectively square but still
        // consumes a slot on the stack.
        c->clip_radius = (i == 0) ? INNER_R : 1.0f;
        c->children[c->n_children++] = child;
        child = c;
    }
    return child;
}

// Render into a scratch canvas and report whether the probe pixel got ink.
static bool paints_at(ZView root, int px, int py) {
    static uint32_t pixels[(int)BOX * (int)BOX];
    memset(pixels, 0, sizeof(pixels));
    ZCanvas c = {
        .pixels = pixels,
        .width = (int)BOX, .height = (int)BOX, .stride_px = (int)BOX,
        .clip_x0 = 0, .clip_y0 = 0, .clip_x1 = (int)BOX, .clip_y1 = (int)BOX,
        .n_rclip = 0, .text = NULL,
    };
    z_render(&c, root);
    // Any alpha at all counts as painted: the corner is antialiased, so the
    // question is "did ink reach here", not "is it fully opaque".
    return (pixels[py * (int)BOX + px] >> 24) != 0;
}

// Capture stderr for the duration of one render, so the one-shot warning can be
// observed. Returns true if `needle` appeared.
static bool render_captures_warning(ZView root, const char *needle) {
    char path[] = "/tmp/zelto-clipcap.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        return false;
    }
    fflush(stderr);
    int saved = dup(fileno(stderr));
    dup2(fd, fileno(stderr));

    static uint32_t pixels[(int)BOX * (int)BOX];
    ZCanvas c = {
        .pixels = pixels,
        .width = (int)BOX, .height = (int)BOX, .stride_px = (int)BOX,
        .clip_x0 = 0, .clip_y0 = 0, .clip_x1 = (int)BOX, .clip_y1 = (int)BOX,
        .n_rclip = 0, .text = NULL,
    };
    z_render(&c, root);

    fflush(stderr);
    dup2(saved, fileno(stderr));
    close(saved);

    FILE *f = fopen(path, "r");
    char buf[4096] = {0};
    size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    buf[n] = '\0';
    if (f) { fclose(f); }
    close(fd);
    remove(path);
    return strstr(buf, needle) != NULL;
}

int main(void) {
    // The probe: inside the innermost clip's top-left corner BOX, but outside
    // the quarter-superellipse of radius INNER_R that rounds it.
    //
    // The curve is |x|^4 + |y|^4 = r^4 measured from the corner's centre, so a
    // diagonal point p is OUTSIDE when 2*((r-p)/r)^4 > 1, i.e. p < r*(1-2^-0.25)
    // = 0.159r. (r/4 is INSIDE — 2*0.75^4 = 0.63 — which is worth writing down
    // because it is the first thing this test got wrong.) r/10 gives 1.31 and
    // is comfortably outside the curve while still well inside the rectangle.
    const int PX = (int)(INNER_R / 10.0f), PY = (int)(INNER_R / 10.0f);

    // --- A. within the cap: the innermost rounding APPLIES -------------------
    // Four nested clips fit on the stack, so the corner really is cut. Both the
    // renderer and the hit walk must agree that nothing is there.
    ZView leaf = NULL;
    {
        ZView root = build(4, &leaf);
        z_layout(root, BOX, BOX, NULL);
        EXPECT_TRUE(!paints_at(root, PX, PY));
        EXPECT_TRUE(z_hit_test(root, PX, PY, Z_HIT_TAP) == NULL);

        // Guard the guard: a point well inside the shape IS both painted and
        // tappable, so case A is not passing because the tree is empty.
        EXPECT_TRUE(paints_at(root, (int)BOX / 2, (int)BOX / 2));
        EXPECT_TRUE(z_hit_test(root, BOX / 2.0, BOX / 2.0, Z_HIT_TAP) == leaf);
    }

    // --- B. within the cap, it does NOT warn --------------------------------
    // Checked BEFORE the overflow case, because the warning is one-shot per
    // process: if four levels warned, the assertion in C could not tell the two
    // apart.
    {
        ZView root = build(4, &leaf);
        z_layout(root, BOX, BOX, NULL);
        EXPECT_TRUE(!render_captures_warning(root, "nested rounded clips"));
    }

    // --- C. past the cap: it WARNS ------------------------------------------
    // THE BRANCH P44 ADDED AND NEVER RAN.
    {
        ZView root = build(5, &leaf);
        z_layout(root, BOX, BOX, NULL);
        EXPECT_TRUE(render_captures_warning(root, "nested rounded clips"));
    }

    // --- D. and it warns exactly ONCE per process ---------------------------
    // The documented behaviour: a per-frame warning on a 60Hz repaint would bury
    // the log it is supposed to make readable.
    {
        ZView root = build(5, &leaf);
        z_layout(root, BOX, BOX, NULL);
        EXPECT_TRUE(!render_captures_warning(root, "nested rounded clips"));
    }

    // --- E. HIT AND PAINT DEGRADE IDENTICALLY -------------------------------
    // The claim that actually matters. Past the cap the innermost rounding is
    // dropped, so the corner becomes SQUARE — and the renderer and the hit walk
    // must both say so. Same probe point, same tree, opposite answer to case A:
    // painted now, and tappable now.
    {
        ZView root = build(5, &leaf);
        z_layout(root, BOX, BOX, NULL);
        bool painted = paints_at(root, PX, PY);
        bool tappable = z_hit_test(root, PX, PY, Z_HIT_TAP) == leaf;
        EXPECT_TRUE(painted);
        EXPECT_TRUE(tappable);
        // Stated as the invariant rather than as two separate facts, so a future
        // change that breaks the symmetry fails here with the right message.
        if (painted != tappable) {
            zt_fail_(__FILE__, __LINE__,
                     "past Z_MAX_ROUND_CLIPS, paint and hit disagree about the "
                     "innermost corner",
                     "painted == tappable",
                     painted ? "painted, not tappable" : "tappable, not painted");
        }
    }

    return zt_result();
}
