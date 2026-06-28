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
void *z_arena_alloc(ZArena *arena, size_t size) {
    size = (size + 15u) & ~(size_t)15u;  // 16-byte align
    if (arena->used + size > arena->cap) {
        size_t need = arena->used + size;
        size_t cap = arena->cap ? arena->cap : 64u * 1024u;
        while (cap < need) {
            cap *= 2;
        }
        arena->base = realloc(arena->base, cap);
        arena->cap = cap;
    }
    void *p = arena->base + arena->used;
    arena->used += size;
    memset(p, 0, size);
    return p;
}

char *z_arena_strdup(ZArena *arena, const char *s) {
    size_t n = strlen(s) + 1;
    char *p = z_arena_alloc(arena, n);
    memcpy(p, s, n);
    return p;
}

void z_arena_reset(ZArena *arena) { arena->used = 0; }

void z_arena_free(ZArena *arena) {
    free(arena->base);
    arena->base = NULL;
    arena->cap = arena->used = 0;
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
