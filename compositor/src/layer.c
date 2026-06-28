// zcomp layer-shell + scene layering + usable-area arrangement.
//
// At init we build five named scene sub-trees (background, bottom, apps, top,
// overlay) as direct children of scene->tree; their child order is the z-order,
// so the status bar (top) always composites over the apps. wlr-layer-shell
// clients (System UI) anchor into the matching layer tree with the wlroots
// wlr_scene_layer_surface_v1 helper, which positions them and subtracts their
// exclusive zone from the usable area. zcomp_arrange() then sizes every app
// toplevel to fill what remains. See compositor-internals.md ("Surfaces & layers").
#include "zcomp/layer.h"

#include <stdlib.h>

#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "zcomp/output.h"
#include "zcomp/toplevel.h"

// Map a layer enum to its scene sub-tree.
static struct wlr_scene_tree *tree_for_layer(ZcompServer *server,
                                             enum zwlr_layer_shell_v1_layer l) {
    switch (l) {
    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return server->layer_bg;
    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:     return server->layer_bottom;
    case ZWLR_LAYER_SHELL_V1_LAYER_TOP:        return server->layer_top;
    case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:    return server->layer_overlay;
    }
    return server->layer_top;
}

void zcomp_arrange(ZcompServer *server) {
    if (wl_list_empty(&server->outputs)) {
        return;
    }
    ZcompOutput *output =
        wl_container_of(server->outputs.next, output, link);
    struct wlr_box full = {
        .x = 0, .y = 0,
        .width = output->wlr_output->width,
        .height = output->wlr_output->height,
    };
    struct wlr_box usable = full;

    // Configure each layer surface, subtracting exclusive zones. Order matters:
    // overlay -> top -> bottom -> background, so an exclusive bar in `top`
    // shrinks the area the lower layers (and the apps) may use.
    static const enum zwlr_layer_shell_v1_layer order[] = {
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
    };
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        ZcompLayerSurface *ls;
        wl_list_for_each(ls, &server->layer_surfaces, link) {
            if (!ls->layer_surface->initialized) {
                continue;
            }
            if (ls->layer_surface->current.layer != order[i]) {
                continue;
            }
            wlr_scene_layer_surface_v1_configure(ls->scene, &full, &usable);
        }
    }

    server->usable = usable;

    // Size every app toplevel to the area left below/around the layers.
    ZcompToplevel *toplevel;
    wl_list_for_each(toplevel, &server->toplevels, link) {
        wlr_scene_node_set_position(&toplevel->scene_tree->node, usable.x,
                                    usable.y);
        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, usable.width,
                                  usable.height);
    }
}

// --- layer-surface lifecycle ---------------------------------------------

static void handle_layer_map(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompLayerSurface *ls = wl_container_of(listener, ls, map);
    zcomp_arrange(ls->server);
}

static void handle_layer_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompLayerSurface *ls = wl_container_of(listener, ls, unmap);
    zcomp_arrange(ls->server);
}

static void handle_layer_commit(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompLayerSurface *ls = wl_container_of(listener, ls, surface_commit);
    // The first commit needs an initial configure before the client can map; a
    // later commit may change anchors/exclusive zone. Re-arrange either way.
    // (wlr_scene_layer_surface_v1_configure sends the configure for us.)
    zcomp_arrange(ls->server);
}

static void handle_layer_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompLayerSurface *ls = wl_container_of(listener, ls, destroy);
    wl_list_remove(&ls->map.link);
    wl_list_remove(&ls->unmap.link);
    wl_list_remove(&ls->surface_commit.link);
    wl_list_remove(&ls->destroy.link);
    wl_list_remove(&ls->link);
    ZcompServer *server = ls->server;
    free(ls);
    zcomp_arrange(server);
}

void zcomp_handle_new_layer_surface(struct wl_listener *listener, void *data) {
    ZcompServer *server = wl_container_of(listener, server, new_layer_surface);
    struct wlr_layer_surface_v1 *layer_surface = data;

    // The protocol allows a NULL output; the compositor must assign one. Use the
    // first (only) output.
    if (!layer_surface->output) {
        if (wl_list_empty(&server->outputs)) {
            wlr_layer_surface_v1_destroy(layer_surface);
            return;
        }
        ZcompOutput *output =
            wl_container_of(server->outputs.next, output, link);
        layer_surface->output = output->wlr_output;
    }

    ZcompLayerSurface *ls = calloc(1, sizeof(*ls));
    if (!ls) {
        wlr_log(WLR_ERROR, "out of memory creating layer surface");
        return;
    }
    ls->server = server;
    ls->layer_surface = layer_surface;
    ls->scene = wlr_scene_layer_surface_v1_create(
        tree_for_layer(server, layer_surface->pending.layer), layer_surface);
    if (!ls->scene) {
        free(ls);
        return;
    }
    layer_surface->data = ls->scene;

    struct wlr_surface *surface = layer_surface->surface;
    ls->map.notify = handle_layer_map;
    wl_signal_add(&layer_surface->surface->events.map, &ls->map);
    ls->unmap.notify = handle_layer_unmap;
    wl_signal_add(&surface->events.unmap, &ls->unmap);
    ls->surface_commit.notify = handle_layer_commit;
    wl_signal_add(&surface->events.commit, &ls->surface_commit);
    ls->destroy.notify = handle_layer_destroy;
    wl_signal_add(&layer_surface->events.destroy, &ls->destroy);

    wl_list_insert(&server->layer_surfaces, &ls->link);
    wlr_log(WLR_INFO, "new layer surface: %s (layer %d)",
            layer_surface->namespace ? layer_surface->namespace : "(unnamed)",
            layer_surface->pending.layer);
}

// --- init ----------------------------------------------------------------

void zcomp_layer_shell_init(ZcompServer *server) {
    // Named scene sub-trees, in z-order (creation order = child order = z-order).
    // The background rect already sits at the bottom of scene->tree; these stack
    // above it: bg < bottom < apps < top < overlay.
    server->layer_bg = wlr_scene_tree_create(&server->scene->tree);
    server->layer_bottom = wlr_scene_tree_create(&server->scene->tree);
    server->apps = wlr_scene_tree_create(&server->scene->tree);
    server->layer_top = wlr_scene_tree_create(&server->scene->tree);
    server->layer_overlay = wlr_scene_tree_create(&server->scene->tree);

    wl_list_init(&server->layer_surfaces);
    server->usable = (struct wlr_box){0};

    // Advertise the layer-shell global at version 4 (wlroots 0.17 supports it;
    // the on_demand keyboard-interactivity from v4 is the highest we rely on).
    server->layer_shell = wlr_layer_shell_v1_create(server->display, 4);
    server->new_layer_surface.notify = zcomp_handle_new_layer_surface;
    wl_signal_add(&server->layer_shell->events.new_surface,
                  &server->new_layer_surface);
}
