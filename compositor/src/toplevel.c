// zcomp toplevel/xdg-shell handling: map xdg_toplevel + xdg_popup surfaces into
// the scene graph, drive their initial configure, and manage focus.
#include "zcomp/toplevel.h"

#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "zcomp/capture.h"
#include "zcomp/server.h"

// app_id the launcher sets (libzelto Z_APP_ID). Lets Home reveal it.
#define ZCOMP_LAUNCHER_APP_ID "os.zelto.launcher"

// Broadcast the xdg "activated" state so exactly the front toplevel is active
// and every other mapped toplevel is cleared. libzelto reads this state and
// drives its lifecycle hook (Active/Inactive). The foreign-toplevel handle's
// activated state is kept in lockstep so the task switcher highlights the same
// window. This is the whole of app lifecycle: no custom protocol, just the
// standard xdg activated state plus the foreign-toplevel mirror.
void zcomp_update_activation(ZcompServer *server,
                             ZcompToplevel *focused) {
    ZcompToplevel *t;
    wl_list_for_each(t, &server->toplevels, link) {
        bool active = (t == focused);
        bool was_active = t->active;
        wlr_xdg_toplevel_set_activated(t->xdg_toplevel, active);
        if (t->ftl_handle) {
            wlr_foreign_toplevel_handle_v1_set_activated(t->ftl_handle, active);
        }
        wlr_log(WLR_INFO, "activation: %s -> %s",
                t->xdg_toplevel->app_id ? t->xdg_toplevel->app_id : "(no id)",
                active ? "ACTIVE" : "inactive");

        // The active -> inactive EDGE is the moment to snapshot this window for
        // the App Switcher: it still holds exactly what the user was looking at,
        // and one instant later it may be occluded, resized or gone. Edge, not
        // level — this loop runs on every focus change and touches every window,
        // so capturing on `!active` would re-photograph every background window
        // each time any of them was switched to. See capture.c.
        t->active = active;
        if (was_active && !active) {
            zcomp_capture_take(t);
        }
    }
}

void zcomp_focus_toplevel(ZcompToplevel *toplevel) {
    if (!toplevel) {
        return;
    }
    ZcompServer *server = toplevel->server;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;

    // Raise the window and hand it the keyboard focus.
    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
    wl_list_remove(&toplevel->link);
    wl_list_insert(&server->toplevels, &toplevel->link);

    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    if (keyboard) {
        wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes,
                                       keyboard->num_keycodes,
                                       &keyboard->modifiers);
    } else {
        wlr_seat_keyboard_notify_enter(seat, surface, NULL, 0, NULL);
    }

    // The front app is now "activated"; clear everyone else.
    zcomp_update_activation(server, toplevel);
}

// A foreign-toplevel client (the launcher's Running list) asked to focus this
// window: route it through the normal focus path, which also re-broadcasts the
// activated state so the previously-front app pauses.
static void handle_ftl_request_activate(struct wl_listener *listener,
                                        void *data) {
    (void)data;
    ZcompToplevel *toplevel =
        wl_container_of(listener, toplevel, ftl_request_activate);
    zcomp_focus_toplevel(toplevel);
}

// ...or asked to close it: send the xdg close so the client exits, which unmaps
// and drops the window (and its handle) from the Running list.
static void handle_ftl_request_close(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompToplevel *toplevel =
        wl_container_of(listener, toplevel, ftl_request_close);
    wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
}

// Mirror an xdg title change onto the foreign-toplevel handle so the task
// switcher's labels stay current.
static void handle_set_title(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompToplevel *toplevel = wl_container_of(listener, toplevel, set_title);
    if (toplevel->ftl_handle && toplevel->xdg_toplevel->title) {
        wlr_foreign_toplevel_handle_v1_set_title(toplevel->ftl_handle,
                                                 toplevel->xdg_toplevel->title);
    }
}

static void handle_map(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompToplevel *toplevel = wl_container_of(listener, toplevel, map);
    ZcompServer *server = toplevel->server;
    const char *app_id = toplevel->xdg_toplevel->app_id;
    toplevel->is_launcher =
        app_id && strcmp(app_id, ZCOMP_LAUNCHER_APP_ID) == 0;
    // Resolve the manifest's no_snapshot= opt-out now, while we are already
    // paying for a window launch, rather than at the capture edge (capture.c).
    zcomp_capture_resolve_policy(toplevel, app_id);
    wl_list_insert(&server->toplevels, &toplevel->link);
    wlr_log(WLR_INFO, "toplevel mapped: %s%s",
            toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title
                                          : "(untitled)",
            toplevel->is_launcher ? " [launcher]" : "");

    // Publish a foreign-toplevel handle so taskbar clients (the launcher) see
    // this window. The client filters its own app_id out of the Running list.
    toplevel->ftl_handle =
        wlr_foreign_toplevel_handle_v1_create(server->foreign_toplevel_manager);
    if (toplevel->ftl_handle) {
        if (toplevel->xdg_toplevel->title) {
            wlr_foreign_toplevel_handle_v1_set_title(
                toplevel->ftl_handle, toplevel->xdg_toplevel->title);
        }
        if (app_id) {
            wlr_foreign_toplevel_handle_v1_set_app_id(toplevel->ftl_handle,
                                                      app_id);
        }
        toplevel->ftl_request_activate.notify = handle_ftl_request_activate;
        wl_signal_add(&toplevel->ftl_handle->events.request_activate,
                      &toplevel->ftl_request_activate);
        toplevel->ftl_request_close.notify = handle_ftl_request_close;
        wl_signal_add(&toplevel->ftl_handle->events.request_close,
                      &toplevel->ftl_request_close);
    }

    // Place it in the usable app area (below the bar) and size it to fill.
    zcomp_arrange(server);
    zcomp_focus_toplevel(toplevel);
}

static void handle_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompToplevel *toplevel = wl_container_of(listener, toplevel, unmap);
    ZcompServer *server = toplevel->server;
    bool was_front = (!wl_list_empty(&server->toplevels) &&
                      server->toplevels.next == &toplevel->link);

    wl_list_remove(&toplevel->link);

    // Drop the foreign-toplevel handle (sends `closed` to taskbar clients, so
    // the window leaves the Running list) and unhook its request listeners.
    if (toplevel->ftl_handle) {
        wl_list_remove(&toplevel->ftl_request_activate.link);
        wl_list_remove(&toplevel->ftl_request_close.link);
        wlr_foreign_toplevel_handle_v1_destroy(toplevel->ftl_handle);
        toplevel->ftl_handle = NULL;
    }

    // If the front app went away, reveal + activate the new MRU front so the
    // exposed app resumes (Active) instead of lingering paused.
    if (was_front && !wl_list_empty(&server->toplevels)) {
        ZcompToplevel *front =
            wl_container_of(server->toplevels.next, front, link);
        zcomp_focus_toplevel(front);
    }
}

static void handle_commit(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompToplevel *toplevel = wl_container_of(listener, toplevel, commit);
    // The very first commit needs a configure reply before the client can map.
    // Hand the client the usable app area (below the bar) so it never paints
    // over the layer surfaces. server->usable is set by zcomp_arrange once the
    // output (and any always-on bar) is up; fall back to the full output if a
    // toplevel somehow races ahead of it.
    if (toplevel->xdg_toplevel->base->initial_commit) {
        struct wlr_box *u = &toplevel->server->usable;
        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
                                  u->width > 0 ? u->width : 0,
                                  u->height > 0 ? u->height : 0);
    }
}

static void handle_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompToplevel *toplevel = wl_container_of(listener, toplevel, destroy);
    // Tell any client still watching this window that it is gone, and free the
    // stored picture. Beside the ftl_handle teardown by design: the snapshot and
    // the foreign-toplevel handle are the same window's two public faces.
    zcomp_capture_toplevel_gone(toplevel);
    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    wl_list_remove(&toplevel->commit.link);
    wl_list_remove(&toplevel->destroy.link);
    wl_list_remove(&toplevel->set_title.link);
    free(toplevel);
}

void zcomp_handle_new_xdg_surface(struct wl_listener *listener, void *data) {
    ZcompServer *server = wl_container_of(listener, server, new_xdg_surface);
    struct wlr_xdg_surface *xdg_surface = data;

    if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
        // Parent a popup under its parent surface's scene node.
        struct wlr_xdg_surface *parent =
            wlr_xdg_surface_try_from_wlr_surface(xdg_surface->popup->parent);
        if (!parent || !parent->data) {
            return;
        }
        struct wlr_scene_tree *parent_tree = parent->data;
        xdg_surface->data =
            wlr_scene_xdg_surface_create(parent_tree, xdg_surface);
        return;
    }
    if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
        return;
    }

    ZcompToplevel *toplevel = calloc(1, sizeof(*toplevel));
    if (!toplevel) {
        wlr_log(WLR_ERROR, "out of memory creating toplevel");
        return;
    }
    toplevel->server = server;
    toplevel->xdg_toplevel = xdg_surface->toplevel;
    wl_list_init(&toplevel->captures);
    // Apps live in the dedicated `apps` sub-tree (between the bottom and top
    // shell layers), so raising within it can never lift a window over the bar.
    toplevel->scene_tree =
        wlr_scene_xdg_surface_create(server->apps, xdg_surface);
    // Back-reference used to parent popups (above) and for hit-testing.
    toplevel->scene_tree->node.data = toplevel;
    xdg_surface->data = toplevel->scene_tree;

    struct wlr_surface *surface = xdg_surface->surface;
    toplevel->map.notify = handle_map;
    wl_signal_add(&surface->events.map, &toplevel->map);
    toplevel->unmap.notify = handle_unmap;
    wl_signal_add(&surface->events.unmap, &toplevel->unmap);
    toplevel->commit.notify = handle_commit;
    wl_signal_add(&surface->events.commit, &toplevel->commit);
    toplevel->destroy.notify = handle_destroy;
    wl_signal_add(&xdg_surface->events.destroy, &toplevel->destroy);
    toplevel->set_title.notify = handle_set_title;
    wl_signal_add(&xdg_surface->toplevel->events.set_title,
                  &toplevel->set_title);
}

void zcomp_home(ZcompServer *server) {
    // Reveal the launcher: find the launcher toplevel and raise+focus it. Since
    // apps are opaque and fill the usable area, raising the launcher to the top
    // of `apps` covers whatever app was foreground.
    ZcompToplevel *toplevel;
    wl_list_for_each(toplevel, &server->toplevels, link) {
        if (toplevel->is_launcher) {
            zcomp_focus_toplevel(toplevel);
            return;
        }
    }
}

void zcomp_switch(ZcompServer *server) {
    // Cycle the foreground: focus the least-recently-used toplevel (the tail of
    // the MRU list), which rotates through every window on repeated presses.
    if (wl_list_empty(&server->toplevels)) {
        return;
    }
    ZcompToplevel *tail =
        wl_container_of(server->toplevels.prev, tail, link);
    zcomp_focus_toplevel(tail);
}
