// zcomp seat/input: wl_seat plus the libinput-backed keyboard/pointer wiring.
// Keyboards are translated through xkbcommon; pointer motion is tracked with a
// wlr_cursor and routed to the surface under it. See compositor-internals.md.
#ifndef ZCOMP_SEAT_H
#define ZCOMP_SEAT_H

#include <wayland-server-core.h>

#include "zcomp/server.h"

struct wlr_keyboard;

typedef struct ZcompKeyboard {
    struct wl_list link;                // ZcompServer.keyboards
    ZcompServer *server;
    struct wlr_keyboard *wlr_keyboard;

    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
} ZcompKeyboard;

// Set up the seat, cursor and input-event plumbing on a freshly inited server.
void zcomp_seat_init(ZcompServer *server);

// backend new_input handler.
void zcomp_handle_new_input(struct wl_listener *listener, void *data);

#endif  // ZCOMP_SEAT_H
