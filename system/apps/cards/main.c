// Zelto sample app #2 — "Cards".
//
// A small, visually distinct libzelto app (a blue scene with a tap counter) used
// as the second launchable app in the System UI demo. It also drives the runtime
// permission flow (P8): a "Use camera" button calls z_perm_request("camera").
// Because zelto-cards.app declares `permissions=camera` but there is no stored
// grant, zsysd routes a prompt to the System-UI consent dialog; tapping Allow
// records the grant, returns Z_PERM_GRANTED over the broker socket, and the UI
// flips to a granted state. Re-tapping returns immediately from the cache (no
// dialog). Maps a plain xdg_toplevel; the compositor sizes it below the bar.
#include <zelto/ui.h>

typedef struct CardsState {
    int count;
    bool cam_queried;       // have we asked zsysd for the initial status yet?
    ZPermStatus cam;        // current camera-permission status
    bool cam_requesting;    // a request is in flight (awaiting the broker reply)
} CardsState;

static void bump(ZApp *app, void *state) {
    CardsState *s = state;
    s->count++;
    z_invalidate(app);
}

// Broker reply: store the decision, clear the in-flight flag, repaint.
static void on_camera(ZApp *app, ZPermStatus status, void *ud) {
    CardsState *s = ud;
    s->cam = status;
    s->cam_requesting = false;
    z_invalidate(app);
}

static void use_camera(ZApp *app, void *state) {
    CardsState *s = state;
    s->cam_requesting = true;
    z_invalidate(app);   // show the pending state before the dialog appears
    z_perm_request("camera", on_camera, s);
}

// A lifecycle banner: green "ACTIVE" in the foreground, amber "PAUSED" when
// backgrounded. Driven by the xdg activated state the compositor broadcasts.
static ZView lifecycle_banner(ZApp *app) {
    bool act = z_app_active(app);
    return Background(act ? z_rgba(0x1d, 0x5e, 0x3a, 0xff)
                          : z_rgba(0x5e, 0x49, 0x1d, 0xff),
        Padding(10,
            Foreground(Z_COLOR_TEXT_INV,
                Font(Z_FONT_CALLOUT, Text(act ? "ACTIVE" : "PAUSED")))));
}

// The camera section: a status line, the "Use camera" request button (kept
// present even after a grant so re-tapping exercises the cached fast path), and
// — once granted — a distinct green "Camera ready" panel proving the flip.
static ZView camera_section(ZApp *app, CardsState *s) {
    (void)app;
    const char *status_txt = s->cam == Z_PERM_GRANTED ? "camera: granted"
                             : s->cam == Z_PERM_DENIED ? "camera: denied"
                                                       : "camera: prompt";
    ZStackOpts opts = {.spacing = 10, .align = Z_ALIGN_CENTER};
    int k = 0;
    opts.children[k++] = Foreground(z_rgba(0x9a, 0xc4, 0xf0, 0xff),
        Font(Z_FONT_CAPTION, Text("%s", status_txt)));
    if (s->cam_requesting) {
        opts.children[k++] = Foreground(z_rgba(0xf0, 0xd0, 0x6b, 0xff),
            Font(Z_FONT_CALLOUT, Text("Requesting...")));
    } else {
        opts.children[k++] = Button(use_camera, "Use camera");
    }
    if (s->cam == Z_PERM_GRANTED) {
        opts.children[k++] = Background(z_rgba(0x1d, 0x9e, 0x55, 0xff),
            CornerRadius(16,
                Frame(220.0f, 64.0f,
                    Foreground(Z_COLOR_TEXT_INV,
                        Font(Z_FONT_CALLOUT, Text("Camera ready"))))));
    }
    return z_stack(Z_AXIS_VERTICAL, &opts);
}

static ZView cards_body(ZApp *app, CardsState *state) {
    // Query the broker once at startup so the first frame reflects reality
    // without a socket round-trip on every rebuild.
    if (!state->cam_queried) {
        state->cam_queried = true;
        state->cam = z_perm_status("camera");
    }

    return Background(z_rgba(0x10, 0x2a, 0x44, 0xff),
        VStack(
            lifecycle_banner(app),
            Foreground(Z_COLOR_TEXT_INV,
                Font(Z_FONT_LARGE_TITLE, Text("Cards"))),
            Frame(140.0f, 140.0f,
                Background(z_rgba(0x2e, 0x9b, 0xff, 0xff),
                    CornerRadius(24,
                        Foreground(Z_COLOR_TEXT_INV,
                            Font(Z_FONT_LARGE_TITLE,
                                Text("%d", state->count)))))),
            Button(bump, "Tap me"),
            camera_section(app, state),
            Spacer(),
            .padding = 28, .spacing = 18, .align = Z_ALIGN_CENTER));
}

Z_APP_ID(CardsState, cards_body, "os.zelto.cards")
