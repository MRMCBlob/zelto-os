// zcomp server lifecycle: backend + renderer + allocator bring-up, the core
// protocol globals (wl_compositor, wl_subcompositor, wl_shm, xdg-shell, wl_seat,
// wl_data_device_manager), the wlr_scene scene graph, and the new_output /
// new_xdg_surface / new_input hooks. See docs/contributing/compositor-internals.md.
#include "zcomp/server.h"

#include <stdlib.h>

#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "zcomp/layer.h"
#include "zcomp/output.h"
#include "zcomp/seat.h"
#include "zcomp/text_input.h"
#include "zcomp/toplevel.h"

// Zelto teal background (matches the bootstrap clear colour).
static const float ZCOMP_BG[4] = {0.04f, 0.52f, 0.55f, 1.0f};

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
    // Sets up wl_shm, linux-dmabuf and the DRM global for clients.
    wlr_renderer_init_wl_display(server->renderer, server->display);

    server->allocator =
        wlr_allocator_autocreate(server->backend, server->renderer);
    if (!server->allocator) {
        wlr_log(WLR_ERROR, "wlr_allocator_autocreate failed");
        goto err_renderer;
    }

    // --- Core protocol globals --------------------------------------------
    // wl_compositor (surfaces) + wl_subcompositor (subsurfaces).
    server->compositor = wlr_compositor_create(server->display, 5,
                                               server->renderer);
    server->subcompositor = wlr_subcompositor_create(server->display);
    // wl_data_device_manager (clipboard / drag-and-drop plumbing). Copy/paste
    // between focused clients rides this: a client offering a text/plain selection
    // (Copy) fires seat.request_set_selection -> wlr_seat_set_selection (seat.c);
    // the focused client reading the current wl_data_offer (Paste) gets it back.
    server->ddm = wlr_data_device_manager_create(server->display);
    // wlr-data-control-unstable-v1 (P22): the same CLIPBOARD selection, but readable
    // by clients that never hold keyboard focus. The on-screen keyboard is a layer
    // surface (no focus), so its Paste key reads the clipboard through this instead
    // of wl_data_device. wlroots ships the marshalling in libwlroots (no XML here).
    server->data_control =
        wlr_data_control_manager_v1_create(server->display);

    // --- Scene graph + output layout --------------------------------------
    server->output_layout = wlr_output_layout_create();
    server->scene = wlr_scene_create();
    server->scene_layout =
        wlr_scene_attach_output_layout(server->scene, server->output_layout);

    // Bottom-most background fill. Sized to the output when one appears.
    server->background =
        wlr_scene_rect_create(&server->scene->tree, 0, 0, ZCOMP_BG);

    wl_list_init(&server->outputs);
    server->new_output.notify = handle_new_output;
    wl_signal_add(&server->backend->events.new_output, &server->new_output);

    // --- xdg-shell (native app windows) -----------------------------------
    wl_list_init(&server->toplevels);
    server->xdg_shell = wlr_xdg_shell_create(server->display, 3);
    server->new_xdg_surface.notify = zcomp_handle_new_xdg_surface;
    wl_signal_add(&server->xdg_shell->events.new_surface,
                  &server->new_xdg_surface);

    // --- wlr-layer-shell + the layered scene -------------------------------
    // Builds the background/bottom/apps/top/overlay scene sub-trees (apps adopt
    // the `apps` tree, above) and advertises the layer-shell global for System UI.
    zcomp_layer_shell_init(server);

    // --- wlr-foreign-toplevel-management (the task switcher) ---------------
    // Publishes a handle per mapped app window so the launcher (a client) can
    // list running apps and request activate/close over a standard protocol.
    // zcomp keeps each handle's title/app_id/activated state in sync with the
    // toplevel it shadows (see toplevel.c).
    server->foreign_toplevel_manager =
        wlr_foreign_toplevel_manager_v1_create(server->display);

    // --- ext-idle-notify-v1 (idle/lock lifecycle) --------------------------
    // Advertise the idle-notifier global. The seat pumps activity into it on
    // every input event (seat.c); idle-aware clients (zelto-lock) register
    // per-timeout notifications and run the dim/lock/off policy themselves.
    server->idle_notifier = wlr_idle_notifier_v1_create(server->display);

    // --- Seat + input ------------------------------------------------------
    zcomp_seat_init(server);

    // --- text-input-v3 + input-method-v2 (on-screen keyboard bridge) -------
    // Advertise both globals and bring up the relay that bridges an app's text
    // field (text-input) to the on-screen keyboard (input-method). Must follow
    // the seat: the relay routes text-input focus off seat keyboard focus.
    zcomp_text_input_init(server);

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
    // Expose the socket so clients can connect (sets WAYLAND_DISPLAY).
    const char *socket = wl_display_add_socket_auto(server->display);
    if (!socket) {
        wlr_log(WLR_ERROR, "failed to create Wayland socket");
        return;
    }
    setenv("WAYLAND_DISPLAY", socket, true);
    wlr_log(WLR_INFO, "Wayland socket: %s", socket);

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
