// zcomp frame rendering. Proof of the GPU path: clear the output to a solid
// color, then draw one rectangle, using the wlroots GLES2 render pass (which
// owns the EGL/GLES context). A gentle animation on the rectangle proves the
// vsync loop is actually ticking.
#include "zcomp/render.h"

#include <math.h>
#include <time.h>

#include <wlr/render/pass.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

#include "zcomp/output.h"

// Seconds elapsed since the output came online.
static double elapsed_seconds(const ZcompOutput *output) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - output->start_time.tv_sec) +
           (double)(now.tv_nsec - output->start_time.tv_nsec) / 1e9;
}

void zcomp_output_render(ZcompOutput *output) {
    struct wlr_output *wlr_output = output->wlr_output;

    struct wlr_output_state state;
    wlr_output_state_init(&state);

    struct wlr_render_pass *pass =
        wlr_output_begin_render_pass(wlr_output, &state, NULL, NULL);
    if (!pass) {
        wlr_log(WLR_ERROR, "begin_render_pass failed for %s",
                wlr_output->name);
        wlr_output_state_finish(&state);
        return;
    }

    int width = wlr_output->width;
    int height = wlr_output->height;

    // 1. Clear to a solid color (Zelto teal).
    wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
        .box = {.x = 0, .y = 0, .width = width, .height = height},
        .color = {.r = 0.04f, .g = 0.52f, .b = 0.55f, .a = 1.0f},
    });

    // 2. One rectangle (Zelto amber), sliding horizontally to show the loop runs.
    double t = elapsed_seconds(output);
    int rw = width / 4;
    int rh = height / 4;
    int travel = (width - rw > 0) ? (width - rw) : 0;
    int x = (int)((0.5 + 0.5 * sin(t)) * travel);
    int y = (height - rh) / 2;
    wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
        .box = {.x = x, .y = y, .width = rw, .height = rh},
        .color = {.r = 0.98f, .g = 0.40f, .b = 0.15f, .a = 1.0f},
    });

    wlr_render_pass_submit(pass);
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    // Keep the vsync loop alive.
    wlr_output_schedule_frame(wlr_output);
}
