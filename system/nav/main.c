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
#include <stdlib.h>
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
// The binary is /usr/bin/zelto-recents on the device; the desktop simulator runs
// uninstalled build-host binaries, so it points ZELTO_RECENTS_BIN at the actual
// build path (same env-override idiom as ZELTO_DATA_DIR/ZELTO_FONT).
static void on_recents(ZApp *app, void *state) {
    (void)app;
    (void)state;
    const char *bin = getenv("ZELTO_RECENTS_BIN");
    spawn(bin && *bin ? bin : RECENTS_BIN);
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

// The three Android-style nav marks, drawn as crisp vector strokes (a chevron, a
// circle, a square) via the toolkit's Stroke primitive rather than ASCII glyphs —
// proper marks that rectangles/text can't render. Points are in the unit box.
#define NAV_GLYPH 30.0f
#define NAV_STROKE 3.0f

static ZView icon_back(void) {
    static const float pts[] = {0.60f, 0.16f, 0.30f, 0.50f, 0.60f, 0.84f};
    return Frame(NAV_GLYPH, NAV_GLYPH,
        Stroke(.points = pts, .count = 3, .thickness = NAV_STROKE,
               .color = Z_COLOR_TEXT));
}
static ZView icon_home(void) {
    // A 12-gon approximating a circle (r=0.40 about the centre), closed.
    static const float pts[] = {
        0.90f, 0.50f, 0.85f, 0.70f, 0.70f, 0.85f, 0.50f, 0.90f,
        0.30f, 0.85f, 0.15f, 0.70f, 0.10f, 0.50f, 0.15f, 0.30f,
        0.30f, 0.15f, 0.50f, 0.10f, 0.70f, 0.15f, 0.85f, 0.30f};
    return Frame(NAV_GLYPH, NAV_GLYPH,
        Stroke(.points = pts, .count = 12, .thickness = NAV_STROKE,
               .color = Z_COLOR_TEXT, .closed = true));
}
static ZView icon_recents(void) {
    static const float pts[] = {0.24f, 0.24f, 0.76f, 0.24f, 0.76f, 0.76f,
                                0.24f, 0.76f};
    return Frame(NAV_GLYPH, NAV_GLYPH,
        Stroke(.points = pts, .count = 4, .thickness = NAV_STROKE,
               .color = Z_COLOR_TEXT, .closed = true));
}

// One nav button: a vector mark, the whole grown column tappable. Grow(1) so the
// three buttons split the bar width into even thirds — each is a third of the
// screen wide and the full bar tall, which is an enormous, thumb-sized target.
//
// The captions are gone. A back chevron, a circle and a square are the three most
// over-learned marks on any phone, and printing "Back" under an arrow is the kind
// of belt-and-braces labelling that makes a system look unsure of itself. Losing
// them also lets the marks grow and the bar breathe.
static ZView nav_button(ZView icon, ZAction act) {
    return Grow(1.0f,
        OnTap(act,
            VStack(icon, .spacing = 0, .align = Z_ALIGN_CENTER)));
}

static ZView nav_body(ZApp *app, NavState *s) {
    (void)s;
    // The nav bar is a MATERIAL: the compositor blurs whatever the bar sits over
    // (the home's wallpaper, or the app you are in) and hands it back to us, and we
    // tint it. Square corners — it is flush to the bottom edge of the screen.
    int w = z_app_width(app);
    z_backdrop(app, 0.0f, 0.0f, (float)w, (float)NAV_H, 0.0f);
    z_full_repaint(app);   // translucent surface: nothing erases stale pixels

    return Background(Z_COLOR_MATERIAL_THIN,
        HStack(
            nav_button(icon_back(), on_back),
            nav_button(icon_home(), on_home),
            nav_button(icon_recents(), on_recents),
            .padding = 6, .spacing = 0, .align = Z_ALIGN_CENTER));
}

Z_LAYER_APP(NavState, nav_body,
            .layer = Z_LAYER_TOP,
            .anchor = Z_ANCHOR_BOTTOM | Z_ANCHOR_LEFT | Z_ANCHOR_RIGHT,
            .exclusive_zone = NAV_H,
            .height = NAV_H)
