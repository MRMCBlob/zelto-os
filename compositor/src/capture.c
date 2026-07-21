// zcomp per-toplevel window capture — the compositor half of
// zelto-toplevel-capture-v1.
//
// WHY THIS EXISTS. The App Switcher wants to show each backgrounded window as
// it looked when you left it. wlr-screencopy captures an OUTPUT, and a
// backgrounded window is precisely what is NOT on the output, so a switcher that
// captures on demand photographs itself. Until now the cards showed app icons.
//
// WHEN WE CAPTURE. At the active -> inactive edge, from zcomp_update_activation()
// in toplevel.c. That is the instant the window stops being what the user is
// looking at, so it is both the last moment its contents are meaningful and the
// moment iOS takes the same snapshot. Capturing on demand instead would be too
// late: by then the window may be occluded, resized, or gone.
//
// WHAT WE CAPTURE. Every Zelto app is a single wl_surface (plus popups), so the
// snapshot is just that surface's committed buffer — no scene sub-tree render
// pass. libzelto is a software renderer, so the buffer is wl_shm and we can read
// it straight off the CPU. Half resolution: a card is ~430x760 against a
// 720x1440 window, so full-resolution pixels would be thrown away immediately.
//
// WHO OWNS THE PIXELS. The compositor, unambiguously. The client that shows the
// card is a separate, short-lived process, and the app whose window it shows may
// well have exited — that is the case the card exists for. So one image per
// ZcompToplevel, refreshed at each deactivate edge and freed with the toplevel.
//
// HOW IT REACHES THE CLIENT. A sealed memfd per delivery. Sealing (F_SEAL_WRITE
// | F_SEAL_SHRINK) is not politeness: without it a compositor-side resize of the
// mapping would SIGBUS the shell that has it mapped. A NEW memfd per snapshot,
// rather than rewriting one, is the same argument in time rather than space — a
// client animating over a mapped snapshot must not have the pixels change under
// it mid-animation.
#include "zcomp/capture.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm_fourcc.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "zcomp/toplevel.h"
#include "zelto-toplevel-capture-v1-protocol.h"

// A card is roughly a third of the screen; half resolution is already more
// detail than it can show, and halving is an exact box filter (no resampling
// artefacts) that costs one pass.
#define CAPTURE_DOWNSCALE 2

// One client's capture object for one window.
typedef struct ZcompCapture {
    struct wl_list link;            // ZcompToplevel.captures
    struct wl_resource *resource;
    ZcompToplevel *toplevel;        // NULL once the window is gone
} ZcompCapture;

// --- taking the picture ----------------------------------------------------

// Box-halve an ARGB8888 image. Averaging the 2x2 block rather than dropping
// three of every four pixels is what keeps text on the card legible.
static void halve(const uint32_t *src, int sw, int sh, size_t src_stride,
                  uint32_t *dst, int dw, int dh) {
    size_t pitch = src_stride / 4;
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            uint32_t a = 0, r = 0, g = 0, b = 0, n = 0;
            for (int j = 0; j < CAPTURE_DOWNSCALE; j++) {
                int sy = y * CAPTURE_DOWNSCALE + j;
                if (sy >= sh) {
                    break;
                }
                for (int i = 0; i < CAPTURE_DOWNSCALE; i++) {
                    int sx = x * CAPTURE_DOWNSCALE + i;
                    if (sx >= sw) {
                        break;
                    }
                    uint32_t p = src[(size_t)sy * pitch + (size_t)sx];
                    a += (p >> 24) & 0xff;
                    r += (p >> 16) & 0xff;
                    g += (p >> 8) & 0xff;
                    b += p & 0xff;
                    n++;
                }
            }
            if (n == 0) {
                n = 1;
            }
            dst[(size_t)y * dw + x] = ((a / n) << 24) | ((r / n) << 16) |
                                      ((g / n) << 8) | (b / n);
        }
    }
}

// Package the stored image as a sealed, read-only fd and send it.
// Returns false without sending if anything fails; a missing snapshot is not an
// error, it just means the client keeps showing its icon fallback.
static bool capture_send(ZcompCapture *cap) {
    ZcompToplevel *t = cap->toplevel;
    if (!t || !t->snap_data || t->snap_w <= 0 || t->snap_h <= 0) {
        return false;
    }
    size_t stride = (size_t)t->snap_w * 4;
    size_t size = stride * (size_t)t->snap_h;

    int fd = memfd_create("zelto-snapshot", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        wlr_log(WLR_ERROR, "capture: memfd_create failed");
        return false;
    }
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return false;
    }
    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return false;
    }
    memcpy(map, t->snap_data, size);
    munmap(map, size);

    // Seal before the client ever sees it: the client maps this fd, and a later
    // write or shrink from this side would corrupt or SIGBUS it.
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_SEAL) < 0) {
        wlr_log(WLR_ERROR, "capture: could not seal snapshot fd; not sending");
        close(fd);
        return false;
    }

    zelto_toplevel_capture_v1_send_snapshot(cap->resource, fd, t->snap_w,
                                            t->snap_h, (int32_t)stride,
                                            DRM_FORMAT_ARGB8888);
    close(fd);   // the client got its own reference
    return true;
}

void zcomp_capture_take(ZcompToplevel *toplevel) {
    if (!toplevel || !toplevel->xdg_toplevel) {
        return;
    }
    ZcompServer *server = toplevel->server;

    // PRIVACY. Do not photograph a window while the screen is held by a modal
    // layer surface. server->focused_layer is set exactly while a layer surface
    // holds EXCLUSIVE keyboard interactivity (layer.c layer_sync_keyboard) —
    // which is what zelto-lock does when it locks. Backgrounding an app under a
    // lock screen must not mint a fresh picture of its contents; the previous
    // snapshot (taken while the user was actually looking at it) stands.
    if (server && server->focused_layer) {
        wlr_log(WLR_INFO, "capture: suppressed (screen held by a modal layer)");
        return;
    }

    struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
    if (!surface || !surface->buffer) {
        return;
    }

    // Read the SOURCE buffer, not the wlr_client_buffer wrapping it.
    //
    // wlr_client_buffer is the compositor's uploaded TEXTURE of a client buffer;
    // it exposes no data pointer, so asking it for CPU access fails outright
    // (which is what the first cut of this did, on every capture). Its `source`
    // is the client's actual wl_shm buffer, which does support data_ptr_access.
    // wlroots 0.17 has no wlr_texture_read_pixels, so there is no other route —
    // wlr_renderer_read_pixels reads the bound render target, i.e. the output,
    // which is the very thing that cannot show a backgrounded window.
    //
    // `source` is NULL if the client destroyed the buffer before it was
    // released. libzelto retains a double-buffer pool across frames so its
    // buffers outlive this, but a client that does not is entitled to, and then
    // there is simply no picture to take.
    struct wlr_buffer *buf = surface->buffer->source;
    if (!buf) {
        wlr_log(WLR_INFO, "capture: client buffer already released; "
                          "keeping fallback");
        return;
    }
    void *data = NULL;
    uint32_t format = 0;
    size_t stride = 0;
    if (!wlr_buffer_begin_data_ptr_access(buf, WLR_BUFFER_DATA_PTR_ACCESS_READ,
                                          &data, &format, &stride)) {
        // A GPU-side buffer with no CPU mapping. libzelto renders in software so
        // this should not happen, but a future client could; fall back to the
        // icon poster rather than trying to read it.
        wlr_log(WLR_INFO, "capture: buffer has no CPU access; keeping fallback");
        return;
    }
    if (format != DRM_FORMAT_ARGB8888 && format != DRM_FORMAT_XRGB8888) {
        wlr_buffer_end_data_ptr_access(buf);
        wlr_log(WLR_INFO, "capture: unsupported format 0x%x", format);
        return;
    }

    int sw = buf->width, sh = buf->height;
    int dw = sw / CAPTURE_DOWNSCALE, dh = sh / CAPTURE_DOWNSCALE;
    if (dw <= 0 || dh <= 0) {
        wlr_buffer_end_data_ptr_access(buf);
        return;
    }

    // Reuse the existing allocation when the window has not resized, so a user
    // flipping between two apps does not churn the heap.
    if (toplevel->snap_w != dw || toplevel->snap_h != dh) {
        free(toplevel->snap_data);
        toplevel->snap_data = calloc((size_t)dw * (size_t)dh, 4);
        toplevel->snap_w = toplevel->snap_data ? dw : 0;
        toplevel->snap_h = toplevel->snap_data ? dh : 0;
    }
    if (toplevel->snap_data) {
        halve(data, sw, sh, stride, toplevel->snap_data, dw, dh);
        // A surface may be XRGB (no meaningful alpha). Force it opaque so a
        // client blitting ARGB does not render the card fully transparent.
        if (format == DRM_FORMAT_XRGB8888) {
            size_t n = (size_t)dw * (size_t)dh;
            for (size_t i = 0; i < n; i++) {
                toplevel->snap_data[i] |= 0xff000000u;
            }
        }
    }
    wlr_buffer_end_data_ptr_access(buf);

    if (!toplevel->snap_data) {
        return;
    }
    wlr_log(WLR_INFO, "capture: snapshot %s %dx%d",
            toplevel->xdg_toplevel->app_id ? toplevel->xdg_toplevel->app_id
                                           : "(no id)",
            dw, dh);

    // Push it to anyone already watching (the switcher may be open and paging).
    ZcompCapture *cap;
    wl_list_for_each(cap, &toplevel->captures, link) {
        capture_send(cap);
    }
}

void zcomp_capture_toplevel_gone(ZcompToplevel *toplevel) {
    if (!toplevel) {
        return;
    }
    ZcompCapture *cap, *tmp;
    wl_list_for_each_safe(cap, tmp, &toplevel->captures, link) {
        zelto_toplevel_capture_v1_send_gone(cap->resource);
        wl_list_remove(&cap->link);
        wl_list_init(&cap->link);
        cap->toplevel = NULL;   // the resource outlives the window
    }
    free(toplevel->snap_data);
    toplevel->snap_data = NULL;
    toplevel->snap_w = toplevel->snap_h = 0;
}

// --- protocol --------------------------------------------------------------

static void capture_handle_destroy(struct wl_client *client,
                                   struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct zelto_toplevel_capture_v1_interface capture_impl = {
    .destroy = capture_handle_destroy,
};

static void capture_resource_destroy(struct wl_resource *resource) {
    ZcompCapture *cap = wl_resource_get_user_data(resource);
    if (!cap) {
        return;
    }
    if (cap->toplevel) {
        wl_list_remove(&cap->link);
    }
    free(cap);
}

static void manager_handle_destroy(struct wl_client *client,
                                   struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

// Resolve a foreign-toplevel handle resource back to the window it shadows.
//
// wlroots owns this protocol and offers no public from_resource(), and its
// resource user_data is an implementation detail we should not read. But a
// handle's `resources` list IS public (one entry per client that has bound the
// handle), so match through it. The handle is published per window
// (toplevel.c handle_map) and this runs once per card rather than per frame, so
// the nested walk is both correct and cheap.
static bool handle_owns_resource(struct wlr_foreign_toplevel_handle_v1 *handle,
                                 struct wl_resource *res) {
    struct wl_resource *r;
    wl_resource_for_each(r, &handle->resources) {
        if (r == res) {
            return true;
        }
    }
    return false;
}

static ZcompToplevel *toplevel_from_ftl_resource(ZcompServer *server,
                                                 struct wl_resource *res) {
    if (!res) {
        return NULL;
    }
    ZcompToplevel *t;
    wl_list_for_each(t, &server->toplevels, link) {
        if (t->ftl_handle && handle_owns_resource(t->ftl_handle, res)) {
            return t;
        }
    }
    return NULL;
}

static void manager_handle_get_capture(struct wl_client *client,
                                       struct wl_resource *resource,
                                       uint32_t id,
                                       struct wl_resource *toplevel_resource) {
    ZcompServer *server = wl_resource_get_user_data(resource);

    struct wl_resource *out = wl_resource_create(
        client, &zelto_toplevel_capture_v1_interface,
        wl_resource_get_version(resource), id);
    if (!out) {
        wl_client_post_no_memory(client);
        return;
    }
    ZcompCapture *cap = calloc(1, sizeof(*cap));
    if (!cap) {
        wl_resource_destroy(out);
        wl_client_post_no_memory(client);
        return;
    }
    cap->resource = out;
    wl_list_init(&cap->link);
    wl_resource_set_implementation(out, &capture_impl, cap,
                                   capture_resource_destroy);

    ZcompToplevel *t = toplevel_from_ftl_resource(server, toplevel_resource);
    if (!t) {
        // The window closed between the client listing it and asking for its
        // picture. Not an error — say so and leave the object inert.
        zelto_toplevel_capture_v1_send_gone(out);
        return;
    }
    cap->toplevel = t;
    wl_list_insert(&t->captures, &cap->link);

    // The usual case: the window was backgrounded before the switcher opened, so
    // the picture already exists and the client gets it without waiting.
    capture_send(cap);
}

static const struct zelto_toplevel_capture_manager_v1_interface manager_impl = {
    .destroy = manager_handle_destroy,
    .get_capture = manager_handle_get_capture,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version,
                         uint32_t id) {
    ZcompServer *server = data;
    struct wl_resource *resource = wl_resource_create(
        client, &zelto_toplevel_capture_manager_v1_interface, (int)version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, server, NULL);
}

void zcomp_capture_init(ZcompServer *server) {
    wl_global_create(server->display,
                     &zelto_toplevel_capture_manager_v1_interface, 1, server,
                     manager_bind);
}
