// zcomp text-input-v3 <-> input-method-v2 relay. See zcomp/text_input.h for the
// architecture. Modelled on the standard wlroots relay (sway/river): one seat,
// at most one input method (the on-screen keyboard), any number of text inputs
// (one per app that has a focusable text field), and focus routed off the seat's
// keyboard focus so an app's field talks to the keyboard without knowing it exists.
#include "zcomp/text_input.h"

#include <stdlib.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/util/log.h>

#include "zcomp/server.h"

typedef struct ZcompTextInputRelay ZcompTextInputRelay;

// One text-input client (an app's text field), tracked by the relay.
typedef struct ZcompTextInput {
    struct ZcompTextInputRelay *relay;
    struct wlr_text_input_v3 *wlr;
    struct wl_list link;                 // ZcompTextInputRelay.text_inputs

    struct wl_listener enable;
    struct wl_listener commit;
    struct wl_listener disable;
    struct wl_listener destroy;
} ZcompTextInput;

// The relay: both managers plus the live text-input list and the single input
// method. Owned by the server (server->text_relay); freed on display teardown.
struct ZcompTextInputRelay {
    ZcompServer *server;
    struct wlr_text_input_manager_v3 *ti_manager;
    struct wlr_input_method_manager_v2 *im_manager;

    struct wl_list text_inputs;                  // ZcompTextInput.link
    struct wlr_input_method_v2 *input_method;     // the keyboard (NULL = none)

    struct wl_listener new_text_input;
    struct wl_listener new_input_method;
    struct wl_listener im_commit;
    struct wl_listener im_destroy;
    struct wl_listener focus_change;             // seat keyboard focus
};

// The text input that currently owns keyboard focus (the compositor sent it
// enter). Only a focused text input relays to/from the input method.
static ZcompTextInput *focused_text_input(ZcompTextInputRelay *relay) {
    ZcompTextInput *ti;
    wl_list_for_each(ti, &relay->text_inputs, link) {
        if (ti->wlr->focused_surface) {
            return ti;
        }
    }
    return NULL;
}

// Push a text input's enable/surrounding-text/content-type state to the input
// method (the keyboard) so it shows/hides and knows the field's context. Sends a
// single done to commit the batch, per input-method-v2.
static void send_im_state(ZcompTextInputRelay *relay, ZcompTextInput *ti) {
    struct wlr_input_method_v2 *im = relay->input_method;
    if (!im) {
        return;   // no keyboard bound: nothing to drive
    }
    struct wlr_text_input_v3 *wlr = ti->wlr;
    if (wlr->current_enabled) {
        wlr_input_method_v2_send_activate(im);
        if (wlr->active_features & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT) {
            wlr_input_method_v2_send_surrounding_text(
                im, wlr->current.surrounding.text ? wlr->current.surrounding.text
                                                  : "",
                wlr->current.surrounding.cursor, wlr->current.surrounding.anchor);
        }
        wlr_input_method_v2_send_text_change_cause(im,
                                                   wlr->current.text_change_cause);
        if (wlr->active_features & WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE) {
            wlr_input_method_v2_send_content_type(im,
                                                  wlr->current.content_type.hint,
                                                  wlr->current.content_type.purpose);
        }
    } else {
        wlr_input_method_v2_send_deactivate(im);
    }
    wlr_input_method_v2_send_done(im);
}

// --- text-input events ----------------------------------------------------
// A field was focused + enabled (or its state changed while enabled): drive the
// keyboard. Both enable and commit funnel through send_im_state.
static void handle_ti_enable(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompTextInput *ti = wl_container_of(listener, ti, enable);
    send_im_state(ti->relay, ti);
}
static void handle_ti_commit(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompTextInput *ti = wl_container_of(listener, ti, commit);
    send_im_state(ti->relay, ti);
}
static void handle_ti_disable(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompTextInput *ti = wl_container_of(listener, ti, disable);
    send_im_state(ti->relay, ti);   // current_enabled is now false -> deactivate
}
static void handle_ti_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompTextInput *ti = wl_container_of(listener, ti, destroy);
    // If this field was focused + enabled, hide the keyboard as it goes away.
    if (ti->wlr->focused_surface && ti->relay->input_method) {
        wlr_input_method_v2_send_deactivate(ti->relay->input_method);
        wlr_input_method_v2_send_done(ti->relay->input_method);
    }
    wl_list_remove(&ti->enable.link);
    wl_list_remove(&ti->commit.link);
    wl_list_remove(&ti->disable.link);
    wl_list_remove(&ti->destroy.link);
    wl_list_remove(&ti->link);
    free(ti);
}

static void handle_new_text_input(struct wl_listener *listener, void *data) {
    ZcompTextInputRelay *relay =
        wl_container_of(listener, relay, new_text_input);
    struct wlr_text_input_v3 *wlr_ti = data;
    // Only serve text inputs bound to our seat.
    if (wlr_ti->seat != relay->server->seat) {
        return;
    }
    ZcompTextInput *ti = calloc(1, sizeof(*ti));
    if (!ti) {
        return;
    }
    ti->relay = relay;
    ti->wlr = wlr_ti;
    ti->enable.notify = handle_ti_enable;
    wl_signal_add(&wlr_ti->events.enable, &ti->enable);
    ti->commit.notify = handle_ti_commit;
    wl_signal_add(&wlr_ti->events.commit, &ti->commit);
    ti->disable.notify = handle_ti_disable;
    wl_signal_add(&wlr_ti->events.disable, &ti->disable);
    ti->destroy.notify = handle_ti_destroy;
    wl_signal_add(&wlr_ti->events.destroy, &ti->destroy);
    wl_list_insert(&relay->text_inputs, &ti->link);

    // If its surface already holds keyboard focus, send enter immediately (the
    // usual case is enter arrives later via the focus-change hook below).
    struct wlr_surface *focused = relay->server->seat->keyboard_state.focused_surface;
    if (focused && wl_resource_get_client(focused->resource) ==
                       wl_resource_get_client(wlr_ti->resource)) {
        wlr_text_input_v3_send_enter(wlr_ti, focused);
    }
}

// --- input-method (keyboard) events ---------------------------------------
// The keyboard committed a batch (commit_string / preedit / delete + commit):
// relay it to the focused text input as the standard text-input-v3 events.
static void handle_im_commit(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompTextInputRelay *relay = wl_container_of(listener, relay, im_commit);
    struct wlr_input_method_v2 *im = relay->input_method;
    ZcompTextInput *ti = focused_text_input(relay);
    if (!ti || !im) {
        return;
    }
    struct wlr_text_input_v3 *wlr = ti->wlr;
    if (im->current.preedit.text) {
        wlr_text_input_v3_send_preedit_string(wlr, im->current.preedit.text,
                                              im->current.preedit.cursor_begin,
                                              im->current.preedit.cursor_end);
    }
    if (im->current.commit_text) {
        wlr_text_input_v3_send_commit_string(wlr, im->current.commit_text);
    }
    if (im->current.delete.before_length || im->current.delete.after_length) {
        wlr_text_input_v3_send_delete_surrounding_text(
            wlr, im->current.delete.before_length, im->current.delete.after_length);
    }
    wlr_text_input_v3_send_done(wlr);
}

static void handle_im_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompTextInputRelay *relay = wl_container_of(listener, relay, im_destroy);
    wl_list_remove(&relay->im_commit.link);
    wl_list_remove(&relay->im_destroy.link);
    relay->input_method = NULL;
    // No keyboard anymore; a focused field simply gets no on-screen keyboard
    // until a new input method binds. (A hardware keyboard still works.)
}

static void handle_new_input_method(struct wl_listener *listener, void *data) {
    ZcompTextInputRelay *relay =
        wl_container_of(listener, relay, new_input_method);
    struct wlr_input_method_v2 *im = data;
    if (im->seat != relay->server->seat) {
        return;
    }
    if (relay->input_method) {
        // Only one on-screen keyboard at a time; tell the newcomer to give up.
        wlr_input_method_v2_send_unavailable(im);
        return;
    }
    relay->input_method = im;
    relay->im_commit.notify = handle_im_commit;
    wl_signal_add(&im->events.commit, &relay->im_commit);
    relay->im_destroy.notify = handle_im_destroy;
    wl_signal_add(&im->events.destroy, &relay->im_destroy);
    wlr_log(WLR_INFO, "input method bound (on-screen keyboard ready)");

    // If a field is already focused + enabled (the keyboard app started while a
    // text field had focus), activate it now so the keyboard shows.
    ZcompTextInput *ti = focused_text_input(relay);
    if (ti && ti->wlr->current_enabled) {
        send_im_state(relay, ti);
    }
}

// --- keyboard focus routing ------------------------------------------------
// text-input-v3 focus follows the seat's keyboard focus: send leave to a text
// input whose surface lost focus (deactivating the keyboard first if it was
// enabled), and enter to a text input on the newly-focused surface.
static void handle_focus_change(struct wl_listener *listener, void *data) {
    ZcompTextInputRelay *relay = wl_container_of(listener, relay, focus_change);
    struct wlr_seat_keyboard_focus_change_event *ev = data;
    ZcompTextInput *ti;
    wl_list_for_each(ti, &relay->text_inputs, link) {
        struct wlr_surface *focused = ti->wlr->focused_surface;
        if (focused && focused != ev->new_surface) {
            // Lost focus: hide the keyboard if this field had it up, then leave.
            if (ti->wlr->current_enabled && relay->input_method) {
                wlr_input_method_v2_send_deactivate(relay->input_method);
                wlr_input_method_v2_send_done(relay->input_method);
            }
            wlr_text_input_v3_send_leave(ti->wlr);
        }
    }
    if (!ev->new_surface) {
        return;
    }
    struct wl_client *surf_client =
        wl_resource_get_client(ev->new_surface->resource);
    wl_list_for_each(ti, &relay->text_inputs, link) {
        if (!ti->wlr->focused_surface &&
            wl_resource_get_client(ti->wlr->resource) == surf_client) {
            wlr_text_input_v3_send_enter(ti->wlr, ev->new_surface);
        }
    }
}

void zcomp_text_input_init(ZcompServer *server) {
    ZcompTextInputRelay *relay = calloc(1, sizeof(*relay));
    if (!relay) {
        wlr_log(WLR_ERROR, "out of memory creating text-input relay");
        return;
    }
    relay->server = server;
    wl_list_init(&relay->text_inputs);

    relay->ti_manager = wlr_text_input_manager_v3_create(server->display);
    relay->im_manager = wlr_input_method_manager_v2_create(server->display);
    server->text_input_manager = relay->ti_manager;
    server->input_method_manager = relay->im_manager;

    relay->new_text_input.notify = handle_new_text_input;
    wl_signal_add(&relay->ti_manager->events.text_input, &relay->new_text_input);
    relay->new_input_method.notify = handle_new_input_method;
    wl_signal_add(&relay->im_manager->events.input_method,
                  &relay->new_input_method);
    relay->focus_change.notify = handle_focus_change;
    wl_signal_add(&server->seat->keyboard_state.events.focus_change,
                  &relay->focus_change);

    server->text_relay = relay;
}
