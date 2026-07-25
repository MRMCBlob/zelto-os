// zelto-shot — the system screenshot service.
//
// A one-shot process: connect, copy the output, write a PNG into the photo
// library, post a notification, exit. The compositor forks it when the capture
// chord fires (compositor/src/seat.c), the same way zsysd forks zelto-consent
// and the home indicator forks zelto-recents.
//
// ---------------------------------------------------------------------------
// WHY A SEPARATE PROCESS AND NOT THE COMPOSITOR
// ---------------------------------------------------------------------------
// Everything needed to take the picture already existed and none of it was
// wired to anything the user can reach: wlr-screencopy has been enabled in
// server.c since the simulator harness was built, and until now its only client
// was `grim`. So the missing pieces were never the capture — they were a place
// to put the file, a name for it, and something that tells you it happened.
//
// The encode is why this is not simply a function inside zcomp. A 720x1440 PNG
// is tens of milliseconds of deflate, and the compositor's event loop is the one
// thread in the system that must never block — a stall there is a dropped frame
// on every surface at once. Forking moves the cost off that loop entirely, and
// costs nothing that matters: a screenshot is not on a frame budget.
//
// ---------------------------------------------------------------------------
// WHAT THIS PROCESS DOES *NOT* DECIDE
// ---------------------------------------------------------------------------
// Whether a screenshot is ALLOWED. That is checked in the compositor, before
// this binary is executed, because the compositor is the only process that knows
// whether a modal layer surface is holding the screen — and because a guard a
// client applies to itself is not a control. See zcomp_screenshot() in
// compositor/src/seat.c and the note there about the limit of this arrangement.
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <wayland-client.h>

#include "common/photos.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"

typedef struct Shot {
    struct wl_shm *shm;
    struct wl_output *output;
    struct zwlr_screencopy_manager_v1 *screencopy;

    // The frame the compositor described and the buffer we handed it back.
    uint32_t format, width, height, stride;
    bool y_invert;
    struct wl_buffer *buffer;
    void *data;
    size_t size;

    bool ready;     // pixels are ours to read
    bool failed;    // the compositor refused or the copy broke
} Shot;

// --- shm buffer ------------------------------------------------------------

static int anon_fd(size_t size) {
    int fd = memfd_create("zelto-shot", MFD_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// --- screencopy ------------------------------------------------------------

static void frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
                         uint32_t format, uint32_t width, uint32_t height,
                         uint32_t stride) {
    Shot *s = data;
    s->format = format;
    s->width = width;
    s->height = height;
    s->stride = stride;

    s->size = (size_t)stride * (size_t)height;
    int fd = anon_fd(s->size);
    if (fd < 0) {
        s->failed = true;
        return;
    }
    s->data = mmap(NULL, s->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (s->data == MAP_FAILED) {
        s->data = NULL;
        close(fd);
        s->failed = true;
        return;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(s->shm, fd, (int32_t)s->size);
    s->buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)width,
                                          (int32_t)height, (int32_t)stride,
                                          format);
    wl_shm_pool_destroy(pool);
    close(fd);

    // Version 1 of the protocol has no buffer_done: the copy is requested as
    // soon as a buffer that matches the announced format exists.
    zwlr_screencopy_frame_v1_copy(frame, s->buffer);
}

static void frame_flags(void *data, struct zwlr_screencopy_frame_v1 *frame,
                        uint32_t flags) {
    (void)frame;
    Shot *s = data;
    // The compositor is allowed to hand back a bottom-up image. Nothing warns
    // about this and the result is a picture that is merely upside down, which
    // is exactly the sort of defect that gets noticed only after it ships.
    s->y_invert = (flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0;
}

static void frame_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
                        uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec) {
    (void)frame; (void)sec_hi; (void)sec_lo; (void)nsec;
    ((Shot *)data)->ready = true;
}

static void frame_failed(void *data, struct zwlr_screencopy_frame_v1 *frame) {
    (void)frame;
    ((Shot *)data)->failed = true;
}

static void frame_damage(void *data, struct zwlr_screencopy_frame_v1 *frame,
                         uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    (void)data; (void)frame; (void)x; (void)y; (void)w; (void)h;
}
static void frame_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *frame,
                         uint32_t format, uint32_t w, uint32_t h) {
    (void)data; (void)frame; (void)format; (void)w; (void)h;
}
static void frame_buffer_done(void *data,
                              struct zwlr_screencopy_frame_v1 *frame) {
    (void)data; (void)frame;
}

// Every member must be non-NULL even for events this version never sends —
// libwayland calls through the table without checking.
static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = frame_buffer,
    .flags = frame_flags,
    .ready = frame_ready,
    .failed = frame_failed,
    .damage = frame_damage,
    .linux_dmabuf = frame_dmabuf,
    .buffer_done = frame_buffer_done,
};

// --- registry --------------------------------------------------------------

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version) {
    (void)version;
    Shot *s = data;
    if (strcmp(iface, wl_shm_interface.name) == 0) {
        s->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, wl_output_interface.name) == 0 && !s->output) {
        s->output = wl_registry_bind(reg, name, &wl_output_interface, 1);
    } else if (strcmp(iface, zwlr_screencopy_manager_v1_interface.name) == 0) {
        // Version 1: buffer -> copy -> ready is the whole exchange. The later
        // versions add dmabuf and multi-buffer negotiation, neither of which a
        // software encoder that wants CPU pixels has any use for.
        s->screencopy = wl_registry_bind(
            reg, name, &zwlr_screencopy_manager_v1_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *reg,
                                   uint32_t name) {
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

// --- pixels ----------------------------------------------------------------

// Normalise a wl_shm frame into the ARGB8888 word order libzelto uses, undoing
// a bottom-up flip on the way. Returns a freshly allocated width*height buffer.
//
// wl_shm formats are LITTLE-ENDIAN 32-bit words, so WL_SHM_FORMAT_XRGB8888 is
// already 0xXXRRGGBB in a uint32_t and needs no work; the *BGR variants are the
// same words with red and blue exchanged. Anything else is refused loudly rather
// than written out as a picture with the colours swapped — a screenshot that is
// subtly wrong is worse than one that did not happen, because nobody checks.
static uint32_t *normalise(const Shot *s) {
    bool bgr;
    switch (s->format) {
    case WL_SHM_FORMAT_ARGB8888:
    case WL_SHM_FORMAT_XRGB8888:
        bgr = false;
        break;
    case WL_SHM_FORMAT_ABGR8888:
    case WL_SHM_FORMAT_XBGR8888:
        bgr = true;
        break;
    default:
        fprintf(stderr, "zelto-shot: unsupported frame format 0x%x\n",
                s->format);
        return NULL;
    }
    // An X-format frame carries no meaningful alpha; forcing it opaque is what
    // makes the encoder pick RGB and keeps a fully transparent PNG from being
    // written out of a perfectly good screen.
    bool opaque = (s->format == WL_SHM_FORMAT_XRGB8888 ||
                   s->format == WL_SHM_FORMAT_XBGR8888);

    uint32_t *out = malloc((size_t)s->width * (size_t)s->height * 4);
    if (!out) {
        return NULL;
    }
    size_t pitch = s->stride / 4;
    for (uint32_t y = 0; y < s->height; y++) {
        uint32_t sy = s->y_invert ? (s->height - 1 - y) : y;
        const uint32_t *in = (const uint32_t *)s->data + (size_t)sy * pitch;
        uint32_t *o = out + (size_t)y * s->width;
        for (uint32_t x = 0; x < s->width; x++) {
            uint32_t p = in[x];
            if (bgr) {
                p = (p & 0xff00ff00u) | ((p & 0x00ff0000u) >> 16) |
                    ((p & 0x000000ffu) << 16);
            }
            o[x] = opaque ? (p | 0xff000000u) : p;
        }
    }
    return out;
}

// --- the notification ------------------------------------------------------
//
// Posted by talking to zsysd DIRECTLY rather than through z_notify_post, because
// that call resolves the poster through the running ZApp and this process has no
// window and no app loop. The wire format is one JSON line, which is what every
// other non-app caller of the broker uses (compositor/src/seat.c does the same
// for the media keys).
static void post_notification(const char *thumb_path, int count) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime) {
        runtime = "/run";
    }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/zsysd.sock", runtime);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return;
    }
    char body[192];
    snprintf(body, sizeof(body), "%d in your library", count);

    char msg[1024];
    int m = snprintf(
        msg, sizeof(msg),
        "{\"op\":\"notify_post\",\"app_id\":\"%s\",\"title\":\"Screenshot saved\","
        "\"body\":\"%s\",\"channel\":\"\",\"tap_route\":\"zelto://photos\","
        "\"action_id\":\"\",\"action_title\":\"\",\"image\":\"%s\"}\n",
        ZELTO_PHOTOS_APP_ID, body, thumb_path);
    if (m > 0 && m < (int)sizeof(msg) && write(fd, msg, (size_t)m) == m) {
        char reply[64];
        ssize_t r = read(fd, reply, sizeof(reply) - 1);   // synchronous post
        (void)r;
    }
    close(fd);
}

// --- main ------------------------------------------------------------------

int main(void) {
    Shot s = {0};
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) {
        fprintf(stderr, "zelto-shot: no Wayland display\n");
        return 1;
    }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &registry_listener, &s);
    wl_display_roundtrip(dpy);

    if (!s.shm || !s.output || !s.screencopy) {
        fprintf(stderr, "zelto-shot: compositor offers no screencopy "
                        "(shm=%d output=%d screencopy=%d)\n",
                s.shm != NULL, s.output != NULL, s.screencopy != NULL);
        return 1;
    }

    struct zwlr_screencopy_frame_v1 *frame =
        zwlr_screencopy_manager_v1_capture_output(s.screencopy, 0, s.output);
    zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, &s);

    while (!s.ready && !s.failed) {
        if (wl_display_dispatch(dpy) < 0) {
            break;
        }
    }
    if (!s.ready || !s.data) {
        fprintf(stderr, "zelto-shot: capture failed\n");
        return 1;
    }

    uint32_t *px = normalise(&s);
    if (!px) {
        return 1;
    }

    char id[ZELTO_PHOTO_ID_MAX];
    if (!zelto_photo_new_id(id, sizeof(id))) {
        fprintf(stderr, "zelto-shot: could not mint a photo id\n");
        free(px);
        return 1;
    }
    // wl_shm pixels are STRAIGHT alpha, not premultiplied. Saying so is the
    // whole contract of the last argument; see sdk/src/image_write.c.
    bool ok = zelto_photo_store(id, px, (int)s.width, (int)s.height,
                                (size_t)s.width * 4, false);
    free(px);
    if (!ok) {
        fprintf(stderr, "zelto-shot: could not write the photo\n");
        return 1;
    }

    char full[ZELTO_PHOTO_PATH_MAX], thumb[ZELTO_PHOTO_PATH_MAX];
    zelto_photo_path(id, full, sizeof(full));
    zelto_thumb_path(id, thumb, sizeof(thumb));

    // ONE LINE, NAMING THE FILE. The lock-suppression test's whole lesson (P42:
    // "an anonymous line cannot carry which thing happened") applies here too —
    // "a screenshot was taken" is not assertable, "THIS id, at THIS size, is in
    // the library" is.
    fprintf(stderr, "zelto-shot: saved %s %ux%u -> %s\n", id, s.width, s.height,
            full);
    fflush(stderr);

    post_notification(thumb, zelto_photos_count());

    wl_display_roundtrip(dpy);
    return 0;
}
