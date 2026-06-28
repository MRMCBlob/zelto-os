// zcomp toplevel: one xdg_toplevel surface (a native app window) mapped into the
// scene graph. See docs/contributing/compositor-internals.md ("Surfaces & layers").
#ifndef ZCOMP_TOPLEVEL_H
#define ZCOMP_TOPLEVEL_H

#include <wayland-server-core.h>

#include "zcomp/server.h"

struct wlr_xdg_toplevel;
struct wlr_scene_tree;

typedef struct ZcompToplevel {
    struct wl_list link;                // ZcompServer.toplevels
    ZcompServer *server;
    struct wlr_xdg_toplevel *xdg_toplevel;
    struct wlr_scene_tree *scene_tree;  // this surface's node in the scene

    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
} ZcompToplevel;

// new_xdg_surface handler: adopt a freshly created xdg_surface (toplevel or
// popup) into the scene graph.
void zcomp_handle_new_xdg_surface(struct wl_listener *listener, void *data);

// Give keyboard focus to a toplevel's surface (and raise it).
void zcomp_focus_toplevel(ZcompToplevel *toplevel);

#endif  // ZCOMP_TOPLEVEL_H
