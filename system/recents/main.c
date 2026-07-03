// Zelto System UI — Recents (task overview).
//
// The "window view" reached from the bottom nav bar's Recents button. An
// ordinary libzelto OVERLAY layer-shell modal (like the consent/chooser
// overlays): a dim full-screen backdrop with a centred card listing every
// running app window from z_running_apps() (wlr-foreign-toplevel-management,
// P7). It is the launcher's old "Running" section moved into its own surface and
// reused wholesale — nothing new in the compositor.
//
//   - Tap a card     -> z_task_activate(that window) + dismiss (the overlay quits
//                        so the activated app shows).
//   - Tap its X      -> z_task_close(that window); the overview refreshes and the
//                        card drops out.
//   - Tap the backdrop / Close / Escape -> dismiss without switching.
//
// The home launcher is filtered out of the list (it is reachable via the Home
// button), so Recents shows only app windows, like Android's overview.
// See docs/platform/app-lifecycle.md.
#include <stdint.h>
#include <string.h>

#include <zelto/ui.h>

#include "common/app_icons.h"

#define LAUNCHER_APP_ID "os.zelto.launcher"
#define XKB_KEY_Escape 0xff1b

typedef struct RecentsState {
    int unused;
} RecentsState;

// Tap a card: switch to that window, then close the overlay so it appears.
static void on_pick(ZApp *app, void *state, void *data) {
    (void)state;
    z_task_activate(app, (const ZTask *)data);
    z_app_quit(app);
}

// Tap a card's X: close that window. Stay open and repaint so the overview
// updates (the closed window's card drops out of z_running_apps next build).
static void on_close(ZApp *app, void *state, void *data) {
    (void)state;
    z_task_close(app, (const ZTask *)data);
    z_invalidate(app);
}

// Dismiss without switching (backdrop / Close button).
static void on_dismiss(ZApp *app, void *state) {
    (void)state;
    z_app_quit(app);
}

// Escape also dismisses (this is a keyboard-exclusive modal).
static void on_key(ZApp *app, void *state, uint32_t keysym) {
    (void)state;
    if (keysym == XKB_KEY_Escape) {
        z_app_quit(app);
    }
}

// The app's icon for a running window (resolved from its manifest by app_id),
// or the shared placeholder — a small rounded tile at the leading edge of the
// card. Emblems sit on a neutral tile (Recents has no per-app colour).
static ZView task_icon(const ZTask *t) {
    char buf[192];
    const char *icon = (zelto_icon_for_app_id(t->app_id, buf, sizeof(buf)) &&
                        z_image_loads(buf))
                           ? buf
                           : zelto_placeholder_icon();
    return Frame(52.0f, 52.0f,
        Background(Z_COLOR_SURFACE_3,
            CornerRadius(12,
                ZStack(Frame(40.0f, 40.0f, Image(icon)),
                       .align = Z_ALIGN_CENTER))));
}

// One task card: icon + title + Active/Paused, with a red X close button nested
// deeper (deepest hit-test handler wins, so the X closes and the rest of the
// card switches). The data pointer is the ZTask in the stable z_running_apps
// snapshot.
static ZView task_card(const ZTask *t) {
    // Prefer the manifest display name ("Fetch") over the window title, which for
    // a libzelto app is its body-function symbol ("fetch_body"); fall back to the
    // title, then the app_id.
    char name[128];
    const char *title =
        (t->app_id && zelto_name_for_app_id(t->app_id, name, sizeof(name)))
            ? name
            : (t->title ? t->title : (t->app_id ? t->app_id : "App"));
    ZColor bg = t->active ? Z_COLOR_SUCCESS_DIM
                          : Z_COLOR_SURFACE_2;
    return OnTapData(on_pick, (void *)t,
        Background(bg,
            CornerRadius(16,
                HStack(
                    task_icon(t),
                    VStack(
                        Foreground(Z_COLOR_TEXT_INV,
                            Font(Z_FONT_CALLOUT, Text("%s", title))),
                        Foreground(Z_COLOR_TEXT_MUTED,
                            Text("%s", t->active ? "Active" : "Paused")),
                        .spacing = 4, .align = Z_ALIGN_LEADING),
                    Spacer(),
                    OnTapData(on_close, (void *)t,
                        Background(Z_COLOR_DANGER,
                            Frame(56.0f, 56.0f,
                                CornerRadius(12,
                                    Foreground(Z_COLOR_TEXT_INV,
                                        Font(Z_FONT_TITLE, Text("X"))))))),
                    .padding = 16, .spacing = 14, .align = Z_ALIGN_CENTER))));
}

static ZView recents_body(ZApp *app, RecentsState *s) {
    (void)s;
    int n = 0;
    const ZTask *t = z_running_apps(app, &n);

    ZStackOpts card_opts = {.padding = 24, .spacing = 14,
                            .align = Z_ALIGN_LEADING};
    int k = 0;
    card_opts.children[k++] = Foreground(Z_COLOR_TEXT_INV,
        Font(Z_FONT_TITLE, Text("Recents")));

    int shown = 0;
    for (int i = 0; i < n && k < Z_MAX_CHILDREN - 2; i++) {
        if (t[i].app_id && strcmp(t[i].app_id, LAUNCHER_APP_ID) == 0) {
            continue;   // home is reachable via the Home button, not Recents
        }
        card_opts.children[k++] = task_card(&t[i]);
        shown++;
    }
    if (shown == 0) {
        card_opts.children[k++] = Foreground(Z_COLOR_TEXT_MUTED,
            Text("No running apps"));
    }
    card_opts.children[k++] = Background(Z_COLOR_SURFACE_3,
        Button(on_dismiss, "Close"));

    float card_h = 150.0f + 96.0f * (float)(shown + 1);
    ZView card = Background(Z_COLOR_SURFACE,
        CornerRadius(20,
            Frame(620.0f, card_h,
                z_stack(Z_AXIS_VERTICAL, &card_opts))));

    // Dim full-screen backdrop (tap to dismiss) with the card centred and the
    // whole surface a keyboard target for Escape.
    return OnTap(on_dismiss,
        OnKey(on_key,
            Background(Z_COLOR_SCRIM,
                VStack(
                    Spacer(),
                    HStack(Spacer(), card, Spacer(), .align = Z_ALIGN_CENTER),
                    Spacer(),
                    .align = Z_ALIGN_CENTER))));
}

Z_LAYER_APP(RecentsState, recents_body,
            .layer = Z_LAYER_OVERLAY,
            .anchor = Z_ANCHOR_TOP | Z_ANCHOR_BOTTOM | Z_ANCHOR_LEFT |
                      Z_ANCHOR_RIGHT,
            .exclusive_zone = 0,
            .width = 0,
            .height = 0,
            .keyboard = true)
