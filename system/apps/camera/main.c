// Camera — the capture half of the content pipeline.
//
// The app is deliberately thin, because everything under it already exists: the
// `camera` permission has been brokered end to end since P8 (the Cards demo has
// requested it for phases without anything ever capturing), the frame source and
// its lifecycle are libzelto's (sdk/src/camera.c, which is also where the
// synthetic-source decision is written down), and the still goes into the same
// shared library the screenshot service writes to (system/common/photos.h). What
// is here is a preview, a shutter, and the consent flow that gates both.
//
// THE CONSENT FLOW IS CHAINED, NOT FIRED-AND-FORGOTTEN. P39 fixed a bug where
// two permission requests in flight at once collided in the broker; the Sensors
// app has asked for one grant at a time ever since, and this app has only one to
// ask for — but it still opens the stream from the REPLY rather than optimistically
// beside it, so a denial means no stream rather than a stream that dies later.
#include <stdio.h>
#include <string.h>

#include <zelto/ui.h>

#include "common/app_chrome.h"
#include "common/photos.h"

typedef struct CameraState {
    bool asked;              // one-time permission request
    bool granted;
    bool denied;
    int stream;              // handle from z_camera_open, or -1
    int64_t frames;          // frames seen (proves the stream is live)
    int shots;               // stills written this session
    char last_id[ZELTO_PHOTO_ID_MAX];
    int64_t last_seq;        // the frame number the last still came from
} CameraState;

static void on_frame(ZApp *app, const ZCameraFrame *f, void *ud) {
    CameraState *s = ud;
    (void)app;
    s->frames = f->seq + 1;
}

// The grant is decided. Open the stream only on a yes — a denied camera means
// the preview never starts, rather than starting and failing quietly.
static void on_camera_perm(ZApp *app, ZPermStatus status, void *ud) {
    CameraState *s = ud;
    s->granted = (status == Z_PERM_GRANTED);
    s->denied = !s->granted;
    if (s->granted) {
        // 15fps: enough that the preview reads as live, far below the 30 cap.
        // A preview is not a game loop and every frame costs a full repaint.
        s->stream = z_camera_open(app, 15, on_frame, s);
    } else {
        fprintf(stderr, "zelto-camera: camera denied; no stream opened\n");
    }
    z_invalidate(app);
}

// THE SHUTTER. The live frame's pixels go straight into the shared library —
// the same store, the same id scheme and the same thumbnail rule the screenshot
// service uses, so a photo is a photo however it was made and Photos needs to
// know nothing about where it came from.
static void shutter(ZApp *app, void *state) {
    CameraState *s = state;
    int w = 0, h = 0;
    int64_t seq = 0;
    const uint32_t *px = z_camera_frame_pixels(&w, &h, &seq);
    if (!px) {
        fprintf(stderr, "zelto-camera: shutter with no live frame\n");
        return;
    }
    char id[ZELTO_PHOTO_ID_MAX];
    if (!zelto_photo_new_id(id, sizeof(id))) {
        return;
    }
    // Straight alpha: the source builds opaque ARGB, not premultiplied. Saying
    // which is the whole contract of that argument (sdk/src/image_write.c).
    if (zelto_photo_store(id, px, w, h, (size_t)w * 4, false)) {
        s->shots++;
        s->last_seq = seq;
        snprintf(s->last_id, sizeof(s->last_id), "%s", id);
        // NAMES THE FRAME, not just the file. The frame's sequence number is
        // encoded in its own top-left pixel, so a test can decode the written
        // PNG and prove the still is the frame that was LIVE when the shutter
        // fired rather than a stale buffer — which is the only thing that makes
        // a synthetic source worth testing against.
        fprintf(stderr, "zelto-camera: captured %s from frame %lld (%dx%d)\n",
                id, (long long)seq, w, h);
    }
    z_invalidate(app);
}

static ZView camera_body(ZApp *app, CameraState *s) {
    if (!s->asked) {
        s->asked = true;
        s->stream = -1;
        z_perm_request("camera", on_camera_perm, s);
    }

    float w = (float)z_app_width(app);
    float column = w - 2.0f * Z_SPACE_2XL;

    // The preview pane is a fixed 3:4 window that the frame COVERS — a camera
    // viewfinder is a crop of the sensor, not a letterboxed picture of it, and
    // the aspect is the pane's rather than the frame's so the layout does not
    // move when a device port hands over a differently-shaped sensor.
    float pane_h = column * 4.0f / 3.0f;
    const char *key = z_camera_preview_key();
    ZView pane = Background(Z_COLOR_SURFACE,
        CornerRadius(Z_RADIUS_CARD,
            Frame(column, pane_h,
                  key ? Cover(Image(key)) : Rect(.color = Z_COLOR_SURFACE_2))));

    char status[96];
    if (s->denied) {
        snprintf(status, sizeof(status), "Camera access denied");
    } else if (!key) {
        snprintf(status, sizeof(status), "Starting camera...");
    } else if (s->shots > 0) {
        snprintf(status, sizeof(status), s->shots == 1 ? "%d photo saved"
                                                       : "%d photos saved",
                 s->shots);
    } else {
        snprintf(status, sizeof(status), "Live");
    }

    ZStackOpts col = {.spacing = Z_SPACE_M, .align = Z_ALIGN_CENTER};
    int k = 0;
    col.children[k++] = Weight(Z_WEIGHT_BOLD,
        Foreground(Z_COLOR_TEXT,
            WrapText(app, "Camera", .width = column, .size = Z_FONT_LARGE_TITLE,
                     .weight = Z_WEIGHT_BOLD)));
    col.children[k++] = pane;
    // WrapText, not Text. "Starting camera..." is 718 units wide in a 632-unit
    // column at the largest accessibility size — a Text measures to one line
    // however long and paints straight off the edge, and a status line is
    // exactly the kind of short string nobody thinks to check. Found by the
    // audit, not by looking.
    col.children[k++] = Foreground(Z_COLOR_TEXT_MUTED,
        WrapText(app, status, .width = column, .size = Z_FONT_SUBHEAD));
    // The shutter is only offered while there is something to capture. Wrapped
    // in a row so it sizes to its own label: a lone Button as a column child
    // stretches to the column's width, which reads as a broken full-width bar
    // with its text stuck to the left.
    if (key) {
        col.children[k++] = HStack(Button(shutter, "Capture"),
                                   .align = Z_ALIGN_CENTER);
    }

    return Background(Z_COLOR_BG,
        Fill(Padding(Z_SPACE_2XL, z_stack(Z_AXIS_VERTICAL, &col))));
}

Z_APP_ID(CameraState, camera_body, "os.zelto.camera")
