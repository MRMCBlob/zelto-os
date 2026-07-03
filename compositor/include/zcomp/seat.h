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

// Advertise the wlr-virtual-pointer + virtual-keyboard globals and wire any
// virtual device a tool (wlrctl/wtype) creates into the seat's cursor/keyboard.
// The simulator's input-injection path — see server.c + docs/tooling/simulator.md.
void zcomp_virtual_input_init(ZcompServer *server);

#endif  // ZCOMP_SEAT_H
