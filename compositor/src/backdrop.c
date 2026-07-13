// zcomp backdrop blur — the compositor half of the system material.
//
// HOW IT WORKS. wlr_scene composites the whole output in one call, so there is
// no hook that lets us blur "what is underneath surface X" mid-composite. We get
// the same result with a second, off-screen composite:
//
//   1. hide every surface that has asked for a backdrop (and its own backdrop),
//   2. wlr_scene_output_build_state() renders what is left — i.e. exactly the
//      scene BELOW those surfaces — into a swapchain buffer, which we read back
//      on the CPU instead of committing,
//   3. downsample x4, box-blur twice (a good enough Gaussian), and resample each
//      client's requested rectangle out of it, masking the result to the system's
//      continuous-corner (squircle) geometry,
//   4. hand each rectangle to a wlr_scene_buffer parked directly beneath the
//      surface that asked for it, and un-hide everything.
//
// The caller's ordinary wlr_scene_output_commit() then paints the real frame,
// with the blurred plate already sitting under the translucent client.
//
// This costs a second composite, so it is rate-limited (BLUR_INTERVAL_MS): the
// content behind a shade or a dock is near-static, and a blur is a low-frequency
// signal, so refreshing it at ~10 Hz is indistinguishable from doing it at vsync
// and costs a tenth as much. Everything here is CPU/pixman-friendly: no GL.
#include "zcomp/backdrop.h"

#include <drm_fourcc.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "zcomp/layer.h"
#include "zcomp/output.h"
#include "zelto-backdrop-v1-protocol.h"

// The system's material constants. These are the compositor's, not the client's,
// so every material in the OS matches: one blur, one corner geometry.
#define BLUR_DOWNSCALE 4       // work at 1/4 resolution (blur is low-frequency)
#define BLUR_RADIUS 7          // box radius at 1/4 res (~28px full-res per pass)
#define BLUR_PASSES 2          // two boxes ≈ a Gaussian
#define BLUR_INTERVAL_MS 90    // refresh cadence of the second composite
#define CORNER_N 4.0f          // squircle exponent (iOS-ish continuous corner)

// --- a CPU-backed wlr_buffer ----------------------------------------------
// The blurred plate lives in ordinary memory we own. wlroots can texture from any
// wlr_buffer that exposes a data pointer (the pixman renderer reads it directly;
// the GLES2 renderer uploads it), which makes this the simplest way to get our own
// pixels into the scene graph.

typedef struct CpuBuffer {
    struct wlr_buffer base;
    uint32_t *data;    // ARGB8888, width*height
    size_t stride;
} CpuBuffer;

static const struct wlr_buffer_impl cpu_buffer_impl;

static CpuBuffer *cpu_buffer_from(struct wlr_buffer *buf) {
    if (buf->impl != &cpu_buffer_impl) {
        return NULL;
    }
    return (CpuBuffer *)buf;
}

static void cpu_buffer_destroy(struct wlr_buffer *buf) {
    CpuBuffer *cb = cpu_buffer_from(buf);
    if (!cb) {
        return;
    }
    free(cb->data);
    free(cb);
}

static bool cpu_buffer_begin_data_ptr_access(struct wlr_buffer *buf,
                                             uint32_t flags, void **data,
                                             uint32_t *format, size_t *stride) {
    (void)flags;
    CpuBuffer *cb = cpu_buffer_from(buf);
    if (!cb) {
        return false;
    }
    *data = cb->data;
    *format = DRM_FORMAT_ARGB8888;
    *stride = cb->stride;
    return true;
}

static void cpu_buffer_end_data_ptr_access(struct wlr_buffer *buf) {
    (void)buf;
}

static const struct wlr_buffer_impl cpu_buffer_impl = {
    .destroy = cpu_buffer_destroy,
    .begin_data_ptr_access = cpu_buffer_begin_data_ptr_access,
    .end_data_ptr_access = cpu_buffer_end_data_ptr_access,
};

static CpuBuffer *cpu_buffer_create(int width, int height) {
    if (width <= 0 || height <= 0) {
        return NULL;
    }
    CpuBuffer *cb = calloc(1, sizeof(*cb));
    if (!cb) {
        return NULL;
    }
    cb->stride = (size_t)width * 4;
    cb->data = calloc((size_t)width * (size_t)height, 4);
    if (!cb->data) {
        free(cb);
        return NULL;
    }
    wlr_buffer_init(&cb->base, &cpu_buffer_impl, width, height);
    return cb;
}

// --- one client's backdrop -------------------------------------------------

typedef struct ZcompBackdrop {
    struct wl_list link;                  // ZcompServer.backdrops
    ZcompServer *server;
    struct wl_resource *resource;
    struct wlr_surface *surface;          // the surface it sits beneath

    // Requested rectangle, surface-local. width <= 0 = no backdrop.
    int x, y, width, height, radius;

    struct wlr_scene_buffer *node;        // the blurred plate in the scene
    CpuBuffer *buffer;                    // pixels the plate points at
    int buf_w, buf_h;                     // size the pixels were allocated for

    struct wl_listener surface_destroy;
} ZcompBackdrop;

// The scene tree of the layer surface that owns `surface`, or NULL if the surface
// is not one we place (only layer surfaces — the System UI — can have a backdrop;
// an ordinary app window is not a material).
static struct wlr_scene_tree *tree_for_surface(ZcompServer *server,
                                               struct wlr_surface *surface) {
    ZcompLayerSurface *ls;
    wl_list_for_each(ls, &server->layer_surfaces, link) {
        if (ls->layer_surface->surface == surface) {
            return ls->scene->tree;
        }
    }
    return NULL;
}

static void backdrop_drop_node(ZcompBackdrop *bd) {
    if (bd->node) {
        wlr_scene_node_destroy(&bd->node->node);
        bd->node = NULL;
    }
    // The scene buffer held the last lock on the pixels; dropping ours lets them go.
    if (bd->buffer) {
        wlr_buffer_drop(&bd->buffer->base);
        bd->buffer = NULL;
    }
    bd->buf_w = bd->buf_h = 0;
}

static void backdrop_destroy(ZcompBackdrop *bd) {
    backdrop_drop_node(bd);
    wl_list_remove(&bd->surface_destroy.link);
    wl_list_remove(&bd->link);
    free(bd);
}

static void handle_surface_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    ZcompBackdrop *bd = wl_container_of(listener, bd, surface_destroy);
    // Keep the resource alive (the client still owns it) but let the pixels and
    // the scene node go with the surface they hung under.
    backdrop_drop_node(bd);
    bd->surface = NULL;
    bd->width = bd->height = 0;
}

// --- the blur --------------------------------------------------------------

// Average a `n`x`n` block of the source into one destination pixel (the
// downsample), reading XRGB/ARGB 32-bit source pixels.
static void downsample(const uint32_t *src, int sw, int sh, size_t src_stride,
                       uint32_t *dst, int dw, int dh) {
    size_t src_pitch = src_stride / 4;
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int j = 0; j < BLUR_DOWNSCALE; j++) {
                int sy = y * BLUR_DOWNSCALE + j;
                if (sy >= sh) {
                    break;
                }
                for (int i = 0; i < BLUR_DOWNSCALE; i++) {
                    int sx = x * BLUR_DOWNSCALE + i;
                    if (sx >= sw) {
                        break;
                    }
                    uint32_t p = src[(size_t)sy * src_pitch + (size_t)sx];
                    r += (p >> 16) & 0xff;
                    g += (p >> 8) & 0xff;
                    b += p & 0xff;
                    n++;
                }
            }
            if (n == 0) {
                n = 1;
            }
            dst[(size_t)y * dw + x] =
                0xff000000u | ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    }
}

// One separable box-blur pass over an ARGB image (horizontal then vertical),
// using a running sum so the cost is independent of the radius.
static void box_blur(uint32_t *img, uint32_t *tmp, int w, int h, int radius) {
    // Horizontal: img -> tmp.
    for (int y = 0; y < h; y++) {
        const uint32_t *row = img + (size_t)y * w;
        uint32_t *out = tmp + (size_t)y * w;
        int r = 0, g = 0, b = 0, n = 0;
        for (int x = -radius; x <= radius; x++) {
            int sx = x < 0 ? 0 : (x >= w ? w - 1 : x);
            uint32_t p = row[sx];
            r += (int)((p >> 16) & 0xff);
            g += (int)((p >> 8) & 0xff);
            b += (int)(p & 0xff);
            n++;
        }
        for (int x = 0; x < w; x++) {
            out[x] = 0xff000000u | (uint32_t)(r / n) << 16 |
                     (uint32_t)(g / n) << 8 | (uint32_t)(b / n);
            int add = x + radius + 1;
            int sub = x - radius;
            add = add >= w ? w - 1 : add;
            sub = sub < 0 ? 0 : sub;
            uint32_t pa = row[add], ps = row[sub];
            r += (int)((pa >> 16) & 0xff) - (int)((ps >> 16) & 0xff);
            g += (int)((pa >> 8) & 0xff) - (int)((ps >> 8) & 0xff);
            b += (int)(pa & 0xff) - (int)(ps & 0xff);
        }
    }
    // Vertical: tmp -> img.
    for (int x = 0; x < w; x++) {
        int r = 0, g = 0, b = 0, n = 0;
        for (int y = -radius; y <= radius; y++) {
            int sy = y < 0 ? 0 : (y >= h ? h - 1 : y);
            uint32_t p = tmp[(size_t)sy * w + x];
            r += (int)((p >> 16) & 0xff);
            g += (int)((p >> 8) & 0xff);
            b += (int)(p & 0xff);
            n++;
        }
        for (int y = 0; y < h; y++) {
            img[(size_t)y * w + x] = 0xff000000u | (uint32_t)(r / n) << 16 |
                                     (uint32_t)(g / n) << 8 |
                                     (uint32_t)(b / n);
            int add = y + radius + 1;
            int sub = y - radius;
            add = add >= h ? h - 1 : add;
            sub = sub < 0 ? 0 : sub;
            uint32_t pa = tmp[(size_t)add * w + x], ps = tmp[(size_t)sub * w + x];
            r += (int)((pa >> 16) & 0xff) - (int)((ps >> 16) & 0xff);
            g += (int)((pa >> 8) & 0xff) - (int)((ps >> 8) & 0xff);
            b += (int)(pa & 0xff) - (int)(ps & 0xff);
        }
    }
}

// Coverage of the continuous-corner (squircle) rounded rect at a pixel centre:
// 1 inside, 0 outside, antialiased across the edge. Matches the SDK's corner
// geometry so a compositor-drawn plate and a client-drawn card round the same way.
static float corner_coverage(float px, float py, float w, float h, float r) {
    if (r <= 0.5f) {
        return 1.0f;
    }
    if (r > w * 0.5f) {
        r = w * 0.5f;
    }
    if (r > h * 0.5f) {
        r = h * 0.5f;
    }
    // Distance from the pixel into the corner box, in units of r.
    float dx = 0.0f, dy = 0.0f;
    if (px < r) {
        dx = r - px;
    } else if (px > w - r) {
        dx = px - (w - r);
    }
    if (py < r) {
        dy = r - py;
    } else if (py > h - r) {
        dy = py - (h - r);
    }
    if (dx <= 0.0f || dy <= 0.0f) {
        return 1.0f;   // an edge band, not a corner
    }
    float u = dx / r, v = dy / r;
    // |u|^n + |v|^n <= 1 is inside. Compute the superellipse value with plain
    // multiplies (n = 4), then turn it into a ~1px antialiased ramp.
    float u2 = u * u, v2 = v * v;
    float e = u2 * u2 + v2 * v2;   // n = 4
    // e == 1 on the edge; the gradient there is ~4/r px^-1, so scale accordingly.
    float d = (1.0f - e) * (r / (CORNER_N * 1.0f));
    if (d >= 0.5f) {
        return 1.0f;
    }
    if (d <= -0.5f) {
        return 0.0f;
    }
    return d + 0.5f;
}

// Resample the blurred low-res image into the backdrop's own pixels, masked to the
// rounded rect. `ox`,`oy` is the backdrop's top-left in OUTPUT coordinates.
static void plate_from_blur(ZcompBackdrop *bd, const uint32_t *blur, int bw,
                            int bh, int ox, int oy) {
    CpuBuffer *cb = bd->buffer;
    int w = bd->buf_w, h = bd->buf_h;
    float radius = (float)bd->radius;
    for (int y = 0; y < h; y++) {
        int sy = (oy + y) / BLUR_DOWNSCALE;
        sy = sy < 0 ? 0 : (sy >= bh ? bh - 1 : sy);
        uint32_t *out = cb->data + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            int sx = (ox + x) / BLUR_DOWNSCALE;
            sx = sx < 0 ? 0 : (sx >= bw ? bw - 1 : sx);
            uint32_t p = blur[(size_t)sy * bw + sx];
            float cov = corner_coverage((float)x + 0.5f, (float)y + 0.5f,
                                        (float)w, (float)h, radius);
            if (cov <= 0.0f) {
                out[x] = 0;
                continue;
            }
            // Premultiplied ARGB (what the renderers expect).
            uint32_t a = (uint32_t)(cov * 255.0f + 0.5f);
            uint32_t r = ((p >> 16) & 0xff) * a / 255;
            uint32_t g = ((p >> 8) & 0xff) * a / 255;
            uint32_t b = (p & 0xff) * a / 255;
            out[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

static uint32_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

// Show/hide every backdrop-owning surface and its plate, so the off-screen pass
// renders only what is BELOW them.
static void set_blurred_visible(ZcompServer *server, bool visible) {
    ZcompBackdrop *bd;
    wl_list_for_each(bd, &server->backdrops, link) {
        if (bd->node) {
            wlr_scene_node_set_enabled(&bd->node->node, visible);
        }
        if (!bd->surface) {
            continue;
        }
        struct wlr_scene_tree *tree = tree_for_surface(server, bd->surface);
        if (tree) {
            wlr_scene_node_set_enabled(&tree->node, visible);
        }
    }
}

void zcomp_backdrop_frame(ZcompServer *server, ZcompOutput *output) {
    if (wl_list_empty(&server->backdrops)) {
        return;
    }
    // Anything to do? (A backdrop object with no region is inert.)
    bool any = false;
    ZcompBackdrop *bd;
    wl_list_for_each(bd, &server->backdrops, link) {
        if (bd->surface && bd->width > 0 && bd->height > 0) {
            any = true;
            break;
        }
    }
    if (!any) {
        return;
    }

    uint32_t now = now_ms();
    if (server->blur_last_ms != 0 &&
        now - server->blur_last_ms < BLUR_INTERVAL_MS) {
        return;
    }
    server->blur_last_ms = now;

    struct wlr_scene_output *so = output->scene_output;
    int ow = output->wlr_output->width, oh = output->wlr_output->height;
    if (ow <= 0 || oh <= 0) {
        return;
    }

    // Render the scene WITHOUT the materials into an off-screen buffer. Both this
    // pass and the caller's real commit must repaint in full: the swapchain buffer
    // we borrow here goes back to the pool holding a frame that was never shown, so
    // a damage-only render afterwards would composite onto the wrong contents.
    set_blurred_visible(server, false);
    wlr_damage_ring_add_whole(&so->damage_ring);

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    uint32_t *low = NULL, *tmp = NULL;
    int lw = 0, lh = 0;

    if (wlr_scene_output_build_state(so, &state, NULL) &&
        (state.committed & WLR_OUTPUT_STATE_BUFFER) && state.buffer) {
        void *data = NULL;
        uint32_t fmt = 0;
        size_t stride = 0;
        if (wlr_buffer_begin_data_ptr_access(state.buffer,
                                             WLR_BUFFER_DATA_PTR_ACCESS_READ,
                                             &data, &fmt, &stride)) {
            if (fmt == DRM_FORMAT_XRGB8888 || fmt == DRM_FORMAT_ARGB8888) {
                lw = (ow + BLUR_DOWNSCALE - 1) / BLUR_DOWNSCALE;
                lh = (oh + BLUR_DOWNSCALE - 1) / BLUR_DOWNSCALE;
                low = malloc((size_t)lw * lh * 4);
                tmp = malloc((size_t)lw * lh * 4);
                if (low && tmp) {
                    downsample(data, ow, oh, stride, low, lw, lh);
                    for (int i = 0; i < BLUR_PASSES; i++) {
                        box_blur(low, tmp, lw, lh, BLUR_RADIUS);
                    }
                } else {
                    free(low);
                    free(tmp);
                    low = tmp = NULL;
                }
            } else {
                static bool warned = false;
                if (!warned) {
                    wlr_log(WLR_ERROR,
                            "backdrop: unsupported output format 0x%x; no blur",
                            fmt);
                    warned = true;
                }
            }
            wlr_buffer_end_data_ptr_access(state.buffer);
        }
    }
    wlr_output_state_finish(&state);

    set_blurred_visible(server, true);
    wlr_damage_ring_add_whole(&so->damage_ring);

    if (!low) {
        return;
    }

    // Push each client's rectangle into its plate.
    wl_list_for_each(bd, &server->backdrops, link) {
        if (!bd->surface || bd->width <= 0 || bd->height <= 0) {
            backdrop_drop_node(bd);
            continue;
        }
        struct wlr_scene_tree *tree = tree_for_surface(server, bd->surface);
        if (!tree) {
            backdrop_drop_node(bd);
            continue;
        }
        int ox = tree->node.x + bd->x;
        int oy = tree->node.y + bd->y;

        if (!bd->node) {
            bd->node = wlr_scene_buffer_create(tree->node.parent, NULL);
            if (!bd->node) {
                continue;
            }
        }
        // Sit directly beneath the surface it belongs to, in the same layer.
        wlr_scene_node_place_below(&bd->node->node, &tree->node);
        wlr_scene_node_set_position(&bd->node->node, ox, oy);

        if (bd->buf_w != bd->width || bd->buf_h != bd->height || !bd->buffer) {
            if (bd->buffer) {
                wlr_buffer_drop(&bd->buffer->base);
            }
            bd->buffer = cpu_buffer_create(bd->width, bd->height);
            bd->buf_w = bd->width;
            bd->buf_h = bd->height;
            if (!bd->buffer) {
                bd->buf_w = bd->buf_h = 0;
                continue;
            }
        }
        plate_from_blur(bd, low, lw, lh, ox, oy);
        // The scene keeps its own lock; ours (from create) stays until we drop it.
        wlr_scene_buffer_set_buffer(bd->node, &bd->buffer->base);
        wlr_scene_node_set_enabled(&bd->node->node, true);
    }

    free(low);
    free(tmp);
}

// --- protocol --------------------------------------------------------------

static void backdrop_handle_destroy(struct wl_client *client,
                                    struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void backdrop_handle_set_region(struct wl_client *client,
                                       struct wl_resource *resource, int32_t x,
                                       int32_t y, int32_t width, int32_t height,
                                       int32_t corner_radius) {
    (void)client;
    ZcompBackdrop *bd = wl_resource_get_user_data(resource);
    if (!bd) {
        return;
    }
    bd->x = x;
    bd->y = y;
    bd->width = width;
    bd->height = height;
    bd->radius = corner_radius < 0 ? 0 : corner_radius;
    if (width <= 0 || height <= 0) {
        backdrop_drop_node(bd);
    } else {
        // A moving panel (a shade being dragged) re-declares its region every
        // frame; refresh on the next output frame rather than waiting out the
        // rate limit, or the plate lags the panel it belongs to.
        bd->server->blur_last_ms = 0;
    }
}

static const struct zelto_backdrop_v1_interface backdrop_impl = {
    .destroy = backdrop_handle_destroy,
    .set_region = backdrop_handle_set_region,
};

static void backdrop_resource_destroy(struct wl_resource *resource) {
    ZcompBackdrop *bd = wl_resource_get_user_data(resource);
    if (bd) {
        backdrop_destroy(bd);
    }
}

static void manager_handle_destroy(struct wl_client *client,
                                   struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void manager_handle_get_backdrop(struct wl_client *client,
                                        struct wl_resource *resource,
                                        uint32_t id,
                                        struct wl_resource *surface_resource) {
    ZcompServer *server = wl_resource_get_user_data(resource);
    struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);

    struct wl_resource *out = wl_resource_create(
        client, &zelto_backdrop_v1_interface,
        wl_resource_get_version(resource), id);
    if (!out) {
        wl_client_post_no_memory(client);
        return;
    }

    ZcompBackdrop *bd = calloc(1, sizeof(*bd));
    if (!bd) {
        wl_resource_destroy(out);
        wl_client_post_no_memory(client);
        return;
    }
    bd->server = server;
    bd->resource = out;
    bd->surface = surface;
    bd->surface_destroy.notify = handle_surface_destroy;
    wl_signal_add(&surface->events.destroy, &bd->surface_destroy);
    wl_list_insert(&server->backdrops, &bd->link);

    wl_resource_set_implementation(out, &backdrop_impl, bd,
                                   backdrop_resource_destroy);
}

static const struct zelto_backdrop_manager_v1_interface manager_impl = {
    .destroy = manager_handle_destroy,
    .get_backdrop = manager_handle_get_backdrop,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version,
                         uint32_t id) {
    ZcompServer *server = data;
    struct wl_resource *resource = wl_resource_create(
        client, &zelto_backdrop_manager_v1_interface, (int)version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, server, NULL);
}

void zcomp_backdrop_init(ZcompServer *server) {
    wl_list_init(&server->backdrops);
    server->blur_last_ms = 0;
    wl_global_create(server->display, &zelto_backdrop_manager_v1_interface, 1,
                     server, manager_bind);
}
