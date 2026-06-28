// zcomp output management: mode setup, scene/output-layout attach, and the
// per-output vsync loop that composites the scene graph (background + every
// client surface) with damage tracking.
#include "zcomp/output.h"

#include <stdlib.h>

#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "zcomp/server.h"

static void handle_frame(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompOutput *output = wl_container_of(listener, output, frame);

    // Render and present the scene graph for this output, then tell clients
    // their buffers were shown so they can draw the next frame.
    wlr_scene_output_commit(output->scene_output, NULL);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(output->scene_output, &now);

    // Keep the vsync loop ticking. wlr_scene damage-tracks per swapchain buffer,
    // so a running loop fills every rotating buffer with the full composite
    // (background + client surfaces) rather than leaving stale regions.
    wlr_output_schedule_frame(output->wlr_output);
}

static void handle_request_state(struct wl_listener *listener, void *data) {
    ZcompOutput *output = wl_container_of(listener, output, request_state);
    const struct wlr_output_event_request_state *event = data;
    wlr_output_commit_state(output->wlr_output, event->state);
    // A mode change resizes the usable area; re-anchor layers and apps.
    wlr_scene_rect_set_size(output->server->background,
                            output->wlr_output->width,
                            output->wlr_output->height);
    zcomp_arrange(output->server);
}

static void handle_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompOutput *output = wl_container_of(listener, output, destroy);
    wlr_log(WLR_INFO, "output destroyed: %s", output->wlr_output->name);
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    free(output);
}

void zcomp_output_create(ZcompServer *server, struct wlr_output *wlr_output) {
    // Bind this output to our renderer/allocator before committing a buffer.
    if (!wlr_output_init_render(wlr_output, server->allocator,
                               server->renderer)) {
        wlr_log(WLR_ERROR, "wlr_output_init_render failed for %s",
                wlr_output->name);
        return;
    }

    // Enable the output and pick its preferred mode (if it has a mode list).
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode) {
        wlr_output_state_set_mode(&state, mode);
    }
    if (!wlr_output_commit_state(wlr_output, &state)) {
        wlr_log(WLR_ERROR, "failed to commit initial state for %s",
                wlr_output->name);
    }
    wlr_output_state_finish(&state);

    ZcompOutput *output = calloc(1, sizeof(*output));
    if (!output) {
        wlr_log(WLR_ERROR, "out of memory creating output");
        return;
    }
    output->server = server;
    output->wlr_output = wlr_output;

    output->frame.notify = handle_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    output->request_state.notify = handle_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);
    output->destroy.notify = handle_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    wl_list_insert(&server->outputs, &output->link);

    // Place the output in the layout (single output: top-left) and bind the
    // scene output so the scene graph composites onto it.
    struct wlr_output_layout_output *l_output =
        wlr_output_layout_add_auto(server->output_layout, wlr_output);
    output->scene_output = wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, l_output,
                                       output->scene_output);

    // Stretch the background fill across the full output.
    wlr_scene_rect_set_size(server->background, wlr_output->width,
                            wlr_output->height);

    wlr_log(WLR_INFO, "output %s online: %dx%d", wlr_output->name,
            wlr_output->width, wlr_output->height);

    // Now that an output exists, the usable area = full output (no layers yet);
    // arrange seeds server->usable so the first app gets a correct initial size.
    zcomp_arrange(server);

    // Prime the render loop. Thereafter the scene schedules frames itself
    // whenever its content changes (damage tracking).
    wlr_output_schedule_frame(wlr_output);
}
