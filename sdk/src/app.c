// libzelto app runtime: the Wayland client side. Connects to the compositor,
// binds the core globals + xdg-shell, drives the configure handshake, and runs
// the build -> layout -> paint -> commit loop, submitting an shm buffer to its
// surface. See docs/contributing/sdk-internals.md ("Pipeline").
// (_GNU_SOURCE for memfd_create comes from the project-wide build args.)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "internal.h"
#include "xdg-shell-client-protocol.h"

#define Z_DEFAULT_FONT "/usr/share/zelto/fonts/ZeltoSans.ttf"

struct ZApp {
    void *state;
    ZBodyFn body;
    const char *title;
    ZArena arena;
    ZText *text;

    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct wl_output *output;

    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;

    int width, height;          // surface size in pixels
    int out_width, out_height;  // advertised output mode

    bool configured;
    bool running;
    bool dirty;
};

// --- shm buffer -----------------------------------------------------------
static struct wl_buffer *make_buffer(ZApp *app, uint32_t **out_pixels,
                                     int *out_stride) {
    int stride = app->width * 4;
    int size = stride * app->height;

    int fd = memfd_create("zelto-shm", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) {
        if (fd >= 0) {
            close(fd);
        }
        return NULL;
    }
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(app->shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        pool, 0, app->width, app->height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    *out_pixels = data;
    *out_stride = stride / 4;
    return buffer;
}

// Free the buffer (and its backing) once the compositor releases it.
static void buffer_release(void *data, struct wl_buffer *buffer) {
    (void)data;
    wl_buffer_destroy(buffer);
}
static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

// --- build/layout/paint/commit -------------------------------------------
static void render(ZApp *app) {
    uint32_t *pixels = NULL;
    int stride_px = 0;
    struct wl_buffer *buffer = make_buffer(app, &pixels, &stride_px);
    if (!buffer) {
        fprintf(stderr, "zelto: failed to allocate shm buffer\n");
        return;
    }
    // Clear to fully transparent; the app's background fills the rest.
    memset(pixels, 0, (size_t)stride_px * app->height * 4);

    // Build the view tree from app state into the per-build arena.
    z_arena_reset(&app->arena);
    z_build_arena = &app->arena;
    ZView root = app->body(app, app->state);

    z_layout(root, (float)app->width, (float)app->height, app->text);

    ZCanvas canvas = {
        .pixels = pixels,
        .width = app->width,
        .height = app->height,
        .stride_px = stride_px,
        .text = app->text,
    };
    z_render(&canvas, root);

    wl_buffer_add_listener(buffer, &buffer_listener, NULL);
    wl_surface_attach(app->surface, buffer, 0, 0);
    wl_surface_damage_buffer(app->surface, 0, 0, app->width, app->height);
    wl_surface_commit(app->surface);
}

// --- xdg-shell handlers ---------------------------------------------------
static void wm_base_ping(void *data, struct xdg_wm_base *wm_base,
                         uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                                  uint32_t serial) {
    ZApp *app = data;
    xdg_surface_ack_configure(xdg_surface, serial);
    app->configured = true;
    app->dirty = true;
}
static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height,
                               struct wl_array *states) {
    (void)toplevel;
    (void)states;
    ZApp *app = data;
    // Honour a non-zero compositor-suggested size; otherwise keep our own.
    if (width > 0 && height > 0) {
        app->width = width;
        app->height = height;
    }
}
static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    (void)toplevel;
    ((ZApp *)data)->running = false;
}
static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

// --- output (to learn the display size) -----------------------------------
static void output_geometry(void *data, struct wl_output *o, int32_t x,
                            int32_t y, int32_t pw, int32_t ph, int32_t sub,
                            const char *make, const char *model,
                            int32_t transform) {
    (void)data; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub;
    (void)make; (void)model; (void)transform;
}
static void output_mode(void *data, struct wl_output *o, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh) {
    (void)o; (void)refresh;
    ZApp *app = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        app->out_width = width;
        app->out_height = height;
    }
}
static void output_done(void *data, struct wl_output *o) { (void)data; (void)o; }
static void output_scale(void *data, struct wl_output *o, int32_t s) {
    (void)data; (void)o; (void)s;
}
static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
};

// --- registry -------------------------------------------------------------
static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
    ZApp *app = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        app->compositor = wl_registry_bind(registry, name,
                                           &wl_compositor_interface,
                                           version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        app->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        app->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface,
                                        version < 3 ? version : 3);
        xdg_wm_base_add_listener(app->wm_base, &wm_base_listener, app);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (!app->output) {
            app->output = wl_registry_bind(registry, name, &wl_output_interface,
                                           version < 2 ? version : 2);
            wl_output_add_listener(app->output, &output_listener, app);
        }
    }
}
static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name) {
    (void)data; (void)registry; (void)name;
}
static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

// --- entry ----------------------------------------------------------------
int z_app_main(void *state, ZBodyFn body, const char *title) {
    ZApp app = {0};
    app.state = state;
    app.body = body;
    app.title = title ? title : "Zelto App";
    app.running = true;

    const char *font = getenv("ZELTO_FONT");
    app.text = z_text_open(font ? font : Z_DEFAULT_FONT);
    if (!app.text) {
        fprintf(stderr, "zelto: warning: could not open font (text disabled)\n");
    }

    app.display = wl_display_connect(NULL);
    if (!app.display) {
        fprintf(stderr, "zelto: cannot connect to Wayland display\n");
        return 1;
    }
    app.registry = wl_display_get_registry(app.display);
    wl_registry_add_listener(app.registry, &registry_listener, &app);
    // Round-trip once to bind globals, again to receive the output mode.
    wl_display_roundtrip(app.display);
    wl_display_roundtrip(app.display);

    if (!app.compositor || !app.shm || !app.wm_base) {
        fprintf(stderr, "zelto: compositor missing required globals\n");
        return 1;
    }

    // Default surface size: fill the output if we learned its mode.
    app.width = app.out_width > 0 ? app.out_width : 800;
    app.height = app.out_height > 0 ? app.out_height : 600;

    app.surface = wl_compositor_create_surface(app.compositor);
    app.xdg_surface = xdg_wm_base_get_xdg_surface(app.wm_base, app.surface);
    xdg_surface_add_listener(app.xdg_surface, &xdg_surface_listener, &app);
    app.xdg_toplevel = xdg_surface_get_toplevel(app.xdg_surface);
    xdg_toplevel_add_listener(app.xdg_toplevel, &toplevel_listener, &app);
    xdg_toplevel_set_title(app.xdg_toplevel, app.title);
    xdg_toplevel_set_app_id(app.xdg_toplevel, "os.zelto.sample");
    wl_surface_commit(app.surface);

    while (app.running && wl_display_dispatch(app.display) != -1) {
        if (app.configured && app.dirty) {
            app.dirty = false;
            render(&app);
            wl_display_flush(app.display);
        }
    }

    z_text_close(app.text);
    z_arena_free(&app.arena);
    wl_display_disconnect(app.display);
    return 0;
}

void z_invalidate(ZApp *app) { app->dirty = true; }
void z_app_quit(ZApp *app) { app->running = false; }
