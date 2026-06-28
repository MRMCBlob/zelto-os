// zcomp toplevel/xdg-shell handling: map xdg_toplevel + xdg_popup surfaces into
// the scene graph, drive their initial configure, and manage focus.
#include "zcomp/toplevel.h"

#include <stdlib.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "zcomp/server.h"

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
}

static void handle_map(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompToplevel *toplevel = wl_container_of(listener, toplevel, map);
    wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
    wlr_log(WLR_INFO, "toplevel mapped: %s",
            toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title
                                          : "(untitled)");
    zcomp_focus_toplevel(toplevel);
}

static void handle_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompToplevel *toplevel = wl_container_of(listener, toplevel, unmap);
    wl_list_remove(&toplevel->link);
}

static void handle_commit(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompToplevel *toplevel = wl_container_of(listener, toplevel, commit);
    // The very first commit needs a configure reply before the client can map.
    // Setting size 0,0 lets the client choose its own size.
    if (toplevel->xdg_toplevel->base->initial_commit) {
        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
    }
}

static void handle_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompToplevel *toplevel = wl_container_of(listener, toplevel, destroy);
    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    wl_list_remove(&toplevel->commit.link);
    wl_list_remove(&toplevel->destroy.link);
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
    toplevel->scene_tree =
        wlr_scene_xdg_surface_create(&server->scene->tree, xdg_surface);
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
}
