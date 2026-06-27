// zcomp output management: mode setup + the per-output vsync render loop.
#include "zcomp/output.h"

#include <stdlib.h>

#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#include "zcomp/render.h"
#include "zcomp/server.h"

static void handle_frame(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompOutput *output = wl_container_of(listener, output, frame);
    zcomp_output_render(output);
}

static void handle_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompOutput *output = wl_container_of(listener, output, destroy);
    wlr_log(WLR_INFO, "output destroyed: %s", output->wlr_output->name);
    wl_list_remove(&output->frame.link);
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

    ZcompOutput *output = calloc(1, sizeof(*output));
    if (!output) {
        wlr_log(WLR_ERROR, "out of memory creating output");
        return;
    }
    output->server = server;
    output->wlr_output = wlr_output;
    clock_gettime(CLOCK_MONOTONIC, &output->start_time);

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

    output->frame.notify = handle_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    output->destroy.notify = handle_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    wl_list_insert(&server->outputs, &output->link);

    wlr_log(WLR_INFO, "output %s online: %dx%d", wlr_output->name,
            wlr_output->width, wlr_output->height);

    // Kick off the render loop.
    wlr_output_schedule_frame(wlr_output);
}
