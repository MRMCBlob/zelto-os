// libzelto internals: the retained node representation, the per-build arena, and
// the cross-module entry points (layout, render, text). Not a public header.
// See docs/contributing/sdk-internals.md.
#ifndef ZELTO_INTERNAL_H
#define ZELTO_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zelto/ui.h"

typedef enum ZKind {
    Z_K_STACK,
    Z_K_RECT,
    Z_K_TEXT,
    Z_K_SPACER,
} ZKind;

// One UI node. Arena-allocated per build; the computed frame (x,y,w,h) is filled
// by the layout pass and consumed by the renderer.
struct ZNode {
    ZKind kind;

    // Stack params.
    ZAxis axis;
    ZAlign align;
    float spacing;

    // Common box params.
    float padding;        // inset applied inside this node's frame
    float grow;           // flex weight on the parent's main axis (Spacer = 1)
    float fixed_w;        // Frame width  (0 = auto)
    float fixed_h;        // Frame height (0 = auto)

    // Visuals.
    bool has_bg;
    ZColor bg;
    float radius;
    ZColor color;         // Rect fill
    char *text;           // Text content (arena-owned)
    float font_size;
    ZColor fg;

    // Children.
    ZView children[Z_MAX_CHILDREN];
    int n_children;

    // Computed frame (layout output), in surface pixels.
    float x, y, w, h;
};

// --- Per-build arena ------------------------------------------------------
typedef struct ZArena {
    uint8_t *base;
    size_t cap;
    size_t used;
} ZArena;

void *z_arena_alloc(ZArena *arena, size_t size);
char *z_arena_strdup(ZArena *arena, const char *s);
void z_arena_reset(ZArena *arena);
void z_arena_free(ZArena *arena);

// The builder functions allocate from the arena of the app currently building.
// Single-threaded app loop, so a thread-local-free global is fine.
extern ZArena *z_build_arena;

// --- Text -----------------------------------------------------------------
typedef struct ZText ZText;          // opaque font/shaping context
ZText *z_text_open(const char *font_path);
void z_text_close(ZText *t);
// Measure a shaped line at `size` px; returns advance width, fills ascent/descent.
float z_text_measure(ZText *t, const char *s, float size, float *ascent,
                     float *descent);

// --- Layout ---------------------------------------------------------------
// Lay out `root` to fill a (w x h) surface, writing x/y/w/h into every node.
// `text` is used to measure Text nodes.
void z_layout(ZView root, float w, float h, ZText *text);

// --- Render ---------------------------------------------------------------
// A 32-bit ARGB (little-endian: B,G,R,A bytes) software target.
typedef struct ZCanvas {
    uint32_t *pixels;
    int width, height;
    int stride_px;       // pixels per row
    ZText *text;
} ZCanvas;

// Paint the laid-out tree into the canvas.
void z_render(ZCanvas *canvas, ZView root);
// Blit one shaped line; used by the renderer (kept here so layout can share
// the measure path). pen_x/pen_y is the top-left of the text box.
void z_text_draw(ZCanvas *canvas, const char *s, float size, ZColor color,
                 float pen_x, float pen_y);

#endif  // ZELTO_INTERNAL_H
