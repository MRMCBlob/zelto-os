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
    ZTextField field;       // paste target (P22): copy in Notepad, switch here,
                            // paste — the clipboard crosses the process boundary
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
    ZColor bg = got ? Z_COLOR_SUCCESS_DIM
                    : Z_COLOR_SURFACE_2;
    return Background(bg,
        CornerRadius(14,
            Frame(520.0f, 96.0f,
                VStack(
                    Foreground(Z_COLOR_TEXT_MUTED,
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

    // TOP-anchored content + an INLINE fixed-height action-bar slot below the
    // shade's grab strip — same structure as Notepad (an overlay's buttons proved
    // un-hittable).
    ZView bar = z_selection_bar(app);
    ZStackOpts col = {.padding = 28, .spacing = 14, .align = Z_ALIGN_CENTER};
    int k = 0;
    col.children[k++] = Rect(.height = 84.0f);   // clears the shade grab strip
    col.children[k++] =
        Frame(0.0f, 52.0f, bar ? bar : z_rect(&(ZRectOpts){.height = 1.0f}));
    col.children[k++] =
        Foreground(Z_COLOR_TEXT_INV, Font(Z_FONT_TITLE, Text("Notes")));
    // Paste target: focus this field and paste text copied in ANOTHER app — proof
    // the clipboard crosses the process boundary (P22).
    col.children[k++] = Frame(520.0f, 0.0f,
        TextField(app, &state->field, "Paste copied text here..."));
    col.children[k++] = payload_panel("Shared text", state->shared,
                                      state->got_share);
    col.children[k++] = payload_panel("Opened link", state->url,
                                      state->got_url);
    col.children[k++] = Spacer();

    return Background(Z_COLOR_BG,
        z_stack(Z_AXIS_VERTICAL, &col));
}

Z_APP_ID(NotesState, notes_body, "os.zelto.notes")
