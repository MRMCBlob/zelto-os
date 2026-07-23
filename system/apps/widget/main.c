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
    return Background(Z_COLOR_BG,
        VStack(
            Spacer(),
            Foreground(Z_COLOR_TEXT_INV,
                Font(Z_FONT_LARGE_TITLE, Text("Widget"))),
            Foreground(Z_COLOR_TEXT_MUTED,
                Font(Z_FONT_CALLOUT,
                     Text("Installed at runtime from a signed .zap"))),
            Spacer(),
            Background(Z_COLOR_PRIMARY,
                Button(bump, "Tap me")),
            Foreground(Z_COLOR_TEXT_MUTED,
                Font(Z_FONT_CAPTION, Text("taps: %d", state->taps))),
            Spacer(),
            .padding = Z_SPACE_L, .spacing = Z_SPACE_M, .align = Z_ALIGN_CENTER));
}

Z_APP_ID(WidgetState, widget_body, "os.zelto.widget")
