// zcomp seat/input. Brings up wl_seat, a wlr_cursor for pointer tracking, and
// per-device keyboard wiring (xkbcommon keymaps). Pointer events are routed to
// the surface under the cursor; keyboard events to the focused surface.
#include "zcomp/seat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/input-event-codes.h>

#include <wlr/backend.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "zcomp/server.h"
#include "zcomp/toplevel.h"

struct wlr_surface *zcomp_surface_at(ZcompServer *server, double lx, double ly,
                                     double *sx, double *sy) {
    struct wlr_scene_node *node =
        wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER) {
        return NULL;
    }
    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface =
        wlr_scene_surface_try_from_buffer(scene_buffer);
    if (!scene_surface) {
        return NULL;
    }
    return scene_surface->surface;
}

// Pump user activity into the idle notifier so idle-aware clients (the lock
// screen) reset their dim/lock/off timeouts. Called on every input event; the
// timeout policy itself lives entirely in the client, keeping zcomp policy-free.
static void notify_activity(ZcompServer *server) {
    if (server->idle_notifier) {
        wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
    }
}

// --- pointer -------------------------------------------------------------

// The grabbed surface was destroyed mid-drag: drop the grab.
static void handle_grab_surface_destroy(struct wl_listener *listener,
                                        void *data) {
    (void)data;
    ZcompServer *server =
        wl_container_of(listener, server, grab_surface_destroy);
    wl_list_remove(&server->grab_surface_destroy.link);
    server->grab_surface = NULL;
}

// Begin / end an implicit pointer grab on `surface` (origin = its top-left in
// layout coords). While held, every pointer event is re-routed to it regardless
// of what the cursor is over, so a drag that runs off the pressed surface keeps
// its events. A destroy listener drops the grab if the surface goes away.
static void begin_grab(ZcompServer *server, struct wlr_surface *surface,
                       double ox, double oy) {
    server->grab_surface = surface;
    server->grab_ox = ox;
    server->grab_oy = oy;
    server->grab_surface_destroy.notify = handle_grab_surface_destroy;
    wl_signal_add(&surface->events.destroy, &server->grab_surface_destroy);
}

static void end_grab(ZcompServer *server) {
    if (server->grab_surface) {
        wl_list_remove(&server->grab_surface_destroy.link);
        server->grab_surface = NULL;
    }
}

static void process_cursor_motion(ZcompServer *server, uint32_t time) {
    // Implicit pointer grab: while a button is held, keep delivering events to
    // the surface that received the press — in its own coordinates — rather than
    // refocusing to whatever the cursor is now over. Re-enter it every motion
    // (notify_enter is a no-op when it already holds focus, but re-asserts it if
    // the surface's commit/resize dropped focus), so a drag that runs off the
    // pressed surface — pulling the shade down over an app while it grows to
    // full — keeps every motion AND the closing button-up.
    if (server->grab_surface) {
        double sx = server->cursor->x - server->grab_ox;
        double sy = server->cursor->y - server->grab_oy;
        wlr_seat_pointer_notify_enter(server->seat, server->grab_surface, sx, sy);
        wlr_seat_pointer_notify_motion(server->seat, time, sx, sy);
        return;
    }
    // Always show the default arrow. Zelto is touch-first and its surfaces don't
    // set a pointer cursor, so without this the cursor would be invisible whenever
    // it is over a (usually full-screen) surface — only the bare background showed
    // one. A client that sets its own cursor via request_set_cursor still overrides
    // this. (Mainly matters in the desktop simulator, where you drive it by mouse.)
    wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");

    double sx, sy;
    struct wlr_surface *surface =
        zcomp_surface_at(server, server->cursor->x, server->cursor->y, &sx, &sy);
    if (!surface) {
        // Nothing under the cursor: drop focus + hover (cursor already set above).
        server->hover_surface = NULL;
        wlr_seat_pointer_clear_focus(server->seat);
        return;
    }
    // Remember the hovered surface + its layout origin so a following button
    // press can grab it without re-running the hit test (see handle_cursor_button).
    server->hover_surface = surface;
    server->hover_ox = server->cursor->x - sx;
    server->hover_oy = server->cursor->y - sy;
    wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(server->seat, time, sx, sy);
}

static void handle_cursor_motion(struct wl_listener *listener, void *data) {
    ZcompServer *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;
    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x,
                    event->delta_y);
    notify_activity(server);
    process_cursor_motion(server, event->time_msec);
}

static void handle_cursor_motion_absolute(struct wl_listener *listener,
                                          void *data) {
    ZcompServer *server =
        wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x,
                             event->y);
    notify_activity(server);
    process_cursor_motion(server, event->time_msec);
}

// The button half of the pointer path, independent of where the event came from:
// a real device (handle_cursor_button) or the scripted-gesture test hook, which
// must take exactly this path — the implicit grab is the whole reason a drag
// keeps reaching the surface it began on.
static void process_cursor_button(ZcompServer *server, uint32_t time,
                                  uint32_t button, enum wlr_button_state state) {
    bool pressed = state == WLR_BUTTON_PRESSED;
    notify_activity(server);

    // Begin an implicit grab on the first press so motion keeps reaching the
    // pressed surface once the drag runs off it (see process_cursor_motion). Use
    // the hovered surface recorded by the preceding motion — re-running the hit
    // test here can momentarily miss (a surface mid-resize), which would drop the
    // grab and let a drag escape the instant it left the pressed surface.
    if (pressed && !server->grab_surface && server->hover_surface) {
        begin_grab(server, server->hover_surface, server->hover_ox,
                   server->hover_oy);
    }

    // Re-assert the grab's focus before delivering the button: an idle resize of
    // the grabbed surface between the last motion and this button (e.g. the shade
    // settling) can drop pointer focus, and with no motion to re-enter it the
    // button — notably the drag-closing release — would otherwise miss it.
    if (server->grab_surface) {
        wlr_seat_pointer_notify_enter(server->seat, server->grab_surface,
                                      server->cursor->x - server->grab_ox,
                                      server->cursor->y - server->grab_oy);
    }

    wlr_seat_pointer_notify_button(server->seat, time, button, state);

    // End the grab once every button is up, then refocus to whatever the cursor
    // now rests over (so the next hover/tap targets the right surface).
    if (!pressed && server->seat->pointer_state.button_count == 0) {
        end_grab(server);
        process_cursor_motion(server, time);
    }
}

static void handle_cursor_button(struct wl_listener *listener, void *data) {
    ZcompServer *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;
    process_cursor_button(server, event->time_msec, event->button, event->state);
}

static void handle_cursor_axis(struct wl_listener *listener, void *data) {
    ZcompServer *server = wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *event = data;
    notify_activity(server);
    wlr_seat_pointer_notify_axis(server->seat, event->time_msec,
                                 event->orientation, event->delta,
                                 event->delta_discrete, event->source);
}

static void handle_cursor_frame(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompServer *server = wl_container_of(listener, server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
}

// --- keyboard ------------------------------------------------------------

static void handle_kb_modifiers(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompKeyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
    struct wlr_seat *seat = keyboard->server->seat;
    wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(seat, &keyboard->wlr_keyboard->modifiers);
}

// --- volume keys -> settings broker ---------------------------------------
// The volume rocker is a system setting (sys.volume 0..10 / sys.mute), so the
// compositor actuates the media keys by writing the zsysd settings store — the
// same brokered source the status bar and the zelto-volume HUD read. A short
// synchronous connect to $XDG_RUNTIME_DIR/zsysd.sock: read the current value,
// adjust, write it back. No new protocol; the broker fans the change out. (The
// brightness actuation in P19 lived in the Settings app; the media keys are the
// compositor's because only it sees the raw keysyms.)
static int zsysd_open(void) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime) {
        runtime = "/run";
    }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/zsysd.sock", runtime);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// Read an integer setting through the broker (settings_get); `fallback` on any
// error or an unset key. Parses the {"value":"N"} reply's quoted value.
static int zsysd_get_int(const char *key, int fallback) {
    int fd = zsysd_open();
    if (fd < 0) {
        return fallback;
    }
    char req[128];
    int n = snprintf(req, sizeof(req),
                     "{\"op\":\"settings_get\",\"key\":\"%s\"}\n", key);
    int out = fallback;
    if (n > 0 && write(fd, req, (size_t)n) == n) {
        char buf[128] = {0};
        ssize_t r = read(fd, buf, sizeof(buf) - 1);
        if (r > 0) {
            buf[r] = '\0';
            const char *v = strstr(buf, "\"value\"");
            if (v && (v = strchr(v, ':')) && (v = strchr(v, '"'))) {
                if (v[1] != '"') {   // non-empty value
                    out = atoi(v + 1);
                }
            }
        }
    }
    close(fd);
    return out;
}

static void zsysd_set_int(const char *key, int value) {
    int fd = zsysd_open();
    if (fd < 0) {
        return;
    }
    char req[128];
    int n = snprintf(req, sizeof(req),
                     "{\"op\":\"settings_set\",\"key\":\"%s\",\"value\":\"%d\"}\n",
                     key, value);
    if (n > 0) {
        ssize_t w = write(fd, req, (size_t)n);
        (void)w;
    }
    close(fd);
}

// Nudge the volume by delta (clamped 0..10). Unmutes on volume-up so a press
// after muting is audible again — the phone behaviour.
static void volume_bump(int delta) {
    int v = zsysd_get_int("sys.volume", 5) + delta;
    if (v < 0) {
        v = 0;
    } else if (v > 10) {
        v = 10;
    }
    zsysd_set_int("sys.volume", v);
    if (delta > 0) {
        zsysd_set_int("sys.mute", 0);
    }
}

static void volume_toggle_mute(void) {
    zsysd_set_int("sys.mute", zsysd_get_int("sys.mute", 0) ? 0 : 1);
}

// --- screenshot ------------------------------------------------------------
//
// THE CAPTURE CHORD, and the reason its privacy guard lives HERE.
//
// Taking the picture is zelto-shot's job (system/shot); deciding whether one may
// be taken is the compositor's, and the split is not arbitrary. Only zcomp knows
// whether a modal layer surface is currently holding the screen —
// server->focused_layer is set exactly while a layer surface holds EXCLUSIVE
// keyboard interactivity, which is what zelto-lock does when it locks. A client
// cannot see that, so a check inside zelto-shot would be a client applying a
// rule to itself, which is a claim rather than a control.
//
// This is the SAME guard, on the same signal, as zcomp_capture_take()'s refusal
// to photograph a window that is backgrounded under a lock screen (P42/P43). It
// is the more serious of the two: a window snapshot leaks one app's contents to
// the App Switcher, a screenshot of a locked phone writes the whole screen into
// a library that anybody holding the handset can browse.
//
// WHAT THIS DOES NOT COVER, stated rather than implied. wlr-screencopy remains
// bound-able by any client, so this stops the SYSTEM screenshot path and not
// every possible capture. Closing that hole means gating the protocol itself,
// which would also stop `grim` — and grim is how the entire shot catalogue
// photographs the lock screen. That trade is a privacy-stage decision with a
// harness consequence, so it is made there and not smuggled in here.
//
// The log line NAMES what happened for the same reason capture.c's does: "a
// screenshot did not happen" is not assertable from an anonymous line, and this
// guard is otherwise exactly the kind of code that is reviewed once and never
// executed.
#define ZELTO_SHOT_BIN "/usr/bin/zelto-shot"

static void zcomp_screenshot(ZcompServer *server) {
    if (server && server->focused_layer) {
        wlr_log(WLR_INFO,
                "shot: suppressed (screen held by a modal layer)");
        return;
    }
    // The simulator runs uninstalled binaries out of build-host, so the path is
    // overridable exactly like ZELTO_RECENTS_BIN / ZELTO_CONSENT_BIN.
    const char *bin = getenv("ZELTO_SHOT_BIN");
    if (!bin || !bin[0]) {
        bin = ZELTO_SHOT_BIN;
    }
    wlr_log(WLR_INFO, "shot: capturing (%s)", bin);

    // Double fork so the encoder is reparented to init and this process never
    // owes it a wait(). The compositor's event loop installs no SIGCHLD handler,
    // and a zombie per screenshot is a leak that only shows up after a long
    // uptime — which is to say, never during a test.
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            execlp(bin, bin, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    } else if (pid > 0) {
        waitpid(pid, NULL, 0);   // reaps the intermediate immediately
    }
}

// Compositor-level chords, recognized before app delivery (System UI gestures):
//   Home -> reveal the launcher;  Tab ("Switch") -> cycle foreground app;
//   the media keys drive the volume setting (rocker HUD + bar glyph);
//   Print -> a screenshot into the photo library.
// Returns true if the key was consumed and must not reach the client.
static bool handle_chord(ZcompServer *server, xkb_keysym_t sym) {
    switch (sym) {
    case XKB_KEY_Home:
        zcomp_home(server);
        return true;
    // The capture chord. On a handset this is the power+volume-down combination;
    // there is no such combination to press in the simulator or in QEMU, and a
    // keysym is what both harnesses can actually deliver (wtype in the sim,
    // input-send-event over QMP on the target). Naming it Print rather than
    // inventing a modifier chord also keeps it out of the way of the text input
    // path, which every letter key goes through.
    case XKB_KEY_Print:
        zcomp_screenshot(server);
        return true;
    case XKB_KEY_Tab:
        zcomp_switch(server);
        return true;
    case XKB_KEY_XF86AudioRaiseVolume:
        volume_bump(+1);
        return true;
    case XKB_KEY_XF86AudioLowerVolume:
        volume_bump(-1);
        return true;
    case XKB_KEY_XF86AudioMute:
        volume_toggle_mute();
        return true;
    default:
        return false;
    }
}

static void handle_kb_key(struct wl_listener *listener, void *data) {
    ZcompKeyboard *keyboard = wl_container_of(listener, keyboard, key);
    struct wlr_seat *seat = keyboard->server->seat;
    struct wlr_keyboard_key_event *event = data;
    notify_activity(keyboard->server);

    // Intercept global window-management chords on press; forward everything
    // else to the focused client. evdev keycodes are +8 in xkb.
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED &&
        keyboard->wlr_keyboard->xkb_state) {
        xkb_keysym_t sym = xkb_state_key_get_one_sym(
            keyboard->wlr_keyboard->xkb_state, event->keycode + 8);
        if (handle_chord(keyboard->server, sym)) {
            return;
        }
    }

    wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
    wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode,
                                 event->state);
}

static void handle_kb_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompKeyboard *keyboard = wl_container_of(listener, keyboard, destroy);
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->destroy.link);
    wl_list_remove(&keyboard->link);
    free(keyboard);
}

static void new_keyboard(ZcompServer *server, struct wlr_input_device *device) {
    struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

    ZcompKeyboard *keyboard = calloc(1, sizeof(*keyboard));
    keyboard->server = server;
    keyboard->wlr_keyboard = wlr_keyboard;

    // Default (us) keymap via xkbcommon.
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap =
        xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (keymap) {
        wlr_keyboard_set_keymap(wlr_keyboard, keymap);
        xkb_keymap_unref(keymap);
    }
    xkb_context_unref(context);
    wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

    keyboard->modifiers.notify = handle_kb_modifiers;
    wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
    keyboard->key.notify = handle_kb_key;
    wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
    keyboard->destroy.notify = handle_kb_destroy;
    wl_signal_add(&device->events.destroy, &keyboard->destroy);

    wlr_seat_set_keyboard(server->seat, wlr_keyboard);
    wl_list_insert(&server->keyboards, &keyboard->link);
}

// --- seat plumbing -------------------------------------------------------

static void update_capabilities(ZcompServer *server) {
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->keyboards)) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(server->seat, caps);
}

void zcomp_handle_new_input(struct wl_listener *listener, void *data) {
    ZcompServer *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;
    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        wlr_log(WLR_INFO, "new keyboard: %s", device->name);
        new_keyboard(server, device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
    case WLR_INPUT_DEVICE_TOUCH:
        wlr_log(WLR_INFO, "new pointer device: %s", device->name);
        wlr_cursor_attach_input_device(server->cursor, device);
        break;
    default:
        break;
    }
    update_capabilities(server);
}

static void handle_request_cursor(struct wl_listener *listener, void *data) {
    ZcompServer *server = wl_container_of(listener, server, request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    struct wlr_seat_client *focused =
        server->seat->pointer_state.focused_client;
    if (focused == event->seat_client) {
        wlr_cursor_set_surface(server->cursor, event->surface,
                               event->hotspot_x, event->hotspot_y);
    }
}

static void handle_request_set_selection(struct wl_listener *listener,
                                         void *data) {
    ZcompServer *server =
        wl_container_of(listener, server, request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

void zcomp_seat_init(ZcompServer *server) {
    wl_list_init(&server->keyboards);

    // Pointer cursor, tracked against the output layout.
    server->cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
    server->cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

    server->cursor_motion.notify = handle_cursor_motion;
    wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
    server->cursor_motion_absolute.notify = handle_cursor_motion_absolute;
    wl_signal_add(&server->cursor->events.motion_absolute,
                  &server->cursor_motion_absolute);
    server->cursor_button.notify = handle_cursor_button;
    wl_signal_add(&server->cursor->events.button, &server->cursor_button);
    server->cursor_axis.notify = handle_cursor_axis;
    wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
    server->cursor_frame.notify = handle_cursor_frame;
    wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);

    // The seat itself + input enumeration.
    server->seat = wlr_seat_create(server->display, "seat0");
    server->new_input.notify = zcomp_handle_new_input;
    wl_signal_add(&server->backend->events.new_input, &server->new_input);
    server->request_cursor.notify = handle_request_cursor;
    wl_signal_add(&server->seat->events.request_set_cursor,
                  &server->request_cursor);
    server->request_set_selection.notify = handle_request_set_selection;
    wl_signal_add(&server->seat->events.request_set_selection,
                  &server->request_set_selection);

    update_capabilities(server);
}

// --- virtual input (simulator injection) ---------------------------------
// A tool (wlrctl) created a virtual pointer: attach it to the seat's cursor so
// its motion/button/axis events flow through exactly the same handlers as a real
// pointer — the injected taps are indistinguishable from hardware ones.
static void handle_new_virtual_pointer(struct wl_listener *listener,
                                       void *data) {
    ZcompServer *server =
        wl_container_of(listener, server, new_virtual_pointer);
    struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
    wlr_log(WLR_INFO, "new virtual pointer (simulator input)");
    wlr_cursor_attach_input_device(server->cursor,
                                   &event->new_pointer->pointer.base);
}

// A tool (wtype) created a virtual keyboard: set it up like any keyboard (xkb
// keymap + key/modifier routing) so its keystrokes reach the focused surface.
static void handle_new_virtual_keyboard(struct wl_listener *listener,
                                        void *data) {
    ZcompServer *server =
        wl_container_of(listener, server, new_virtual_keyboard);
    struct wlr_virtual_keyboard_v1 *keyboard = data;
    wlr_log(WLR_INFO, "new virtual keyboard (simulator input)");
    new_keyboard(server, &keyboard->keyboard.base);
    update_capabilities(server);
}

// ---------------------------------------------------------------------------
// Scripted gestures (test hook).
//
// The screenshot harness can already click (wlrctl), but it cannot HOLD a button
// — and a press-and-hold is the whole substance of a drag and a long-press, the
// two gestures with the most machinery behind them (the implicit grab here, the
// slop/latch recognizer in the SDK). Verifying those needs input that presses,
// moves, and only then releases.
//
// Rather than teach the harness a new Wayland protocol, zcomp drives its own seat
// through the SAME functions a real device does — process_cursor_motion /
// process_cursor_button, implicit grab and all — so a scripted gesture is
// indistinguishable from a finger, and a bug in the grab path cannot hide behind
// a test that bypasses it.
//
//   ZCOMP_DRAG="x0 y0 x1 y1 [ms]"   press at (x0,y0), glide to (x1,y1), release
//   ZCOMP_HOLD="x y [ms]"           press at (x,y), hold still, release (long-press)
//   ZCOMP_INPUT_DELAY=ms            wait before starting (default 4000: let the
//                                   app map and settle first)
//
// Test-only, env-gated, and off in any normal run. See docs/tooling/simulator.md.
#define ZCOMP_GESTURE_STEP_MS 16     // ~60Hz, like a real pointer

typedef enum {
    GESTURE_NONE = 0,
    GESTURE_DRAG,
    GESTURE_HOLD,
} GestureKind;

static struct {
    ZcompServer *server;
    struct wl_event_source *timer;
    GestureKind kind;
    double x0, y0, x1, y1;
    int duration_ms;
    int elapsed_ms;
    bool pressed;
} g_gesture;

static uint32_t gesture_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void gesture_warp(ZcompServer *server, double x, double y) {
    wlr_cursor_warp(server->cursor, NULL, x, y);
    process_cursor_motion(server, gesture_now_ms());
    wlr_seat_pointer_notify_frame(server->seat);
}

static void gesture_button(ZcompServer *server, enum wlr_button_state state) {
    process_cursor_button(server, gesture_now_ms(), BTN_LEFT, state);
    wlr_seat_pointer_notify_frame(server->seat);
}

static int gesture_tick(void *data) {
    (void)data;
    ZcompServer *server = g_gesture.server;

    if (!g_gesture.pressed) {
        // Hover first, THEN press: the grab latches onto the surface the preceding
        // motion hovered, so a press with no motion before it would grab nothing.
        gesture_warp(server, g_gesture.x0, g_gesture.y0);
        gesture_button(server, WLR_BUTTON_PRESSED);
        g_gesture.pressed = true;
        wl_event_source_timer_update(g_gesture.timer, ZCOMP_GESTURE_STEP_MS);
        return 0;
    }

    g_gesture.elapsed_ms += ZCOMP_GESTURE_STEP_MS;
    double t = g_gesture.duration_ms > 0
                   ? (double)g_gesture.elapsed_ms / (double)g_gesture.duration_ms
                   : 1.0;
    if (t > 1.0) {
        t = 1.0;
    }

    if (g_gesture.kind == GESTURE_DRAG) {
        // Move in steps, not one jump: the recognizer needs to cross the slop and
        // then keep receiving motion (and it derives velocity from the samples), so
        // a single teleport would read as a different gesture entirely.
        gesture_warp(server, g_gesture.x0 + (g_gesture.x1 - g_gesture.x0) * t,
                     g_gesture.y0 + (g_gesture.y1 - g_gesture.y0) * t);
    }
    // A HOLD deliberately sends no motion: any movement past the slop would cancel
    // the long-press and become a pan.

    if (t >= 1.0) {
        gesture_button(server, WLR_BUTTON_RELEASED);
        wlr_log(WLR_INFO, "scripted gesture complete");
        return 0;   // one-shot: the timer is not re-armed
    }
    wl_event_source_timer_update(g_gesture.timer, ZCOMP_GESTURE_STEP_MS);
    return 0;
}

static void gesture_init(ZcompServer *server) {
    const char *drag = getenv("ZCOMP_DRAG");
    const char *hold = getenv("ZCOMP_HOLD");
    if (!drag && !hold) {
        return;
    }

    g_gesture.server = server;
    if (drag) {
        g_gesture.kind = GESTURE_DRAG;
        g_gesture.duration_ms = 300;
        if (sscanf(drag, "%lf %lf %lf %lf %d", &g_gesture.x0, &g_gesture.y0,
                   &g_gesture.x1, &g_gesture.y1, &g_gesture.duration_ms) < 4) {
            wlr_log(WLR_ERROR, "ZCOMP_DRAG: want \"x0 y0 x1 y1 [ms]\"");
            return;
        }
    } else {
        g_gesture.kind = GESTURE_HOLD;
        g_gesture.duration_ms = 800;   // past the long-press threshold
        if (sscanf(hold, "%lf %lf %d", &g_gesture.x0, &g_gesture.y0,
                   &g_gesture.duration_ms) < 2) {
            wlr_log(WLR_ERROR, "ZCOMP_HOLD: want \"x y [ms]\"");
            return;
        }
        g_gesture.x1 = g_gesture.x0;
        g_gesture.y1 = g_gesture.y0;
    }

    // Late by default: an app spawned at boot is not mapped for several seconds,
    // and a gesture delivered before it maps lands on whatever is underneath (the
    // launcher) — which looks exactly like a broken gesture. Wait for the app.
    int delay = 6500;
    const char *d = getenv("ZCOMP_INPUT_DELAY");
    if (d && *d) {
        delay = atoi(d);
    }

    g_gesture.timer = wl_event_loop_add_timer(
        wl_display_get_event_loop(server->display), gesture_tick, NULL);
    wl_event_source_timer_update(g_gesture.timer, delay);
    wlr_log(WLR_INFO, "scripted %s gesture armed (+%dms)",
            g_gesture.kind == GESTURE_DRAG ? "drag" : "hold", delay);
}

// ZCOMP_SHOT_AT="ms [ms ...]" — fire the capture chord at each moment after
// startup.
//
// The same shape as ZCOMP_DRAG/ZCOMP_HOLD and for the same reason: the harness
// has no way to press a key combination, and the alternatives are worse. wtype
// would need the keysym to survive the whole input path in a headless boot and
// would put a second dependency between the test and its subject; driving the
// capture by TAPPING something would be a coordinate, which is the thing this
// project has repeatedly watched rot.
//
// Crucially this enters through zcomp_screenshot(), the SAME function the chord
// calls — so the lock guard is on the path under test rather than beside it. A
// hook that bypassed the guard would make the suppression test prove nothing,
// which is the failure mode the ACTUATE harness shipped for four phases.
//
// IT TAKES A LIST, and that is what makes the privacy assertion honest. "No
// screenshot was written while the screen was locked" passes trivially in a boot
// that never managed to take one at all — the same trap that put three shots
// photographing nothing into the P41 catalogue. Two moments in ONE boot, one
// before the lock engages and one after, means the run carries its own positive
// control: the same process, the same binaries, the same everything except the
// lock.
#define ZCOMP_SHOT_MAX 4

static struct {
    ZcompServer *server;
    struct wl_event_source *timer;
    int at_ms[ZCOMP_SHOT_MAX];
    int count;
    int next;
} g_shot;

static int shot_tick(void *data) {
    (void)data;
    zcomp_screenshot(g_shot.server);
    g_shot.next++;
    if (g_shot.next < g_shot.count) {
        int delta = g_shot.at_ms[g_shot.next] - g_shot.at_ms[g_shot.next - 1];
        wl_event_source_timer_update(g_shot.timer, delta > 0 ? delta : 1);
    }
    return 0;
}

static void shot_init(ZcompServer *server) {
    const char *at = getenv("ZCOMP_SHOT_AT");
    if (!at || !at[0]) {
        return;
    }
    const char *p = at;
    while (g_shot.count < ZCOMP_SHOT_MAX) {
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p) {
            break;
        }
        if (v > 0) {
            // Ascending, so the deltas above are positive. An out-of-order list
            // is the caller's mistake and is reported rather than reordered.
            if (g_shot.count > 0 && v <= g_shot.at_ms[g_shot.count - 1]) {
                wlr_log(WLR_ERROR, "ZCOMP_SHOT_AT: times must ascend");
                return;
            }
            g_shot.at_ms[g_shot.count++] = (int)v;
        }
        p = end;
    }
    if (g_shot.count == 0) {
        wlr_log(WLR_ERROR, "ZCOMP_SHOT_AT: want \"ms [ms ...]\"");
        return;
    }
    g_shot.server = server;
    g_shot.timer = wl_event_loop_add_timer(
        wl_display_get_event_loop(server->display), shot_tick, NULL);
    wl_event_source_timer_update(g_shot.timer, g_shot.at_ms[0]);
    wlr_log(WLR_INFO, "scripted screenshot armed (%d at +%dms...)",
            g_shot.count, g_shot.at_ms[0]);
}

void zcomp_virtual_input_init(ZcompServer *server) {
    gesture_init(server);
    shot_init(server);

    server->virtual_pointer =
        wlr_virtual_pointer_manager_v1_create(server->display);
    server->new_virtual_pointer.notify = handle_new_virtual_pointer;
    wl_signal_add(&server->virtual_pointer->events.new_virtual_pointer,
                  &server->new_virtual_pointer);

    server->virtual_keyboard =
        wlr_virtual_keyboard_manager_v1_create(server->display);
    server->new_virtual_keyboard.notify = handle_new_virtual_keyboard;
    wl_signal_add(&server->virtual_keyboard->events.new_virtual_keyboard,
                  &server->new_virtual_keyboard);
}
