// zcomp output: one display output (a DRM connector, or a window with the
// nested/headless backends). Owns the per-output, scene-driven vsync render loop.
#ifndef ZCOMP_OUTPUT_H
#define ZCOMP_OUTPUT_H

#include <time.h>
#include <wayland-server-core.h>

#include "zcomp/server.h"

struct wlr_output;
struct wlr_scene_output;

typedef struct ZcompOutput {
    struct wl_list link;                // ZcompServer.outputs
    ZcompServer *server;
    struct wlr_output *wlr_output;
    struct wlr_scene_output *scene_output;

    struct wl_listener frame;           // wlr_output->events.frame (vsync)
    struct wl_listener request_state;   // wlr_output->events.request_state
    struct wl_listener destroy;         // wlr_output->events.destroy
} ZcompOutput;

// Adopt a freshly advertised output: configure its mode, enable it, attach it to
// the scene/output-layout, and start its vsync render loop.
void zcomp_output_create(ZcompServer *server, struct wlr_output *wlr_output);

#endif  // ZCOMP_OUTPUT_H
