// zcomp seat/input. Brings up wl_seat, a wlr_cursor for pointer tracking, and
// per-device keyboard wiring (xkbcommon keymaps). Pointer events are routed to
// the surface under the cursor; keyboard events to the focused surface.
#include "zcomp/seat.h"

#include <stdlib.h>

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
    double sx, sy;
    struct wlr_surface *surface =
        zcomp_surface_at(server, server->cursor->x, server->cursor->y, &sx, &sy);
    if (!surface) {
        // Nothing under the cursor: show the default arrow, drop focus + hover.
        server->hover_surface = NULL;
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
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

static void handle_cursor_button(struct wl_listener *listener, void *data) {
    ZcompServer *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;
    bool pressed = event->state == WLR_BUTTON_PRESSED;
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

    wlr_seat_pointer_notify_button(server->seat, event->time_msec,
                                   event->button, event->state);

    // End the grab once every button is up, then refocus to whatever the cursor
    // now rests over (so the next hover/tap targets the right surface).
    if (!pressed && server->seat->pointer_state.button_count == 0) {
        end_grab(server);
        process_cursor_motion(server, event->time_msec);
    }
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

// Compositor-level chords, recognized before app delivery (System UI gestures):
//   Home -> reveal the launcher;  Tab ("Switch") -> cycle foreground app.
// Returns true if the key was consumed and must not reach the client.
static bool handle_chord(ZcompServer *server, xkb_keysym_t sym) {
    switch (sym) {
    case XKB_KEY_Home:
        zcomp_home(server);
        return true;
    case XKB_KEY_Tab:
        zcomp_switch(server);
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
