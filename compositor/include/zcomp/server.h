// zcomp server: owns the Wayland display, wlroots backend, renderer and the set
// of outputs. This is the top-level compositor object.
// See docs/contributing/compositor-internals.md.
#ifndef ZCOMP_SERVER_H
#define ZCOMP_SERVER_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct wlr_backend;
struct wlr_session;
struct wlr_renderer;
struct wlr_allocator;

// ZcompServer is the root of the compositor. One per process.
typedef struct ZcompServer {
    struct wl_display *display;
    struct wlr_backend *backend;
    struct wlr_session *session;        // NULL for the headless/nested backends
    struct wlr_renderer *renderer;      // GLES2 (EGL) renderer
    struct wlr_allocator *allocator;

    struct wl_listener new_output;      // backend->events.new_output
    struct wl_list outputs;             // ZcompOutput.link
} ZcompServer;

// Bring up the display, backend, renderer and allocator, and start listening for
// outputs. Returns false (and leaves nothing to clean up) on failure.
bool zcomp_server_init(ZcompServer *server);

// Run the Wayland event loop until the display terminates.
void zcomp_server_run(ZcompServer *server);

// Tear everything down.
void zcomp_server_finish(ZcompServer *server);

#endif  // ZCOMP_SERVER_H
