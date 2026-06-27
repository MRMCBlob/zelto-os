// zcomp server lifecycle: backend + renderer + allocator bring-up and the
// new_output hook. See docs/contributing/compositor-internals.md.
#include "zcomp/server.h"

#include <stdlib.h>

#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#include "zcomp/output.h"

static void handle_new_output(struct wl_listener *listener, void *data) {
    ZcompServer *server = wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;
    wlr_log(WLR_INFO, "new output: %s", wlr_output->name);
    zcomp_output_create(server, wlr_output);
}

bool zcomp_server_init(ZcompServer *server) {
    server->display = wl_display_create();
    if (!server->display) {
        wlr_log(WLR_ERROR, "wl_display_create failed");
        return false;
    }

    // Autocreate picks DRM/libinput on a TTY, or the Wayland/X11 backend when
    // nested, or headless as a fallback. session is populated for DRM.
    server->backend = wlr_backend_autocreate(server->display, &server->session);
    if (!server->backend) {
        wlr_log(WLR_ERROR, "wlr_backend_autocreate failed");
        goto err_display;
    }

    // GLES2 renderer (creates the EGL/GLES context) + allocator.
    server->renderer = wlr_renderer_autocreate(server->backend);
    if (!server->renderer) {
        wlr_log(WLR_ERROR, "wlr_renderer_autocreate failed");
        goto err_backend;
    }
    wlr_renderer_init_wl_display(server->renderer, server->display);

    server->allocator =
        wlr_allocator_autocreate(server->backend, server->renderer);
    if (!server->allocator) {
        wlr_log(WLR_ERROR, "wlr_allocator_autocreate failed");
        goto err_renderer;
    }

    wl_list_init(&server->outputs);
    server->new_output.notify = handle_new_output;
    wl_signal_add(&server->backend->events.new_output, &server->new_output);

    if (!wlr_backend_start(server->backend)) {
        wlr_log(WLR_ERROR, "wlr_backend_start failed");
        goto err_allocator;
    }

    return true;

err_allocator:
    wlr_allocator_destroy(server->allocator);
err_renderer:
    wlr_renderer_destroy(server->renderer);
err_backend:
    wlr_backend_destroy(server->backend);
err_display:
    wl_display_destroy(server->display);
    return false;
}

void zcomp_server_run(ZcompServer *server) {
    wlr_log(WLR_INFO, "entering event loop");
    wl_display_run(server->display);
}

void zcomp_server_finish(ZcompServer *server) {
    wl_display_destroy_clients(server->display);
    wlr_allocator_destroy(server->allocator);
    wlr_renderer_destroy(server->renderer);
    wlr_backend_destroy(server->backend);
    wl_display_destroy(server->display);
}
