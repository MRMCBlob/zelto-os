// Zelto demo app — "Share" (the intent source).
//
// Exercises the app-to-app intents broker (P9) from the sending side:
//   - "Share text" calls z_share() with a text/plain item. zsysd resolves the
//     apps that accept text/* (the Notes app), shows the System-UI share sheet,
//     and delivers the payload to the pick — which fires its z_on_share_target.
//   - "Open note link" calls z_open_url("zelto://note/42"). zsysd resolves the
//     "zelto" scheme to its single handler (Notes) and delivers it directly (no
//     sheet), firing the handler's z_on_open_url.
// This app declares no handlers itself — it is purely a source. Maps a plain
// xdg_toplevel; the compositor sizes it below the bar.
#include <zelto/ui.h>

typedef struct ShareState {
    int shares;   // how many times we've shared (just for visible feedback)
} ShareState;

static void share_text(ZApp *app, void *state) {
    ShareState *s = state;
    ZShareItem item = {.mime = "text/plain", .text = "Hello from Share!"};
    z_share(&item, 1);
    s->shares++;
    z_invalidate(app);
}

static void open_link(ZApp *app, void *state) {
    (void)state;
    z_open_url("zelto://note/42");
    z_invalidate(app);
}

static ZView share_body(ZApp *app, ShareState *state) {
    (void)app;
    return Background(Z_COLOR_BG,
        VStack(
            Foreground(Z_COLOR_TEXT_INV,
                Font(Z_FONT_LARGE_TITLE, Text("Share"))),
            Foreground(Z_COLOR_TEXT_MUTED,
                Font(Z_FONT_CALLOUT,
                    Text("Hand content to another app"))),
            Spacer(),
            Background(Z_COLOR_PRIMARY,
                Button(share_text, "Share text")),
            Background(Z_COLOR_SURFACE_3,
                Button(open_link, "Open note link")),
            Foreground(Z_COLOR_TEXT_MUTED,
                Font(Z_FONT_CAPTION,
                    Text("shared %d time(s)", state->shares))),
            Spacer(),
            .padding = Z_SPACE_L, .spacing = Z_SPACE_M, .align = Z_ALIGN_CENTER));
}

Z_APP_ID(ShareState, share_body, "os.zelto.share")
