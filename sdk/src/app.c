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
#include <time.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <ctype.h>
#include <fcntl.h>

#include "internal.h"
#include "ext-idle-notify-v1-client-protocol.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"
#include "input-method-unstable-v2-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "zelto-backdrop-v1-client-protocol.h"
#include "zelto-toplevel-capture-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define Z_DEFAULT_FONT "/usr/share/zelto/fonts/Satoshi-Variable.ttf"

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

    // Window snapshot (zelto-toplevel-capture-v1). `cap` is created lazily, the
    // first time the app asks for this window's picture, so an app that never
    // calls z_snapshot costs nothing. `snap_key` is this record's stable key
    // into the image cache; `has_snap` says whether a picture has arrived yet.
    struct zelto_toplevel_capture_v1 *cap;
    char snap_key[48];
    bool has_snap;
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
    // Cached input-region request (z_layer_set_input_region), to dedup commits.
    // ir_none = an EMPTY region (catches nothing, fully input-transparent — the
    // brightness-dim overlay); distinct from ir_whole (NULL region = whole).
    bool ir_valid, ir_whole, ir_none;
    int ir_x, ir_y, ir_w, ir_h;
    // Cached EXCLUSIVE-keyboard state (z_layer_set_keyboard), to dedup commits.
    // A lock screen flips this true while locked, false on unlock.
    bool kbd_valid, kbd_exclusive;

    // zelto-backdrop-v1: the blurred material under this surface (z_backdrop).
    // A client cannot see through itself, so the compositor blurs the scene under
    // the rectangle we declare and parks it beneath us; we paint our translucent
    // material tint over the result. Bound only if the compositor advertises it
    // (a plain Wayland server does not — the material then degrades to its tint).
    // The last declared region is cached so a body() that re-declares it every
    // frame (a shade whose panel is being dragged) only sends a request when it
    // actually moves.
    struct zelto_backdrop_manager_v1 *backdrop_manager;
    struct zelto_backdrop_v1 *backdrop;
    bool bd_valid;
    int bd_x, bd_y, bd_w, bd_h, bd_radius;

    // ext-idle-notify: the notifier global (bound when advertised) plus the list
    // of live idle notifications this app registered (z_idle_notify). Each fires
    // idled/resumed on the seat's activity clock; the lock screen drives its
    // dim/lock/off state machine off them.
    struct ext_idle_notifier_v1 *idle_notifier;

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

    // Window-snapshot manager (zelto-toplevel-capture-v1). Bound only when the
    // compositor advertises it, so a client built against an older zcomp simply
    // never gets a picture and keeps its icon fallback. See z_snapshot().
    struct zelto_toplevel_capture_manager_v1 *cap_manager;

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

    // Notifications. Poster side: the action receiver fired when an action this
    // app posted is tapped in the shade (pushed over ctrl_fd as a deliver of
    // kind "notify_action"); buffered like an intent if it arrives before the
    // handler is registered (an app relaunched to handle its action).
    ZNotifyActionCb notify_action_cb;
    void *notify_action_ud;
    bool pend_action;
    int64_t pend_action_id;
    char pend_action_buf[64];
    // Shade side: this app (zelto-shade) subscribed as the notification sink, so
    // zsysd pushes notify_show / notify_hide on ctrl_fd.
    ZNotifyShowCb notify_show_cb;
    ZNotifyHideCb notify_hide_cb;
    void *notify_sink_ud;

    // Settings observer: this app called z_settings_observe, so zsysd pushes
    // settings_changed on ctrl_fd whenever any system setting is set (by anyone,
    // including this app). Folded into ctrl_dispatch_line like the notify sink.
    ZSettingsCb settings_cb;
    void *settings_ud;

    // text-input-v3 (P21, the app-with-a-text-field side). One text_input per
    // app for the seat; the compositor sends enter/leave as the surface gains/
    // loses keyboard focus. active_field is the focused TextField (set by a tap);
    // when one is active AND the surface is entered we enable the text input, so
    // the compositor raises the on-screen keyboard, and route its commit_string /
    // delete_surrounding_text into that field's buffer.
    struct zwp_text_input_manager_v3 *ti_manager;
    struct zwp_text_input_v3 *text_input;
    bool ti_entered;                 // compositor sent enter (surface has focus)
    bool ti_enabled;                 // we called enable+commit for active_field
    ZTextField *active_field;        // focused text field (NULL = none)

    // input-method-v2 (P21, the on-screen-keyboard side). Only zelto-keyboard
    // binds it (via z_im_bind). The compositor drives activate/deactivate (show/
    // hide); the keyboard sends commit_string / delete_surrounding_text back. The
    // commit serial equals the number of `done` events received (im_serial).
    struct zwp_input_method_manager_v2 *im_manager;
    struct zwp_input_method_v2 *input_method;
    uint32_t im_serial;              // count of done events (commit serial)
    bool im_active, im_pending_active;
    ZImVisibilityCb im_show_cb, im_hide_cb;
    void *im_ud;

    // Clipboard (P22). Copy/Cut take ownership of the CLIPBOARD selection through
    // the CORE wl_data_device (data_device_mgr + data_device): a wl_data_source is
    // created whose send handler writes the copied text into the requesting
    // client's pipe. last_serial is the most recent input-event serial, required by
    // wl_data_device.set_selection. Paste READS through wlr-data-control (dc_manager
    // + dc_device) instead, because that selection is delivered to every bound
    // client regardless of keyboard focus — the on-screen keyboard is a focus-less
    // layer surface, so it can only read the clipboard this way (the core
    // wl_data_device offer goes to the focused client only). dc_offer is the current
    // selection offer (or NULL); dc_offer_text whether it advertises a text mime;
    // dc_pending_* accumulate a just-introduced offer's mimes until it is promoted
    // to the selection. A z_clipboard_get pipes the offer's bytes into clip_fd,
    // parked in the app loop like perm_fd until EOF, then fires clip_cb.
    struct wl_data_device_manager *data_device_mgr;
    struct wl_data_device *data_device;
    struct wl_data_offer *core_offer;   // last core offer, destroyed to avoid leak
    uint32_t last_serial;

    struct zwlr_data_control_manager_v1 *dc_manager;
    struct zwlr_data_control_device_v1 *dc_device;
    struct zwlr_data_control_offer_v1 *dc_offer;
    bool dc_offer_text;
    struct zwlr_data_control_offer_v1 *dc_pending_offer;
    bool dc_pending_text;
    int clip_fd;                     // in-flight paste read (parked); -1 = idle
    ZClipboardCb clip_cb;
    void *clip_ud;
    char clip_buf[Z_TEXTFIELD_CAP];
    size_t clip_len;

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
    double drag_vel_x;           // latest horizontal finger velocity (px/s)
    // Interruptible Navigator back-swipe (P33): an edge-drag drives the top
    // screen's transition 1:1 with the finger (the screen below sliding under it)
    // instead of popping only on release. Latched at the slop-cross when the press
    // began within the left edge inset and the nav stack can pop.
    bool nav_back;
    ZScroll *drag_scroll;        // scroll being dragged (NULL = none)
    ZView pan_target;            // custom OnPan target (NULL = none)
    // The pan handler resolved when the drag began, cached as a plain function
    // pointer so the gesture survives body() rebuilds: pan_target is an arena
    // ZView that the next rebuild can move/free, so dereferencing it mid-gesture
    // (a body whose tree changes shape between phases — the shade idle strip
    // vs. its pulled-open panel) reads stale memory and drops events.
    ZPanHandler pan_handler;
    // The OnPanData variant, cached the same way and for the same reason. Its
    // `data` is NOT arena memory (the binding points it at stable host state), but
    // it is resolved from the arena node at the slop-cross, so it is latched here
    // alongside the handler rather than re-read from pan_target mid-gesture.
    ZPanDataHandler pan_handler_data;
    void *pan_data;

    // Long-press recognizer. Armed on a press that lands on an OnLongPress node;
    // the app loop polls with a finite timeout so a finger held still wakes us at
    // the threshold. Firing it suppresses the tap the release would emit, and any
    // motion past the slop (-> pan) disarms it first.
    bool long_press_armed, long_pressed;
    double press_s;              // monotonic time of the press
    ZView long_press_target;     // OnLongPress node under the press (NULL = none)

    // One-shot timer (z_after). Not tied to seat activity — a plain wall-clock
    // deadline the app loop bounds its poll() on, firing `after_cb` once. Drives
    // self-dismissing transient UI (the volume HUD). after_armed=false = none.
    bool after_armed;
    double after_deadline;       // monotonic s at which after_cb fires
    ZTimerCb after_cb;
    void *after_ud;

    // Repeating tick (z_tick_every): a periodic z_invalidate heartbeat for
    // self-refreshing widgets (a clock). Independent of the one-shot z_after.
    // tick_want is the shortest interval requested during the CURRENT build
    // (reset to 0 before body(), set by each z_tick_every call); after the build
    // tick_interval/tick_deadline are (re)armed from it, so a build requesting
    // none disarms the beat.
    bool tick_armed;
    double tick_interval;        // armed cadence (s)
    double tick_deadline;        // monotonic s of the next fire
    double tick_want;            // min interval requested this build (0 = none)

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
// Full output size (the advertised mode), independent of this surface's own
// (possibly collapsed) size. Falls back to the surface size before the mode is
// known. The pull-down shade reads these to size its expanded panel while it is
// still a thin strip.
int z_screen_width(ZApp *app) {
    return app->out_width > 0 ? app->out_width : app->width;
}
int z_screen_height(ZApp *app) {
    return app->out_height > 0 ? app->out_height : app->height;
}

// The active app, set for the lifetime of app_run(). The permission API
// (z_perm_status/z_perm_request) takes no ZApp to match the platform docs, so it
// resolves the single per-process app through this. Defined below.
static ZApp *z_active_app;
// The active app's app_id, for the storage client (storage.c), which scopes the
// per-app private data dir by it just as the perm/intent client keys its mailbox.
const char *z_active_app_id(void) {
    return z_active_app ? z_active_app->app_id : NULL;
}
ZTextField *z_app_active_field(ZApp *app) {
    return app ? app->active_field : NULL;
}
static void perm_handle_reply(ZApp *app);
static void press_release(ZApp *app);   // press-feedback spring (P31), defined below
static void stamp_press(ZApp *app, ZView root);
static void clip_handle_read(ZApp *app);
static void ctrl_handle_read(ZApp *app);
static void ctrl_connect_register(ZApp *app);
static void deliver_action(ZApp *app, int64_t id, const char *action_id);

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

// Deepest custom OnPan / OnPanData target under (x,y).
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
    return (n->on_pan || n->on_pan_data) ? n : NULL;
}

// Deepest OnLongPress target under (x,y).
static ZView find_long_press(ZView n, double x, double y) {
    if (!n || !point_in(n, x, y)) {
        return NULL;
    }
    for (int i = n->n_children - 1; i >= 0; i--) {
        ZView h = find_long_press(n->children[i], x, y);
        if (h) {
            return h;
        }
    }
    return n->on_long_press ? n : NULL;
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
    z_keyed_frame_begin(&app->ui.implicit);
    app->ui.transitioning = false;   // the Navigator re-asserts this if mid-slide

    ZView old_root = app->root;
    app->tick_want = 0.0;   // widgets re-declare their cadence each build
    ZView new_root = app->body(app, app->state);
    z_keyed_frame_end(&app->ui.implicit);  // GC keyed cells no view requested

    // Widget heartbeat: (re)arm the repeating tick from what this build asked for
    // (the shortest z_tick_every cadence). Only re-seed the deadline when the beat
    // starts or its interval changes, so a steady 1s tick keeps counting down
    // rather than resetting to a full second on every rebuild.
    if (app->tick_want > 0.0) {
        if (!app->tick_armed || app->tick_interval != app->tick_want) {
            app->tick_interval = app->tick_want;
            app->tick_deadline = z_now_seconds() + app->tick_interval;
            app->tick_armed = true;
        }
    } else {
        app->tick_armed = false;
    }

    z_layout(new_root, (float)app->width, (float)app->height, app->text);

    // Resolve keyboard focus against the fresh tree.
    app->focused = find_focusable(new_root);
    if (app->focused) {
        app->focused->focused = true;
    }

    // Drive text-input-v3 off the focused text field (P21): enable when a field
    // is focused AND our surface has text-input focus (compositor sent enter),
    // which makes the compositor raise the on-screen keyboard; disable otherwise
    // (blur / app backgrounded), which hides it. The flags dedup the commits.
    if (app->text_input) {
        bool want = app->active_field != NULL && app->ti_entered;
        if (want && !app->ti_enabled) {
            zwp_text_input_v3_enable(app->text_input);
            zwp_text_input_v3_set_content_type(
                app->text_input, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
                ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
            zwp_text_input_v3_set_surrounding_text(
                app->text_input, app->active_field->text,
                (uint32_t)app->active_field->caret,
                (uint32_t)app->active_field->caret);
            zwp_text_input_v3_commit(app->text_input);
            app->ti_enabled = true;
        } else if (!want && app->ti_enabled) {
            zwp_text_input_v3_disable(app->text_input);
            zwp_text_input_v3_commit(app->text_input);
            app->ti_enabled = false;
        }
    }

    // Press feedback (P31): stamp the live press amount onto the tappable node
    // under the frozen press point, so the renderer draws its highlight veil. Done
    // before reconcile so a change in the stamped amount damages the control.
    stamp_press(app, new_root);

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
        // when-in-use: pause this app's sensor/GPS streams while it is backgrounded
        // (battery + privacy), resume them on return. No-op if it opened none.
        z_sensor_set_paused(!active);
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
    (void)p; (void)serial; (void)surface;
    // Pointer left the surface: fade any press highlight out (P31).
    press_release((ZApp *)data);
}
// Movement past this many pixels turns a press into a pan (cancelling the tap).
#define Z_PAN_SLOP 8.0
// A still press held this long (seconds) without crossing the slop is a long-press.
#define Z_LONG_PRESS_S 0.45

// Fire the armed long-press: disarm, mark long_pressed so the release suppresses
// the tap, and invoke the target's handler with the press position.
static void fire_long_press(ZApp *app) {
    app->long_press_armed = false;
    app->long_pressed = true;
    ZView t = app->long_press_target;
    if (t && t->on_long_press) {
        t->on_long_press(app, app->state, t->long_press_data,
                         (float)app->press_x, (float)app->press_y);
    }
    // The hold cue (the swelling press veil) ends when the action fires.
    z_animated_spring_with(&app->ui.press, 0.0f, Z_SPRING_PRESS);
}

// --- press feedback (P31) --------------------------------------------------
// One global spring (a phone is single-touch) drives the touch-down highlight
// for whatever tappable node is under the finger. press_begin springs it up when
// a press lands on a tappable node; press_release springs it back on a drag /
// cancel; press_flash_release guarantees a brief visible pulse on a quick tap
// (down+up in one gesture may release before the spring climbed), so a tap always
// registers. stamp_press re-hit-tests at the frozen press point each build and
// stamps node->press, so the veil survives body() rebuilds with no dangling
// pointer into the discarded arena. (hit_test is defined earlier in this file.)
static void press_begin(ZApp *app) {
    if (hit_test(app->root, app->ptr_x, app->ptr_y)) {
        z_animated_spring_with(&app->ui.press, 1.0f, Z_SPRING_PRESS);
    }
}
static void press_release(ZApp *app) {
    if (app->ui.press.value > 0.003f || app->ui.press.animating) {
        z_animated_spring_with(&app->ui.press, 0.0f, Z_SPRING_PRESS);
    }
}
static void press_flash_release(ZApp *app) {
    // Ensure a floor so a fast tap still flashes, then fade out.
    if (app->ui.press.value < 0.55f) {
        app->ui.press.value = 0.55f;
    }
    z_animated_spring_with(&app->ui.press, 0.0f, Z_SPRING_PRESS);
}
static void stamp_press(ZApp *app, ZView root) {
    ZUI *ui = &app->ui;
    float pv = ui->press.value;
    if (!ui->press.animating && pv <= 0.003f) {
        return;
    }
    ZView pn = hit_test(root, app->press_x, app->press_y);
    if (pn) {
        pn->press = pv;
    }
}

static void dispatch_pan(ZApp *app, ZPanPhase phase) {
    // Use the cached handler, not pan_target->on_pan: pan_target is an arena view
    // a rebuild during the gesture may have freed/moved.
    if (!app->pan_handler && !app->pan_handler_data) {
        return;
    }
    ZPanEvent e = {
        .x = (float)app->ptr_x,
        .y = (float)app->ptr_y,
        .translation_x = (float)(app->ptr_x - app->press_x),
        .translation_y = (float)(app->ptr_y - app->press_y),
        .velocity_x = (float)app->drag_vel_x,
        .velocity_y = (float)app->drag_vel_y,
        .phase = phase,
    };
    if (app->pan_handler_data) {
        app->pan_handler_data(app, app->state, app->pan_data, &e);
    } else {
        app->pan_handler(app, app->state, &e);
    }
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time,
                           wl_fixed_t sx, wl_fixed_t sy) {
    (void)p; (void)time;
    ZApp *app = data;
    app->ptr_x = wl_fixed_to_double(sx);
    app->ptr_y = wl_fixed_to_double(sy);

    if (!app->ptr_down || app->long_pressed) {
        // Once a long-press has fired the gesture is consumed: ignore motion
        // until release so it can neither start a pan nor re-fire.
        return;
    }

    double dx = app->ptr_x - app->press_x;
    double dy = app->ptr_y - app->press_y;
    if (!app->panning && (dx * dx + dy * dy) > (Z_PAN_SLOP * Z_PAN_SLOP)) {
        // Slop crossed: this is a drag, not a long-press. Disarm the timer and
        // pick a target (custom OnPan first, else the scroll container under the
        // press) and begin.
        app->long_press_armed = false;
        // A drag is not a tap: release the press highlight as the pan begins.
        press_release(app);
        app->panning = true;
        // A drag that began within the left edge inset, on a poppable nav stack, is
        // the interruptible back-swipe: it drives the top screen out 1:1 (P33),
        // taking precedence over any pan/scroll under the finger.
        if (app->press_x < 32.0 && z_nav_can_back(&app->ui.nav)) {
            app->nav_back = true;
            z_nav_back_begin(&app->ui.nav);
        } else {
            app->pan_target = find_pan(app->root, app->press_x, app->press_y);
            app->pan_handler = app->pan_target ? app->pan_target->on_pan : NULL;
            app->pan_handler_data =
                app->pan_target ? app->pan_target->on_pan_data : NULL;
            app->pan_data = app->pan_target ? app->pan_target->pan_data : NULL;
            if (!app->pan_target) {
                ZView sv = find_scroll(app->root, app->press_x, app->press_y);
                app->drag_scroll = sv ? sv->scroll : NULL;
                if (app->drag_scroll) {
                    z_scroll_begin_drag(app->drag_scroll);
                }
            }
            dispatch_pan(app, Z_PAN_BEGIN);
        }
    }

    if (app->panning) {
        double now = z_now_seconds();
        double mdt = now - app->last_motion_s;
        if (mdt < 0.001) {
            mdt = 0.001;
        }
        app->drag_vel_y = (app->ptr_y - app->last_y) / mdt;   // finger vel (px/s)
        app->drag_vel_x = (app->ptr_x - app->last_x) / mdt;
        if (app->nav_back) {
            // Edge back-swipe: map the horizontal travel to transition progress
            // (1 present -> 0 popped), resisting past either end with the rubber
            // band, and drive the top screen there 1:1.
            float W = (float)app->width;
            float dx = (float)(app->ptr_x - app->press_x);
            if (dx < 0.0f) {
                dx = z_rubber_band(dx, W);          // wrong-way (left): resist
            } else if (dx > W) {
                dx = W + z_rubber_band(dx - W, W);
            }
            z_nav_back_drag(&app->ui.nav, 1.0f - (W > 0.0f ? dx / W : 0.0f));
        } else if (app->drag_scroll) {
            // Content follows the finger: dragging down (y increases) scrolls up.
            z_scroll_drag_by(app->drag_scroll, -(float)(app->ptr_y - app->last_y));
        }
        if (!app->nav_back) {
            dispatch_pan(app, Z_PAN_CHANGED);
        }
        app->last_x = app->ptr_x;
        app->last_y = app->ptr_y;
        app->last_motion_s = now;
    }
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t state) {
    (void)p; (void)time;
    ZApp *app = data;
    // Remember the serial: wl_data_device.set_selection (Copy) needs a recent
    // input-event serial, and a Copy is driven by a tap (this button).
    app->last_serial = serial;
    if (button != BTN_LEFT) {
        return;
    }
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        // Arm the recognizer; defer the tap-vs-pan decision to release/motion.
        app->ptr_down = true;
        app->panning = false;
        app->nav_back = false;
        app->drag_scroll = NULL;
        app->pan_target = NULL;
        app->pan_handler = NULL;
        app->pan_handler_data = NULL;
        app->pan_data = NULL;
        app->drag_vel_y = 0.0;
        app->drag_vel_x = 0.0;
        app->press_x = app->last_x = app->ptr_x;
        app->press_y = app->last_y = app->ptr_y;
        app->last_motion_s = z_now_seconds();
        // Arm the long-press only if the press landed on an OnLongPress node, so
        // the app loop's finite poll timeout (below) is used only when needed.
        app->press_s = app->last_motion_s;
        app->long_pressed = false;
        app->long_press_target = find_long_press(app->root, app->ptr_x,
                                                 app->ptr_y);
        app->long_press_armed = app->long_press_target != NULL;
        // Press feedback: highlight the tappable control under the finger (P31).
        press_begin(app);
        return;
    }
    // Release.
    if (!app->ptr_down) {
        return;
    }
    app->ptr_down = false;
    app->long_press_armed = false;
    if (app->long_pressed) {
        // The long-press already handled this gesture; swallow the tap/pan-end.
        app->long_pressed = false;
        app->panning = false;
        press_release(app);
        return;
    }
    if (!app->panning) {
        // No drag: it was a tap. Flash the press feedback, then run the handler.
        ZView hit = hit_test(app->root, app->ptr_x, app->ptr_y);
        press_flash_release(app);
        dispatch_tap(app, hit);
        return;
    }
    // Drag end. An interruptible back-swipe either flings the pop through or snaps
    // back (finger velocity injected); otherwise fling a scroll / notify a pan.
    if (app->nav_back) {
        float W = (float)app->width;
        double total_dx = app->ptr_x - app->press_x;
        bool pop = total_dx > (double)W * 0.30 || app->drag_vel_x > 500.0;
        // Convert finger px/s to progress-units/s (rightward drag lowers progress).
        float vprog = W > 0.0f ? -(float)app->drag_vel_x / W : 0.0f;
        z_nav_back_end(&app->ui.nav, pop, vprog);
        app->nav_back = false;
    } else if (app->drag_scroll) {
        z_scroll_end_drag(app->drag_scroll, -(float)app->drag_vel_y);
        dispatch_pan(app, Z_PAN_END);
    } else {
        dispatch_pan(app, Z_PAN_END);
    }
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
    (void)kb; (void)time;
    ZApp *app = data;
    app->last_serial = serial;
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

// --- text-input-v3 (app text field side) ----------------------------------
// The compositor sends enter/leave as this app's surface gains/loses keyboard
// focus; commit_string / delete_surrounding_text carry the on-screen keyboard's
// edits, which we apply straight into the focused ZTextField's buffer.
// --- text-field buffer edits (caret + selection, P22) ----------------------
// The buffer is edited at the caret, replacing any selection. The P21 keyboard
// path (commit_string / delete_surrounding_text) and the P22 clipboard path
// (paste / cut) both funnel through these, so a typed character, a pasted string
// and a cut all respect the selection the same way.
static void field_clamp(ZTextField *f) {
    if (f->caret < 0) f->caret = 0;
    if (f->caret > f->len) f->caret = f->len;
    if (f->anchor < 0) f->anchor = 0;
    if (f->anchor > f->len) f->anchor = f->len;
}
static bool field_has_sel(const ZTextField *f) { return f->anchor != f->caret; }
static void field_sel_range(const ZTextField *f, int *lo, int *hi) {
    *lo = f->anchor < f->caret ? f->anchor : f->caret;
    *hi = f->anchor < f->caret ? f->caret : f->anchor;
}

// Replace the selection (or, with none, the empty range at the caret) with `ins`,
// leaving the caret after the inserted text and the selection collapsed.
static void field_replace(ZApp *app, ZTextField *f, const char *ins) {
    if (!f) {
        return;
    }
    field_clamp(f);
    int lo, hi;
    field_sel_range(f, &lo, &hi);
    int add = ins ? (int)strlen(ins) : 0;
    int tail = f->len - hi;
    // Truncate the insert if it would overflow the fixed buffer.
    if (lo + add + tail >= Z_TEXTFIELD_CAP) {
        add = Z_TEXTFIELD_CAP - 1 - lo - tail;
        if (add < 0) {
            add = 0;
        }
    }
    memmove(f->text + lo + add, f->text + hi, (size_t)tail);
    if (add > 0) {
        memcpy(f->text + lo, ins, (size_t)add);
    }
    f->len = lo + add + tail;
    f->text[f->len] = '\0';
    f->caret = lo + add;
    f->anchor = f->caret;
    if (f->on_change) {
        f->on_change(app, app->state);
    }
}

// Delete `count` bytes before the caret (backspace), or the selection if any.
static void field_delete_before(ZApp *app, ZTextField *f, int count) {
    if (!f) {
        return;
    }
    if (field_has_sel(f)) {
        field_replace(app, f, NULL);
        return;
    }
    field_clamp(f);
    int del = count;
    if (del > f->caret) {
        del = f->caret;
    }
    if (del <= 0) {
        return;
    }
    memmove(f->text + f->caret - del, f->text + f->caret,
            (size_t)(f->len - f->caret));
    f->len -= del;
    f->caret -= del;
    f->anchor = f->caret;
    f->text[f->len] = '\0';
    if (f->on_change) {
        f->on_change(app, app->state);
    }
}

void z_app_focus_field(ZApp *app, ZTextField *f) {
    if (app->active_field == f) {
        return;
    }
    app->active_field = f;
    z_invalidate(app);   // render() re-evaluates text-input enable state
}
bool z_app_field_active(ZApp *app, const ZTextField *f) {
    return app && f && app->active_field == f;
}

static void ti_enter(void *data, struct zwp_text_input_v3 *ti,
                     struct wl_surface *surface) {
    (void)ti; (void)surface;
    ZApp *app = data;
    app->ti_entered = true;
    app->ti_enabled = false;   // fresh focus: render() re-enables if a field is up
    z_invalidate(app);
}
static void ti_leave(void *data, struct zwp_text_input_v3 *ti,
                     struct wl_surface *surface) {
    (void)ti; (void)surface;
    ZApp *app = data;
    app->ti_entered = false;
    app->ti_enabled = false;
}
static void ti_preedit_string(void *data, struct zwp_text_input_v3 *ti,
                              const char *text, int32_t cursor_begin,
                              int32_t cursor_end) {
    (void)data; (void)ti; (void)text; (void)cursor_begin; (void)cursor_end;
    // Composing (preedit) text is not shown inline in the MVP.
}
static void ti_commit_string(void *data, struct zwp_text_input_v3 *ti,
                             const char *text) {
    (void)ti;
    ZApp *app = data;
    ZTextField *f = app->active_field;
    if (!f || !text || !text[0]) {
        return;
    }
    // Insert at the caret, replacing any selection (P22) — not a blind append.
    field_replace(app, f, text);
    z_invalidate(app);
}
static void ti_delete_surrounding_text(void *data, struct zwp_text_input_v3 *ti,
                                       uint32_t before_length,
                                       uint32_t after_length) {
    (void)ti; (void)after_length;
    ZApp *app = data;
    ZTextField *f = app->active_field;
    if (!f) {
        return;
    }
    // Delete before the caret (or the selection); caret-aware (P22).
    field_delete_before(app, f, (int)before_length);
    z_invalidate(app);
}
static void ti_done(void *data, struct zwp_text_input_v3 *ti, uint32_t serial) {
    (void)data; (void)ti; (void)serial;
    // We apply commit_string / delete eagerly above (our single trusted keyboard
    // sends them immediately before done), so done needs no extra work.
}
static const struct zwp_text_input_v3_listener text_input_listener = {
    .enter = ti_enter,
    .leave = ti_leave,
    .preedit_string = ti_preedit_string,
    .commit_string = ti_commit_string,
    .delete_surrounding_text = ti_delete_surrounding_text,
    .done = ti_done,
};

// --- input-method-v2 (on-screen keyboard side) -----------------------------
// zelto-keyboard binds this. activate/deactivate (batched, applied on done) tell
// it to show/hide; it sends commit_string / delete back through z_im_*.
static void im_activate(void *data, struct zwp_input_method_v2 *im) {
    (void)im;
    ((ZApp *)data)->im_pending_active = true;
}
static void im_deactivate(void *data, struct zwp_input_method_v2 *im) {
    (void)im;
    ((ZApp *)data)->im_pending_active = false;
}
static void im_surrounding_text(void *data, struct zwp_input_method_v2 *im,
                                const char *text, uint32_t cursor,
                                uint32_t anchor) {
    (void)data; (void)im; (void)text; (void)cursor; (void)anchor;
}
static void im_text_change_cause(void *data, struct zwp_input_method_v2 *im,
                                 uint32_t cause) {
    (void)data; (void)im; (void)cause;
}
static void im_content_type(void *data, struct zwp_input_method_v2 *im,
                            uint32_t hint, uint32_t purpose) {
    (void)data; (void)im; (void)hint; (void)purpose;
}
static void im_done(void *data, struct zwp_input_method_v2 *im) {
    (void)im;
    ZApp *app = data;
    app->im_serial++;   // commit serial = number of done events received
    if (app->im_pending_active != app->im_active) {
        app->im_active = app->im_pending_active;
        if (app->im_active) {
            if (app->im_show_cb) app->im_show_cb(app, app->im_ud);
        } else {
            if (app->im_hide_cb) app->im_hide_cb(app, app->im_ud);
        }
        z_invalidate(app);
    }
}
static void im_unavailable(void *data, struct zwp_input_method_v2 *im) {
    (void)im;
    // Another input method already owns the seat: we will get nothing. Drop it.
    ((ZApp *)data)->input_method = NULL;
}
static const struct zwp_input_method_v2_listener input_method_listener = {
    .activate = im_activate,
    .deactivate = im_deactivate,
    .surrounding_text = im_surrounding_text,
    .text_change_cause = im_text_change_cause,
    .content_type = im_content_type,
    .done = im_done,
    .unavailable = im_unavailable,
};

// --- clipboard: core wl_data_device SET side (Copy/Cut) --------------------
// A copy takes ownership of the CLIPBOARD selection: we make a wl_data_source that
// offers text/plain and, on the receiving client's request, writes the copied
// text into its pipe. The text is owned by the source (a ZClipSource), so a later
// copy that replaces this source frees it cleanly on `cancelled` — no aliasing
// with the app's own buffers.
typedef struct ZClipSource {
    char *text;
} ZClipSource;

static void ds_target(void *data, struct wl_data_source *src,
                      const char *mime) {
    (void)data; (void)src; (void)mime;
}
static void ds_send(void *data, struct wl_data_source *src, const char *mime,
                    int32_t fd) {
    (void)src; (void)mime;
    ZClipSource *cs = data;
    const char *s = cs && cs->text ? cs->text : "";
    size_t n = strlen(s), off = 0;
    while (off < n) {
        ssize_t w = write(fd, s + off, n - off);
        if (w <= 0) {
            break;
        }
        off += (size_t)w;
    }
    close(fd);
}
static void ds_cancelled(void *data, struct wl_data_source *src) {
    ZClipSource *cs = data;
    wl_data_source_destroy(src);
    if (cs) {
        free(cs->text);
        free(cs);
    }
}
static void ds_dnd_drop_performed(void *data, struct wl_data_source *src) {
    (void)data; (void)src;
}
static void ds_dnd_finished(void *data, struct wl_data_source *src) {
    (void)data; (void)src;
}
static void ds_action(void *data, struct wl_data_source *src, uint32_t action) {
    (void)data; (void)src; (void)action;
}
static const struct wl_data_source_listener data_source_listener = {
    .target = ds_target,
    .send = ds_send,
    .cancelled = ds_cancelled,
    .dnd_drop_performed = ds_dnd_drop_performed,
    .dnd_finished = ds_dnd_finished,
    .action = ds_action,
};

// Minimal core wl_data_device listener: we set the clipboard through this device
// but read through data-control, so all we do here is destroy the offers the
// compositor hands us while focused (they'd otherwise leak). DnD events are unused.
static void dd_data_offer(void *data, struct wl_data_device *dev,
                          struct wl_data_offer *offer) {
    (void)dev;
    ZApp *app = data;
    if (app->core_offer) {
        wl_data_offer_destroy(app->core_offer);
    }
    app->core_offer = offer;   // no listener attached; we never read it
}
static void dd_selection(void *data, struct wl_data_device *dev,
                         struct wl_data_offer *offer) {
    (void)dev;
    ZApp *app = data;
    if (!offer && app->core_offer) {
        wl_data_offer_destroy(app->core_offer);
        app->core_offer = NULL;
    }
}
static void dd_enter(void *data, struct wl_data_device *dev, uint32_t serial,
                     struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
                     struct wl_data_offer *offer) {
    (void)data; (void)dev; (void)serial; (void)surface; (void)x; (void)y;
    (void)offer;
}
static void dd_leave(void *data, struct wl_data_device *dev) {
    (void)data; (void)dev;
}
static void dd_motion(void *data, struct wl_data_device *dev, uint32_t time,
                      wl_fixed_t x, wl_fixed_t y) {
    (void)data; (void)dev; (void)time; (void)x; (void)y;
}
static void dd_drop(void *data, struct wl_data_device *dev) {
    (void)data; (void)dev;
}
static const struct wl_data_device_listener data_device_listener = {
    .data_offer = dd_data_offer,
    .enter = dd_enter,
    .leave = dd_leave,
    .motion = dd_motion,
    .drop = dd_drop,
    .selection = dd_selection,
};

void z_clipboard_set(const char *text) {
    ZApp *app = z_active_app;
    if (!app || !app->data_device || !app->data_device_mgr || !text) {
        return;
    }
    ZClipSource *cs = calloc(1, sizeof(*cs));
    if (!cs) {
        return;
    }
    cs->text = strdup(text);
    struct wl_data_source *src =
        wl_data_device_manager_create_data_source(app->data_device_mgr);
    wl_data_source_add_listener(src, &data_source_listener, cs);
    // Offer the common text flavours so any paster finds a match.
    wl_data_source_offer(src, "text/plain");
    wl_data_source_offer(src, "text/plain;charset=utf-8");
    wl_data_source_offer(src, "UTF8_STRING");
    wl_data_source_offer(src, "TEXT");
    wl_data_source_offer(src, "STRING");
    wl_data_device_set_selection(app->data_device, src, app->last_serial);
    wl_display_flush(app->display);
}

// --- clipboard: data-control READ side (Paste) -----------------------------
// Reads go through wlr-data-control so a client that never holds keyboard focus
// (the on-screen keyboard) can still read the selection. The device fires
// data_offer (introducing an offer) + offer (its mimes) + selection (promoting it
// to the current selection); we track the current offer and whether it has text.
static bool clip_is_text_mime(const char *m) {
    return m && (strcmp(m, "text/plain") == 0 ||
                 strcmp(m, "text/plain;charset=utf-8") == 0 ||
                 strcmp(m, "UTF8_STRING") == 0 || strcmp(m, "TEXT") == 0 ||
                 strcmp(m, "STRING") == 0 || strncmp(m, "text/", 5) == 0);
}
static void dco_offer(void *data, struct zwlr_data_control_offer_v1 *offer,
                      const char *mime) {
    ZApp *app = data;
    if (offer == app->dc_pending_offer && clip_is_text_mime(mime)) {
        app->dc_pending_text = true;
    }
}
static const struct zwlr_data_control_offer_v1_listener dc_offer_listener = {
    .offer = dco_offer,
};

static void dc_data_offer(void *data,
                          struct zwlr_data_control_device_v1 *dev,
                          struct zwlr_data_control_offer_v1 *offer) {
    (void)dev;
    ZApp *app = data;
    // A new offer is being introduced; drop any prior introduced-but-unselected
    // one (e.g. a primary-selection offer we ignore), then start tracking mimes.
    if (app->dc_pending_offer && app->dc_pending_offer != app->dc_offer) {
        zwlr_data_control_offer_v1_destroy(app->dc_pending_offer);
    }
    app->dc_pending_offer = offer;
    app->dc_pending_text = false;
    zwlr_data_control_offer_v1_add_listener(offer, &dc_offer_listener, app);
}
static void dc_selection(void *data,
                         struct zwlr_data_control_device_v1 *dev,
                         struct zwlr_data_control_offer_v1 *offer) {
    (void)dev;
    ZApp *app = data;
    // Promote the introduced offer to the current selection. Destroy the previous
    // current offer (unless it's the same object).
    if (app->dc_offer && app->dc_offer != offer) {
        zwlr_data_control_offer_v1_destroy(app->dc_offer);
    }
    app->dc_offer = offer;
    app->dc_offer_text = (offer && offer == app->dc_pending_offer)
                             ? app->dc_pending_text
                             : false;
    if (offer == app->dc_pending_offer) {
        app->dc_pending_offer = NULL;   // it's now the current offer
    }
}
static void dc_finished(void *data,
                        struct zwlr_data_control_device_v1 *dev) {
    ZApp *app = data;
    zwlr_data_control_device_v1_destroy(dev);
    if (app->dc_device == dev) {
        app->dc_device = NULL;
    }
}
static void dc_primary_selection(void *data,
                                 struct zwlr_data_control_device_v1 *dev,
                                 struct zwlr_data_control_offer_v1 *offer) {
    (void)data; (void)dev; (void)offer;
    // We don't use the primary selection; its introduced offer is cleaned up by
    // the next data_offer (dc_data_offer drops an unselected pending offer).
}
static const struct zwlr_data_control_device_v1_listener dc_device_listener = {
    .data_offer = dc_data_offer,
    .selection = dc_selection,
    .finished = dc_finished,
    .primary_selection = dc_primary_selection,
};

void z_clipboard_get(ZClipboardCb cb, void *ud) {
    ZApp *app = z_active_app;
    if (!cb) {
        return;
    }
    // Busy (a get already in flight), no text on the clipboard, or no device:
    // report empty rather than blocking.
    if (!app || app->clip_fd >= 0 || !app->dc_offer || !app->dc_offer_text) {
        cb(app, NULL, ud);
        return;
    }
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0) {
        cb(app, NULL, ud);
        return;
    }
    zwlr_data_control_offer_v1_receive(app->dc_offer, "text/plain", fds[1]);
    close(fds[1]);
    wl_display_flush(app->display);
    app->clip_fd = fds[0];
    app->clip_cb = cb;
    app->clip_ud = ud;
    app->clip_len = 0;
}

// The paste pipe is readable: accumulate until EOF (or the buffer fills), then
// fire the callback with the collected text.
static void clip_handle_read(ZApp *app) {
    if (app->clip_len < sizeof(app->clip_buf) - 1) {
        ssize_t r = read(app->clip_fd, app->clip_buf + app->clip_len,
                         sizeof(app->clip_buf) - 1 - app->clip_len);
        if (r > 0) {
            app->clip_len += (size_t)r;
            if (app->clip_len < sizeof(app->clip_buf) - 1) {
                return;   // more may come
            }
        }
        // r <= 0 (EOF/error) or the buffer is now full: finish below.
    }
    app->clip_buf[app->clip_len] = '\0';
    close(app->clip_fd);
    app->clip_fd = -1;
    ZClipboardCb cb = app->clip_cb;
    void *ud = app->clip_ud;
    app->clip_cb = NULL;
    app->clip_ud = NULL;
    if (cb) {
        cb(app, app->clip_buf, ud);
    }
}

// --- text-field selection geometry + gestures (P22) ------------------------
// Map a surface-local x to a byte offset in the field, and drive word-select /
// drag-extend. The mapping measures text prefixes against the field's text origin
// (its laid-out frame + padding), read from the retained tree.
static ZView find_field_node(ZView n, const ZTextField *f) {
    if (!n) {
        return NULL;
    }
    if (n->field == f) {
        return n;
    }
    for (int i = 0; i < n->n_children; i++) {
        ZView r = find_field_node(n->children[i], f);
        if (r) {
            return r;
        }
    }
    return NULL;
}

static int field_offset_at_x(ZApp *app, ZTextField *f, float x) {
    if (!f || f->len <= 0) {
        return 0;
    }
    ZView node = find_field_node(app->root, f);
    float ox = node ? node->x + node->padding : 0.0f;
    float size = (float)Z_FONT_BODY;
    char buf[Z_TEXTFIELD_CAP];
    int best = 0;
    float bestd = 1e30f;
    for (int i = 0; i <= f->len; i++) {
        memcpy(buf, f->text, (size_t)i);
        buf[i] = '\0';
        float asc, desc;
        float w = app->text ? z_text_measure(app->text, buf, size,
                                              Z_WEIGHT_REGULAR, &asc, &desc)
                            : 0.0f;
        float d = ox + w - x;
        if (d < 0.0f) {
            d = -d;
        }
        if (d < bestd) {
            bestd = d;
            best = i;
        }
    }
    return best;
}

void z_field_tap(ZApp *app, ZTextField *f) {
    z_app_focus_field(app, f);
    int off = field_offset_at_x(app, f, (float)app->ptr_x);
    f->caret = off;
    f->anchor = off;
    z_invalidate(app);
}

void z_field_select_word(ZApp *app, ZTextField *f, float x) {
    z_app_focus_field(app, f);
    if (f->len <= 0) {
        f->caret = f->anchor = 0;
        z_invalidate(app);
        return;
    }
    int off = field_offset_at_x(app, f, x);
    if (off >= f->len) {
        off = f->len - 1;
    }
    int s = off, e = off;
    while (s > 0 && !isspace((unsigned char)f->text[s - 1])) {
        s--;
    }
    while (e < f->len && !isspace((unsigned char)f->text[e])) {
        e++;
    }
    if (s == e) {
        // Landed on whitespace: select that one character so there's something.
        s = off;
        e = off < f->len ? off + 1 : off;
    }
    f->anchor = s;
    f->caret = e;
    z_invalidate(app);
}

void z_field_drag_extend(ZApp *app, ZTextField *f, float x, bool begin) {
    z_app_focus_field(app, f);
    int off = field_offset_at_x(app, f, x);
    if (begin) {
        if (field_has_sel(f)) {
            // Grab the near handle: keep the far selection end as the anchor.
            int lo, hi;
            field_sel_range(f, &lo, &hi);
            f->anchor = (abs(off - lo) < abs(off - hi)) ? hi : lo;
            f->caret = off;
        } else {
            f->anchor = off;
            f->caret = off;
        }
    } else {
        f->caret = off;
    }
    z_invalidate(app);
}

// --- clipboard: field actions (Copy/Cut/Paste/Select-all) ------------------
// These act on the app's focused field. Copy/Cut require a selection; Paste
// inserts the clipboard at the caret (replacing any selection) asynchronously.
static void field_copy_text(ZTextField *f, char *out, size_t cap) {
    int lo, hi;
    field_sel_range(f, &lo, &hi);
    int n = hi - lo;
    if (n >= (int)cap) {
        n = (int)cap - 1;
    }
    if (n < 0) {
        n = 0;
    }
    memcpy(out, f->text + lo, (size_t)n);
    out[n] = '\0';
}

void z_field_copy(ZApp *app) {
    ZTextField *f = app ? app->active_field : NULL;
    if (!f || !field_has_sel(f)) {
        return;
    }
    char buf[Z_TEXTFIELD_CAP];
    field_copy_text(f, buf, sizeof(buf));
    z_clipboard_set(buf);
}

void z_field_cut(ZApp *app) {
    ZTextField *f = app ? app->active_field : NULL;
    if (!f || !field_has_sel(f)) {
        return;
    }
    char buf[Z_TEXTFIELD_CAP];
    field_copy_text(f, buf, sizeof(buf));
    z_clipboard_set(buf);
    field_replace(app, f, NULL);   // delete the selection
    z_invalidate(app);
}

static void field_paste_cb(ZApp *app, const char *text, void *ud) {
    (void)ud;
    ZTextField *f = app->active_field;
    if (!f) {
        return;
    }
    // DIAGNOSTIC: mark an empty read so a captured frame distinguishes "paste
    // fired but clipboard empty" from "the Paste tap missed the button".
    field_replace(app, f, (text && text[0]) ? text : "<EMPTY>");
    z_invalidate(app);
}

void z_field_paste(ZApp *app) {
    (void)app;
    z_clipboard_get(field_paste_cb, NULL);
}

void z_field_select_all(ZApp *app) {
    ZTextField *f = app ? app->active_field : NULL;
    if (!f) {
        return;
    }
    f->anchor = 0;
    f->caret = f->len;
    z_invalidate(app);
}

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
    // Drop the snapshot subscription with the window it watched. The cached
    // BITMAP is deliberately left alone: the record is about to be recycled for
    // some future window, but a card may still be painting this picture during
    // the switcher's close animation, and the cache's own LRU will reclaim it.
    if (rec->cap) {
        zelto_toplevel_capture_v1_destroy(rec->cap);
        rec->cap = NULL;
    }
    rec->has_snap = false;
    rec->snap_key[0] = '\0';
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

// --- window snapshots (zelto-toplevel-capture-v1) --------------------------
// The compositor photographs a window as it leaves the foreground and hands the
// image over as a sealed memfd. We map it, copy it into the image cache under
// this record's stable key, and unmap immediately.
//
// The copy is deliberate. Keeping the mapping alive and pointing the cache
// straight at it would save a memcpy, but it would tie a cache entry's lifetime
// to a file descriptor and make every later refresh a question of who still has
// the old pixels mapped. One ~500KB memcpy per app switch is not worth that; the
// expensive thing the memfd route avoids is a PNG encode per switch, and it
// still avoids it.
//
// The compositor sends PREMULTIPLIED ARGB8888 (it is copying an libzelto
// surface, and libzelto's renderer writes premultiplied), which is exactly what
// ZImage wants — so there is no format conversion anywhere on this path.
static void cap_handle_snapshot(void *data,
                                struct zelto_toplevel_capture_v1 *cap,
                                int32_t fd, int32_t width, int32_t height,
                                int32_t stride, uint32_t format) {
    (void)cap;
    (void)format;
    ZTaskRec *rec = data;
    if (width <= 0 || height <= 0 || stride < width * 4) {
        close(fd);
        return;
    }
    size_t size = (size_t)stride * (size_t)height;
    void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        return;
    }
    uint32_t *px = malloc((size_t)width * (size_t)height * 4);
    if (px) {
        for (int y = 0; y < height; y++) {
            memcpy(px + (size_t)y * (size_t)width,
                   (const char *)map + (size_t)y * (size_t)stride,
                   (size_t)width * 4);
        }
        if (z_image_adopt(rec->snap_key, width, height, px)) {
            rec->has_snap = true;
            z_invalidate(rec_app(rec));   // repaint the card with its picture
        }
    }
    munmap(map, size);
}

static void cap_handle_gone(void *data, struct zelto_toplevel_capture_v1 *cap) {
    (void)data;
    // Nothing to undo: any picture already delivered stays valid and on screen.
    // A switcher showing the card of an app that has just exited is precisely
    // what the snapshot is for.
    zelto_toplevel_capture_v1_destroy(cap);
    ((ZTaskRec *)data)->cap = NULL;
}

static const struct zelto_toplevel_capture_v1_listener cap_listener = {
    .snapshot = cap_handle_snapshot,
    .gone = cap_handle_gone,
};

const char *z_snapshot(ZApp *app, const ZTask *task) {
    if (!app || !task || !task->handle) {
        return NULL;
    }
    ZTaskRec *rec = NULL;
    for (int i = 0; i < Z_MAX_TASKS; i++) {
        if (app->ftl_recs[i].used && app->ftl_recs[i].handle == task->handle) {
            rec = &app->ftl_recs[i];
            break;
        }
    }
    if (!rec) {
        return NULL;
    }
    // Subscribe on first ask. The picture arrives asynchronously, so this call
    // returns NULL now and the caller repaints when it lands (z_invalidate
    // above) — which is why the fallback has to be a real fallback, not an
    // error path: every card renders its icon at least once.
    if (!rec->cap && app->cap_manager) {
        snprintf(rec->snap_key, sizeof(rec->snap_key), "\x01snap:%p",
                 (void *)rec->handle);
        rec->cap = zelto_toplevel_capture_manager_v1_get_capture(
            app->cap_manager, rec->handle);
        if (rec->cap) {
            zelto_toplevel_capture_v1_add_listener(rec->cap, &cap_listener,
                                                   rec);
        }
    }
    return rec->has_snap ? rec->snap_key : NULL;
}

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
    } else if (strcmp(interface,
                      zelto_toplevel_capture_manager_v1_interface.name) == 0) {
        // Window snapshots for the App Switcher's cards. Optional: an older
        // compositor never advertises it and z_snapshot then always returns
        // NULL, so the caller keeps painting the app-icon poster.
        app->cap_manager = wl_registry_bind(
            registry, name, &zelto_toplevel_capture_manager_v1_interface, 1);
    } else if (strcmp(interface, ext_idle_notifier_v1_interface.name) == 0) {
        // Idle notifications (the lock screen drives its dim/lock/off machine
        // off these). Every app binds it harmlessly; zelto-lock is the consumer.
        app->idle_notifier = wl_registry_bind(
            registry, name, &ext_idle_notifier_v1_interface,
            version < 1 ? version : 1);
    } else if (strcmp(interface, zwp_text_input_manager_v3_interface.name) == 0) {
        // Text-input (P21): every app with a text field binds it; a text_input
        // object is created per app in app_run. Harmless for apps with no field.
        app->ti_manager = wl_registry_bind(
            registry, name, &zwp_text_input_manager_v3_interface, 1);
    } else if (strcmp(interface,
                      zwp_input_method_manager_v2_interface.name) == 0) {
        // Input-method (P21): only zelto-keyboard uses it (via z_im_bind); every
        // app binds the manager harmlessly, the keyboard alone creates the object.
        app->im_manager = wl_registry_bind(
            registry, name, &zwp_input_method_manager_v2_interface, 1);
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        // Clipboard (P22): every app binds it to Copy (set the CLIPBOARD selection).
        app->data_device_mgr = wl_registry_bind(
            registry, name, &wl_data_device_manager_interface,
            version < 3 ? version : 3);
    } else if (strcmp(interface,
                      zwlr_data_control_manager_v1_interface.name) == 0) {
        // Clipboard READ (P22): data-control delivers the selection regardless of
        // focus, so the focus-less keyboard (and any app) can Paste. Bound when the
        // compositor advertises it.
        app->dc_manager = wl_registry_bind(
            registry, name, &zwlr_data_control_manager_v1_interface,
            version < 2 ? version : 2);
    } else if (strcmp(interface,
                      zelto_backdrop_manager_v1_interface.name) == 0) {
        // The blurred system material (P37). Only zcomp advertises it.
        app->backdrop_manager = wl_registry_bind(
            registry, name, &zelto_backdrop_manager_v1_interface, 1);
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
        // Inset margins from the anchored edges (e.g. float the shade below the
        // status bar). The set_margin request exists since layer-shell v1.
        zwlr_layer_surface_v1_set_margin(
            app->layer_surface, app->layer_opts.margin_top,
            app->layer_opts.margin_right, app->layer_opts.margin_bottom,
            app->layer_opts.margin_left);
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
    app->clip_fd = -1;
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

    // text-input-v3 (P21): create one text_input for the seat so a TextField can
    // raise the on-screen keyboard when focused. Independent of the surface role
    // (works for xdg apps and layer apps). Harmless if the app has no text field.
    if (app->ti_manager && app->seat) {
        app->text_input = zwp_text_input_manager_v3_get_text_input(
            app->ti_manager, app->seat);
        zwp_text_input_v3_add_listener(app->text_input, &text_input_listener,
                                       app);
    }

    // Clipboard (P22): the core wl_data_device is the Copy (set-selection) side;
    // the data-control device is the focus-independent Paste (read) side. Both are
    // per-seat and role-independent (xdg apps + layer apps, incl. the keyboard).
    if (app->data_device_mgr && app->seat) {
        app->data_device = wl_data_device_manager_get_data_device(
            app->data_device_mgr, app->seat);
        wl_data_device_add_listener(app->data_device, &data_device_listener,
                                    app);
    }
    if (app->dc_manager && app->seat) {
        app->dc_device = zwlr_data_control_manager_v1_get_data_device(
            app->dc_manager, app->seat);
        zwlr_data_control_device_v1_add_listener(app->dc_device,
                                                 &dc_device_listener, app);
    }

    // Open the persistent intents control connection and register this app_id as
    // a mailbox, so zsysd can push delivered deep links / shares to us (and we
    // can send resolve requests on it). Best-effort: no broker -> no intents.
    ctrl_connect_register(app);

    // Press feedback (P31): give the global press spring its back-pointer, and
    // read Reduce Motion once (a live re-read is a later refinement) — under it
    // every spring collapses to an instant jump.
    app->ui.press.app = app;
    app->ui.reduce_motion = z_setting_get_int("sys.reduce_motion", 0) != 0;

    // Deterministic press freeze-frame for the screenshot harness: pin the press
    // spring at ZELTO_PRESS_AMT (default 1) over (ZELTO_PRESS_X, ZELTO_PRESS_Y) in
    // surface px, so stamp_press highlights the tappable node there on a still
    // frame with no injected pointer input (mirrors ZELTO_HOME_ANIM_FRAMES). The
    // env is process-global, so every System-UI surface booted at once would pin
    // the same point; ZELTO_PRESS_APP (matched against this app's app_id, else its
    // title) scopes it to ONE surface, so a press over the bottom nav bar doesn't
    // also stamp the launcher behind it. Unset = every surface (back-compat).
    const char *pxs = getenv("ZELTO_PRESS_X");
    const char *pa = getenv("ZELTO_PRESS_APP");
    bool press_match =
        !(pa && pa[0]) || (app->app_id && strcmp(pa, app->app_id) == 0) ||
        (app->title && strcmp(pa, app->title) == 0);
    if (pxs && pxs[0] && press_match) {
        const char *pys = getenv("ZELTO_PRESS_Y");
        const char *pas = getenv("ZELTO_PRESS_AMT");
        app->press_x = app->last_x = atof(pxs);
        app->press_y = app->last_y = pys && pys[0] ? atof(pys) : 0.0;
        float amt = pas && pas[0] ? (float)atof(pas) : 1.0f;
        app->ui.press.value = app->ui.press.target = amt;
        app->ui.press.animating = false;
    }

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

        struct pollfd pfds[5 + Z_NET_POLL_MAX];
        pfds[0].fd = wl_display_get_fd(dpy);
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        nfds_t nf = 1;
        int perm_slot = -1, ctrl_slot = -1, clip_slot = -1, sensor_slot = -1,
            net_slot = -1, net_n = 0;
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
        // Async clipboard paste read (P22): the data-control offer's pipe.
        if (app->clip_fd >= 0) {
            clip_slot = (int)nf;
            pfds[nf].fd = app->clip_fd;
            pfds[nf].events = POLLIN;
            pfds[nf].revents = 0;
            nf++;
        }
        // Sensor/GPS stream socket (sensors.c): one fd carries every subscription.
        int sfd = z_sensor_poll_fd();
        if (sfd >= 0) {
            sensor_slot = (int)nf;
            pfds[nf].fd = sfd;
            pfds[nf].events = POLLIN;
            pfds[nf].revents = 0;
            nf++;
        }
        // Async HTTP/WebSocket sockets (net.c) contribute their in-flight fds.
        net_slot = (int)nf;
        net_n = z_net_collect_fds(&pfds[nf], Z_NET_POLL_MAX);
        nf += (nfds_t)net_n;

        // While a long-press is armed (a finger held still, no fd traffic),
        // bound the wait so we wake at the threshold to fire it; otherwise block.
        int timeout = -1;
        if (app->long_press_armed && app->ptr_down && !app->panning) {
            double remain = Z_LONG_PRESS_S - (z_now_seconds() - app->press_s);
            timeout = remain <= 0.0 ? 0 : (int)(remain * 1000.0) + 1;
        }
        // A pending one-shot timer (z_after) bounds the wait too; take whichever
        // deadline comes first (a -1 "block forever" always loses to a finite ms).
        if (app->after_armed) {
            double remain = app->after_deadline - z_now_seconds();
            int at = remain <= 0.0 ? 0 : (int)(remain * 1000.0) + 1;
            if (timeout < 0 || at < timeout) {
                timeout = at;
            }
        }
        // The repeating widget tick bounds the wait the same way (a clock beat).
        if (app->tick_armed) {
            double remain = app->tick_deadline - z_now_seconds();
            int tt = remain <= 0.0 ? 0 : (int)(remain * 1000.0) + 1;
            if (timeout < 0 || tt < timeout) {
                timeout = tt;
            }
        }

        if (poll(pfds, nf, timeout) < 0) {
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
        // Clipboard paste pipe: read the offered bytes (fires clip_cb at EOF).
        if (clip_slot >= 0 &&
            (pfds[clip_slot].revents & (POLLIN | POLLHUP | POLLERR))) {
            clip_handle_read(app);
        }
        // Sensor/GPS stream socket: read + dispatch any pushed samples.
        if (sensor_slot >= 0 &&
            (pfds[sensor_slot].revents & (POLLIN | POLLHUP | POLLERR))) {
            z_sensor_handle_ready();
        }
        // Drive any ready HTTP/WebSocket sockets (callbacks fire from here).
        if (net_n > 0) {
            z_net_handle_ready(&pfds[net_slot], net_n);
        }

        // Long-press: a still finger held past the threshold fires now (the poll
        // timeout above woke us even with no fd traffic). The handler typically
        // mutates state + z_invalidate, so the render below paints the result.
        if (app->long_press_armed && app->ptr_down && !app->panning &&
            z_now_seconds() - app->press_s >= Z_LONG_PRESS_S) {
            fire_long_press(app);
        }

        // One-shot timer: fire once the deadline passes (the poll timeout above
        // woke us). Disarm BEFORE calling so the handler may re-arm a new timer.
        if (app->after_armed && z_now_seconds() >= app->after_deadline) {
            ZTimerCb cb = app->after_cb;
            void *ud = app->after_ud;
            app->after_armed = false;
            app->after_cb = NULL;
            app->after_ud = NULL;
            if (cb) {
                cb(app, ud);
            }
        }

        // Repeating widget tick: re-arm the next beat off the deadline (no drift
        // catch-up) and invalidate so the clock/glance re-renders. Only fires once
        // per interval, so a settled home screen repaints on the beat, not per vsync.
        if (app->tick_armed && z_now_seconds() >= app->tick_deadline) {
            app->tick_deadline = z_now_seconds() + app->tick_interval;
            z_invalidate(app);
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
    if (app->clip_fd >= 0) {
        close(app->clip_fd);
        app->clip_fd = -1;
    }
    z_active_app = NULL;

    if (app->text_input) {
        zwp_text_input_v3_destroy(app->text_input);
    }
    if (app->input_method) {
        zwp_input_method_v2_destroy(app->input_method);
    }

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

// Force a full repaint of the next frame. render() resets ui.transitioning
// before calling body() and treats it (like a screen slide) as a full repaint,
// so a body() that sets it via this call repaints the whole surface this frame.
void z_full_repaint(ZApp *app) { app->ui.transitioning = true; }

void z_layer_resize(ZApp *app, int width, int height) {
    if (!app || !app->is_layer || !app->layer_surface) {
        return;
    }
    // No-op if nothing changed, so a body() that calls this every rebuild does
    // not spam set_size/commit (each commit asks for a fresh configure).
    if (app->layer_opts.width == width && app->layer_opts.height == height) {
        return;
    }
    app->layer_opts.width = width;
    app->layer_opts.height = height;
    zwlr_layer_surface_v1_set_size(app->layer_surface,
                                   (uint32_t)(width > 0 ? width : 0),
                                   (uint32_t)(height > 0 ? height : 0));
    // Commit so the request takes effect; the compositor replies with a
    // configure carrying the resolved size, which repaints at the new height.
    wl_surface_commit(app->surface);
}

void z_layer_set_exclusive_zone(ZApp *app, int zone) {
    if (!app || !app->is_layer || !app->layer_surface) {
        return;
    }
    if (app->layer_opts.exclusive_zone == zone) {
        return;   // unchanged: don't spam set_exclusive_zone/commit
    }
    app->layer_opts.exclusive_zone = zone;
    zwlr_layer_surface_v1_set_exclusive_zone(app->layer_surface, zone);
    // Commit so the compositor re-arranges the usable area (zcomp_arrange).
    wl_surface_commit(app->surface);
}

void z_layer_set_input_region(ZApp *app, int x, int y, int w, int h) {
    if (!app || !app->is_layer || !app->surface || !app->compositor) {
        return;
    }
    bool whole = (w <= 0 || h <= 0);
    // Dedup: a body() that calls this every rebuild should not re-commit a region
    // that has not changed. (A previous empty-region request is never equal.)
    if (app->ir_valid && !app->ir_none && app->ir_whole == whole && (whole ||
        (app->ir_x == x && app->ir_y == y && app->ir_w == w && app->ir_h == h))) {
        return;
    }
    app->ir_valid = true;
    app->ir_none = false;
    app->ir_whole = whole;
    app->ir_x = x;
    app->ir_y = y;
    app->ir_w = w;
    app->ir_h = h;
    if (whole) {
        // NULL region = the whole surface accepts input (the default).
        wl_surface_set_input_region(app->surface, NULL);
    } else {
        struct wl_region *region = wl_compositor_create_region(app->compositor);
        wl_region_add(region, x, y, w, h);
        wl_surface_set_input_region(app->surface, region);
        wl_region_destroy(region);
    }
    wl_surface_commit(app->surface);
}

void z_layer_set_input_none(ZApp *app) {
    if (!app || !app->is_layer || !app->surface || !app->compositor) {
        return;
    }
    if (app->ir_valid && app->ir_none) {
        return;   // already empty
    }
    app->ir_valid = true;
    app->ir_none = true;
    app->ir_whole = false;
    // An EMPTY wl_region (no rectangles) = the surface catches no pointer input,
    // so every event falls through to whatever is beneath. This is the OPPOSITE
    // of a NULL region (whole-surface input); it is how a visually full-screen
    // overlay — the brightness-dim scrim — paints over the app yet steals none of
    // its taps. Distinct from z_layer_set_input_region(.., 0,0,0,0), which the
    // SDK treats as "whole surface".
    struct wl_region *region = wl_compositor_create_region(app->compositor);
    wl_surface_set_input_region(app->surface, region);
    wl_region_destroy(region);
    wl_surface_commit(app->surface);
}

void z_backdrop(ZApp *app, float x, float y, float w, float h, float radius) {
    if (!app || !app->surface || !app->backdrop_manager) {
        return;   // no compositor support: the material is its tint alone
    }
    int nx = (int)(x + 0.5f), ny = (int)(y + 0.5f);
    int nw = (int)(w + 0.5f), nh = (int)(h + 0.5f);
    int nr = (int)(radius + 0.5f);
    if (nw < 0) { nw = 0; }
    if (nh < 0) { nh = 0; }
    // Dedup: a material is re-declared on every rebuild (it is part of the body),
    // and a settled surface rebuilds without moving. Only send on a real change —
    // each set_region wakes the compositor's blur pass.
    if (app->bd_valid && app->bd_x == nx && app->bd_y == ny && app->bd_w == nw &&
        app->bd_h == nh && app->bd_radius == nr) {
        return;
    }
    if (!app->backdrop) {
        app->backdrop = zelto_backdrop_manager_v1_get_backdrop(
            app->backdrop_manager, app->surface);
        if (!app->backdrop) {
            return;
        }
    }
    app->bd_valid = true;
    app->bd_x = nx;
    app->bd_y = ny;
    app->bd_w = nw;
    app->bd_h = nh;
    app->bd_radius = nr;
    zelto_backdrop_v1_set_region(app->backdrop, nx, ny, nw, nh, nr);
    wl_surface_commit(app->surface);
}

void z_layer_set_keyboard(ZApp *app, bool exclusive) {
    if (!app || !app->is_layer || !app->layer_surface) {
        return;
    }
    // Dedup: a body() that asserts its keyboard mode every rebuild should not
    // re-commit an unchanged interactivity (each commit asks a fresh configure).
    if (app->kbd_valid && app->kbd_exclusive == exclusive) {
        return;
    }
    app->kbd_valid = true;
    app->kbd_exclusive = exclusive;
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        app->layer_surface,
        exclusive ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE
                  : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    // Commit so the request takes effect; the compositor grabs/releases the
    // keyboard (layer.c layer_sync_keyboard / layer_release_keyboard on commit).
    wl_surface_commit(app->surface);
}

// --- input method (on-screen keyboard side, P21) ---------------------------
// zelto-keyboard calls z_im_bind to become the seat's input method; the
// compositor then drives show/hide (im_activate/deactivate -> the callbacks) and
// each key tap sends a committed string or a backspace to the focused field.
void z_im_bind(ZApp *app, ZImVisibilityCb on_show, ZImVisibilityCb on_hide,
               void *ud) {
    if (!app) {
        return;
    }
    app->im_show_cb = on_show;
    app->im_hide_cb = on_hide;
    app->im_ud = ud;
    if (!app->input_method && app->im_manager && app->seat) {
        app->input_method = zwp_input_method_manager_v2_get_input_method(
            app->im_manager, app->seat);
        zwp_input_method_v2_add_listener(app->input_method,
                                         &input_method_listener, app);
    }
}

void z_im_commit_text(ZApp *app, const char *utf8) {
    if (!app || !app->input_method || !utf8 || !utf8[0]) {
        return;
    }
    zwp_input_method_v2_commit_string(app->input_method, utf8);
    zwp_input_method_v2_commit(app->input_method, app->im_serial);
}

void z_im_backspace(ZApp *app) {
    if (!app || !app->input_method) {
        return;
    }
    // Delete one byte before the cursor (ASCII in the MVP); commit the batch.
    zwp_input_method_v2_delete_surrounding_text(app->input_method, 1, 0);
    zwp_input_method_v2_commit(app->input_method, app->im_serial);
}

// --- idle notifications (ext-idle-notify-v1) -------------------------------
// One registered idle notification. Heap-allocated (outlives a body() rebuild),
// wrapping the protocol object plus the client callbacks.
struct ZIdle {
    struct ext_idle_notification_v1 *notification;
    ZApp *app;
    ZIdleCb on_idled;
    ZIdleCb on_resumed;
    void *ud;
};

static void idle_handle_idled(void *data,
                              struct ext_idle_notification_v1 *n) {
    (void)n;
    ZIdle *idle = data;
    if (idle->on_idled) {
        idle->on_idled(idle->app, idle->ud);
    }
}
static void idle_handle_resumed(void *data,
                                struct ext_idle_notification_v1 *n) {
    (void)n;
    ZIdle *idle = data;
    if (idle->on_resumed) {
        idle->on_resumed(idle->app, idle->ud);
    }
}
static const struct ext_idle_notification_v1_listener idle_notification_listener = {
    .idled = idle_handle_idled,
    .resumed = idle_handle_resumed,
};

ZIdle *z_idle_notify(ZApp *app, int timeout_ms, ZIdleCb on_idled,
                     ZIdleCb on_resumed, void *ud) {
    if (!app || !app->idle_notifier || !app->seat || timeout_ms < 0) {
        return NULL;
    }
    ZIdle *idle = calloc(1, sizeof(*idle));
    if (!idle) {
        return NULL;
    }
    idle->app = app;
    idle->on_idled = on_idled;
    idle->on_resumed = on_resumed;
    idle->ud = ud;
    idle->notification = ext_idle_notifier_v1_get_idle_notification(
        app->idle_notifier, (uint32_t)timeout_ms, app->seat);
    if (!idle->notification) {
        free(idle);
        return NULL;
    }
    ext_idle_notification_v1_add_listener(idle->notification,
                                          &idle_notification_listener, idle);
    return idle;
}

void z_idle_cancel(ZIdle *idle) {
    if (!idle) {
        return;
    }
    if (idle->notification) {
        ext_idle_notification_v1_destroy(idle->notification);
    }
    free(idle);
}

// One-shot timer: record a deadline the app loop fires from (see the struct).
void z_after(ZApp *app, int ms, ZTimerCb cb, void *ud) {
    if (!app) {
        return;
    }
    if (ms < 0) {
        ms = 0;
    }
    app->after_armed = true;
    app->after_deadline = z_now_seconds() + (double)ms / 1000.0;
    app->after_cb = cb;
    app->after_ud = ud;
}

void z_after_cancel(ZApp *app) {
    if (!app) {
        return;
    }
    app->after_armed = false;
    app->after_cb = NULL;
    app->after_ud = NULL;
}

// Repeating tick: record the desired cadence for THIS build. The loop arms the
// beat off the shortest request after the build (see build()); calling it every
// build keeps the beat alive, and not calling it lets the beat lapse. Sub-slot of
// the app loop, so it never collides with the one-shot z_after.
void z_tick_every(ZApp *app, int ms) {
    if (!app || ms <= 0) {
        return;
    }
    double want = (double)ms / 1000.0;
    if (app->tick_want <= 0.0 || want < app->tick_want) {
        app->tick_want = want;
    }
}

// --- permission broker (zsysd) client -------------------------------------
// zsysd speaks a newline-delimited JSON-ish protocol over a SOCK_STREAM unix
// socket at $XDG_RUNTIME_DIR/zsysd.sock. The client sends one request line —
// {"op":"perm_status"|"perm_request","app_id":..,"perm":..} — and reads back one
// reply line — {"status":"granted|denied|prompt"}. status is a fast round-trip;
// request may block on a user prompt, so its socket is parked in the app loop.
//
// z_active_app is declared near the top of this file (set for app_run's life).

// How long a client will keep trying to reach the broker after it starts, and
// how often. See zsysd_connect().
#define ZSYSD_STARTUP_GRACE_MS 4000
#define ZSYSD_RETRY_MS 25

static int zsysd_connect_once(const char *runtime) {
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

// Connect to the broker, waiting for it during startup.
//
// zsysd and the System UI clients are launched back to back — /init does
// `zsysd & zelto-bar & ... zelto-lock &` (meta/initramfs/init) and the simulator
// does the same (meta/run-sim.sh) — so a client routinely reaches its first
// z_setting_get before zsysd has finished bind()+listen(). A single-shot connect
// then fails, and EVERY caller here treats that as "no broker" and quietly falls
// back to a compiled-in default. That is not a degraded mode, it is a wrong boot:
// zelto-lock read lock_enabled=0 / dim=8s instead of its configured policy and
// never locked the screen, in roughly one boot in twelve, on the device as well
// as in the sim. It is silent because a default is indistinguishable from a
// setting.
//
// So during a startup grace window, keep retrying. After that window — or once
// this process has ever been connected — go back to a single attempt, because
// then a refused connect means the broker is genuinely gone and a caller must
// not stall the UI thread waiting for it.
static int zsysd_connect(void) {
    static bool ever_connected = false;
    static struct timespec first = {0, 0};

    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime) {
        runtime = "/run";
    }

    int fd = zsysd_connect_once(runtime);
    if (fd >= 0) {
        ever_connected = true;
        return fd;
    }
    if (ever_connected) {
        return -1;   // broker was there and went away: report it, don't wait
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (first.tv_sec == 0 && first.tv_nsec == 0) {
        first = now;
    }
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - first.tv_sec) * 1000 +
                          (now.tv_nsec - first.tv_nsec) / 1000000;
        if (elapsed_ms >= ZSYSD_STARTUP_GRACE_MS) {
            return -1;   // no broker in this system; run standalone
        }
        struct timespec nap = {0, ZSYSD_RETRY_MS * 1000000L};
        nanosleep(&nap, NULL);
        fd = zsysd_connect_once(runtime);
        if (fd >= 0) {
            ever_connected = true;
            return fd;
        }
    }
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

// Parse and act on one pushed control line. zsysd pushes intent deliveries
// ({"op":"deliver",..}) to a mailbox app, and notification show/hide to the
// shade sink ({"op":"notify_show"|"notify_hide",..}).
static void ctrl_dispatch_line(ZApp *app, const char *line) {
    char op[24] = {0};
    if (!ctrl_json_get(line, "op", op, sizeof(op))) {
        return;
    }
    if (strcmp(op, "notify_show") == 0 && app->notify_show_cb) {
        char id[24] = {0}, app_id[96] = {0}, title[128] = {0}, body[192] = {0},
             tap_route[256] = {0}, action_id[64] = {0}, action_title[64] = {0};
        ctrl_json_get(line, "id", id, sizeof(id));
        ctrl_json_get(line, "app_id", app_id, sizeof(app_id));
        ctrl_json_get(line, "title", title, sizeof(title));
        ctrl_json_get(line, "body", body, sizeof(body));
        ctrl_json_get(line, "tap_route", tap_route, sizeof(tap_route));
        ctrl_json_get(line, "action_id", action_id, sizeof(action_id));
        ctrl_json_get(line, "action_title", action_title, sizeof(action_title));
        ZShownNotification n = {
            .id = (int64_t)atoll(id),
            .app_id = app_id,
            .title = title,
            .body = body,
            .tap_route = tap_route,
            .action_id = action_id,
            .action_title = action_title,
        };
        app->notify_show_cb(app, &n, app->notify_sink_ud);
        z_invalidate(app);
        return;
    }
    if (strcmp(op, "notify_hide") == 0 && app->notify_hide_cb) {
        char id[24] = {0};
        ctrl_json_get(line, "id", id, sizeof(id));
        app->notify_hide_cb(app, (int64_t)atoll(id), app->notify_sink_ud);
        z_invalidate(app);
        return;
    }
    if (strcmp(op, "settings_changed") == 0 && app->settings_cb) {
        char key[64] = {0}, value[160] = {0};
        ctrl_json_get(line, "key", key, sizeof(key));
        ctrl_json_get(line, "value", value, sizeof(value));
        app->settings_cb(app, key, value, app->settings_ud);
        z_invalidate(app);
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
    } else if (strcmp(kind, "notify_action") == 0) {
        char id[24] = {0}, action[64] = {0};
        ctrl_json_get(line, "id", id, sizeof(id));
        ctrl_json_get(line, "action", action, sizeof(action));
        deliver_action(app, (int64_t)atoll(id), action);
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

// --- notifications (zsysd) client ------------------------------------------
// An app posts a notification over a transient zsysd connection (like
// z_perm_status): it sends one {"op":"notify_post",..} line and blocks reading
// the {"id":"N"} reply — synchronous because the broker may show the consent
// dialog first, exactly as the perm path does. The poster's action receiver and
// the shade's show/hide sink ride the persistent ctrl_fd instead (pushed by
// zsysd), folded into ctrl_dispatch_line above. All numeric ids travel as quoted
// strings so the one json_get parser handles every field.

// The builder ZNotification holds the fields until z_notify_post sends them.
struct ZNotification {
    char title[128];
    char body[192];
    char channel[64];
    char tap_route[256];
    char action_id[64];
    char action_title[64];
};

ZNotification *z_notify_new(const char *title, const char *body) {
    ZNotification *n = calloc(1, sizeof(*n));
    if (!n) {
        return NULL;
    }
    snprintf(n->title, sizeof(n->title), "%s", title ? title : "");
    snprintf(n->body, sizeof(n->body), "%s", body ? body : "");
    return n;
}

void z_notify_set_channel(ZNotification *n, const char *channel_id) {
    if (n && channel_id) {
        snprintf(n->channel, sizeof(n->channel), "%s", channel_id);
    }
}

void z_notify_set_tap_route(ZNotification *n, const char *url) {
    if (n && url) {
        snprintf(n->tap_route, sizeof(n->tap_route), "%s", url);
    }
}

void z_notify_add_action(ZNotification *n, const char *id, const char *title) {
    if (n && id && title) {
        snprintf(n->action_id, sizeof(n->action_id), "%s", id);
        snprintf(n->action_title, sizeof(n->action_title), "%s", title);
    }
}

// Wait for the one-line reply on `fd` while keeping the app's Wayland connection
// serviced. z_notify_post is synchronous (it returns the assigned id), but the
// broker may sit on the request for seconds while the consent dialog is up; a
// plain blocking read would stop draining the Wayland socket and the compositor
// would drop the client. So we pump wayland (the same prepare_read/poll/
// read_events dance as the main loop) until the reply fd is readable.
static bool notify_wait_reply(ZApp *app, int fd, char *buf, size_t n) {
    struct wl_display *dpy = app->display;
    for (;;) {
        while (wl_display_prepare_read(dpy) != 0) {
            wl_display_dispatch_pending(dpy);
        }
        wl_display_flush(dpy);
        struct pollfd pfds[2];
        pfds[0].fd = wl_display_get_fd(dpy);
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = fd;
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;
        if (poll(pfds, 2, -1) < 0) {
            wl_display_cancel_read(dpy);
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (pfds[0].revents & POLLIN) {
            if (wl_display_read_events(dpy) < 0) {
                return false;
            }
        } else {
            wl_display_cancel_read(dpy);
        }
        wl_display_dispatch_pending(dpy);
        if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            return zsysd_read_line(fd, buf, n);
        }
    }
}

int64_t z_notify_post(ZNotification *n) {
    if (!n) {
        return -1;
    }
    ZApp *app = z_active_app;
    int64_t id = -1;
    int fd = (app) ? zsysd_connect() : -1;
    if (fd >= 0) {
        char msg[1024];
        int m = snprintf(
            msg, sizeof(msg),
            "{\"op\":\"notify_post\",\"app_id\":\"%s\",\"title\":\"%s\","
            "\"body\":\"%s\",\"channel\":\"%s\",\"tap_route\":\"%s\","
            "\"action_id\":\"%s\",\"action_title\":\"%s\"}\n",
            app->app_id ? app->app_id : "", n->title, n->body, n->channel,
            n->tap_route, n->action_id, n->action_title);
        if (m > 0 && m < (int)sizeof(msg) &&
            write(fd, msg, (size_t)m) == m) {
            char line[64];
            // Pump wayland while waiting if we have a display (an app); a
            // display-less caller (CLI/test) just blocks on the read.
            bool ok = app->display
                          ? notify_wait_reply(app, fd, line, sizeof(line))
                          : zsysd_read_line(fd, line, sizeof(line));
            if (ok) {
                char idbuf[24] = {0};
                if (ctrl_json_get(line, "id", idbuf, sizeof(idbuf))) {
                    id = (int64_t)atoll(idbuf);
                }
            }
        }
        close(fd);
    }
    free(n);
    return id;
}

void z_notify_cancel(int64_t id) {
    int fd = zsysd_connect();
    if (fd < 0) {
        return;
    }
    char msg[64];
    int m = snprintf(msg, sizeof(msg),
                     "{\"op\":\"notify_cancel\",\"id\":\"%lld\"}\n",
                     (long long)id);
    if (m > 0 && m < (int)sizeof(msg)) {
        ssize_t w = write(fd, msg, (size_t)m);
        (void)w;
    }
    close(fd);
}

void z_notify_define_channel(const char *id, const char *name,
                             ZImportance imp) {
    int fd = zsysd_connect();
    if (fd < 0) {
        return;
    }
    char msg[256];
    int m = snprintf(msg, sizeof(msg),
                     "{\"op\":\"notify_channel\",\"id\":\"%s\",\"name\":\"%s\","
                     "\"importance\":\"%d\"}\n",
                     id ? id : "", name ? name : "", (int)imp);
    if (m > 0 && m < (int)sizeof(msg)) {
        ssize_t w = write(fd, msg, (size_t)m);
        (void)w;
    }
    close(fd);
}

void z_notify_set_badge(int count) {
    ZApp *app = z_active_app;
    int fd = (app) ? zsysd_connect() : -1;
    if (fd < 0) {
        return;
    }
    char msg[128];
    int m = snprintf(msg, sizeof(msg),
                     "{\"op\":\"notify_badge\",\"app_id\":\"%s\",\"count\":\"%d\"}\n",
                     app->app_id ? app->app_id : "", count);
    if (m > 0 && m < (int)sizeof(msg)) {
        ssize_t w = write(fd, msg, (size_t)m);
        (void)w;
    }
    close(fd);
}

// A notify_action pushed back to the poster: stash it (so it survives until the
// handler is registered) and fire now if a handler already exists.
static void deliver_action(ZApp *app, int64_t id, const char *action_id) {
    app->pend_action_id = id;
    snprintf(app->pend_action_buf, sizeof(app->pend_action_buf), "%s",
             action_id ? action_id : "");
    app->pend_action = true;
    if (app->notify_action_cb) {
        ZNotifyActionEvent e = {.notification_id = id,
                                .action_id = app->pend_action_buf};
        app->notify_action_cb(app, &e, app->notify_action_ud);
        app->pend_action = false;
    }
    z_invalidate(app);
}

void z_on_notification_action(ZApp *app, ZNotifyActionCb cb, void *ud) {
    app->notify_action_cb = cb;
    app->notify_action_ud = ud;
    if (cb && app->pend_action) {
        app->pend_action = false;
        ZNotifyActionEvent e = {.notification_id = app->pend_action_id,
                                .action_id = app->pend_action_buf};
        cb(app, &e, ud);
        z_invalidate(app);
    }
}

// --- notification shade sink (System UI) -----------------------------------
// The shade subscribes as the single notification sink over its persistent
// ctrl_fd; zsysd then pushes notify_show / notify_hide (handled in
// ctrl_dispatch_line). The shade reports body / action taps back on the same fd.
void z_notify_subscribe(ZApp *app, ZNotifyShowCb on_show, ZNotifyHideCb on_hide,
                        void *ud) {
    app->notify_show_cb = on_show;
    app->notify_hide_cb = on_hide;
    app->notify_sink_ud = ud;
    if (app->ctrl_fd >= 0) {
        const char *msg = "{\"op\":\"notify_subscribe\"}\n";
        ssize_t w = write(app->ctrl_fd, msg, strlen(msg));
        (void)w;
    }
}

void z_notify_report_tap(int64_t id) {
    ZApp *app = z_active_app;
    if (!app || app->ctrl_fd < 0) {
        return;
    }
    char msg[64];
    int m = snprintf(msg, sizeof(msg), "{\"op\":\"notify_tap\",\"id\":\"%lld\"}\n",
                     (long long)id);
    if (m > 0 && m < (int)sizeof(msg)) {
        ssize_t w = write(app->ctrl_fd, msg, (size_t)m);
        (void)w;
    }
}

void z_notify_report_action(int64_t id, const char *action_id) {
    ZApp *app = z_active_app;
    if (!app || app->ctrl_fd < 0) {
        return;
    }
    char msg[128];
    int m = snprintf(msg, sizeof(msg),
                     "{\"op\":\"notify_action\",\"id\":\"%lld\",\"action\":\"%s\"}\n",
                     (long long)id, action_id ? action_id : "");
    if (m > 0 && m < (int)sizeof(msg)) {
        ssize_t w = write(app->ctrl_fd, msg, (size_t)m);
        (void)w;
    }
}

// --- system settings (zsysd) client ----------------------------------------
// The settings broker is a fast in-memory store with a write-through to disk.
// get/set are transient connections (like z_perm_status / z_notify_cancel): get
// is a quick synchronous read (the broker never blocks on it, so no wayland pump
// is needed), set is fire-and-forget. Live updates ride the persistent ctrl_fd:
// z_settings_observe subscribes once, and zsysd pushes settings_changed (handled
// in ctrl_dispatch_line). The setter is broadcast to as well; clients apply
// changes idempotently so observing your own set neither loops nor double-applies.
const char *z_setting_get_str(const char *key, const char *fallback) {
    static char val[256];
    if (!key) {
        return fallback;
    }
    int fd = zsysd_connect();
    if (fd < 0) {
        return fallback;
    }
    char msg[128];
    int m = snprintf(msg, sizeof(msg), "{\"op\":\"settings_get\",\"key\":\"%s\"}\n",
                     key);
    const char *result = fallback;
    if (m > 0 && m < (int)sizeof(msg) && write(fd, msg, (size_t)m) == m) {
        char line[256];
        if (zsysd_read_line(fd, line, sizeof(line)) &&
            ctrl_json_get(line, "value", val, sizeof(val)) && val[0]) {
            result = val;
        }
    }
    close(fd);
    return result;
}

int64_t z_setting_get_int(const char *key, int64_t fallback) {
    const char *s = z_setting_get_str(key, NULL);
    if (!s || !s[0]) {
        return fallback;
    }
    return (int64_t)strtoll(s, NULL, 10);
}

void z_setting_set_str(const char *key, const char *value) {
    if (!key || !value) {
        return;
    }
    int fd = zsysd_connect();
    if (fd < 0) {
        return;
    }
    char msg[256];
    int m = snprintf(msg, sizeof(msg),
                     "{\"op\":\"settings_set\",\"key\":\"%s\",\"value\":\"%s\"}\n",
                     key, value);
    if (m > 0 && m < (int)sizeof(msg)) {
        ssize_t w = write(fd, msg, (size_t)m);
        (void)w;
    }
    close(fd);
}

void z_setting_set_int(const char *key, int64_t value) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%lld", (long long)value);
    z_setting_set_str(key, tmp);
}

void z_settings_observe(ZApp *app, ZSettingsCb cb, void *ud) {
    if (!app) {
        return;
    }
    app->settings_cb = cb;
    app->settings_ud = ud;
    if (cb && app->ctrl_fd >= 0) {
        const char *msg = "{\"op\":\"settings_subscribe\"}\n";
        ssize_t w = write(app->ctrl_fd, msg, strlen(msg));
        (void)w;
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
