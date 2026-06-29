// Zelto sample app #2 — "Cards".
//
// A deliberately small, visually distinct libzelto app (a blue scene with a tap
// counter) used as the second launchable app in the System UI demo, so a frame
// capture can show switching between two different client surfaces. Maps a plain
// xdg_toplevel; the compositor sizes it to the area below the status bar.
#include <zelto/ui.h>

typedef struct CardsState {
    int count;
} CardsState;

static void bump(ZApp *app, void *state) {
    CardsState *s = state;
    s->count++;
    z_invalidate(app);
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

static ZView cards_body(ZApp *app, CardsState *state) {
    return Background(z_rgba(0x10, 0x2a, 0x44, 0xff),
        VStack(
            lifecycle_banner(app),
            Foreground(Z_COLOR_TEXT_INV,
                Font(Z_FONT_LARGE_TITLE, Text("Cards"))),
            Foreground(z_rgba(0x9a, 0xc4, 0xf0, 0xff),
                Text("A second app, launched from the tiles.")),
            Frame(180.0f, 180.0f,
                Background(z_rgba(0x2e, 0x9b, 0xff, 0xff),
                    CornerRadius(24,
                        Foreground(Z_COLOR_TEXT_INV,
                            Font(Z_FONT_LARGE_TITLE,
                                Text("%d", state->count)))))),
            Button(bump, "Tap me"),
            Spacer(),
            .padding = 32, .spacing = 24, .align = Z_ALIGN_CENTER));
}

Z_APP_ID(CardsState, cards_body, "os.zelto.cards")
