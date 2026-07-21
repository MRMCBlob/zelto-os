// zcomp toplevel: one xdg_toplevel surface (a native app window) mapped into the
// scene graph. See docs/contributing/compositor-internals.md ("Surfaces & layers").
#ifndef ZCOMP_TOPLEVEL_H
#define ZCOMP_TOPLEVEL_H

#include <stdint.h>

#include <wayland-server-core.h>

#include "zcomp/server.h"

struct wlr_xdg_toplevel;
struct wlr_scene_tree;
struct wlr_foreign_toplevel_handle_v1;

typedef struct ZcompToplevel {
    struct wl_list link;                // ZcompServer.toplevels
    ZcompServer *server;
    struct wlr_xdg_toplevel *xdg_toplevel;
    struct wlr_scene_tree *scene_tree;  // this surface's node in the scene
    bool is_launcher;                   // the back-most app (Home focuses it)

    // The foreign-toplevel handle shadowing this window (the task switcher sees
    // it). Created on map, destroyed on unmap; title/app_id/activated kept in
    // sync. NULL between unmap and the next map.
    struct wlr_foreign_toplevel_handle_v1 *ftl_handle;

    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener set_title;       // xdg title change -> update ftl_handle

    // Foreign-toplevel handle request listeners (a taskbar client asking to focus
    // or close this window). Live only while ftl_handle is non-NULL.
    struct wl_listener ftl_request_activate;
    struct wl_listener ftl_request_close;

    // --- window capture (zelto-toplevel-capture-v1, see capture.c) ----------
    // Whether this window is currently the foreground one. Tracked here purely
    // so zcomp_update_activation can spot the active -> inactive EDGE, which is
    // when the snapshot is taken.
    bool active;

    // The last snapshot of this window: ARGB8888, snap_w * snap_h, owned by the
    // compositor because the client that displays it is a different, shorter-
    // lived process than the app that drew it. NULL until first backgrounded.
    uint32_t *snap_data;
    int snap_w, snap_h;

    // ZcompCapture objects (clients watching this window for snapshots).
    struct wl_list captures;
} ZcompToplevel;

// new_xdg_surface handler: adopt a freshly created xdg_surface (toplevel or
// popup) into the scene graph.
void zcomp_handle_new_xdg_surface(struct wl_listener *listener, void *data);

// Give keyboard focus to a toplevel's surface (and raise it within `apps`).
void zcomp_focus_toplevel(ZcompToplevel *toplevel);

// Global window-management chords (invoked from the seat keyboard handler):
//   Home   -> raise + focus the launcher (the back-most app), revealing it.
//   Switch -> focus the next toplevel in MRU order (cycle the foreground).
void zcomp_home(ZcompServer *server);
void zcomp_switch(ZcompServer *server);

#endif  // ZCOMP_TOPLEVEL_H
