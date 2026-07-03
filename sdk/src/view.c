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
    n->font_size = (float)Z_FONT_BODY;
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

ZView z_image(const char *path) {
    ZView n = node_new(Z_K_IMAGE);
    n->img_path = path ? z_arena_strdup(z_build_arena, path) : NULL;
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
    label->fg = Z_COLOR_TEXT_INV;

    ZView n = node_new(Z_K_STACK);
    n->axis = Z_AXIS_HORIZONTAL;
    n->align = Z_ALIGN_CENTER;
    n->padding = 14.0f;                 // ~14px inset -> >=44px tall at body size
    n->has_bg = true;
    n->bg = Z_COLOR_PRIMARY;
    n->radius = 12.0f;
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
    char buf[Z_TEXTFIELD_CAP];
    int n = b - a;
    if (n < 0) {
        n = 0;
    }
    if (n >= Z_TEXTFIELD_CAP) {
        n = Z_TEXTFIELD_CAP - 1;
    }
    memcpy(buf, text + a, (size_t)n);
    buf[n] = '\0';
    ZView t = node_new(Z_K_TEXT);
    t->text = z_arena_strdup(z_build_arena, buf);
    t->fg = Z_COLOR_TEXT_INV;
    if (highlight) {
        t->has_bg = true;
        t->bg = z_rgba(0x2e, 0x6b, 0xd0, 0xff);   // selection highlight
    }
    return t;
}
// A thin blinking-style caret (shown at the insertion point, no selection).
static ZView field_caret(void) {
    ZView c = node_new(Z_K_RECT);
    c->color = Z_COLOR_TEXT_INV;
    c->fixed_w = 2.0f;
    c->fixed_h = 22.0f;
    return c;
}
// A selection drag handle (a taller accent bar at each selection end).
static ZView field_handle(void) {
    ZView h = node_new(Z_K_RECT);
    h->color = z_rgba(0x2e, 0x9b, 0xff, 0xff);
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
    n->bg = active ? z_rgba(0x22, 0x2b, 0x38, 0xff) : z_rgba(0x11, 0x16, 0x1f, 0xff);
    n->radius = 10.0f;
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
        label->fg = z_rgba(0x8a, 0x93, 0x9e, 0xff);   // dim placeholder
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

    if (sel) {
        // [before] |handleL| [selected+highlight] |handleR| [after]
        if (lo > 0) {
            n->children[n->n_children++] = field_run(f->text, 0, lo, false);
        }
        n->children[n->n_children++] = field_handle();
        n->children[n->n_children++] = field_run(f->text, lo, hi, true);
        n->children[n->n_children++] = field_handle();
        if (hi < f->len) {
            n->children[n->n_children++] = field_run(f->text, hi, f->len, false);
        }
    } else if (active) {
        // [before caret] |caret| [after caret]
        if (caret > 0) {
            n->children[n->n_children++] = field_run(f->text, 0, caret, false);
        }
        n->children[n->n_children++] = field_caret();
        if (caret < f->len) {
            n->children[n->n_children++] =
                field_run(f->text, caret, f->len, false);
        }
    } else {
        n->children[n->n_children++] = field_run(f->text, 0, f->len, false);
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
        .spacing = 8, .align = Z_ALIGN_CENTER);
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

ZView Cover(ZView view) {
    // Aspect-fill an Image: scale so the frame is fully covered, center-cropping
    // the overflow (vs the default aspect-fit, which letterboxes). Only meaningful
    // on an Image node; elsewhere it is an inert flag.
    view->img_cover = true;
    return view;
}

ZView Font(ZFont size, ZView view) {
    view->font_size = (float)size;
    return view;
}

ZView Grow(float weight, ZView view) {
    view->grow = weight;
    return view;
}

ZView Fill(ZView view) {
    view->fill = true;
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

    ZStackOpts col = {.spacing = 6, .align = Z_ALIGN_LEADING};
    int k = 0;
    if (opts->title && opts->title[0]) {
        col.children[k++] = Foreground(
            Z_COLOR_TEXT_MUTED, Font(Z_FONT_CAPTION, z_text("%s", opts->title)));
    }
    col.children[k++] = content;

    return Background(Z_COLOR_SURFACE,
        CornerRadius(18.0f,
            Padding(16.0f, z_stack(Z_AXIS_VERTICAL, &col))));
}
