// zcomp output: one display output (a DRM connector, or a window with the
// nested/headless backends). Owns the per-output render loop driven by vsync.
#ifndef ZCOMP_OUTPUT_H
#define ZCOMP_OUTPUT_H

#include <time.h>
#include <wayland-server-core.h>

#include "zcomp/server.h"

struct wlr_output;

typedef struct ZcompOutput {
    struct wl_list link;                // ZcompServer.outputs
    ZcompServer *server;
    struct wlr_output *wlr_output;

    struct wl_listener frame;           // wlr_output->events.frame (vsync)
    struct wl_listener destroy;         // wlr_output->events.destroy

    struct timespec start_time;         // for animation timing
} ZcompOutput;

// Adopt a freshly advertised output: configure its mode, enable it, and start
// its vsync render loop.
void zcomp_output_create(ZcompServer *server, struct wlr_output *wlr_output);

#endif  // ZCOMP_OUTPUT_H
