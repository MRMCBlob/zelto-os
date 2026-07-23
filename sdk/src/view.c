// libzelto view-tree builders + the per-build arena. These produce the immutable
// ZView tree that body() returns; the framework lays it out and renders it.
// See docs/contributing/sdk-internals.md ("View tree vs. scene graph").
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

ZArena *z_build_arena = NULL;

// --- arena ----------------------------------------------------------------
#define Z_CHUNK_MIN (256u * 1024u)

static ZChunk *chunk_new(size_t cap) {
    ZChunk *c = malloc(sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->data = malloc(cap);
    if (!c->data) {
        free(c);
        return NULL;
    }
    c->next = NULL;
    c->cap = cap;
    c->used = 0;
    return c;
}

void *z_arena_alloc(ZArena *arena, size_t size) {
    size = (size + 15u) & ~(size_t)15u;  // 16-byte align

    if (!arena->cur) {
        // First use (or after a full free): start a chunk list.
        size_t cap = size > Z_CHUNK_MIN ? size : Z_CHUNK_MIN;
        arena->head = arena->cur = chunk_new(cap);
        if (!arena->cur) {
            return NULL;
        }
    }

    // Bump within the current chunk; if it won't fit, advance to (or append) a
    // chunk that does. Existing chunks are never moved, so live pointers stay valid.
    if (arena->cur->used + size > arena->cur->cap) {
        if (arena->cur->next && size <= arena->cur->next->cap) {
            arena->cur = arena->cur->next;
            arena->cur->used = 0;
        } else {
            size_t cap = size > Z_CHUNK_MIN ? size : Z_CHUNK_MIN;
            ZChunk *c = chunk_new(cap);
            if (!c) {
                return NULL;
            }
            c->next = arena->cur->next;  // splice (handles the reuse-too-small case)
            arena->cur->next = c;
            arena->cur = c;
        }
    }

    void *p = arena->cur->data + arena->cur->used;
    arena->cur->used += size;
    memset(p, 0, size);
    return p;
}

char *z_arena_strdup(ZArena *arena, const char *s) {
    size_t n = strlen(s) + 1;
    char *p = z_arena_alloc(arena, n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

// Rewind for reuse without freeing: next build refills the existing chunks.
void z_arena_reset(ZArena *arena) {
    for (ZChunk *c = arena->head; c; c = c->next) {
        c->used = 0;
    }
    arena->cur = arena->head;
}

void z_arena_free(ZArena *arena) {
    ZChunk *c = arena->head;
    while (c) {
        ZChunk *next = c->next;
        free(c->data);
        free(c);
        c = next;
    }
    arena->head = arena->cur = NULL;
}

static ZView node_new(ZKind kind) {
    struct ZNode *n = z_arena_alloc(z_build_arena, sizeof(*n));
    n->kind = kind;
    n->fg = Z_COLOR_TEXT;
    n->font_size = z_font_units(Z_FONT_BODY);
    return n;
}

// --- builders -------------------------------------------------------------
ZView z_stack(ZAxis axis, const ZStackOpts *opts) {
    ZView n = node_new(Z_K_STACK);
    n->axis = axis;
    n->align = opts->align;
    n->spacing = opts->spacing;
    n->padding = opts->padding;
    n->grow = opts->grow;
    for (int i = 0; i < Z_MAX_CHILDREN; i++) {
        if (!opts->children[i]) {
            break;
        }
        n->children[n->n_children++] = opts->children[i];
    }
    return n;
}

ZView z_spacer(void) {
    ZView n = node_new(Z_K_SPACER);
    n->grow = 1.0f;
    return n;
}

ZView z_rect(const ZRectOpts *opts) {
    ZView n = node_new(Z_K_RECT);
    n->color = opts->color;
    n->fixed_w = opts->width;
    n->fixed_h = opts->height;
    n->radius = opts->radius;
    n->grow = opts->grow;
    return n;
}

ZView z_text(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    ZView n = node_new(Z_K_TEXT);
    n->text = z_arena_strdup(z_build_arena, buf);
    return n;
}

// --- WrapText -------------------------------------------------------------
// Prose that fits its column. The split happens HERE, at build time, with the
// same shaper the renderer will use — see the long note above z_wrap_lines() in
// layout.c for why this is a builder and not a layout pass.
//
// Each line is a Text node over a SLICE of the caller's string ("%.*s"), so a
// paragraph costs one arena copy per line and no allocation for the split.
typedef struct WrapUD {
    ZText *text;
    float size;
    ZWeight weight;
} WrapUD;

// Measure a SLICE. No buffer: z_text_measure_n shapes exactly `len` bytes of the
// caller's string. The first version of this copied each slice into a 512-byte
// stack buffer to NUL-terminate it and CLAMPED to the buffer on overflow, which
// is the worst possible failure for a line breaker — a truncated slice measures
// SHORT, so the breaker decides it fits and emits a line that runs off the
// column. That is the exact bug WrapText exists to prevent, reintroduced inside
// WrapText's own measurement, and silent.
//
// When the font failed to open there is no shaper at all. Returning 0 here would
// mean "nothing is ever too wide", so the whole paragraph would come back as one
// line and overflow — see the fallback in z_text_measure_n, which estimates from
// the byte count instead so the wrap still breaks at roughly the right place.
static float wrap_measure(void *ud, const char *s, int len) {
    WrapUD *w = ud;
    if (len <= 0) {
        return 0.0f;
    }
    return z_text_measure_n(w->text, s, len, w->size, w->weight, NULL, NULL);
}

// One group of at most Z_MAX_CHILDREN lines.
static ZView wrap_group(const ZWrapLine *lines, int n, const ZWrapOpts *opts,
                        float size) {
    ZStackOpts col = {.spacing = opts->line_gap, .align = Z_ALIGN_LEADING};
    for (int i = 0; i < n; i++) {
        ZView t = z_text("%.*s", lines[i].len, lines[i].s);
        t->font_size = size;
        t->weight = opts->weight;
        if (opts->color.a) {
            t->fg = opts->color;
        }
        col.children[i] = t;
    }
    return z_stack(Z_AXIS_VERTICAL, &col);
}

// One line, cut to fit. See the contract in zelto/ui.h.
//
// The ellipsis is U+2026, not three periods: the bundled face has it (gid check
// aside, it measures 21.45 at Body against 24.38 for "..." — a real, narrower
// glyph, not three dots and not .notdef's box), and one glyph is what typography
// asks for.
//
// The scan is linear from the front and stops at the first prefix that does NOT
// fit, so it costs one shaping call per byte KEPT rather than per byte given —
// a 2KB payload cut at 40 characters measures 40 times, not 2000. Monotonicity
// is what makes that correct: adding a byte never shortens the line.
ZView z_text_ellipsize(ZApp *app, const char *s, const ZEllipsizeOpts *opts) {
    static const char ELL[] = "\xe2\x80\xa6";
    float size = z_font_units(opts->size > 0 ? opts->size : Z_FONT_BODY);
    ZText *t = z_app_text(app);
    const char *src = s ? s : "";
    int len = (int)strlen(src);

    ZView out;
    float full = z_text_measure_n(t, src, len, size, opts->weight, NULL, NULL);
    if (opts->width <= 0.0f || full <= opts->width) {
        out = z_text("%s", src);          // fits: untouched, no ellipsis
    } else {
        float ew = z_text_measure_n(t, ELL, 3, size, opts->weight, NULL, NULL);
        float avail = opts->width - ew;
        int cut = 0;
        for (int i = 1; i <= len; i++) {
            // Only ever cut on a character boundary: a continuation byte
            // (10xxxxxx) is the middle of a UTF-8 sequence, and half a sequence
            // is not a shorter string, it is a broken one.
            if (((unsigned char)src[i] & 0xC0) == 0x80) {
                continue;
            }
            if (z_text_measure_n(t, src, i, size, opts->weight, NULL, NULL) >
                avail) {
                break;
            }
            cut = i;
        }
        // A space immediately before an ellipsis reads as a gap in the sentence.
        while (cut > 0 && (src[cut - 1] == ' ' || src[cut - 1] == '\t')) {
            cut--;
        }
        // Even the ellipsis alone does not fit (a column narrower than one
        // glyph). Emit it anyway rather than nothing: an empty label says the
        // field is empty, which is a different and worse lie than a cut one.
        out = z_text("%.*s%s", cut, src, ELL);
    }
    out->font_size = size;
    out->weight = opts->weight;
    if (opts->color.a) {
        out->fg = opts->color;
    }
    return out;
}

// THE LINE CAP IS AN ARRAY SIZE, NOT A FACT ABOUT PROSE, so P45 removed it
// rather than reporting it. P44 capped at Z_MAX_CHILDREN and dropped the rest
// with no diagnostic; the brief that found it asked whether the right answer was
// "cap and warn" or "hard error". It is neither. A hard error kills the Store
// because a package description is long — the toolkit does not get to abort the
// program over content it was handed. And a warning still loses the text; the
// user, who cannot read stderr, sees a paragraph that simply stops.
//
// The 32 came from ZStackOpts.children[], so the fix is structural: fill a
// group, and if the breaker reports anything left, start another and stack the
// groups. A vertical stack of vertical stacks lays out identically to a flat one
// as long as the outer spacing is the same line_gap, which it is. That buys
// 32x32 = 1024 lines — past any prose a phone screen can hold — and only THERE,
// where it really is a caller error rather than an implementation limit, does it
// warn and stop.
ZView z_text_wrap(ZApp *app, const char *s, const ZWrapOpts *opts) {
    float size = z_font_units(opts->size > 0 ? opts->size : Z_FONT_BODY);
    WrapUD ud = {z_app_text(app), size, opts->weight};

    ZWrapLine lines[Z_MAX_CHILDREN];
    ZStackOpts outer = {.spacing = opts->line_gap, .align = Z_ALIGN_LEADING};
    int n_groups = 0;
    ZView first_line = NULL;
    int total = 0;

    const char *p = s ? s : "";
    while (*p && n_groups < Z_MAX_CHILDREN) {
        const char *rest = p;
        int n = z_wrap_lines(p, opts->width, wrap_measure, &ud, lines,
                             Z_MAX_CHILDREN, &rest);
        if (n <= 0 || rest == p) {
            break;                   // no progress: refuse to spin
        }
        total += n;
        if (n == 1 && n_groups == 0 && !*rest) {
            first_line = wrap_group(lines, 1, opts, size)->children[0];
        }
        outer.children[n_groups++] = wrap_group(lines, n, opts, size);
        p = rest;
    }
    if (*p) {
        // 1024 lines of prose in one WrapText. Not a limit anyone reaches by
        // accident, so say so instead of quietly ending the paragraph.
        static bool warned = false;
        if (!warned) {
            warned = true;
            fprintf(stderr,
                    "zelto: WrapText: prose exceeds %d lines; the remainder is "
                    "not drawn. Put prose this long in a Scroll, one WrapText "
                    "per block.\n",
                    Z_MAX_CHILDREN * Z_MAX_CHILDREN);
        }
    }
    if (total == 0) {
        return z_stack(Z_AXIS_VERTICAL, &outer);
    }
    // One line: return it bare, so the common case lays out exactly as a plain
    // Text does (a one-child stack is not the same node for a parent's measure).
    if (first_line) {
        return first_line;
    }
    if (n_groups == 1) {
        return outer.children[0];
    }
    return z_stack(Z_AXIS_VERTICAL, &outer);
}

ZView z_image(const char *path) {
    ZView n = node_new(Z_K_IMAGE);
    n->img_path = path ? z_arena_strdup(z_build_arena, path) : NULL;
    return n;
}

ZView z_stroke(const ZStrokeOpts *opts) {
    ZView n = node_new(Z_K_STROKE);
    int cnt = opts->count;
    if (cnt < 0) {
        cnt = 0;
    }
    if (cnt > 0 && opts->points) {
        n->stroke_pts = z_arena_alloc(z_build_arena, sizeof(float) * 2u * (size_t)cnt);
        if (n->stroke_pts) {
            memcpy(n->stroke_pts, opts->points, sizeof(float) * 2u * (size_t)cnt);
            n->stroke_n = cnt;
        }
    }
    n->stroke_w = opts->thickness > 0.0f ? opts->thickness : 2.0f;
    n->stroke_closed = opts->closed;
    n->color = opts->color;
    return n;
}

ZView z_button(ZAction on_tap, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    // A button is a padded, rounded, filled stack wrapping its label. Building
    // it from primitives keeps layout/paint/hit-test uniform (no new kind).
    ZView label = node_new(Z_K_TEXT);
    label->text = z_arena_strdup(z_build_arena, buf);
    // The filled action is a LIGHT surface (Z_COLOR_PRIMARY is near-white, not a
    // hue), so its label is dark ink — ON_PRIMARY, never TEXT_INV.
    label->fg = Z_COLOR_ON_PRIMARY;
    label->weight = Z_WEIGHT_SEMIBOLD;   // a control label carries its own weight

    ZView n = node_new(Z_K_STACK);
    n->axis = Z_AXIS_HORIZONTAL;
    n->align = Z_ALIGN_CENTER;
    n->padding = 14.0f;                 // ~14px inset -> >=44px tall at body size
    n->has_bg = true;
    n->bg = Z_COLOR_PRIMARY;
    n->radius = Z_RADIUS_CARD;
    n->on_tap = on_tap;
    n->focusable = true;
    n->children[n->n_children++] = label;
    return n;
}

// Tapping a text field focuses it + moves the caret (app.c then raises the on-
// screen keyboard via text-input-v3). Long-press selects the word under the
// finger; a drag extends the selection. The field pointer rides tap_data /
// long_press_data; the pan handler operates on the app's active field. All the
// geometry (mapping x to a byte offset) lives in app.c (z_field_*), which owns the
// laid-out tree it measures against.
static void field_on_tap(ZApp *app, void *state, void *data) {
    (void)state;
    z_field_tap(app, (ZTextField *)data);
}
static void field_on_long_press(ZApp *app, void *state, void *data, float x,
                                float y) {
    (void)state; (void)y;
    z_field_select_word(app, (ZTextField *)data, x);
}
static void field_on_pan(ZApp *app, void *state, const ZPanEvent *e) {
    (void)state;
    ZTextField *f = z_app_active_field(app);
    if (f) {
        z_field_drag_extend(app, f, e->x, e->phase == Z_PAN_BEGIN);
    }
}

// One run of the field's text (a byte slice), optionally highlighted (selection).
static ZView field_run(const char *text, int a, int b, bool highlight) {
    // Three bytes per buffer byte: a SECURE field's runs are a masked copy whose
    // bullet is three UTF-8 bytes wide (P47), so a buffer sized to the field's own
    // capacity would silently truncate a long password's dots at a third of it.
    char buf[Z_TEXTFIELD_CAP * 3];
    int n = b - a;
    if (n < 0) {
        n = 0;
    }
    if (n >= (int)sizeof(buf)) {
        n = (int)sizeof(buf) - 1;
    }
    memcpy(buf, text + a, (size_t)n);
    buf[n] = '\0';
    ZView t = node_new(Z_K_TEXT);
    t->text = z_arena_strdup(z_build_arena, buf);
    t->fg = Z_COLOR_TEXT;
    if (highlight) {
        t->has_bg = true;
        // Selected text is a LIGHT plate with dark ink on it — the same
        // "lit" treatment every other active control in the system uses.
        t->bg = Z_COLOR_PRIMARY;
        t->fg = Z_COLOR_ON_PRIMARY;
    }
    return t;
}
// A thin blinking-style caret (shown at the insertion point, no selection).
static ZView field_caret(void) {
    ZView c = node_new(Z_K_RECT);
    c->color = Z_COLOR_TEXT;
    c->fixed_w = 2.0f;
    c->fixed_h = 22.0f;
    return c;
}
// A selection drag handle (a taller accent bar at each selection end).
static ZView field_handle(void) {
    ZView h = node_new(Z_K_RECT);
    h->color = Z_COLOR_ACCENT;
    h->fixed_w = 4.0f;
    h->fixed_h = 30.0f;
    h->radius = 2.0f;
    return h;
}

// An editable text field. Built from primitives (a rounded box + text runs + an
// optional caret / selection handles) so layout/paint/hit-test stay uniform — no
// new node kind. The `field` pointer marks it for app.c (which routes committed
// characters + selection edits into the buffer). Segments are laid out with zero
// spacing so they read as one line and the x->offset mapping stays accurate.
ZView z_text_field(ZApp *app, ZTextField *f, const char *placeholder) {
    bool active = f && z_app_field_active(app, f);
    bool empty = !f || f->len == 0;

    ZView n = node_new(Z_K_STACK);
    n->axis = Z_AXIS_HORIZONTAL;
    n->align = Z_ALIGN_CENTER;
    n->spacing = 0.0f;
    n->padding = 12.0f;                 // ~44px tall at body size
    n->has_bg = true;
    // A focused field lifts one step up the surface ramp — it does NOT grow a
    // coloured ring. (The old ring was a raw azure hex, the last one in the SDK.)
    n->bg = active ? Z_COLOR_SURFACE_3 : Z_COLOR_SURFACE_2;
    n->radius = Z_RADIUS_CARD;
    n->on_tap_data = field_on_tap;
    n->tap_data = f;
    n->field = f;
    n->field_active = active;
    if (f) {
        n->on_long_press = field_on_long_press;
        n->long_press_data = f;
        n->on_pan = field_on_pan;
    }

    if (empty) {
        ZView label = node_new(Z_K_TEXT);
        label->text = z_arena_strdup(z_build_arena, placeholder ? placeholder : "");
        label->fg = Z_COLOR_TEXT_FAINT;   // dim placeholder
        n->children[n->n_children++] = label;
        if (active) {
            n->children[n->n_children++] = field_caret();
        }
        return n;
    }

    int caret = f->caret < 0 ? 0 : (f->caret > f->len ? f->len : f->caret);
    int anchor = f->anchor < 0 ? 0 : (f->anchor > f->len ? f->len : f->anchor);
    int lo = anchor < caret ? anchor : caret;
    int hi = anchor < caret ? caret : anchor;
    bool sel = active && lo != hi;

    // A SECURE field (P47) never paints its own text. The runs are built from a
    // masked copy rather than by asking the renderer to substitute, so the
    // caret/selection arithmetic below stays exactly what it was: one bullet per
    // BYTE of the buffer, and every offset scaled by the bullet's width in bytes.
    // (The bullet is U+2022, three bytes; the field's offsets are byte offsets.)
    static const char DOT[] = "\xe2\x80\xa2";
    char mask[Z_TEXTFIELD_CAP * 3];
    const char *shown = f->text;
    int mul = 1;
    if (f->secure) {
        int m = 0;
        for (int i = 0; i < f->len && m + 4 <= (int)sizeof(mask); i++) {
            memcpy(mask + m, DOT, 3);
            m += 3;
        }
        mask[m] = '\0';
        shown = mask;
        mul = 3;
    }
    int len = f->len * mul;
    lo *= mul;
    hi *= mul;
    caret *= mul;

    if (sel) {
        // [before] |handleL| [selected+highlight] |handleR| [after]
        if (lo > 0) {
            n->children[n->n_children++] = field_run(shown, 0, lo, false);
        }
        n->children[n->n_children++] = field_handle();
        n->children[n->n_children++] = field_run(shown, lo, hi, true);
        n->children[n->n_children++] = field_handle();
        if (hi < len) {
            n->children[n->n_children++] = field_run(shown, hi, len, false);
        }
    } else if (active) {
        // [before caret] |caret| [after caret]
        if (caret > 0) {
            n->children[n->n_children++] = field_run(shown, 0, caret, false);
        }
        n->children[n->n_children++] = field_caret();
        if (caret < len) {
            n->children[n->n_children++] =
                field_run(shown, caret, len, false);
        }
    } else {
        n->children[n->n_children++] = field_run(shown, 0, len, false);
    }
    return n;
}

// The floating Copy / Cut / Paste / Select-all action bar for the focused field's
// selection. NULL when there is no selection to act on. Built from ordinary
// Buttons wired to the z_field_* actions; the app drops it into its body (e.g.
// pinned above the field) while a selection is live.
static void sel_copy(ZApp *app, void *s) { (void)s; z_field_copy(app); }
static void sel_cut(ZApp *app, void *s) { (void)s; z_field_cut(app); }
static void sel_paste(ZApp *app, void *s) { (void)s; z_field_paste(app); }
static void sel_all(ZApp *app, void *s) { (void)s; z_field_select_all(app); }

ZView z_selection_bar(ZApp *app) {
    ZTextField *f = z_app_active_field(app);
    if (!f) {
        return NULL;   // no focused field: no bar
    }
    // Always render all four actions so their positions are fixed regardless of
    // whether there is a selection (Copy/Cut are no-ops without one). Paste and
    // Select-all are useful on a bare caret (Android shows a paste bubble on tap),
    // so the bar appears whenever a field is focused, not only on a selection.
    // Plain Buttons in an HStack — the same tappable primitive an app's own buttons
    // use — so the bar acts through ordinary hit-testing.
    return HStack(
        Button(sel_copy, "Copy"),
        Button(sel_cut, "Cut"),
        Button(sel_paste, "Paste"),
        Button(sel_all, "Select all"),
        .spacing = Z_SPACE_S, .align = Z_ALIGN_CENTER);
}

// --- modifiers ------------------------------------------------------------
ZView Background(ZColor color, ZView view) {
    view->has_bg = true;
    view->bg = color;
    return view;
}

ZView Foreground(ZColor color, ZView view) {
    view->fg = color;
    return view;
}

ZView Padding(float all, ZView view) {
    view->padding += all;
    return view;
}

ZView Frame(float width, float height, ZView view) {
    view->fixed_w = width;
    view->fixed_h = height;
    return view;
}

ZView CornerRadius(float radius, ZView view) {
    view->radius = radius;
    return view;
}

// Clip(): mask the SUBTREE to this node's frame, rounded. `clip` is the same flag
// a scroll viewport sets (it bounds which pixels are visited); clip_radius is the
// corner mask the renderer then applies inside it. Deliberately NOT the same field
// as `radius`: radius is what this node paints for itself, clip_radius is what it
// imposes on its children, and a node routinely wants one without the other — a
// slab that is a rounded surface AND clips its fill sets both, a bare clipping
// frame with no paint of its own sets only the second.
ZView Clip(float radius, ZView view) {
    view->clip = true;
    view->clip_radius = radius;
    return view;
}

ZView Cover(ZView view) {
    // Aspect-fill an Image: scale so the frame is fully covered, center-cropping
    // the overflow (vs the default aspect-fit, which letterboxes). Only meaningful
    // on an Image node; elsewhere it is an inert flag.
    view->img_cover = true;
    return view;
}

// THE SEAM (P50). Nineteen surfaces call this with a compile-time step and none
// of them knows the user has a text size; z_font_units() is where the step
// becomes a number, so the setting reaches all of them without any of them
// changing. Anything that assigns node->font_size directly must go through it
// too — see z_text_wrap / z_text_ellipsize, which resolve the size once and
// stamp it onto the Text nodes they build.
ZView Font(ZFont size, ZView view) {
    view->font_size = z_font_units(size);
    return view;
}

ZView Weight(ZWeight weight, ZView view) {
    view->weight = weight;
    return view;
}

ZView Grow(float weight, ZView view) {
    view->grow = weight;
    return view;
}

// Same weight, but the child's intrinsic main-axis size is dropped so the weights
// alone divide the axis. See the contrast with Grow in zelto/ui.h.
ZView Share(float weight, ZView view) {
    view->grow = weight;
    view->grow_share = true;
    return view;
}

ZView Shadow(float elevation, ZView view) {
    view->elevation = elevation;
    return view;
}

ZView TextShadow(ZView view) {
    view->text_shadow = true;
    return view;
}

ZView Fill(ZView view) {
    view->fill = true;
    return view;
}

ZView Opacity(float amount, ZView view) {
    // Stored as the complement (fade = 1 - opacity) so the arena-zero default is
    // fully opaque. The renderer multiplies effective alpha by (1 - fade).
    if (amount < 0.0f) {
        amount = 0.0f;
    } else if (amount > 1.0f) {
        amount = 1.0f;
    }
    view->fade = 1.0f - amount;
    return view;
}

ZView OnTap(ZAction action, ZView view) {
    view->on_tap = action;
    return view;
}

ZView OnTapData(ZTapAction action, void *data, ZView view) {
    view->on_tap_data = action;
    view->tap_data = data;
    return view;
}

ZView OnPan(ZPanHandler handler, ZView view) {
    view->on_pan = handler;
    return view;
}

ZView OnPanData(ZPanDataHandler handler, void *data, ZView view) {
    view->on_pan_data = handler;
    view->pan_data = data;
    return view;
}

ZView OnLongPress(ZLongPressHandler handler, void *data, ZView view) {
    view->on_long_press = handler;
    view->long_press_data = data;
    return view;
}

ZView OnKey(ZKeyAction action, ZView view) {
    view->on_key = action;
    view->focusable = true;
    return view;
}

// --- widgets --------------------------------------------------------------
// A home-screen widget: a titled card whose live content a callback builds,
// self-refreshing on a declared cadence. The chrome is a rounded, padded Surface
// card (opaque, so it stays legible over the wallpaper with no alpha-blend
// bookkeeping) with the title as a muted caption above the content. When
// refresh_ms > 0 we arm the shared repeating tick so the card re-renders on its
// own beat (a clock). The body callback runs during this build and returns the
// content view; it receives the live app + the app's state pointer.
ZView z_widget(ZApp *app, const ZWidgetOpts *opts) {
    if (!opts) {
        return z_spacer();
    }
    if (opts->refresh_ms > 0) {
        z_tick_every(app, opts->refresh_ms);
    }
    ZView content =
        opts->body ? opts->body(app, z_app_state(app)) : z_spacer();

    // The card: content FIRST, caption last. A widget is read at a glance, and the
    // thing worth reading is its value, not its name — putting the title on top
    // (as this did) spends the strongest position in the card on the least useful
    // line and leaves the value floating in the middle of a labelled box. Bottom-
    // aligning the caption also gives every widget the same baseline, so a row of
    // them reads as a set (Law of Similarity) whatever their content is.
    ZStackOpts col = {.spacing = Z_SPACE_XS, .align = Z_ALIGN_LEADING};
    int k = 0;
    col.children[k++] = content;
    col.children[k++] = z_spacer();
    if (opts->title && opts->title[0]) {
        col.children[k++] = Weight(Z_WEIGHT_MEDIUM,
            Foreground(Z_COLOR_TEXT_FAINT,
                Font(Z_FONT_CAPTION2, z_text("%s", opts->title))));
    }

    // The MATERIAL: a translucent tint the wallpaper shows through, lifted by a
    // soft shadow — not a flat slab. Over the home the launcher owns the wallpaper
    // pixels, so this is the tint alone (there is nothing behind the launcher to
    // blur).
    //
    // The 1px HAIRLINE around it is what stops a translucent panel dissolving into
    // a busy wallpaper: it is drawn as an outer node filled with the edge colour
    // whose 1px padding lets the ring show around the material inside it. (The
    // modifiers mutate the node they are given rather than wrapping it, so the ring
    // needs a real second node — hence the depth stack.)
    // Z_RADIUS_WIDGET, not Z_RADIUS_PANEL: a widget card must read ROUNDER than
    // the app icons beside it on the same home screen, and at PANEL's 22 units
    // against the icon's 23.3 it read very slightly squarer. See the ladder note
    // in gfx.h for the measurement.
    ZView fill = Background(Z_COLOR_MATERIAL_REGULAR,
        CornerRadius(Z_RADIUS_WIDGET - 1.0f,
            Padding(Z_SPACE_S, z_stack(Z_AXIS_VERTICAL, &col))));
    return Shadow(Z_ELEV_2,
        Background(Z_COLOR_MATERIAL_EDGE,
            CornerRadius(Z_RADIUS_WIDGET,
                ZStack(Fill(fill), .padding = 1.0f))));
}
