// zcomp server: owns the Wayland display, the wlroots backend/renderer, the
// scene graph and every global protocol object the compositor advertises. This
// is the top-level compositor object. See docs/contributing/compositor-internals.md.
#ifndef ZCOMP_SERVER_H
#define ZCOMP_SERVER_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct wlr_backend;
struct wlr_session;
struct wlr_renderer;
struct wlr_allocator;
struct wlr_compositor;
struct wlr_subcompositor;
struct wlr_data_device_manager;
struct wlr_output_layout;
struct wlr_scene;
struct wlr_scene_output_layout;
struct wlr_scene_rect;
struct wlr_xdg_shell;
struct wlr_seat;
struct wlr_cursor;
struct wlr_xcursor_manager;

// ZcompServer is the root of the compositor. One per process.
typedef struct ZcompServer {
    struct wl_display *display;
    struct wlr_backend *backend;
    struct wlr_session *session;        // NULL for the headless/nested backends
    struct wlr_renderer *renderer;      // GLES2 (EGL) or pixman renderer
    struct wlr_allocator *allocator;

    // Core protocol globals advertised to clients.
    struct wlr_compositor *compositor;          // wl_compositor
    struct wlr_subcompositor *subcompositor;    // wl_subcompositor
    struct wlr_data_device_manager *ddm;        // wl_data_device_manager

    // Scene graph: composites every surface over the background each vsync.
    struct wlr_output_layout *output_layout;
    struct wlr_scene *scene;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_scene_rect *background;           // bottom-most fill

    // xdg-shell: the window-management protocol native apps use.
    struct wlr_xdg_shell *xdg_shell;            // xdg_wm_base
    struct wl_listener new_xdg_surface;
    struct wl_list toplevels;                   // ZcompToplevel.link

    // Seat + input.
    struct wlr_seat *seat;                      // wl_seat ("seat0")
    struct wl_listener new_input;
    struct wl_listener request_cursor;
    struct wl_listener request_set_selection;
    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_mgr;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;
    struct wl_list keyboards;                   // ZcompKeyboard.link

    // Outputs.
    struct wl_listener new_output;
    struct wl_list outputs;                     // ZcompOutput.link
} ZcompServer;

// Bring up the display, backend, renderer, allocator and every global, and start
// listening for outputs/inputs. Returns false on failure.
bool zcomp_server_init(ZcompServer *server);

// Run the Wayland event loop until the display terminates.
void zcomp_server_run(ZcompServer *server);

// Tear everything down.
void zcomp_server_finish(ZcompServer *server);

// Find the toplevel whose scene node is topmost at layout coords (lx, ly), and
// report the surface + surface-local coordinates under that point. Used by the
// pointer to route motion/button events. Returns NULL if nothing is there.
struct wlr_surface *zcomp_surface_at(ZcompServer *server, double lx, double ly,
                                     double *sx, double *sy);

#endif  // ZCOMP_SERVER_H
