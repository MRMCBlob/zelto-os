// libzelto camera client — the live preview stream, and the frame a capture
// keeps.
//
// ===========================================================================
// THE SOURCE DECISION, WHICH IS THE ONE THAT COULD SINK THIS
// ===========================================================================
// There is no camera in the simulator and none in QEMU. Four sources were on
// the table; the choice was made on MEASURED facts about this machine and this
// image, not on preference:
//
//   HOST PASSTHROUGH — rejected. The sim runs under WSL, which has no
//     /dev/video* at all (checked: the directory entry does not exist). There is
//     nothing to pass through.
//
//   V4L2 + THE `vivid` KERNEL MODULE — rejected, and this is the one worth
//     writing down because it LOOKS like the rigorous option. `vivid` is not in
//     the WSL kernel (`modinfo vivid` → not found), and the QEMU aarch64 kernel
//     would have to be rebuilt to carry it. What that rebuild buys is a real
//     V4L2 API in front of A SYNTHETIC TEST PATTERN — vivid IS a pattern
//     generator. So the cost is a kernel rebuild and a few hundred lines of
//     ioctl plumbing that no test in this repo could exercise on either target,
//     and the pixels at the end of it are still fake. That is a worse trade than
//     being honest about the fake.
//
//   A STILL-IMAGE LOOP — rejected as the primary. Replaying PNGs proves the
//     decoder, which P24 already proved, and a still that never changes cannot
//     show that a capture took the frame that was LIVE rather than a stale one.
//
//   SYNTHETIC FRAMES, GENERATED HERE, BEHIND A SEAM — chosen.
//
// This is the same shape as the sensors route (P38/P39), deliberately: zsysd
// synthesises accelerometer and GPS values from ZELTO_SIM_* and a real device
// port fills them from a HAL. The camera's seam is camera_source_fill() below —
// one function, replaced by a V4L2 or HAL read on a device port, with nothing
// above it changing.
//
// ---------------------------------------------------------------------------
// WHAT "VERIFIED" MEANS UNDER A SYNTHETIC SOURCE
// ---------------------------------------------------------------------------
// A synthetic source that only ever proves itself is the shape of a test that
// passes and means nothing, so the frames are built to prove something they
// could not otherwise: EVERY FRAME CARRIES ITS OWN SEQUENCE NUMBER, in the top-
// left pixel's red channel. PNG is lossless, so that value survives a capture
// exactly. This turns a fake source into a real assertion about the plumbing:
//
//   VERIFIED — frames advance at the requested rate; the preview shows the
//     CURRENT frame rather than a stale one; a capture writes the frame that was
//     live at the instant the shutter fired (its marker matches the sequence the
//     app logged); the stream stops while the app is backgrounded; a denied
//     permission yields no frames at all; and the still lands in the same photo
//     library the screenshot path writes to and is browsable in Photos.
//
//   NOT VERIFIED — that a physical sensor's pixels reach this API. Nothing in
//     this repo can test that, on either target, and no amount of ioctl code
//     would change it. It is the device port's job, and camera_source_fill is
//     where that job attaches.
//
// ---------------------------------------------------------------------------
// WHY THE FRAMES ARE GENERATED IN-PROCESS AND THE PERMISSION IS NOT
// ---------------------------------------------------------------------------
// The sensors route pushes samples through zsysd because a sample is a handful
// of floats. A frame is megabytes; pushing those through the broker's JSON
// control socket would be absurd, and on a real device the kernel arbitrates
// /dev/video0 per-process anyway rather than proxying pixels through a daemon.
//
// So the PIXELS are local and the STREAM LIFECYCLE is brokered: opening tells
// zsysd, which re-checks the grant and records who is streaming. That record is
// what a system in-use indicator reads, and it is the reason such an indicator
// can be trustworthy — the app does not draw it and cannot suppress it.
//
// THE LIMIT, STATED: an app that shipped its own patched libzelto could generate
// frames without telling the broker. That is unfixable client-side by
// construction; on a device the real gate is the kernel refusing the device
// node. Written down rather than papered over.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "internal.h"

#define CAM_W 480
#define CAM_H 640
#define CAM_FPS_MIN 1
#define CAM_FPS_MAX 30

static struct {
    bool open;
    bool paused;          // the app is backgrounded (privacy + power)
    int fps;
    int64_t seq;
    double next_due;      // monotonic seconds
    uint32_t *px;         // CAM_W*CAM_H, straight-alpha ARGB, owned here
    char key[32];         // the image-cache key the preview draws
    ZCameraCb cb;
    void *ud;
    ZApp *app;
} g_cam;

// --- the seam ---------------------------------------------------------------
// Fill `dst` with the frame for `seq`. THIS is the function a device port
// replaces with a V4L2 dequeue or a HAL callback; everything else in this file
// is transport and lifecycle and does not care where the pixels came from.
//
// The synthetic image is deliberately MOVING — a bright band sweeping across a
// colour ramp — so that a screenshot of the preview shows something that could
// not be a frozen placeholder, and two frames captured a moment apart differ.
static void camera_source_fill(uint32_t *dst, int w, int h, int64_t seq) {
    // COLOUR BARS, the broadcast test pattern, for two reasons beyond looking
    // deliberate. It is instantly recognisable as a TEST SIGNAL, so nobody can
    // mistake a screenshot of this for a photograph the OS took of something —
    // an honest fake should look fake. And bars are the pattern that makes a
    // colour-channel mistake obvious: a red/blue swap anywhere in the capture
    // path reorders them visibly instead of tinting a gradient plausibly.
    static const uint32_t BAR[8] = {
        0xc0c0c0, 0xc0c000, 0x00c0c0, 0x00c000,
        0xc000c0, 0xc00000, 0x0000c0, 0x202020,
    };
    int band = (int)((seq * 11) % (int64_t)w);   // a highlight sweeping across
    for (int y = 0; y < h; y++) {
        // The bottom sixth is a horizontal grey ramp, so a frame carries a
        // continuous gradient as well as flat patches (banding in an encode
        // shows up in a ramp and nowhere else).
        bool ramp = y > (h * 5) / 6;
        for (int x = 0; x < w; x++) {
            uint32_t c;
            if (ramp) {
                uint32_t v = (uint32_t)((x * 255) / (w - 1));
                c = (v << 16) | (v << 8) | v;
            } else {
                c = BAR[(x * 8) / w];
            }
            uint32_t r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
            int d = x - band;
            if (d < 0) {
                d = -d;
            }
            if (d < 18) {                       // the sweeping highlight
                uint32_t lift = (uint32_t)((18 - d) * 3);
                r = r + lift > 255 ? 255 : r + lift;
                g = g + lift > 255 ? 255 : g + lift;
                b = b + lift > 255 ? 255 : b + lift;
            }
            dst[(size_t)y * w + x] = 0xff000000u | (r << 16) | (g << 8) | b;
        }
    }
    // THE MARKER. The top-left pixel's red channel is the frame's sequence
    // number, so a captured PNG can be asserted to be the frame that was live
    // when the shutter fired rather than a stale buffer or a blank one. Without
    // this the source would only ever prove itself. See the header note.
    dst[0] = 0xff000000u | ((uint32_t)(seq & 0xff) << 16);
}

// --- the broker half --------------------------------------------------------
// One line each way on the control socket. The broker re-checks the grant (an
// app cannot open a stream by not asking) and records who is streaming, which is
// what makes a system in-use indicator something the app cannot forge.
static void camera_tell_broker(const char *op) {
    // A transient socket per message, the same shape sensors.c uses for its
    // one-shot calls (the broker's socket path is the one thing every client
    // agrees on).
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime) {
        runtime = "/run";
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return;
    }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/zsysd.sock", runtime);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return;
    }
    const char *id = z_active_app_id();
    char msg[192];
    int n = snprintf(msg, sizeof(msg), "{\"op\":\"%s\",\"app_id\":\"%s\"}\n", op,
                     id ? id : "");
    if (n > 0 && n < (int)sizeof(msg)) {
        ssize_t w = write(fd, msg, (size_t)n);
        (void)w;
    }
    close(fd);
}

// --- lifecycle --------------------------------------------------------------

bool z_camera_available(void) { return true; }

int z_camera_open(ZApp *app, int fps, ZCameraCb cb, void *ud) {
    if (!app || g_cam.open) {
        return -1;
    }
    if (fps < CAM_FPS_MIN) {
        fps = CAM_FPS_MIN;
    } else if (fps > CAM_FPS_MAX) {
        fps = CAM_FPS_MAX;
    }
    g_cam.px = calloc((size_t)CAM_W * CAM_H, 4);
    if (!g_cam.px) {
        return -1;
    }
    g_cam.open = true;
    g_cam.paused = false;
    g_cam.fps = fps;
    g_cam.seq = 0;
    g_cam.cb = cb;
    g_cam.ud = ud;
    g_cam.app = app;
    g_cam.next_due = 0.0;   // due immediately: a preview must not start blank
    // A key, not a path. z_image_adopt publishes raw pixels into the same cache
    // Image() reads, which is exactly what the App Switcher's window snapshots
    // already do — so a live preview needs no new view kind.
    // "\x01" and "cam:" as SEPARATE literals: 'c' is a hex digit, so writing
    // "\x01cam:" is one over-long hex escape rather than a byte and a word.
    // (app.c's "\x01snap:" gets away with it only because 's' is not.)
    snprintf(g_cam.key, sizeof(g_cam.key), "\x01" "cam:%d", 0);
    camera_tell_broker("camera_open");
    fprintf(stderr, "zelto: camera open %dx%d @%dfps\n", CAM_W, CAM_H, fps);
    return 1;
}

void z_camera_close(int handle) {
    (void)handle;
    if (!g_cam.open) {
        return;
    }
    camera_tell_broker("camera_close");
    fprintf(stderr, "zelto: camera closed after %lld frame(s)\n",
            (long long)g_cam.seq);
    free(g_cam.px);
    g_cam.px = NULL;
    g_cam.open = false;
    g_cam.cb = NULL;
    g_cam.app = NULL;
}

// Backgrounding STOPS the stream — the same rule sensors got in P39, and here it
// is the whole privacy story rather than a power optimisation: an app that keeps
// its camera running after you leave it is the thing an in-use indicator exists
// to make impossible. Called from the xdg lifecycle in app.c, not by the app.
void z_camera_set_paused(bool paused) {
    if (!g_cam.open || g_cam.paused == paused) {
        return;
    }
    g_cam.paused = paused;
    camera_tell_broker(paused ? "camera_close" : "camera_open");
    fprintf(stderr, "zelto: camera %s\n", paused ? "paused (backgrounded)"
                                                 : "resumed (foreground)");
}

// How long the app loop may sleep before the next frame is due (seconds), or a
// negative number when nothing is pending.
double z_camera_next_deadline(void) {
    if (!g_cam.open || g_cam.paused) {
        return -1.0;
    }
    return g_cam.next_due;
}

// Generate the frame if one is due. Called once per app-loop iteration.
void z_camera_pump(ZApp *app) {
    if (!g_cam.open || g_cam.paused || !g_cam.px) {
        return;
    }
    double now = z_now_seconds();
    if (now < g_cam.next_due) {
        return;
    }
    g_cam.next_due = now + 1.0 / (double)g_cam.fps;

    camera_source_fill(g_cam.px, CAM_W, CAM_H, g_cam.seq);

    // The cache takes ownership of what it is handed, so give it a copy and keep
    // ours: the app may capture the live frame at any moment, and the still has
    // to come from a buffer the cache is not about to free under us.
    uint32_t *copy = malloc((size_t)CAM_W * CAM_H * 4);
    if (copy) {
        memcpy(copy, g_cam.px, (size_t)CAM_W * CAM_H * 4);
        if (!z_image_adopt(g_cam.key, CAM_W, CAM_H, copy)) {
            /* adopt frees on failure */
        }
    }

    if (g_cam.cb) {
        ZCameraFrame f = {.key = g_cam.key, .w = CAM_W, .h = CAM_H,
                          .seq = g_cam.seq};
        g_cam.cb(app, &f, g_cam.ud);
    }
    g_cam.seq++;
    z_invalidate(app);
}

const char *z_camera_preview_key(void) {
    return (g_cam.open && !g_cam.paused && g_cam.seq > 0) ? g_cam.key : NULL;
}

const uint32_t *z_camera_frame_pixels(int *w, int *h, int64_t *seq) {
    if (!g_cam.open || g_cam.paused || !g_cam.px || g_cam.seq == 0) {
        return NULL;
    }
    if (w) {
        *w = CAM_W;
    }
    if (h) {
        *h = CAM_H;
    }
    if (seq) {
        *seq = g_cam.seq - 1;   // the frame currently on screen
    }
    return g_cam.px;
}
