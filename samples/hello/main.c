// samples/hello - the smallest interactive native Zelto app. A declarative view
// tree (background + title + a Button + state-driven boxes) that responds to
// input: tapping the button (pointer or Enter/Space) bumps a counter, any key
// press bumps another, and z_invalidate rebuilds the UI so the change shows.
// libzelto hit-tests/routes the events; zcomp composites the surface.
// See docs/getting-started/hello-world.md and docs/guides/state-management.md.
#include <zelto/ui.h>

// App state: two counters the UI is derived from. Mutated by the input handlers
// below, which then call z_invalidate to schedule a rebuild.
typedef struct HelloState {
    int taps;
    int keys;
} HelloState;

// Input handlers: ordinary named ZAction/ZKeyAction functions. Each mutates the
// persistent state struct and calls z_invalidate to schedule a rebuild.
static void on_tap(ZApp *app, void *state) {
    HelloState *s = state;
    s->taps++;
    z_invalidate(app);
}

static void on_key(ZApp *app, void *state, uint32_t keysym) {
    (void)keysym;
    HelloState *s = state;
    s->keys++;
    z_invalidate(app);
}

static ZView body(ZApp *app, HelloState *state) {
    (void)app;

    // Tap parity drives a visible colour swap so a single tap is unmistakable.
    ZColor swatch = (state->taps % 2 == 0) ? Z_COLOR_ACCENT : Z_COLOR_PRIMARY;

    return Background(z_rgba(0x12, 0x16, 0x1c, 0xff),
        VStack(
            Font(Z_FONT_TITLE,
                 Foreground(Z_COLOR_TEXT_INV, Text("Hello, Zelto OS"))),

            // The interactive control. on_tap bumps taps; OnKey makes it the
            // keyboard focus target (focus ring) and bumps keys on any press.
            OnKey(on_key,
                Button(on_tap, "Tap me  (taps: %d)", state->taps)),

            // A state-driven box: colour follows tap parity, width follows keys.
            Frame(140.0f + (float)(state->keys % 6) * 40.0f, 96.0f,
                  Rect(.color = swatch, .radius = 16)),

            Foreground(z_rgba(0x9a, 0xa4, 0xad, 0xff),
                       Text("keys pressed: %d", state->keys)),

            Spacer(),
            Foreground(z_rgba(0x9a, 0xa4, 0xad, 0xff),
                       Text("tap the button or press any key")),

            .padding = 48, .spacing = 28, .align = Z_ALIGN_CENTER));
}

Z_APP(HelloState, body)
