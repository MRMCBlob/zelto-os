// Zelto System UI — app chooser (the share sheet).
//
// A modal overlay shown by zsysd when an intent (a share, or a multi-handler
// deep link) has more than the obvious target. It mirrors the consent dialog:
// a libzelto OVERLAY layer-shell app, stretched over the whole output as a dim
// backdrop with a centred card, requesting EXCLUSIVE keyboard so it is a real
// modal. Pointer taps reach it because the overlay composites above every app.
//
// zsysd fork/execs it with argv = the candidate app_ids (one per row). The user
// taps a row; the process exits with that row's 1-based index, which zsysd maps
// back to the chosen app_id. A Cancel row (or no pick) exits 0. The exit code is
// the whole IPC — no result socket, exactly like the consent dialog.
// See docs/platform/ipc-and-intents.md.
#include <stdint.h>
#include <stdlib.h>

#include <zelto/ui.h>

#define MAX_CANDIDATES 24

typedef struct ChooserState {
    char **ids;   // candidate app_ids (argv tail)
    int n;        // candidate count
} ChooserState;

// A row resolves the sheet by the process exit code zsysd reads: its 1-based
// index. The index is carried as the per-row data pointer (boxed as an int).
static void on_pick(ZApp *app, void *state, void *data) {
    (void)app;
    (void)state;
    exit((int)(intptr_t)data);
}
static void on_cancel(ZApp *app, void *state) {
    (void)app;
    (void)state;
    exit(0);
}

// One candidate row: a tappable bar showing the app_id, tinted like a list item.
static ZView candidate_row(const char *app_id, int index) {
    return OnTapData(on_pick, (void *)(intptr_t)index,
        Background(z_rgba(0x26, 0x2f, 0x3a, 0xff),
            CornerRadius(12,
                HStack(
                    Frame(40.0f, 40.0f,
                        Rect(.color = z_rgba(0x2e, 0x9b, 0xff, 0xff),
                             .radius = 10)),
                    Foreground(Z_COLOR_TEXT_INV,
                        Font(Z_FONT_CALLOUT, Text("%s", app_id))),
                    Spacer(),
                    .padding = 14, .spacing = 14, .align = Z_ALIGN_CENTER))));
}

static ZView chooser_body(ZApp *app, ChooserState *s) {
    (void)app;

    ZStackOpts card_opts = {.padding = 24, .spacing = 14,
                            .align = Z_ALIGN_LEADING};
    int k = 0;
    card_opts.children[k++] = Foreground(Z_COLOR_TEXT_INV,
        Font(Z_FONT_TITLE, Text("Share via")));
    for (int i = 0; i < s->n && k < Z_MAX_CHILDREN - 2; i++) {
        card_opts.children[k++] = candidate_row(s->ids[i], i + 1);
    }
    card_opts.children[k++] = Background(z_rgba(0x3a, 0x42, 0x4c, 0xff),
        Button(on_cancel, "Cancel"));

    // Card height grows with the candidate count.
    float card_h = 150.0f + 64.0f * (float)(s->n + 1);
    ZView card = Background(z_rgba(0x1a, 0x20, 0x28, 0xff),
        CornerRadius(20,
            Frame(560.0f, card_h,
                z_stack(Z_AXIS_VERTICAL, &card_opts))));

    // Dim full-screen backdrop with the card centred in it.
    return Background(z_rgba(0x00, 0x00, 0x00, 0xb0),
        VStack(
            Spacer(),
            HStack(Spacer(), card, Spacer(), .align = Z_ALIGN_CENTER),
            Spacer(),
            .align = Z_ALIGN_CENTER));
}

static ZView body_tr(ZApp *app, void *state) {
    return chooser_body(app, (ChooserState *)state);
}

int main(int argc, char **argv) {
    static ChooserState s;
    s.ids = &argv[1];
    s.n = argc - 1;
    if (s.n > MAX_CANDIDATES) {
        s.n = MAX_CANDIDATES;
    }

    ZLayerOpts opts = {
        .layer = Z_LAYER_OVERLAY,
        .anchor = Z_ANCHOR_TOP | Z_ANCHOR_BOTTOM | Z_ANCHOR_LEFT | Z_ANCHOR_RIGHT,
        .exclusive_zone = 0,
        .width = 0,
        .height = 0,
        .keyboard = true,
    };
    return z_layer_app_main(&s, body_tr, "Chooser", &opts);
}
