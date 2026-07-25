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
    Z_K_IMAGE,       // a decoded raster/SVG bitmap, aspect-fit (or Cover: fill) the frame
    Z_K_STROKE,      // a round-capped polyline (vector icon: chevron/circle/square)
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
    bool grow_share;      // Share(): drop this child's intrinsic main size, so the
                          // weights divide the WHOLE axis rather than the slack
    float fixed_w;        // Frame width  (0 = auto)
    float fixed_h;        // Frame height (0 = auto)
    // A FLOOR on the node's OUTER height, applied after padding (0 = none).
    // Deliberately not the same shape as fixed_h, which measure() treats as an
    // INNER height and adds padding to — the trap that has cost this project two
    // phases. A touch target is the box a FINGER lands on, so it is the outer
    // box by definition, and expressing it as fixed_h would make the control a
    // whole padding taller than the minimum it is trying to meet.
    float min_h;

    // Visuals.
    bool has_bg;
    ZColor bg;
    float radius;
    float elevation;      // soft drop-shadow blur px (Shadow(); 0 = flat)
    ZColor color;         // Rect fill
    char *text;           // Text content (arena-owned)
    float text_w;         // Z_K_TEXT: the shaped width of that string — what it
                          // will PAINT. Kept because arrange() then clamps the
                          // node's frame to its parent's inner box, so `w` says
                          // how much room the text was given and this says how
                          // much it needs; the renderer draws from the origin and
                          // does not clip, so text_w > w is glyphs outside the
                          // box, which is invisible in the frames alone.
    float text_h;         // Z_K_TEXT: the LINE HEIGHT the face reported for that
                          // string at that size — the other axis of the same
                          // question, and the one P50 needs. Width overflow comes
                          // from a string somebody else wrote; HEIGHT overflow
                          // comes from a BOX somebody else declared: a row whose
                          // height was read off a spec sheet (Z_ROW_H = 44pt) is
                          // fixed while the type inside it is not, so at a large
                          // text size the line is taller than the row and the
                          // frames still look perfectly reasonable. There is no
                          // clip here either — the glyphs paint through the row's
                          // top and bottom into its neighbours.
    float font_size;
    ZWeight weight;       // text weight (variable-font wght axis); default Regular
    ZColor fg;
    bool text_shadow;     // draw a dark offset copy under the ink (legibility over art)
    char *img_path;       // Z_K_IMAGE: source path (arena-owned; PNG or SVG by ext)
    bool img_cover;       // Z_K_IMAGE: cover (aspect-fill, center-crop) vs default fit
    float *stroke_pts;    // Z_K_STROKE: arena-owned x,y pairs in [0,1] within the frame
    int stroke_n;         // Z_K_STROKE: number of points (pairs)
    float stroke_w;       // Z_K_STROKE: line thickness (px)
    bool stroke_closed;   // Z_K_STROKE: connect the last point back to the first

    // Interactivity. on_tap fires on pointer tap / keyboard activation; on_key
    // receives raw key presses when this node holds focus; focusable marks a
    // keyboard target (Buttons are focusable). key is a stable identity hint for
    // the reconciler (0 = positional).
    ZAction on_tap;
    ZTapAction on_tap_data;  // tap handler carrying tap_data (data-driven rows)
    void *tap_data;
    ZKeyAction on_key;
    ZPanHandler on_pan;   // pan/drag recognizer (NULL = none)
    ZPanDataHandler on_pan_data;  // pan handler carrying pan_data (script closures)
    void *pan_data;
    ZLongPressHandler on_long_press;  // press-and-hold recognizer (NULL = none)
    void *long_press_data;            // per-view data passed to on_long_press
    struct ZTextField *field;         // Z_K_STACK built by TextField: its buffer
    bool field_active;                // this field currently has input focus (caret)
    bool focusable;
    bool focused;         // set by the app loop on the focused node (focus ring)
    float press;          // press-feedback amount (0..1), stamped at render time by
                          // the app loop onto the node under the live press; the
                          // renderer draws a Z_COLOR_PRESS veil scaled by it (P31)
    uint64_t key;         // stable identity for keyed reconcile (0 = positional)

    // Translation (Offset). Added to this node's origin in arrange(), so the
    // whole subtree shifts. Drag/transition motion bakes into layout each frame.
    float off_x, off_y;

    // Opacity (Opacity()). Stored as the COMPLEMENT of opacity (0 = fully opaque,
    // the arena-zero default, so an unwrapped node needs no initialization; 1 =
    // fully transparent). The renderer multiplies a subtree's effective alpha by
    // (1 - fade) as it descends, so wrapping a node fades it and everything under
    // it — the fade half of a slide+fade transition, and the nav cross-fade.
    float fade;

    // Scroll / virtualised-list support.
    bool clip;            // clip this subtree to the node frame (viewport)
    float clip_radius;    // Clip(): round that subtree clip's corners (0 = square)
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
    float offset;          // current scroll offset (may sit PAST a bound mid-drag,
                           // rubber-banded, until release settles it back in)
    float velocity;        // px/s, for fling
    bool flinging;
    // Rubber-band drag state (P33). `raw` is the UN-damped accumulated drag
    // position; `offset` is `raw` passed through z_rubber_band past the bounds, so
    // an over-pull resists instead of hard-clamping. On release, if `raw` is out of
    // range the scroll SETTLES elastically back to the nearest bound (settling),
    // rather than flinging.
    float raw;
    bool dragging;         // a finger drag holds `offset` (may be past a bound)
    bool settling;
    float settle_target;
    float viewport_h;      // last laid-out viewport height
    float content_h;       // last laid-out content height
    float painted_offset;  // offset the buffer was last painted at (reconcile)
    bool used;
};

// A keyed retained animation cell: a ZAnimated looked up by an explicit uint64_t
// key (an item's stable IDENTITY), NOT by call order. This is what lets each
// reflowing bento item keep its own spring across reorders even as its call-order
// position changes every rebuild. `requested` is the per-build mark bit: cells
// not requested in a build are swept (used=false) so removed items don't leak.
typedef struct ZKeyed {
    uint64_t key;
    struct ZAnimated v;
    bool used;
    bool requested;
} ZKeyed;

#define Z_MAX_CELLS 8
#define Z_MAX_KEYED 96
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
    ZKeyed keyed[Z_MAX_KEYED];   // identity-keyed anim cells (z_animated_keyed)
    int keyed_count;
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
    // Global press-feedback spring (P31). A phone is single-touch, so one retained
    // spring drives the touch-down highlight for whatever tappable node is under
    // the finger: the app loop springs it 0->1 on press and 1->0 on release/cancel,
    // re-hit-tests at the frozen press point each build (so it survives body
    // rebuilds without a dangling arena pointer), and stamps node->press. Advanced
    // by z_anim_tick like any spring. `reduce_motion` collapses every spring to an
    // instant jump (Accessibility): read once at startup from sys.reduce_motion.
    struct ZAnimated press;
    bool reduce_motion;
} ZUI;

// Implemented in app.c (ZApp is private there). z_app_width/z_app_height are
// declared publicly in <zelto/ui.h> (included above).
ZUI *z_app_ui(ZApp *app);
void *z_app_state(ZApp *app);
// Text-field focus (P21). z_text_field (view.c) uses these to reflect + set the
// app's active text field; app.c drives text-input-v3 enable/disable off it.
// z_app_focus_field / z_app_field_active are PUBLIC (declared in <zelto/ui.h>,
// included above) — the home screen needs them to inset for the keyboard.
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
// z_animated_spring_with (spring under an explicit motion token) is declared in
// the public <zelto/ui.h>; the press-feedback spring and the page carousel use it.

// Keyed-cell GC bracket (call around each screen's body): begin clears the
// per-build requested marks; end sweeps any keyed cell not requested this build.
void z_keyed_frame_begin(ZScreen *s);
void z_keyed_frame_end(ZScreen *s);

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

// --- Sensor/location event-loop integration (sensors.c) --------------------
// The sensor/GPS streams share ONE persistent socket to zsysd (all of an app's
// subscriptions ride it), so the loop hosts a single fd. z_sensor_poll_fd is that
// fd (or -1 when no stream is open); z_sensor_handle_ready reads and dispatches
// whatever samples arrived. See sensors.c.
// --- Camera loop integration (camera.c) ------------------------------------
// The preview has no fd to wait on — frames are generated locally — so the loop
// bounds its sleep by the next frame's deadline and pumps once per iteration.
void   z_camera_pump(ZApp *app);
double z_camera_next_deadline(void);   // monotonic s, or < 0 when idle
void   z_camera_set_paused(bool paused);

int z_sensor_poll_fd(void);
void z_sensor_handle_ready(void);
// Foreground gate: pause every sensor/location stream at the broker while the app
// is backgrounded, resume on return (the "when-in-use" contract). app.c calls it
// from the xdg lifecycle transition. See sensors.c.
void z_sensor_set_paused(bool paused);

// --- Navigator back-swipe (called from the app loop's pointer handling) ----
// The interruptible edge-swipe (P33). app.c owns the pointer geometry and drives
// the top screen's transition spring through these; see navigation.c.
bool z_nav_can_back(ZNav *nav);
void z_nav_back_begin(ZNav *nav);
void z_nav_back_drag(ZNav *nav, float progress);
void z_nav_back_end(ZNav *nav, bool pop, float velocity);

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

// --- Dynamic Type (type.c) -------------------------------------------------
// The process's text-size state, behind the seam. z_font_units() and z_row_h()
// are public (<zelto/ui.h>); these two are not.
//
// Bold Text is the SECOND term of the same setting and it lands in a different
// place: not on the size, but on the WEIGHT the shaper is set to — see
// set_weight() in text.c, which is the one point every measurement and every
// paint passes through, build-time wrap measurement included.
bool z_text_bold(void);
// Apply a new (step, bold); returns true if either actually moved, i.e. whether
// the caller owes a rebuild.
bool z_text_size_apply(int step, bool bold);

// --- Text -----------------------------------------------------------------
typedef struct ZText ZText;          // opaque font/shaping context
ZText *z_text_open(const char *font_path);
void z_text_close(ZText *t);
// Measure a shaped line at `size` px + `weight`; returns advance width, fills
// ascent/descent. Weight matters: a heavier variable-font instance widens the
// advances, so measure must use the same weight the renderer will paint at.
float z_text_measure(ZText *t, const char *s, float size, ZWeight weight,
                     float *ascent, float *descent);
// The same, over the first `len` bytes of `s` (len < 0 = to the NUL). A line
// breaker measures SLICES of a paragraph it does not own; without a length it
// would have to copy each slice somewhere to NUL-terminate it, and any fixed
// buffer used for that silently truncates — measuring SHORT, so the breaker
// emits a line that overflows. Shaping has taken a length all along.
float z_text_measure_n(ZText *t, const char *s, int len, float size,
                       ZWeight weight, float *ascent, float *descent);

// The app's shaping context (ZApp is private to app.c). Needed by builders that
// have to measure at BUILD time rather than at layout time — WrapText, whose
// whole job is to know how wide a run of prose is before the tree exists.
ZText *z_app_text(ZApp *app);

// --- Layout ---------------------------------------------------------------
// Lay out `root` to fill a (w x h) surface, writing x/y/w/h into every node.
// `text` is used to measure Text nodes.
void z_layout(ZView root, float w, float h, ZText *text);

// --- Line breaking --------------------------------------------------------
// One wrapped line: a SLICE of the caller's string, never a copy (the renderer
// takes a length, and Text("%.*s", len, s) prints one, so a paragraph costs no
// allocation at all).
typedef struct ZWrapLine {
    const char *s;
    int len;
} ZWrapLine;

// Greedy line breaking. `measure` returns the advance width of s[0..len) — the
// caller passes the real shaper, a test passes a fixed-advance stub, which is
// what makes the algorithm testable without a font. '\n' always breaks. A word
// too long to fit alone is broken mid-word rather than allowed to overflow, so
// the result NEVER exceeds max_w. Returns the number of lines written.
//
// `rest` (nullable) receives the first byte NOT consumed, so a caller that hit
// max_lines can continue instead of silently dropping the tail. It points at the
// terminating NUL when the whole string was broken. P44 shipped this without it
// and lost everything past line 32 with no diagnostic — the same silent-degrade
// it had just added a warning for elsewhere in the same commit.
typedef float (*ZWrapMeasure)(void *ud, const char *s, int len);
int z_wrap_lines(const char *text, float max_w, ZWrapMeasure measure, void *ud,
                 ZWrapLine *out, int max_lines, const char **rest);

// --- Hit testing ----------------------------------------------------------
// The inverse of layout: which node owns a point. It lives beside layout.c's
// arrange() because it is pure geometry over the frames arrange() wrote — the
// app loop asks it a question, it does not run the loop — and because that is
// what makes it unit-testable without a compositor connection (see
// test/test_hit_test_clip.c).
//
// The rule it enforces: A NODE IS TAPPABLE EXACTLY WHERE IT IS PAINTED. The
// renderer masks a subtree only at nodes that set `clip` (a Scroll viewport or
// an explicit Clip()); everything else paints wherever layout put it, including
// outside its parent. Hit testing therefore has to mask by the same clip stack
// and nothing else. See the long comment at the implementation.
typedef enum ZHitWant {
    Z_HIT_TAP = 0,      // on_tap / on_tap_data
    Z_HIT_SCROLL,       // a Z_K_SCROLL with a live ZScroll cell
    Z_HIT_PAN,          // on_pan / on_pan_data
    Z_HIT_LONG_PRESS,   // on_long_press
} ZHitWant;

// Deepest (front-most) node of the requested kind whose PAINTED area contains
// (x, y). `root` must already have been laid out.
ZView z_hit_test(ZView root, double x, double y, ZHitWant want);

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

// --- Images (image.c) -----------------------------------------------------
// A decoded bitmap: premultiplied ARGB8888, packed one uint32 per pixel as
// (A<<24)|(R<<16)|(G<<8)|B — the exact layout the renderer writes, so the blit
// is a straight source-over. `ok` records whether the decode succeeded; failed
// decodes are cached too (px == NULL) so a missing/broken file is not retried
// every frame. Owned by the module's global cache; never freed by the caller.
typedef struct ZImage {
    struct ZImage *next;
    char *path;           // cache key (heap-owned)
    int w, h;             // bitmap dimensions (0 when !ok)
    uint32_t *px;         // premultiplied ARGB, w*h (NULL when !ok)
    bool ok;
} ZImage;

// Decode (on a cache miss) and return the bitmap for `path`, or NULL only when
// path is NULL. Dispatches on the file extension: ".svg" -> the in-house stroked
// rasterizer, else libpng. The tree is rebuilt every frame, so this MUST NOT
// decode more than once per path: the result (success or failure) is cached.
const ZImage *z_image_get(const char *path);

// Intrinsic (unscaled) pixel size of `path`'s bitmap; false if it can't load.
// Used by the layout pass to aspect-fit an Image that was given no fixed size.
bool z_image_intrinsic(const char *path, int *w, int *h);

// Put an ALREADY-DECODED bitmap into the cache under `key`, taking ownership of
// `px` (premultiplied ARGB, w*h; freed by the cache, and freed immediately if
// this fails). `key` is any stable string that cannot collide with a file path.
//
// This is how pixels that did not come from a file reach the renderer: a window
// snapshot arrives from the compositor as a memfd (z_snapshot, app.c), and
// adopting it here means the whole existing Image path — layout aspect-fit,
// Cover, the source-over blit — works on it unchanged, with no new node kind.
// Re-adopting the same key REPLACES the bitmap rather than adding an entry.
bool z_image_adopt(const char *key, int w, int h, uint32_t *px);

// --- Render ---------------------------------------------------------------
// A 32-bit ARGB (little-endian: B,G,R,A bytes) software target. clip_* is the
// half-open region paint is restricted to (set to the full canvas for a full
// repaint, or to a damage rect for partial repaint).

// One entry of the ROUNDED clip stack (Clip()). The rectangular clip_* region
// above bounds which pixels are even visited; this masks the corners inside it,
// as antialiased coverage rather than a hard in/out test.
typedef struct ZRoundClip {
    int x0, y0, x1, y1;
    float r;
} ZRoundClip;

// Depth of nested Clip()s a canvas can hold. A rounded clip only nests when one
// masked shape sits inside another, and the deepest chain the system UI builds
// is three (a Control Center slab inside a rounded sheet inside a scrolled
// card), so four carries a level of headroom. Past this the extra levels are
// ignored rather than overflowing: the shape degrades to a squarer mask instead
// of corrupting memory — and render.c says so on stderr, once per process,
// because a soft-fail nobody can see in a screenshot is otherwise diagnosed as
// the wrong radius token. z_hit_test() applies the same cap so touch and paint
// agree even there.
#define Z_MAX_ROUND_CLIPS 4

typedef struct ZCanvas {
    uint32_t *pixels;
    int width, height;
    int stride_px;       // pixels per row
    int clip_x0, clip_y0, clip_x1, clip_y1;
    ZRoundClip rclip[Z_MAX_ROUND_CLIPS];
    int n_rclip;         // 0 on the common path: no rounded clip is active
    ZText *text;
} ZCanvas;

// Restrict subsequent drawing to [x0,x1) x [y0,y1) (clamped to the canvas).
void z_canvas_set_clip(ZCanvas *canvas, int x0, int y0, int x1, int y1);
// Clear the current clip region to fully transparent.
void z_canvas_clear_clip(ZCanvas *canvas);

// Coverage in [0,1] that the active rounded clips leave at pixel (x,y) — 1.0
// when none is active (checked with a single int compare, so the ordinary path
// pays nothing). Every painter multiplies its own coverage by this.
float z_canvas_round_cov(const ZCanvas *canvas, int x, int y);

// Paint the laid-out tree into the canvas (within its current clip).
void z_render(ZCanvas *canvas, ZView root);
// Blit one shaped line; used by the renderer (kept here so layout can share
// the measure path). pen_x/pen_y is the top-left of the text box.
void z_text_draw(ZCanvas *canvas, const char *s, float size, ZWeight weight,
                 ZColor color, float pen_x, float pen_y);

#endif  // ZELTO_INTERNAL_H
