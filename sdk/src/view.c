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

ZView OnKey(ZKeyAction action, ZView view) {
    view->on_key = action;
    view->focusable = true;
    return view;
}
