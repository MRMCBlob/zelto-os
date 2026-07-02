// Zelto System UI — bottom navigation bar (Android-style 3-button nav).
//
// A wlr-layer-shell client anchored to the BOTTOM edge in the TOP layer with an
// exclusive zone, the mirror of the status bar: it reserves a strip the
// compositor keeps clear, so the launcher and every app shrink to sit between
// the top bar and this bar (P6 usable-area math handles a bottom exclusive zone
// the same as a top one). Always-running like the bar/shade; started by init.
//
// Three big tap targets drive the EXISTING window manager — no new protocol:
//   - Back    : activate the previous app (MVP: the most-recent running window
//               that is not the foreground and not the home launcher), via the
//               wlr-foreign-toplevel-management activate path (P7).
//   - Home    : activate the launcher toplevel (app_id os.zelto.launcher) — the
//               same effect as the compositor's Home chord, but driven from here.
//   - Recents : fork/exec the zelto-recents overlay (the window/task overview).
// See docs/contributing/compositor-internals.md + docs/platform/app-lifecycle.md.
#include <string.h>
#include <unistd.h>

#include <zelto/ui.h>

#define NAV_H 64
#define LAUNCHER_APP_ID "os.zelto.launcher"
#define RECENTS_BIN "/usr/bin/zelto-recents"

typedef struct NavState {
    int unused;
} NavState;

// fork/exec a System-UI helper, detached so it outlives this bar.
static void spawn(const char *path) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execlp(path, path, (char *)NULL);
        _exit(127);
    }
}

// Home: find the launcher's toplevel in the foreign-toplevel list and activate
// it (raises the home grid to the front, pausing the previous app).
static void on_home(ZApp *app, void *state) {
    (void)state;
    int n = 0;
    const ZTask *t = z_running_apps(app, &n);
    for (int i = 0; i < n; i++) {
        if (t[i].app_id && strcmp(t[i].app_id, LAUNCHER_APP_ID) == 0) {
            z_task_activate(app, &t[i]);
            return;
        }
    }
}

// Recents: open the task-overview overlay (it reuses z_running_apps wholesale).
static void on_recents(ZApp *app, void *state) {
    (void)app;
    (void)state;
    spawn(RECENTS_BIN);
}

// Back (MVP "back to last app"): activate the first running window that is not
// the current foreground and not the home launcher — i.e. the previously-used
// app. If nothing is behind us, fall back to Home. True in-app Back (synthesising
// Escape to the focused toplevel, which P5 apps handle via z_nav_pop) needs a
// small compositor request and is left as a documented stretch.
static void on_back(ZApp *app, void *state) {
    (void)state;
    int n = 0;
    const ZTask *t = z_running_apps(app, &n);
    const ZTask *target = NULL;
    for (int i = 0; i < n; i++) {
        if (t[i].active) {
            continue;
        }
        if (t[i].app_id && strcmp(t[i].app_id, LAUNCHER_APP_ID) == 0) {
            continue;
        }
        target = &t[i];
        break;
    }
    if (!target) {
        for (int i = 0; i < n; i++) {
            if (t[i].app_id && strcmp(t[i].app_id, LAUNCHER_APP_ID) == 0) {
                target = &t[i];
                break;
            }
        }
    }
    if (target) {
        z_task_activate(app, target);
    }
}

// One nav button: a glyph over a caption, the whole grown column tappable. Grow(1)
// so the three buttons split the bar width into even thirds.
static ZView nav_button(const char *glyph, const char *label, ZAction act) {
    return Grow(1.0f,
        OnTap(act,
            VStack(
                Foreground(Z_COLOR_TEXT_INV,
                    Font(Z_FONT_TITLE, Text("%s", glyph))),
                Foreground(z_rgba(0x9a, 0xa4, 0xad, 0xff),
                    Font(Z_FONT_CAPTION, Text("%s", label))),
                .spacing = 2, .align = Z_ALIGN_CENTER)));
}

static ZView nav_body(ZApp *app, NavState *s) {
    (void)app;
    (void)s;
    return Background(z_rgba(0x10, 0x14, 0x1a, 0xff),
        HStack(
            nav_button("<", "Back", on_back),
            nav_button("O", "Home", on_home),
            nav_button("[]", "Recents", on_recents),
            .padding = 6, .spacing = 0, .align = Z_ALIGN_CENTER));
}

Z_LAYER_APP(NavState, nav_body,
            .layer = Z_LAYER_TOP,
            .anchor = Z_ANCHOR_BOTTOM | Z_ANCHOR_LEFT | Z_ANCHOR_RIGHT,
            .exclusive_zone = NAV_H,
            .height = NAV_H)
