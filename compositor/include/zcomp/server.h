// zcomp server: owns the Wayland display, the wlroots backend/renderer, the
// scene graph and every global protocol object the compositor advertises. This
// is the top-level compositor object. See docs/contributing/compositor-internals.md.
#ifndef ZCOMP_SERVER_H
#define ZCOMP_SERVER_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

struct wlr_backend;
struct wlr_session;
struct wlr_renderer;
struct wlr_allocator;
struct wlr_compositor;
struct wlr_subcompositor;
struct wlr_data_device_manager;
struct wlr_data_control_manager_v1;
struct wlr_output_layout;
struct wlr_scene;
struct wlr_scene_output_layout;
struct wlr_scene_rect;
struct wlr_scene_tree;
struct wlr_layer_shell_v1;
struct wlr_layer_surface_v1;
struct wlr_foreign_toplevel_manager_v1;
struct wlr_idle_notifier_v1;
struct wlr_text_input_manager_v3;
struct wlr_input_method_manager_v2;
struct ZcompTextInputRelay;
struct wlr_xdg_shell;
struct wlr_seat;
struct wlr_cursor;
struct wlr_xcursor_manager;
struct wlr_screencopy_manager_v1;
struct wlr_virtual_pointer_manager_v1;
struct wlr_virtual_keyboard_manager_v1;

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
    // wlr-data-control-unstable-v1: focus-independent clipboard access. A normal
    // client reads the selection only while it holds keyboard focus (wl_data_device
    // delivers the offer to the focused client only); the on-screen keyboard is a
    // layer surface that never takes keyboard focus, so it can't read the clipboard
    // that way. data-control delivers the current selection to every bound client
    // regardless of focus — it's how a clipboard manager (or our keyboard's Paste
    // key) reads the CLIPBOARD selection. wlroots ships the marshalling in
    // libwlroots (no compositor XML, like wl_data_device); Copy still goes through
    // the core wl_data_device path (seat.c request_set_selection). See P22.
    struct wlr_data_control_manager_v1 *data_control;

    // Scene graph: composites every surface over the background each vsync.
    struct wlr_output_layout *output_layout;
    struct wlr_scene *scene;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_scene_rect *background;           // bottom-most fill

    // Named scene sub-trees, created once as direct children of scene->tree, in
    // z-order (child order in a wlr_scene tree IS the z-order). Apps (xdg
    // toplevels + launcher) sit between the bottom and top shell layers, so the
    // status bar (layer_top) always composites over them and raise-to-top within
    // `apps` can never lift an app above the bar.
    struct wlr_scene_tree *layer_bg;            // ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND
    struct wlr_scene_tree *layer_bottom;        // ...BOTTOM
    struct wlr_scene_tree *apps;                // xdg toplevels + launcher
    struct wlr_scene_tree *layer_top;           // ...TOP (status bar)
    struct wlr_scene_tree *layer_overlay;       // ...OVERLAY

    // xdg-shell: the window-management protocol native apps use.
    struct wlr_xdg_shell *xdg_shell;            // xdg_wm_base
    struct wl_listener new_xdg_surface;
    struct wl_list toplevels;                   // ZcompToplevel.link (strict MRU; front = focused)

    // wlr-layer-shell: System UI (status bar / launcher background) anchored
    // surfaces with exclusive zones.
    struct wlr_layer_shell_v1 *layer_shell;
    struct wl_listener new_layer_surface;
    struct wl_list layer_surfaces;              // ZcompLayerSurface.link

    // A layer surface that grabbed EXCLUSIVE keyboard focus (a modal dialog,
    // e.g. the permission consent overlay). NULL when no modal holds the
    // keyboard; on its teardown focus returns to the front app toplevel.
    struct wlr_layer_surface_v1 *focused_layer;

    // wlr-foreign-toplevel-management: publishes a handle per mapped app window
    // so the launcher (a client) can list running apps and request activate/close
    // — the task switcher, over a standard protocol. zcomp keeps each handle's
    // title/app_id/activated state in sync with the toplevel it shadows.
    struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_manager;

    // ext-idle-notify-v1: reports user activity on the seat so idle-aware clients
    // (the lock screen) fire dim/lock/off timeouts. zcomp only pumps activity
    // into it on every input event; the timeout POLICY lives in the client, so
    // the compositor stays free of idle/lock policy (see seat.c notify_activity).
    struct wlr_idle_notifier_v1 *idle_notifier;

    // text-input-v3 + input-method-v2: the on-screen-keyboard bridge (P21). The
    // managers advertise the two globals (apps' text fields bind text-input; the
    // keyboard binds input-method); text_relay owns the bookkeeping that routes a
    // focused, enabled text field to the keyboard and relays committed strings
    // back — so any app's field drives the keyboard with no per-app code. See
    // text_input.c + docs/platform/soft-keyboard.md.
    struct wlr_text_input_manager_v3 *text_input_manager;
    struct wlr_input_method_manager_v2 *input_method_manager;
    struct ZcompTextInputRelay *text_relay;

    // Area left for app windows after subtracting layer exclusive zones.
    // Recomputed by zcomp_arrange().
    struct wlr_box usable;

    // Seat + input.
    struct wlr_seat *seat;                      // wl_seat ("seat0")
    struct wl_listener new_input;
    struct wl_listener request_cursor;
    struct wl_listener request_set_selection;
    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_mgr;
    // Implicit pointer grab: while a button is held, pointer motion stays with
    // the surface that received the press (translated into ITS coordinates) even
    // after the cursor leaves it, instead of refocusing to whatever is now under
    // the cursor. Without this a drag that runs off the pressed surface — pulling
    // the system shade down over an app, or a slider drag off its widget — loses
    // its events at the surface edge. NULL when no button is down; grab_ox/grab_oy
    // are the grabbed surface's top-left in layout coordinates.
    struct wlr_surface *grab_surface;
    double grab_ox, grab_oy;
    struct wl_listener grab_surface_destroy;   // drops the grab if it's destroyed
    // The surface the cursor last hovered (+ its layout origin), updated every
    // non-grabbed motion. A button press promotes this to the grab — more robust
    // than re-running the hit test at the press instant, which can momentarily
    // miss (e.g. a surface mid-resize), which would drop the grab and let a drag
    // escape the moment it leaves the pressed surface.
    struct wlr_surface *hover_surface;
    double hover_ox, hover_oy;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;
    struct wl_list keyboards;                   // ZcompKeyboard.link

    // Outputs.
    struct wl_listener new_output;
    struct wl_list outputs;                     // ZcompOutput.link

    // Headless-simulator test harness (docs/tooling/simulator.md). screencopy
    // lets `grim` capture a frame from the nested/headless compositor; the
    // virtual pointer/keyboard managers let `wlrctl`/`wtype` inject taps and
    // keystrokes, so the simulator is scriptable the way the QEMU QMP harness is.
    // Advertised unconditionally — harmless on a real device (the seat only ever
    // sees a virtual device if some tool creates one), and the whole point on the
    // desktop simulator, where there is no QMP to drive input or dump frames.
    struct wlr_screencopy_manager_v1 *screencopy;
    struct wlr_virtual_pointer_manager_v1 *virtual_pointer;
    struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard;
    struct wl_listener new_virtual_pointer;
    struct wl_listener new_virtual_keyboard;
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

// Recompute the usable app area: anchor every layer surface against the output
// and subtract its exclusive zone, then position/size every app toplevel to fill
// what's left. Call on every layer-surface map/unmap/state change and on output
// mode change. Safe to call with no outputs (no-op).
void zcomp_arrange(ZcompServer *server);

// Bring up the named scene layer sub-trees and the wlr-layer-shell global.
void zcomp_layer_shell_init(ZcompServer *server);

#endif  // ZCOMP_SERVER_H
