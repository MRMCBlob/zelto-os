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
    Z_K_SCROLL,      // clips + translates a single content child by a ZScroll offset
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

    // Interactivity. on_tap fires on pointer tap / keyboard activation; on_key
    // receives raw key presses when this node holds focus; focusable marks a
    // keyboard target (Buttons are focusable). key is a stable identity hint for
    // the reconciler (0 = positional).
    ZAction on_tap;
    ZTapAction on_tap_data;  // tap handler carrying tap_data (data-driven rows)
    void *tap_data;
    ZKeyAction on_key;
    ZPanHandler on_pan;   // pan/drag recognizer (NULL = none)
    ZLongPressHandler on_long_press;  // press-and-hold recognizer (NULL = none)
    void *long_press_data;            // per-view data passed to on_long_press
    struct ZTextField *field;         // Z_K_STACK built by TextField: its buffer
    bool field_active;                // this field currently has input focus (caret)
    bool focusable;
    bool focused;         // set by the app loop on the focused node (focus ring)
    uint64_t key;         // stable identity for keyed reconcile (0 = positional)

    // Translation (Offset). Added to this node's origin in arrange(), so the
    // whole subtree shifts. Drag/transition motion bakes into layout each frame.
    float off_x, off_y;

    // Scroll / virtualised-list support.
    bool clip;            // clip this subtree to the node frame (viewport)
    bool fill;            // in a parent's arrange, expand to the inner box
    bool abs_children;    // position children by their layout_y, not flow
    float layout_y;       // absolute y within an abs_children parent (rows)
    float content_h;      // overrides measured content height for a scroll (0 = auto)
    struct ZScroll *scroll;  // Z_K_SCROLL: the persistent offset/fling cell

    // Children.
    ZView children[Z_MAX_CHILDREN];
    int n_children;

    // Computed frame (layout output), in surface pixels.
    float x, y, w, h;
};

// --- Persistent (retained) UI state --------------------------------------
// These survive rebuilds (the arena does not). They live in per-screen storage
// so call-order allocation gives an animated value / scroll position a stable
// identity across body() calls. See docs/contributing/sdk-internals.md.

// A spring-backed scalar. `value` is what views read; z_animated_spring sets
// `target` and the per-frame tick integrates value/velocity toward it.
struct ZAnimated {
    ZApp *app;
    float value, target, velocity;
    float stiffness, damping, mass;
    bool animating;
    bool used;
};

// A scroll position with momentum. offset >= 0 measured from the top; the layout
// pass fills viewport_h/content_h (used to clamp + for fling settling).
struct ZScroll {
    ZApp *app;
    float offset;          // current scroll offset
    float velocity;        // px/s, for fling
    bool flinging;
    float viewport_h;      // last laid-out viewport height
    float content_h;       // last laid-out content height
    float painted_offset;  // offset the buffer was last painted at (reconcile)
    bool used;
};

#define Z_MAX_CELLS 8
#define Z_MAX_SCREENS 8

// One screen instance: its builder + props, plus the retained cells its body
// allocates by call order, plus the slide-transition progress for this screen.
typedef struct ZScreen {
    ZScreenFn fn;
    void *props;
    struct ZAnimated anims[Z_MAX_CELLS];
    int anim_count, anim_cursor;
    struct ZScroll scrolls[Z_MAX_CELLS];
    int scroll_count, scroll_cursor;
    struct ZAnimated trans;   // 0 = off-screen (right), 1 = fully on screen
    int op;                   // pending op: 0 none, 1 entering, 2 exiting
} ZScreen;

struct ZNav {
    ZApp *app;
    ZScreen stack[Z_MAX_SCREENS];
    int depth;
    bool inited;
};

// The retained UI state embedded in ZApp (kept out of the wayland-heavy ZApp
// struct so the toolkit modules can reach it via z_app_ui without app.c's
// private definition). cur is the screen currently being built.
typedef struct ZUI {
    ZScreen *cur;
    ZScreen implicit;      // host screen: non-nav apps, and the Navigator itself
    struct ZNav nav;
    bool nav_used;
    bool transitioning;    // a screen slide is in flight (force full repaint)
    ZSpring anim_spring;   // active profile for z_animated_spring (z_with_animation)
    double last_s;         // last tick timestamp (monotonic seconds)
    bool have_last;
} ZUI;

// Implemented in app.c (ZApp is private there). z_app_width/z_app_height are
// declared publicly in <zelto/ui.h> (included above).
ZUI *z_app_ui(ZApp *app);
void *z_app_state(ZApp *app);
// Text-field focus (P21). z_text_field (view.c) uses these to reflect + set the
// app's active text field; app.c drives text-input-v3 enable/disable off it.
void z_app_focus_field(ZApp *app, ZTextField *f);
bool z_app_field_active(ZApp *app, const ZTextField *f);
// The app's currently-focused field, or NULL (P22 selection actions operate on it).
ZTextField *z_app_active_field(ZApp *app);

// Text-field selection gestures (P22). view.c builds the field node and wires
// these as its tap/long-press/pan handlers; the geometry (mapping a surface-local
// x to a byte offset) and the anchor/caret bookkeeping live in app.c, which owns
// the laid-out tree (app->root) and the shaping context (app->text). All three
// focus the field first. z_field_tap collapses the caret at the last pointer
// position (a tap handler gets no coordinates); z_field_select_word selects the
// word under x; z_field_drag_extend(begin) picks the moving end at begin, then
// extends the selection to x on each subsequent call.
void z_field_dbg_insert(ZApp *app);   // TEMP diagnostic: insert a literal marker
void z_field_tap(ZApp *app, ZTextField *f);
void z_field_select_word(ZApp *app, ZTextField *f, float x);
void z_field_drag_extend(ZApp *app, ZTextField *f, float x, bool begin);
// The active app's app_id (set for app_run's lifetime). Used by storage.c to
// scope each app's private data directory.
const char *z_active_app_id(void);

// --- Animation ------------------------------------------------------------
// Advance every retained spring + scroll fling by dt seconds. Returns true if
// anything is still in motion (so the caller keeps the frame loop running).
bool z_anim_tick(ZApp *app, float dt);
double z_now_seconds(void);

// --- Networking event-loop integration (net.c) ----------------------------
// The async HTTP/WebSocket clients live in net.c with their own in-flight
// tables; the app loop (app.c) hosts their sockets. z_net_collect_fds appends up
// to `max` pollfd entries for the active sockets (with the right POLLIN/POLLOUT
// events) and returns how many; z_net_handle_ready advances each whose revents
// fired. Cap so they fit the loop's fixed poll array.
#include <poll.h>
#define Z_NET_POLL_MAX 12
int z_net_collect_fds(struct pollfd *pfds, int max);
void z_net_handle_ready(struct pollfd *pfds, int count);

// --- Scroll input (called from the app loop's pointer handling) -----------
void z_scroll_begin_drag(ZScroll *sc);              // pan begin: stop any fling
void z_scroll_drag_by(ZScroll *sc, float dy);       // wheel / pan delta (clamped)
void z_scroll_end_drag(ZScroll *sc, float velocity_y);  // pan end: start a fling

// --- Per-build arena ------------------------------------------------------
// A chunked bump allocator. Growth links a NEW chunk instead of realloc'ing, so
// pointers handed out earlier in a build never move — the retained view tree
// (whose nodes point at each other) depends on that stability. reset() keeps the
// chunks and rewinds them for reuse next build.
typedef struct ZChunk {
    struct ZChunk *next;
    size_t cap, used;
    uint8_t *data;
} ZChunk;

typedef struct ZArena {
    ZChunk *head;   // first chunk (reset rewinds to here)
    ZChunk *cur;    // chunk currently being filled
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

// --- Damage / reconcile ---------------------------------------------------
// An integer pixel rect, half-open [x0,x1) x [y0,y1).
typedef struct ZIRect {
    int x0, y0, x1, y1;
} ZIRect;

#define Z_MAX_DAMAGE 64

// The set of pixel regions that differ between two builds. `full` means repaint
// everything (first frame, structural change, or too many small rects to track).
typedef struct ZDamage {
    ZIRect rects[Z_MAX_DAMAGE];
    int count;
    bool full;
} ZDamage;

void z_damage_reset(ZDamage *d);
void z_damage_add(ZDamage *d, ZIRect r);                 // union-append (-> full if overflow)
void z_damage_merge(ZDamage *dst, const ZDamage *src);   // dst |= src

// Diff the previous laid-out tree against the new one (positional, since there
// are no list keys yet) and accumulate the changed regions into `out`. Unchanged
// subtrees contribute nothing, so their cached layout/paint is reused.
void z_reconcile(ZView old_root, ZView new_root, ZDamage *out);

// --- Render ---------------------------------------------------------------
// A 32-bit ARGB (little-endian: B,G,R,A bytes) software target. clip_* is the
// half-open region paint is restricted to (set to the full canvas for a full
// repaint, or to a damage rect for partial repaint).
typedef struct ZCanvas {
    uint32_t *pixels;
    int width, height;
    int stride_px;       // pixels per row
    int clip_x0, clip_y0, clip_x1, clip_y1;
    ZText *text;
} ZCanvas;

// Restrict subsequent drawing to [x0,x1) x [y0,y1) (clamped to the canvas).
void z_canvas_set_clip(ZCanvas *canvas, int x0, int y0, int x1, int y1);
// Clear the current clip region to fully transparent.
void z_canvas_clear_clip(ZCanvas *canvas);

// Paint the laid-out tree into the canvas (within its current clip).
void z_render(ZCanvas *canvas, ZView root);
// Blit one shaped line; used by the renderer (kept here so layout can share
// the measure path). pen_x/pen_y is the top-left of the text box.
void z_text_draw(ZCanvas *canvas, const char *s, float size, ZColor color,
                 float pen_x, float pen_y);

#endif  // ZELTO_INTERNAL_H
