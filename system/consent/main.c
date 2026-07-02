// Zelto System UI — permission consent dialog.
//
// A modal overlay shown by zsysd when an app requests a permission that has no
// cached decision. It is a libzelto layer-shell app in the OVERLAY layer,
// stretched over the whole output (a dim backdrop) with a centred card and
// Allow / Deny buttons. It requests EXCLUSIVE keyboard interactivity so the
// compositor routes the keyboard to it (a real modal); pointer taps reach it
// because the overlay composites on top of every app and the bar.
//
// zsysd fork/execs it with argv = <app_id> <perm> and waits for it: the process
// exits 0 for Allow, 1 for Deny, and exiting tears down the surface (the dialog
// dismisses). See docs/platform/permissions.md.
#include <stdlib.h>

#include <zelto/ui.h>

typedef struct ConsentState {
    const char *app_id;
    const char *perm;
} ConsentState;

// Buttons resolve the dialog by the process exit code zsysd reads.
static void on_allow(ZApp *app, void *state) {
    (void)app;
    (void)state;
    exit(0);
}
static void on_deny(ZApp *app, void *state) {
    (void)app;
    (void)state;
    exit(1);
}

static ZView consent_body(ZApp *app, ConsentState *s) {
    (void)app;
    // The modal card: title, the "<app> wants to use the <perm>" line, and the
    // Deny / Allow actions (tinted via Background over the default button fill).
    ZView card = Background(Z_COLOR_SURFACE,
        CornerRadius(20,
            Frame(580.0f, 300.0f,
                VStack(
                    Foreground(Z_COLOR_TEXT_INV,
                        Font(Z_FONT_TITLE, Text("Permission request"))),
                    Foreground(Z_COLOR_TEXT_MUTED,
                        Font(Z_FONT_CALLOUT,
                            Text("\"%s\" wants to use the %s", s->app_id,
                                 s->perm))),
                    Spacer(),
                    HStack(
                        Background(Z_COLOR_SURFACE_3,
                            Button(on_deny, "Deny")),
                        Spacer(),
                        Background(Z_COLOR_SUCCESS,
                            Button(on_allow, "Allow")),
                        .spacing = 16, .align = Z_ALIGN_CENTER),
                    .padding = 28, .spacing = 18, .align = Z_ALIGN_LEADING))));

    // Dim full-screen backdrop with the card centred in it.
    return Background(Z_COLOR_SCRIM,
        VStack(
            Spacer(),
            HStack(Spacer(), card, Spacer(), .align = Z_ALIGN_CENTER),
            Spacer(),
            .align = Z_ALIGN_CENTER));
}

static ZView body_tr(ZApp *app, void *state) {
    return consent_body(app, (ConsentState *)state);
}

int main(int argc, char **argv) {
    static ConsentState s;
    s.app_id = argc > 1 ? argv[1] : "An app";
    s.perm = argc > 2 ? argv[2] : "camera";

    ZLayerOpts opts = {
        .layer = Z_LAYER_OVERLAY,
        .anchor = Z_ANCHOR_TOP | Z_ANCHOR_BOTTOM | Z_ANCHOR_LEFT | Z_ANCHOR_RIGHT,
        .exclusive_zone = 0,
        .width = 0,
        .height = 0,
        .keyboard = true,
    };
    return z_layer_app_main(&s, body_tr, "Permission", &opts);
}
