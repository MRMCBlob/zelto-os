// zcomp layer-shell: one wlr-layer-shell surface (System UI — the status bar, a
// launcher background, etc.) anchored into one of the named scene layer trees,
// with an exclusive zone subtracted from the app area. See
// docs/contributing/compositor-internals.md ("Surfaces & layers").
#ifndef ZCOMP_LAYER_H
#define ZCOMP_LAYER_H

#include <wayland-server-core.h>

#include "zcomp/server.h"

struct wlr_layer_surface_v1;
struct wlr_scene_layer_surface_v1;

typedef struct ZcompLayerSurface {
    struct wl_list link;                            // ZcompServer.layer_surfaces
    ZcompServer *server;
    struct wlr_layer_surface_v1 *layer_surface;
    struct wlr_scene_layer_surface_v1 *scene;       // wlroots layout helper

    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener surface_commit;
    struct wl_listener destroy;
} ZcompLayerSurface;

// wlr_layer_shell_v1 new_surface handler: adopt a fresh layer surface into the
// scene graph (in the tree for its layer) and start its configure handshake.
void zcomp_handle_new_layer_surface(struct wl_listener *listener, void *data);

#endif  // ZCOMP_LAYER_H
