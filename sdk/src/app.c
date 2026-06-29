// libzelto app runtime: the Wayland client side. Connects to the compositor,
// binds the core globals + xdg-shell, drives the configure handshake, and runs
// the build -> layout -> paint -> commit loop, submitting an shm buffer to its
// surface. See docs/contributing/sdk-internals.md ("Pipeline").
// (_GNU_SOURCE for memfd_create comes from the project-wide build args.)
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "internal.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define Z_DEFAULT_FONT "/usr/share/zelto/fonts/ZeltoSans.ttf"

// Max running-app windows tracked by the foreign-toplevel client list.
#define Z_MAX_TASKS 32

// A retained record of one running app window, mirroring a foreign-toplevel
// handle. Strings are owned (strdup'd from the handle's title/app_id events).
typedef struct ZTaskRec {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    ZApp *app;            // back-pointer (records live in a fixed app array)
    char *title;
    char *app_id;
    bool active;
    bool used;
} ZTaskRec;

// A retained shm buffer in the double-buffer pool. Buffers persist across frames
// (so unchanged pixels survive) and only their damaged regions are repainted.
typedef struct ZBuf {
    struct wl_buffer *wl;
    uint32_t *px;
    int stride_px;
    int w, h;            // size this buffer was allocated for
    bool busy;           // attached, awaiting wl_buffer.release
    bool valid;          // holds a complete frame from a prior full paint
    ZDamage pending;     // regions changed since this buffer was last painted
} ZBuf;

struct ZApp {
    void *state;
    ZBodyFn body;
    const char *title;
    const char *app_id;
    ZArena arenas[2];    // double arena: keep last build's tree for diffing
    int cur_arena;
    ZText *text;

    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct wl_output *output;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;

    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;

    // Layer-shell role (System UI). When is_layer, the surface is a layer
    // surface anchored per `layer_opts` instead of an xdg_toplevel.
    struct zwlr_layer_shell_v1 *layer_shell;
    struct zwlr_layer_surface_v1 *layer_surface;
    bool is_layer;
    ZLayerOpts layer_opts;

    // Lifecycle: tracks the xdg "activated" state the compositor broadcasts to
    // the front window; transitions fire the handler and force a repaint so a
    // body() reading z_app_active() reflects Active/Paused.
    ZLifecycleHandler lifecycle;
    bool active;

    // Foreign-toplevel-management client (task switcher). Bound only when the
    // compositor advertises the manager; the launcher uses it, ordinary apps
    // ignore it. Records mirror live windows; the snapshot is what body() reads.
    struct zwlr_foreign_toplevel_manager_v1 *ftl_manager;
    ZTaskRec ftl_recs[Z_MAX_TASKS];
    ZTask ftl_snapshot[Z_MAX_TASKS];

    struct wl_callback *frame_cb;   // in-flight frame throttle (NULL = idle)
    ZBuf bufs[2];                   // retained double-buffer pool

    // Permission broker (zsysd) client. perm_fd is the socket of an in-flight
    // z_perm_request, watched in the app loop alongside the wayland fd; when the
    // reply arrives (after the user answers the consent dialog) the callback
    // fires. -1 when no request is outstanding (z_perm_status is synchronous).
    int perm_fd;
    ZPermCallback perm_cb;
    void *perm_ud;

    // Intents (zsysd) client. ctrl_fd is a persistent connection registered at
    // startup ({"op":"register"}) and parked in the app loop for the app's life:
    // it is the app's mailbox into which zsysd pushes delivered intents
    // ({"op":"deliver",..}), and the channel z_share/z_open_url send resolve
    // requests on. ctrl_buf accumulates a partial pushed line across reads.
    int ctrl_fd;
    char ctrl_buf[512];
    size_t ctrl_len;
    ZUrlCb url_cb;
    void *url_ud;
    ZShareCb share_cb;
    void *share_ud;
    // A delivered intent that arrived before its handler was registered (an app
    // launched to handle an intent gets the push before its first body() runs).
    // It is dispatched the moment z_on_open_url / z_on_share_target is called.
    bool pend_url;
    char pend_url_buf[256];
    bool pend_share;
    char pend_mime[64];
    char pend_text[256];

    // Keyboard translation (raw keycodes -> keysyms) via xkbcommon.
    struct xkb_context *xkb_ctx;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;

    // Pointer state: last surface-local position + the node a press landed on.
    double ptr_x, ptr_y;

    // Pan/drag recognizer. A press waits to see if it crosses the slop threshold
    // (-> pan) or releases first (-> tap). While panning it drives a scroll
    // container or a custom OnPan target and tracks velocity for the fling.
    bool ptr_down, panning;
    double press_x, press_y;     // where the press landed
    double last_x, last_y;       // previous motion sample
    double last_motion_s;        // its timestamp (monotonic s)
    double drag_vel_y;           // latest finger velocity (px/s)
    ZScroll *drag_scroll;        // scroll being dragged (NULL = none)
    ZView pan_target;            // custom OnPan target (NULL = none)

    // Retained build output: the laid-out root from the most recent build, used
    // to hit-test pointer events and route keys until the next build replaces it.
    ZView root;
    ZView focused;              // first focusable node (keyboard target)

    ZUI ui;                     // retained toolkit state (animation/scroll/nav)

    int width, height;          // surface size in pixels
    int out_width, out_height;  // advertised output mode

    bool configured;
    bool running;
    bool dirty;
};

// Accessors so the toolkit modules (animation/scroll/navigation) reach the
// retained state without app.c's wayland-heavy ZApp definition.
ZUI *z_app_ui(ZApp *app) { return &app->ui; }
void *z_app_state(ZApp *app) { return app->state; }
// Recover the owning app from a foreign-toplevel record.
static ZApp *rec_app(ZTaskRec *rec) { return rec->app; }
int z_app_width(ZApp *app) { return app->width; }
int z_app_height(ZApp *app) { return app->height; }

// The active app, set for the lifetime of app_run(). The permission API
// (z_perm_status/z_perm_request) takes no ZApp to match the platform docs, so it
// resolves the single per-process app through this. Defined below.
static ZApp *z_active_app;
static void perm_handle_reply(ZApp *app);
static void ctrl_handle_read(ZApp *app);
static void ctrl_connect_register(ZApp *app);

// --- shm buffer pool ------------------------------------------------------
// The compositor finished reading a buffer: free it for reuse.
static void buffer_release(void *data, struct wl_buffer *buffer) {
    (void)buffer;
    ((ZBuf *)data)->busy = false;
}
static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

// (Re)allocate a pool buffer to the current surface size. Retained for reuse.
static bool buf_alloc(ZApp *app, ZBuf *b) {
    int stride = app->width * 4;
    size_t size = (size_t)stride * app->height;

    int fd = memfd_create("zelto-shm", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
        if (fd >= 0) {
            close(fd);
        }
        return false;
    }
    void *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) {
        close(fd);
        return false;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(app->shm, fd, (int32_t)size);
    struct wl_buffer *wl = wl_shm_pool_create_buffer(
        pool, 0, app->width, app->height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    b->wl = wl;
    b->px = px;
    b->stride_px = stride / 4;
    b->w = app->width;
    b->h = app->height;
    b->busy = false;
    b->valid = false;            // freshly allocated: needs a full paint
    z_damage_reset(&b->pending);
    wl_buffer_add_listener(wl, &buffer_listener, b);
    return true;
}

static void buf_free(ZBuf *b) {
    if (b->wl) {
        wl_buffer_destroy(b->wl);
    }
    if (b->px) {
        munmap(b->px, (size_t)b->stride_px * 4 * b->h);
    }
    memset(b, 0, sizeof(*b));
}

// Pick a free, correctly-sized pool buffer, (re)allocating as needed. Returns
// NULL only if allocation fails or both buffers are still in flight.
static ZBuf *buf_acquire(ZApp *app) {
    for (int i = 0; i < 2; i++) {
        ZBuf *b = &app->bufs[i];
        if (b->wl && (b->w != app->width || b->h != app->height)) {
            if (!b->busy) {
                buf_free(b);
            } else {
                continue;        // wrong size but in flight; skip
            }
        }
        if (!b->wl) {
            if (!buf_alloc(app, b)) {
                continue;
            }
        }
        if (!b->busy) {
            return b;
        }
    }
    return NULL;
}

// --- focus + hit-testing --------------------------------------------------
// First focusable node in DFS order becomes the keyboard target.
static ZView find_focusable(ZView n) {
    if (!n) {
        return NULL;
    }
    if (n->focusable) {
        return n;
    }
    for (int i = 0; i < n->n_children; i++) {
        ZView f = find_focusable(n->children[i]);
        if (f) {
            return f;
        }
    }
    return NULL;
}

static bool point_in(ZView n, double x, double y) {
    return x >= n->x && x < n->x + n->w && y >= n->y && y < n->y + n->h;
}

// Deepest (top-most) node with an on_tap handler whose frame contains (x,y).
static ZView hit_test(ZView n, double x, double y) {
    if (!n || !point_in(n, x, y)) {
        return NULL;
    }
    // Children paint last-on-top; search them front-to-back first.
    for (int i = n->n_children - 1; i >= 0; i--) {
        ZView h = hit_test(n->children[i], x, y);
        if (h) {
            return h;
        }
    }
    return (n->on_tap || n->on_tap_data) ? n : NULL;
}

// Deepest scroll container under (x,y) — the wheel/vertical-drag target.
static ZView find_scroll(ZView n, double x, double y) {
    if (!n || !point_in(n, x, y)) {
        return NULL;
    }
    for (int i = n->n_children - 1; i >= 0; i--) {
        ZView h = find_scroll(n->children[i], x, y);
        if (h) {
            return h;
        }
    }
    return (n->kind == Z_K_SCROLL && n->scroll) ? n : NULL;
}

// Deepest custom OnPan target under (x,y).
static ZView find_pan(ZView n, double x, double y) {
    if (!n || !point_in(n, x, y)) {
        return NULL;
    }
    for (int i = n->n_children - 1; i >= 0; i--) {
        ZView h = find_pan(n->children[i], x, y);
        if (h) {
            return h;
        }
    }
    return n->on_pan ? n : NULL;
}

// --- build/layout/paint/commit -------------------------------------------
static const struct wl_callback_listener frame_listener;

static void render(ZApp *app) {
    // Advance springs/flings before building so body() reads this frame's values.
    // dt comes from a monotonic clock (render runs both from the main loop and
    // the frame callback), clamped so a long stall doesn't explode the integrator.
    double now = z_now_seconds();
    float dt = app->ui.have_last ? (float)(now - app->ui.last_s) : 1.0f / 60.0f;
    // Track wall-clock so animations finish in real time even when the (software,
    // TCG-emulated) frame rate is low; the spring integrator sub-steps internally,
    // so a large dt stays stable. Cap only to absorb a long stall/first frame.
    if (dt < 0.001f) {
        dt = 0.001f;
    } else if (dt > 0.25f) {
        dt = 0.25f;
    }
    app->ui.last_s = now;
    app->ui.have_last = true;
    bool anim_active = z_anim_tick(app, dt);

    // Build the new view tree into the *other* arena, so the previous build's
    // tree (app->root, in the current arena) stays intact for diffing.
    int other = 1 - app->cur_arena;
    z_arena_reset(&app->arenas[other]);
    z_build_arena = &app->arenas[other];

    // The body runs in the implicit (host) screen's retained scope; a Navigator
    // inside it switches scope per managed screen as it builds them.
    app->ui.cur = &app->ui.implicit;
    app->ui.implicit.anim_cursor = 0;
    app->ui.implicit.scroll_cursor = 0;
    app->ui.transitioning = false;   // the Navigator re-asserts this if mid-slide

    ZView old_root = app->root;
    ZView new_root = app->body(app, app->state);

    z_layout(new_root, (float)app->width, (float)app->height, app->text);

    // Resolve keyboard focus against the fresh tree.
    app->focused = find_focusable(new_root);
    if (app->focused) {
        app->focused->focused = true;
    }

    // Diff against the previous tree to find the changed regions.
    ZDamage dmg;
    z_reconcile(old_root, new_root, &dmg);

    // Commit the new tree as current (the old arena is reused next render).
    app->cur_arena = other;
    app->root = new_root;

    ZBuf *buf = buf_acquire(app);
    if (!buf) {
        // Both buffers in flight (rare with the frame-callback throttle); try
        // again on the next frame callback.
        app->dirty = true;
        return;
    }

    ZCanvas canvas = {
        .pixels = buf->px,
        .width = app->width,
        .height = app->height,
        .stride_px = buf->stride_px,
        .text = app->text,
    };

    // What this buffer must repaint = the regions changed this frame, plus any
    // it missed while it was idle (it holds an older frame). A buffer with no
    // valid prior content (first use / after resize) gets a full repaint.
    ZDamage repaint = buf->pending;
    z_damage_merge(&repaint, &dmg);
    if (!buf->valid) {
        repaint.full = true;
    }
    // A screen-slide moves whole screens; the per-node damage degenerates into
    // many rects, and the partial path re-walks the tree once per rect. Repaint
    // the frame once instead. Likewise collapse heavily fragmented damage.
    if (app->ui.transitioning || repaint.count > 8) {
        repaint.full = true;
    }

    if (repaint.full) {
        z_canvas_set_clip(&canvas, 0, 0, app->width, app->height);
        z_canvas_clear_clip(&canvas);
        z_render(&canvas, new_root);
        wl_surface_damage_buffer(app->surface, 0, 0, app->width, app->height);
        buf->valid = true;
    } else {
        // Repaint only the damaged rects: clear each, clip to it, repaint the
        // tree (the painter reproduces exactly those pixels), and report it.
        for (int i = 0; i < repaint.count; i++) {
            ZIRect r = repaint.rects[i];
            z_canvas_set_clip(&canvas, r.x0, r.y0, r.x1, r.y1);
            z_canvas_clear_clip(&canvas);
            z_render(&canvas, new_root);
            wl_surface_damage_buffer(app->surface, r.x0, r.y0,
                                     r.x1 - r.x0, r.y1 - r.y0);
        }
    }

    // This buffer is now up to date; the *other* buffer still needs this frame's
    // changes applied the next time it is used. Propagate what was actually
    // repainted: a full repaint means the other buffer is wholly stale too (else
    // it could later do a partial repaint over an old mid-transition frame).
    ZBuf *other_buf = &app->bufs[1 - (buf - app->bufs)];
    z_damage_reset(&buf->pending);
    if (repaint.full) {
        other_buf->pending.full = true;
    } else {
        z_damage_merge(&other_buf->pending, &dmg);
    }

    buf->busy = true;
    wl_surface_attach(app->surface, buf->wl, 0, 0);

    // Throttle repaints to the compositor's frame clock: the next render only
    // happens once this buffer has been shown (frame callback) and state is
    // still dirty. This drives the build->layout->paint loop off vsync.
    app->frame_cb = wl_surface_frame(app->surface);
    wl_callback_add_listener(app->frame_cb, &frame_listener, app);

    wl_surface_commit(app->surface);

    // Keep the loop running while anything is still in motion: the frame
    // callback will re-render next vsync. Once everything settles, dirty stays
    // false and the loop idles until the next input/invalidate.
    if (anim_active) {
        app->dirty = true;
    }
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    (void)time;
    ZApp *app = data;
    wl_callback_destroy(cb);
    app->frame_cb = NULL;
    if (app->configured && app->dirty) {
        app->dirty = false;
        render(app);
    }
}
static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

// Run a node's tap handler (pointer tap or keyboard activation). A data-carrying
// handler (rows) takes precedence over a plain one.
static void dispatch_tap(ZApp *app, ZView node) {
    if (!node) {
        return;
    }
    if (node->on_tap_data) {
        node->on_tap_data(app, app->state, node->tap_data);
    } else if (node->on_tap) {
        node->on_tap(app, app->state);
    }
}

// --- xdg-shell handlers ---------------------------------------------------
static void wm_base_ping(void *data, struct xdg_wm_base *wm_base,
                         uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                                  uint32_t serial) {
    ZApp *app = data;
    xdg_surface_ack_configure(xdg_surface, serial);
    app->configured = true;
    app->dirty = true;
}
static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height,
                               struct wl_array *states) {
    (void)toplevel;
    ZApp *app = data;
    // Honour a non-zero compositor-suggested size; otherwise keep our own.
    if (width > 0 && height > 0) {
        app->width = width;
        app->height = height;
    }
    // Lifecycle rides the xdg "activated" state: present here = foreground.
    bool active = false;
    if (states) {
        uint32_t *st;
        wl_array_for_each(st, states) {
            if (*st == XDG_TOPLEVEL_STATE_ACTIVATED) {
                active = true;
            }
        }
    }
    if (active != app->active) {
        app->active = active;
        if (app->lifecycle) {
            app->lifecycle(app, app->state,
                           active ? Z_LC_ACTIVE : Z_LC_INACTIVE);
        }
        // Repaint so any z_app_active()-driven banner updates even without a
        // registered handler.
        app->dirty = true;
    }
}
static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    (void)toplevel;
    ZApp *app = data;
    if (app->lifecycle) {
        app->lifecycle(app, app->state, Z_LC_STOPPED);
    }
    app->running = false;
}
static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

// --- layer-shell (System UI surface role) ---------------------------------
static void layer_surface_configure(void *data,
                                    struct zwlr_layer_surface_v1 *ls,
                                    uint32_t serial, uint32_t width,
                                    uint32_t height) {
    ZApp *app = data;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    // The compositor resolves our anchors/size into a concrete size (e.g. a
    // top-anchored bar gets the full output width and our requested height).
    if (width > 0) {
        app->width = (int)width;
    }
    if (height > 0) {
        app->height = (int)height;
    }
    app->configured = true;
    app->dirty = true;
}
static void layer_surface_closed(void *data,
                                 struct zwlr_layer_surface_v1 *ls) {
    (void)ls;
    ((ZApp *)data)->running = false;
}
static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

// --- output (to learn the display size) -----------------------------------
static void output_geometry(void *data, struct wl_output *o, int32_t x,
                            int32_t y, int32_t pw, int32_t ph, int32_t sub,
                            const char *make, const char *model,
                            int32_t transform) {
    (void)data; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub;
    (void)make; (void)model; (void)transform;
}
static void output_mode(void *data, struct wl_output *o, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh) {
    (void)o; (void)refresh;
    ZApp *app = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        app->out_width = width;
        app->out_height = height;
    }
}
static void output_done(void *data, struct wl_output *o) { (void)data; (void)o; }
static void output_scale(void *data, struct wl_output *o, int32_t s) {
    (void)data; (void)o; (void)s;
}
static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
};

// --- pointer --------------------------------------------------------------
static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t sx,
                          wl_fixed_t sy) {
    (void)p; (void)serial; (void)surface;
    ZApp *app = data;
    app->ptr_x = wl_fixed_to_double(sx);
    app->ptr_y = wl_fixed_to_double(sy);
}
static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial,
                          struct wl_surface *surface) {
    (void)data; (void)p; (void)serial; (void)surface;
}
// Movement past this many pixels turns a press into a pan (cancelling the tap).
#define Z_PAN_SLOP 8.0

static void dispatch_pan(ZApp *app, ZPanPhase phase) {
    if (!app->pan_target || !app->pan_target->on_pan) {
        return;
    }
    ZPanEvent e = {
        .x = (float)app->ptr_x,
        .y = (float)app->ptr_y,
        .translation_x = (float)(app->ptr_x - app->press_x),
        .translation_y = (float)(app->ptr_y - app->press_y),
        .velocity_x = 0.0f,
        .velocity_y = (float)app->drag_vel_y,
        .phase = phase,
    };
    app->pan_target->on_pan(app, app->state, &e);
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time,
                           wl_fixed_t sx, wl_fixed_t sy) {
    (void)p; (void)time;
    ZApp *app = data;
    app->ptr_x = wl_fixed_to_double(sx);
    app->ptr_y = wl_fixed_to_double(sy);

    if (!app->ptr_down) {
        return;
    }

    double dx = app->ptr_x - app->press_x;
    double dy = app->ptr_y - app->press_y;
    if (!app->panning && (dx * dx + dy * dy) > (Z_PAN_SLOP * Z_PAN_SLOP)) {
        // Slop crossed: this is a drag. Pick a target (custom OnPan first, else
        // the scroll container under the press) and begin.
        app->panning = true;
        app->pan_target = find_pan(app->root, app->press_x, app->press_y);
        if (!app->pan_target) {
            ZView sv = find_scroll(app->root, app->press_x, app->press_y);
            app->drag_scroll = sv ? sv->scroll : NULL;
            if (app->drag_scroll) {
                z_scroll_begin_drag(app->drag_scroll);
            }
        }
        dispatch_pan(app, Z_PAN_BEGIN);
    }

    if (app->panning) {
        double now = z_now_seconds();
        double mdt = now - app->last_motion_s;
        if (mdt < 0.001) {
            mdt = 0.001;
        }
        double vy = (app->ptr_y - app->last_y) / mdt;   // finger velocity (px/s)
        app->drag_vel_y = vy;
        if (app->drag_scroll) {
            // Content follows the finger: dragging down (y increases) scrolls up.
            z_scroll_drag_by(app->drag_scroll, -(float)(app->ptr_y - app->last_y));
        }
        dispatch_pan(app, Z_PAN_CHANGED);
        app->last_x = app->ptr_x;
        app->last_y = app->ptr_y;
        app->last_motion_s = now;
    }
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t state) {
    (void)p; (void)serial; (void)time;
    ZApp *app = data;
    if (button != BTN_LEFT) {
        return;
    }
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        // Arm the recognizer; defer the tap-vs-pan decision to release/motion.
        app->ptr_down = true;
        app->panning = false;
        app->drag_scroll = NULL;
        app->pan_target = NULL;
        app->drag_vel_y = 0.0;
        app->press_x = app->last_x = app->ptr_x;
        app->press_y = app->last_y = app->ptr_y;
        app->last_motion_s = z_now_seconds();
        return;
    }
    // Release.
    if (!app->ptr_down) {
        return;
    }
    app->ptr_down = false;
    if (!app->panning) {
        // No drag: it was a tap. Hit-test and run the deepest handler.
        ZView hit = hit_test(app->root, app->ptr_x, app->ptr_y);
        dispatch_tap(app, hit);
        return;
    }
    // Drag end: edge-swipe-from-left pops the navigator; otherwise fling/notify.
    double total_dx = app->ptr_x - app->press_x;
    if (app->press_x < 32.0 && total_dx > (double)app->width * 0.30) {
        z_nav_pop(z_navigation(app));
    } else if (app->drag_scroll) {
        z_scroll_end_drag(app->drag_scroll, -(float)app->drag_vel_y);
    }
    dispatch_pan(app, Z_PAN_END);
    app->panning = false;
}

static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time,
                         uint32_t axis, wl_fixed_t value) {
    (void)p; (void)time;
    ZApp *app = data;
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
        return;
    }
    ZView sv = find_scroll(app->root, app->ptr_x, app->ptr_y);
    if (sv && sv->scroll) {
        // Wheel notches arrive in ~10px units; scale to a comfortable step.
        z_scroll_drag_by(sv->scroll, (float)wl_fixed_to_double(value) * 4.0f);
    }
}
static void pointer_frame(void *data, struct wl_pointer *p) {
    (void)data; (void)p;
}
static void pointer_axis_source(void *data, struct wl_pointer *p,
                                uint32_t source) {
    (void)data; (void)p; (void)source;
}
static void pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t time,
                              uint32_t axis) {
    (void)data; (void)p; (void)time; (void)axis;
}
static void pointer_axis_discrete(void *data, struct wl_pointer *p,
                                  uint32_t axis, int32_t discrete) {
    (void)data; (void)p; (void)axis; (void)discrete;
}
static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

// --- keyboard -------------------------------------------------------------
static void kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
                      int32_t fd, uint32_t size) {
    (void)kb;
    ZApp *app = data;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        fprintf(stderr, "zelto: keymap format %u unsupported\n", format);
        close(fd);
        return;
    }
    if (!app->xkb_ctx) {
        close(fd);
        return;
    }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        fprintf(stderr, "zelto: keymap mmap failed\n");
        return;
    }
    struct xkb_keymap *keymap = xkb_keymap_new_from_string(
        app->xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!keymap) {
        fprintf(stderr, "zelto: keymap compile failed (size %u)\n", size);
        return;
    }
    struct xkb_state *st = xkb_state_new(keymap);
    if (!st) {
        fprintf(stderr, "zelto: xkb_state_new failed\n");
        xkb_keymap_unref(keymap);
        return;
    }
    if (app->xkb_state) {
        xkb_state_unref(app->xkb_state);
    }
    if (app->xkb_keymap) {
        xkb_keymap_unref(app->xkb_keymap);
    }
    app->xkb_keymap = keymap;
    app->xkb_state = st;
    fprintf(stderr, "zelto: xkb keymap compiled (keyboard ready)\n");
}
static void kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
                     struct wl_surface *surface, struct wl_array *keys) {
    (void)data; (void)kb; (void)serial; (void)surface; (void)keys;
}
static void kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
                     struct wl_surface *surface) {
    (void)data; (void)kb; (void)serial; (void)surface;
}
static void kb_key(void *data, struct wl_keyboard *kb, uint32_t serial,
                   uint32_t time, uint32_t key, uint32_t state) {
    (void)kb; (void)serial; (void)time;
    ZApp *app = data;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
        return;
    }
    // Translate to a keysym when xkb is available; if not, deliver the raw
    // event with sym 0 so handlers still fire (key delivery shouldn't depend on
    // the keymap dataset being present). evdev keycodes are +8 in xkb.
    xkb_keysym_t sym = XKB_KEY_NoSymbol;
    if (app->xkb_state) {
        sym = xkb_state_key_get_one_sym(app->xkb_state, key + 8);
    }
    // System back: Escape / Backspace pop the navigator if it has a stack.
    if ((sym == XKB_KEY_Escape || sym == XKB_KEY_BackSpace) &&
        app->ui.nav_used && app->ui.nav.depth > 1) {
        z_nav_pop(&app->ui.nav);
        return;
    }
    ZView f = app->focused;
    if (f && f->on_key) {
        f->on_key(app, app->state, (uint32_t)sym);
    } else if (f && f->on_tap &&
               (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter ||
                sym == XKB_KEY_space)) {
        // Activate the focused control from the keyboard.
        dispatch_tap(app, f);
    }
}
static void kb_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
                         uint32_t mods_depressed, uint32_t mods_latched,
                         uint32_t mods_locked, uint32_t group) {
    (void)kb; (void)serial;
    ZApp *app = data;
    if (app->xkb_state) {
        xkb_state_update_mask(app->xkb_state, mods_depressed, mods_latched,
                              mods_locked, 0, 0, group);
    }
}
static void kb_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate,
                           int32_t delay) {
    (void)data; (void)kb; (void)rate; (void)delay;
}
static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = kb_keymap,
    .enter = kb_enter,
    .leave = kb_leave,
    .key = kb_key,
    .modifiers = kb_modifiers,
    .repeat_info = kb_repeat_info,
};

// --- seat -----------------------------------------------------------------
static void seat_capabilities(void *data, struct wl_seat *seat,
                              uint32_t caps) {
    ZApp *app = data;
    bool has_ptr = caps & WL_SEAT_CAPABILITY_POINTER;
    bool has_kb = caps & WL_SEAT_CAPABILITY_KEYBOARD;

    if (has_ptr && !app->pointer) {
        app->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(app->pointer, &pointer_listener, app);
    } else if (!has_ptr && app->pointer) {
        wl_pointer_destroy(app->pointer);
        app->pointer = NULL;
    }
    if (has_kb && !app->keyboard) {
        app->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(app->keyboard, &keyboard_listener, app);
    } else if (!has_kb && app->keyboard) {
        wl_keyboard_destroy(app->keyboard);
        app->keyboard = NULL;
    }
}
static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data; (void)seat; (void)name;
}
static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

// --- foreign-toplevel management (task switcher client) -------------------
// Each handle the compositor publishes mirrors one running app window. We keep a
// retained record per handle (title/app_id/active), mutated by its events, and
// expose a filtered snapshot to body() via z_running_apps.
static void ftl_handle_title(void *data,
                             struct zwlr_foreign_toplevel_handle_v1 *h,
                             const char *title) {
    (void)h;
    ZTaskRec *rec = data;
    free(rec->title);
    rec->title = title ? strdup(title) : NULL;
    z_invalidate(rec_app(rec));
}
static void ftl_handle_app_id(void *data,
                              struct zwlr_foreign_toplevel_handle_v1 *h,
                              const char *app_id) {
    (void)h;
    ZTaskRec *rec = data;
    free(rec->app_id);
    rec->app_id = app_id ? strdup(app_id) : NULL;
    z_invalidate(rec_app(rec));
}
static void ftl_handle_output_enter(
    void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
    struct wl_output *o) {
    (void)data; (void)h; (void)o;
}
static void ftl_handle_output_leave(
    void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
    struct wl_output *o) {
    (void)data; (void)h; (void)o;
}
static void ftl_handle_state(void *data,
                             struct zwlr_foreign_toplevel_handle_v1 *h,
                             struct wl_array *state) {
    (void)h;
    ZTaskRec *rec = data;
    bool active = false;
    uint32_t *s;
    wl_array_for_each(s, state) {
        if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) {
            active = true;
        }
    }
    rec->active = active;
    z_invalidate(rec_app(rec));
}
static void ftl_handle_done(void *data,
                            struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void)h;
    z_invalidate(rec_app((ZTaskRec *)data));
}
static void ftl_handle_closed(void *data,
                              struct zwlr_foreign_toplevel_handle_v1 *h) {
    ZTaskRec *rec = data;
    ZApp *app = rec_app(rec);
    zwlr_foreign_toplevel_handle_v1_destroy(h);
    free(rec->title);
    free(rec->app_id);
    rec->title = rec->app_id = NULL;
    rec->handle = NULL;
    rec->active = false;
    rec->used = false;
    z_invalidate(app);
}
static void ftl_handle_parent(void *data,
                              struct zwlr_foreign_toplevel_handle_v1 *h,
                              struct zwlr_foreign_toplevel_handle_v1 *parent) {
    (void)data; (void)h; (void)parent;
}
static const struct zwlr_foreign_toplevel_handle_v1_listener ftl_handle_listener = {
    .title = ftl_handle_title,
    .app_id = ftl_handle_app_id,
    .output_enter = ftl_handle_output_enter,
    .output_leave = ftl_handle_output_leave,
    .state = ftl_handle_state,
    .done = ftl_handle_done,
    .closed = ftl_handle_closed,
    .parent = ftl_handle_parent,
};

// The manager announces a new window: claim a free record, stash the back-
// pointer to the app, and listen on the handle.
static void ftl_manager_toplevel(
    void *data, struct zwlr_foreign_toplevel_manager_v1 *mgr,
    struct zwlr_foreign_toplevel_handle_v1 *handle) {
    (void)mgr;
    ZApp *app = data;
    for (int i = 0; i < Z_MAX_TASKS; i++) {
        ZTaskRec *rec = &app->ftl_recs[i];
        if (!rec->used) {
            rec->used = true;
            rec->handle = handle;
            rec->title = rec->app_id = NULL;
            rec->active = false;
            rec->app = app;
            zwlr_foreign_toplevel_handle_v1_add_listener(
                handle, &ftl_handle_listener, rec);
            z_invalidate(app);
            return;
        }
    }
    // Table full: ignore (the window simply won't appear in the switcher).
}
static void ftl_manager_finished(
    void *data, struct zwlr_foreign_toplevel_manager_v1 *mgr) {
    (void)data;
    zwlr_foreign_toplevel_manager_v1_destroy(mgr);
}
static const struct zwlr_foreign_toplevel_manager_v1_listener
    ftl_manager_listener = {
        .toplevel = ftl_manager_toplevel,
        .finished = ftl_manager_finished,
};

// --- registry -------------------------------------------------------------
static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
    ZApp *app = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        app->compositor = wl_registry_bind(registry, name,
                                           &wl_compositor_interface,
                                           version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        app->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        app->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface,
                                        version < 3 ? version : 3);
        xdg_wm_base_add_listener(app->wm_base, &wm_base_listener, app);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        app->layer_shell = wl_registry_bind(
            registry, name, &zwlr_layer_shell_v1_interface,
            version < 4 ? version : 4);
    } else if (strcmp(interface,
                      zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
        // Task switcher: bind the manager and start receiving running-window
        // handles. Every app binds it (harmless); the launcher is the consumer.
        app->ftl_manager = wl_registry_bind(
            registry, name, &zwlr_foreign_toplevel_manager_v1_interface,
            version < 3 ? version : 3);
        zwlr_foreign_toplevel_manager_v1_add_listener(
            app->ftl_manager, &ftl_manager_listener, app);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (!app->output) {
            app->output = wl_registry_bind(registry, name, &wl_output_interface,
                                           version < 2 ? version : 2);
            wl_output_add_listener(app->output, &output_listener, app);
        }
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        if (!app->seat) {
            app->seat = wl_registry_bind(registry, name, &wl_seat_interface,
                                         version < 5 ? version : 5);
            wl_seat_add_listener(app->seat, &seat_listener, app);
        }
    }
}
static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name) {
    (void)data; (void)registry; (void)name;
}
static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

// --- entry ----------------------------------------------------------------
// Create the surface with the role chosen at launch: an xdg_toplevel (normal
// app) or a wlr-layer-shell surface (System UI). Both feed the same render loop.
static void create_surface(ZApp *app) {
    app->surface = wl_compositor_create_surface(app->compositor);

    if (app->is_layer) {
        // Anchor a layer surface (status bar / launcher background). The output
        // is left NULL so the compositor assigns its only output.
        app->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
            app->layer_shell, app->surface, NULL,
            (enum zwlr_layer_shell_v1_layer)app->layer_opts.layer, app->title);
        zwlr_layer_surface_v1_set_anchor(app->layer_surface,
                                         app->layer_opts.anchor);
        zwlr_layer_surface_v1_set_exclusive_zone(app->layer_surface,
                                                 app->layer_opts.exclusive_zone);
        zwlr_layer_surface_v1_set_size(app->layer_surface,
                                       (uint32_t)(app->layer_opts.width > 0
                                                      ? app->layer_opts.width
                                                      : 0),
                                       (uint32_t)(app->layer_opts.height > 0
                                                      ? app->layer_opts.height
                                                      : 0));
        // A passive surface (status bar) takes no keyboard focus; a modal dialog
        // requests EXCLUSIVE so the compositor routes the keyboard to it.
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            app->layer_surface,
            app->layer_opts.keyboard
                ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE
                : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        zwlr_layer_surface_v1_add_listener(app->layer_surface,
                                           &layer_surface_listener, app);
        // Seed a size; the configure event delivers the authoritative one.
        if (app->layer_opts.width > 0) {
            app->width = app->layer_opts.width;
        }
        if (app->layer_opts.height > 0) {
            app->height = app->layer_opts.height;
        }
        // Commit with no buffer to trigger the first configure.
        wl_surface_commit(app->surface);
        return;
    }

    app->xdg_surface = xdg_wm_base_get_xdg_surface(app->wm_base, app->surface);
    xdg_surface_add_listener(app->xdg_surface, &xdg_surface_listener, app);
    app->xdg_toplevel = xdg_surface_get_toplevel(app->xdg_surface);
    xdg_toplevel_add_listener(app->xdg_toplevel, &toplevel_listener, app);
    xdg_toplevel_set_title(app->xdg_toplevel, app->title);
    xdg_toplevel_set_app_id(app->xdg_toplevel, app->app_id);
    wl_surface_commit(app->surface);
}

// Shared runtime: open resources, bind globals, create the surface for the
// chosen role, run the build->paint loop, tear down. Caller pre-fills state/
// body/title/app_id and (for the layer role) is_layer + layer_opts.
static int app_run(ZApp *app) {
    app->running = true;
    app->perm_fd = -1;
    app->ctrl_fd = -1;
    z_active_app = app;

    const char *font = getenv("ZELTO_FONT");
    app->text = z_text_open(font ? font : Z_DEFAULT_FONT);
    if (!app->text) {
        fprintf(stderr, "zelto: warning: could not open font (text disabled)\n");
    }

    app->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!app->xkb_ctx) {
        fprintf(stderr, "zelto: warning: no xkb context (keyboard disabled)\n");
    }

    app->display = wl_display_connect(NULL);
    if (!app->display) {
        fprintf(stderr, "zelto: cannot connect to Wayland display\n");
        return 1;
    }
    app->registry = wl_display_get_registry(app->display);
    wl_registry_add_listener(app->registry, &registry_listener, app);
    // Round-trip once to bind globals, again to receive the output mode.
    wl_display_roundtrip(app->display);
    wl_display_roundtrip(app->display);

    bool need_shell = app->is_layer ? (app->layer_shell != NULL)
                                    : (app->wm_base != NULL);
    if (!app->compositor || !app->shm || !need_shell) {
        fprintf(stderr, "zelto: compositor missing required globals%s\n",
                app->is_layer ? " (wlr-layer-shell?)" : "");
        return 1;
    }

    // Default surface size: fill the output if we learned its mode.
    app->width = app->out_width > 0 ? app->out_width : 800;
    app->height = app->out_height > 0 ? app->out_height : 600;

    create_surface(app);

    // Open the persistent intents control connection and register this app_id as
    // a mailbox, so zsysd can push delivered deep links / shares to us (and we
    // can send resolve requests on it). Best-effort: no broker -> no intents.
    ctrl_connect_register(app);

    // Multi-fd loop: poll the wayland fd plus (when present) the zsysd perm
    // socket of an in-flight request and the persistent intents control socket,
    // so a broker reply or a pushed intent wakes us without blocking on wayland.
    // The wl_display_prepare_read/read_events dance is the canonical way to mix a
    // wayland fd with other fds in one poll without losing events.
    struct wl_display *dpy = app->display;
    while (app->running) {
        while (wl_display_prepare_read(dpy) != 0) {
            wl_display_dispatch_pending(dpy);
        }
        wl_display_flush(dpy);

        struct pollfd pfds[3];
        pfds[0].fd = wl_display_get_fd(dpy);
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        nfds_t nf = 1;
        int perm_slot = -1, ctrl_slot = -1;
        if (app->perm_fd >= 0) {
            perm_slot = (int)nf;
            pfds[nf].fd = app->perm_fd;
            pfds[nf].events = POLLIN;
            pfds[nf].revents = 0;
            nf++;
        }
        if (app->ctrl_fd >= 0) {
            ctrl_slot = (int)nf;
            pfds[nf].fd = app->ctrl_fd;
            pfds[nf].events = POLLIN;
            pfds[nf].revents = 0;
            nf++;
        }

        if (poll(pfds, nf, -1) < 0) {
            wl_display_cancel_read(dpy);
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        // Wayland: complete the armed read if data came, else cancel it.
        if (pfds[0].revents & POLLIN) {
            if (wl_display_read_events(dpy) < 0) {
                break;
            }
        } else {
            wl_display_cancel_read(dpy);
        }
        if (wl_display_dispatch_pending(dpy) < 0) {
            break;
        }

        // Perm broker reply (cached fast-path or post-prompt decision).
        if (perm_slot >= 0 &&
            (pfds[perm_slot].revents & (POLLIN | POLLHUP | POLLERR))) {
            perm_handle_reply(app);
        }
        // Intents control socket: a pushed deliver (or its EOF).
        if (ctrl_slot >= 0 &&
            (pfds[ctrl_slot].revents & (POLLIN | POLLHUP | POLLERR))) {
            ctrl_handle_read(app);
        }

        // Render when state is dirty and no frame is in flight; render() arms a
        // frame callback so the next paint waits for vsync. Input handlers and
        // the perm callback set dirty via z_invalidate.
        if (app->configured && app->dirty && !app->frame_cb) {
            app->dirty = false;
            render(app);
        }
    }

    if (app->perm_fd >= 0) {
        close(app->perm_fd);
        app->perm_fd = -1;
    }
    if (app->ctrl_fd >= 0) {
        close(app->ctrl_fd);
        app->ctrl_fd = -1;
    }
    z_active_app = NULL;

    if (app->xkb_state) {
        xkb_state_unref(app->xkb_state);
    }
    if (app->xkb_keymap) {
        xkb_keymap_unref(app->xkb_keymap);
    }
    if (app->xkb_ctx) {
        xkb_context_unref(app->xkb_ctx);
    }
    z_text_close(app->text);
    buf_free(&app->bufs[0]);
    buf_free(&app->bufs[1]);
    z_arena_free(&app->arenas[0]);
    z_arena_free(&app->arenas[1]);
    wl_display_disconnect(app->display);
    return 0;
}

int z_app_main_id(void *state, ZBodyFn body, const char *title,
                  const char *app_id) {
    ZApp app = {0};
    app.state = state;
    app.body = body;
    app.title = title ? title : "Zelto App";
    app.app_id = app_id ? app_id : "os.zelto.app";
    return app_run(&app);
}

int z_app_main(void *state, ZBodyFn body, const char *title) {
    return z_app_main_id(state, body, title, "os.zelto.app");
}

int z_layer_app_main(void *state, ZBodyFn body, const char *title,
                     const ZLayerOpts *opts) {
    ZApp app = {0};
    app.state = state;
    app.body = body;
    app.title = title ? title : "Zelto Layer";
    app.app_id = "os.zelto.layer";
    app.is_layer = true;
    if (opts) {
        app.layer_opts = *opts;
    }
    return app_run(&app);
}

void z_invalidate(ZApp *app) { app->dirty = true; }
void z_app_quit(ZApp *app) { app->running = false; }

// --- permission broker (zsysd) client -------------------------------------
// zsysd speaks a newline-delimited JSON-ish protocol over a SOCK_STREAM unix
// socket at $XDG_RUNTIME_DIR/zsysd.sock. The client sends one request line —
// {"op":"perm_status"|"perm_request","app_id":..,"perm":..} — and reads back one
// reply line — {"status":"granted|denied|prompt"}. status is a fast round-trip;
// request may block on a user prompt, so its socket is parked in the app loop.
//
// z_active_app is declared near the top of this file (set for app_run's life).
static int zsysd_connect(void) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime) {
        runtime = "/run";
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/zsysd.sock", runtime);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static ZPermStatus zsysd_parse_status(const char *line) {
    if (strstr(line, "granted")) {
        return Z_PERM_GRANTED;
    }
    if (strstr(line, "denied")) {
        return Z_PERM_DENIED;
    }
    return Z_PERM_PROMPT;
}

// Read one newline-terminated reply line (blocking). False on EOF/error.
static bool zsysd_read_line(int fd, char *buf, size_t n) {
    size_t len = 0;
    while (len + 1 < n) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r <= 0) {
            return false;
        }
        if (c == '\n') {
            break;
        }
        buf[len++] = c;
    }
    buf[len] = '\0';
    return true;
}

static int zsysd_send(int fd, const char *op, const char *app_id,
                      const char *perm) {
    char msg[256];
    int m = snprintf(msg, sizeof(msg),
                     "{\"op\":\"%s\",\"app_id\":\"%s\",\"perm\":\"%s\"}\n", op,
                     app_id ? app_id : "", perm);
    if (m <= 0 || m >= (int)sizeof(msg)) {
        return -1;
    }
    return write(fd, msg, (size_t)m) == m ? 0 : -1;
}

ZPermStatus z_perm_status(const char *name) {
    ZApp *app = z_active_app;
    if (!app || !name) {
        return Z_PERM_PROMPT;
    }
    int fd = zsysd_connect();
    if (fd < 0) {
        return Z_PERM_PROMPT;   // no broker reachable: behave as "may request"
    }
    ZPermStatus st = Z_PERM_PROMPT;
    if (zsysd_send(fd, "perm_status", app->app_id, name) == 0) {
        char line[256];
        if (zsysd_read_line(fd, line, sizeof(line))) {
            st = zsysd_parse_status(line);
        }
    }
    close(fd);
    return st;
}

void z_perm_request(const char *name, ZPermCallback cb, void *ud) {
    ZApp *app = z_active_app;
    // Deny if there is no app, no name, or a request is already outstanding
    // (one at a time keeps the loop integration trivial).
    if (!app || !name || app->perm_fd >= 0) {
        if (cb) {
            cb(app, Z_PERM_DENIED, ud);
        }
        return;
    }
    int fd = zsysd_connect();
    if (fd < 0 || zsysd_send(fd, "perm_request", app->app_id, name) != 0) {
        if (fd >= 0) {
            close(fd);
        }
        if (cb) {
            cb(app, Z_PERM_DENIED, ud);
        }
        return;
    }
    // Park the fd; poll() in the loop wakes when zsysd replies (which may be
    // after the user answers a consent dialog), then dispatches the callback.
    app->perm_fd = fd;
    app->perm_cb = cb;
    app->perm_ud = ud;
}

// The perm socket is readable: read the decision and fire the callback.
static void perm_handle_reply(ZApp *app) {
    char line[256];
    ZPermStatus st = Z_PERM_DENIED;
    if (zsysd_read_line(app->perm_fd, line, sizeof(line))) {
        st = zsysd_parse_status(line);
    }
    close(app->perm_fd);
    app->perm_fd = -1;
    ZPermCallback cb = app->perm_cb;
    void *ud = app->perm_ud;
    app->perm_cb = NULL;
    app->perm_ud = NULL;
    if (cb) {
        cb(app, st, ud);
    }
}

// --- intents (zsysd) client -----------------------------------------------
// A persistent control connection to zsysd is the app's intents mailbox. At
// startup the app registers its app_id on it; zsysd then pushes deep links and
// shares destined for this app as one-line {"op":"deliver",..} messages, which
// the app loop reads and dispatches to z_on_open_url / z_on_share_target. The
// same socket carries this app's own z_share/z_open_url resolve requests up to
// the broker. The protocol is the same newline-delimited JSON-ish framing the
// perm path uses; a tiny json_get extracts quoted string values.
static bool ctrl_json_get(const char *buf, const char *key, char *out,
                          size_t n) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf, pat);
    if (!p) {
        return false;
    }
    p = strchr(p + strlen(pat), ':');
    if (!p) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

// Connect the persistent control socket and register this app_id as its mailbox.
static void ctrl_connect_register(ZApp *app) {
    int fd = zsysd_connect();
    if (fd < 0) {
        return;   // no broker: app simply receives/sends no intents
    }
    char msg[160];
    int m = snprintf(msg, sizeof(msg), "{\"op\":\"register\",\"app_id\":\"%s\"}\n",
                     app->app_id ? app->app_id : "");
    if (m <= 0 || write(fd, msg, (size_t)m) != m) {
        close(fd);
        return;
    }
    app->ctrl_fd = fd;
    app->ctrl_len = 0;
}

// Deliver a shared item to the app: stash it (so it survives until the handler
// is registered) and dispatch now if a handler already exists.
static void deliver_share(ZApp *app, const char *mime, const char *text) {
    snprintf(app->pend_mime, sizeof(app->pend_mime), "%s", mime ? mime : "");
    snprintf(app->pend_text, sizeof(app->pend_text), "%s", text ? text : "");
    app->pend_share = true;
    if (app->share_cb) {
        ZShareItem it = {app->pend_mime, app->pend_text};
        app->share_cb(app, &it, 1, app->share_ud);
        app->pend_share = false;
    }
    z_invalidate(app);
}

// Deliver an opened URL to the app (same stash-then-dispatch rule).
static void deliver_url(ZApp *app, const char *url) {
    snprintf(app->pend_url_buf, sizeof(app->pend_url_buf), "%s", url ? url : "");
    app->pend_url = true;
    if (app->url_cb) {
        app->url_cb(app, app->pend_url_buf, app->url_ud);
        app->pend_url = false;
    }
    z_invalidate(app);
}

// Parse and act on one pushed control line ({"op":"deliver",..}).
static void ctrl_dispatch_line(ZApp *app, const char *line) {
    char op[24] = {0};
    if (!ctrl_json_get(line, "op", op, sizeof(op))) {
        return;
    }
    if (strcmp(op, "deliver") != 0) {
        return;
    }
    char kind[16] = {0};
    ctrl_json_get(line, "kind", kind, sizeof(kind));
    if (strcmp(kind, "share") == 0) {
        char mime[64] = {0}, payload[256] = {0};
        ctrl_json_get(line, "mime", mime, sizeof(mime));
        ctrl_json_get(line, "payload", payload, sizeof(payload));
        deliver_share(app, mime, payload);
    } else if (strcmp(kind, "open_url") == 0) {
        char url[256] = {0};
        ctrl_json_get(line, "url", url, sizeof(url));
        deliver_url(app, url);
    }
}

// The control socket is readable: accumulate and dispatch every complete line.
static void ctrl_handle_read(ZApp *app) {
    if (app->ctrl_len >= sizeof(app->ctrl_buf) - 1) {
        app->ctrl_len = 0;   // overlong line: drop (our messages are small)
    }
    ssize_t r = read(app->ctrl_fd, app->ctrl_buf + app->ctrl_len,
                     sizeof(app->ctrl_buf) - 1 - app->ctrl_len);
    if (r <= 0) {
        close(app->ctrl_fd);
        app->ctrl_fd = -1;
        return;
    }
    app->ctrl_len += (size_t)r;
    app->ctrl_buf[app->ctrl_len] = '\0';
    char *start = app->ctrl_buf;
    char *nl;
    while ((nl = strchr(start, '\n')) != NULL) {
        *nl = '\0';
        ctrl_dispatch_line(app, start);
        start = nl + 1;
    }
    // Shift any partial trailing line to the front of the buffer.
    size_t rem = app->ctrl_len - (size_t)(start - app->ctrl_buf);
    memmove(app->ctrl_buf, start, rem);
    app->ctrl_len = rem;
}

// Senders. Both resolve through the broker over the persistent control socket;
// the broker shows the chooser (if a choice), launches/activates the target and
// pushes the deliver to it. Fire-and-forget: the sender expects no reply.
void z_open_url(const char *url) {
    ZApp *app = z_active_app;
    if (!app || app->ctrl_fd < 0 || !url) {
        return;
    }
    char msg[320];
    int m = snprintf(msg, sizeof(msg),
                     "{\"op\":\"intent_resolve\",\"action\":\"open_url\","
                     "\"url\":\"%s\"}\n",
                     url);
    if (m > 0 && m < (int)sizeof(msg)) {
        ssize_t w = write(app->ctrl_fd, msg, (size_t)m);
        (void)w;
    }
}

void z_share(ZShareItem *items, int count) {
    ZApp *app = z_active_app;
    if (!app || app->ctrl_fd < 0 || !items || count < 1) {
        return;
    }
    // The MVP shares a single text item; resolve it by its MIME type.
    const char *mime = items[0].mime ? items[0].mime : "text/plain";
    const char *text = items[0].text ? items[0].text : "";
    char msg[384];
    int m = snprintf(msg, sizeof(msg),
                     "{\"op\":\"intent_resolve\",\"action\":\"share\","
                     "\"mime\":\"%s\",\"payload\":\"%s\"}\n",
                     mime, text);
    if (m > 0 && m < (int)sizeof(msg)) {
        ssize_t w = write(app->ctrl_fd, msg, (size_t)m);
        (void)w;
    }
}

void z_on_open_url(ZApp *app, ZUrlCb cb, void *ud) {
    app->url_cb = cb;
    app->url_ud = ud;
    if (cb && app->pend_url) {
        app->pend_url = false;
        cb(app, app->pend_url_buf, ud);
        z_invalidate(app);
    }
}

void z_on_share_target(ZApp *app, ZShareCb cb, void *ud) {
    app->share_cb = cb;
    app->share_ud = ud;
    if (cb && app->pend_share) {
        app->pend_share = false;
        ZShareItem it = {app->pend_mime, app->pend_text};
        cb(app, &it, 1, ud);
        z_invalidate(app);
    }
}

// --- lifecycle + task-switcher public API ---------------------------------
void z_on_lifecycle(ZApp *app, ZLifecycleHandler handler) {
    app->lifecycle = handler;
}
bool z_app_active(ZApp *app) { return app->active; }

const ZTask *z_running_apps(ZApp *app, int *count) {
    // Rebuild the snapshot from the live records, filtering out this app's own
    // window (matched by app_id) so the launcher never lists itself.
    int n = 0;
    for (int i = 0; i < Z_MAX_TASKS && n < Z_MAX_TASKS; i++) {
        ZTaskRec *rec = &app->ftl_recs[i];
        if (!rec->used || !rec->handle) {
            continue;
        }
        if (rec->app_id && app->app_id &&
            strcmp(rec->app_id, app->app_id) == 0) {
            continue;
        }
        app->ftl_snapshot[n] = (ZTask){
            .title = rec->title,
            .app_id = rec->app_id,
            .active = rec->active,
            .handle = rec->handle,
        };
        n++;
    }
    if (count) {
        *count = n;
    }
    return app->ftl_snapshot;
}

void z_task_activate(ZApp *app, const ZTask *task) {
    if (!task || !task->handle || !app->seat) {
        return;
    }
    zwlr_foreign_toplevel_handle_v1_activate(task->handle, app->seat);
}

void z_task_close(ZApp *app, const ZTask *task) {
    (void)app;
    if (!task || !task->handle) {
        return;
    }
    zwlr_foreign_toplevel_handle_v1_close(task->handle);
}
