// Zelto demo app — "Widget" (the P13 packaging demo).
//
// A deliberately trivial libzelto app. It is NOT baked into the image as an
// installed app: there is no /usr/bin/zelto-widget and no
// /usr/share/zelto/apps/zelto-widget.app. Instead it is cross-built, packaged
// into a signed `widget.zap` (meta/mkzap.sh), and staged onto the image as a raw
// package file. At runtime `zelto-install` verifies that .zap and unpacks it onto
// the persistent disk under /var/zelto, after which the launcher shows a Widget
// tile and tapping it runs THIS binary from its installed path.
//
// So if you can see this app on screen, the whole install pipeline worked:
// signature + per-file hash verification, unpack to /var/zelto/installed, and a
// runtime-registered manifest the launcher picked up. See docs/packaging/*.
#include <zelto/ui.h>

typedef struct WidgetState {
    int taps;
} WidgetState;

static void bump(ZApp *app, void *state) {
    WidgetState *s = state;
    s->taps++;
    z_invalidate(app);
}

static ZView widget_body(ZApp *app, WidgetState *state) {
    (void)app;
    return Background(z_rgba(0x16, 0x2a, 0x1e, 0xff),
        VStack(
            Spacer(),
            Foreground(Z_COLOR_TEXT_INV,
                Font(Z_FONT_LARGE_TITLE, Text("Widget"))),
            Foreground(z_rgba(0x9f, 0xe6, 0xb8, 0xff),
                Font(Z_FONT_CALLOUT,
                     Text("Installed at runtime from a signed .zap"))),
            Spacer(),
            Background(z_rgba(0x2e, 0xb6, 0x6e, 0xff),
                Button(bump, "Tap me")),
            Foreground(z_rgba(0xbf, 0xe8, 0xcf, 0xff),
                Font(Z_FONT_CAPTION, Text("taps: %d", state->taps))),
            Spacer(),
            .padding = 32, .spacing = 18, .align = Z_ALIGN_CENTER));
}

Z_APP_ID(WidgetState, widget_body, "os.zelto.widget")
