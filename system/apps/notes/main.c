// Zelto demo app — "Notes" (the intent target).
//
// The receiving side of the app-to-app intents broker (P9). It declares in its
// manifest that it accepts text/plain shares (`share_targets=text/plain`) and
// opens "zelto" deep links (`links=zelto`). It registers the two handlers and
// shows whatever was last delivered:
//   - z_on_share_target: another app shared text to us (via the share sheet).
//   - z_on_open_url: a "zelto://" link was opened on us.
// Either intent may have launched this app to handle it; libzelto queues a
// delivery that arrives before the handler is registered and dispatches it the
// moment z_on_share_target / z_on_open_url is called, so the first frame already
// shows the payload. Maps a plain xdg_toplevel below the bar.
#include <stdio.h>
#include <string.h>

#include <zelto/ui.h>

typedef struct NotesState {
    bool registered;        // have we wired the intent handlers yet?
    bool got_share;
    char shared[256];       // last shared text
    bool got_url;
    char url[256];          // last opened deep link
} NotesState;

// Incoming share: copy the first text item into our state and repaint.
static void on_share(ZApp *app, const ZShareItem *items, int count, void *ud) {
    NotesState *s = ud;
    if (count > 0 && items[0].text) {
        snprintf(s->shared, sizeof(s->shared), "%s", items[0].text);
        s->got_share = true;
    }
    z_invalidate(app);
}

// Incoming deep link: copy the URL into our state and repaint.
static void on_url(ZApp *app, const char *url, void *ud) {
    NotesState *s = ud;
    snprintf(s->url, sizeof(s->url), "%s", url ? url : "");
    s->got_url = true;
    z_invalidate(app);
}

// A delivered-payload panel: green when something arrived, muted placeholder
// otherwise. Distinct colour so a capture proves the intent reached us.
static ZView payload_panel(const char *label, const char *value, bool got) {
    ZColor bg = got ? z_rgba(0x1d, 0x6e, 0x44, 0xff)
                    : z_rgba(0x2a, 0x30, 0x38, 0xff);
    return Background(bg,
        CornerRadius(14,
            Frame(520.0f, 96.0f,
                VStack(
                    Foreground(z_rgba(0xbf, 0xd8, 0xcc, 0xff),
                        Font(Z_FONT_CAPTION, Text("%s", label))),
                    Foreground(Z_COLOR_TEXT_INV,
                        Font(Z_FONT_CALLOUT,
                            Text("%s", got ? value : "(nothing yet)"))),
                    .spacing = 8, .align = Z_ALIGN_LEADING, .padding = 16))));
}

static ZView notes_body(ZApp *app, NotesState *state) {
    // Register the intent handlers on the first build. Doing it here (rather than
    // before the loop, where no ZApp exists yet) is fine: libzelto holds any
    // intent delivered in the meantime and flushes it into the handler now.
    if (!state->registered) {
        state->registered = true;
        z_on_share_target(app, on_share, state);
        z_on_open_url(app, on_url, state);
    }

    return Background(z_rgba(0x12, 0x20, 0x18, 0xff),
        VStack(
            Foreground(Z_COLOR_TEXT_INV,
                Font(Z_FONT_LARGE_TITLE, Text("Notes"))),
            Foreground(z_rgba(0xa8, 0xc8, 0xb4, 0xff),
                Font(Z_FONT_CALLOUT, Text("Receives shares + deep links"))),
            Spacer(),
            payload_panel("Shared text", state->shared, state->got_share),
            payload_panel("Opened link", state->url, state->got_url),
            Spacer(),
            .padding = 32, .spacing = 18, .align = Z_ALIGN_CENTER));
}

Z_APP_ID(NotesState, notes_body, "os.zelto.notes")
