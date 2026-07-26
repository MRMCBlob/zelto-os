// Zelto System UI — the home indicator: gesture navigation, iOS-style.
//
// This replaces the Android-style three-button bar (Back/Home/Recents) that used
// to own the bottom of the screen. A phone that hands three permanent buttons a
// full 64px strip is spending its scarcest real estate on chrome; the iOS answer
// is to spend it on a 5px pill and put the navigation in the gestures that start
// from it. The pill is not a button — it is an AFFORDANCE, a hint that the bottom
// edge is grabbable, which is why it has no press state and no tap action.
//
// A wlr-layer-shell client anchored to the BOTTOM edge in the TOP layer with an
// exclusive zone, the mirror of the status bar (P6 usable-area math handles a
// bottom exclusive zone the same as a top one). The zone is ZELTO_HOMEBAR_H —
// Apple's 34pt bottom safe-area inset, converted (P44: it used to be 34 raw
// units, i.e. the point value spent as pixels) — so apps stop above the pill
// instead of drawing under it. Always-running like the bar/shade;
// started by init in the slot the nav bar used to hold.
//
// The gestures, all read off ONE upward pan from the strip:
//   - flick up fast      : Home  (activate the launcher toplevel, as the old
//                          Home button did, via wlr-foreign-toplevel activate)
//   - drag up and pause  : App switcher (fork/exec the zelto-recents overlay)
//   - swipe sideways     : switch to the adjacent running app (this is what the
//                          old Back button did — "back to the last app" — moved
//                          onto the gesture iOS uses for it)
//
// Flick-vs-drag is decided on RELEASE VELOCITY, not distance, which is how iOS
// tells the two apart: a decisive throw means "just take me home", while a slow
// drag that comes to rest means "I am looking for something" — the switcher. A
// pure distance rule would send every long fast flick to the switcher instead.
// See docs/contributing/compositor-internals.md + docs/platform/app-lifecycle.md.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <zelto/ui.h>

#include "common/safe_areas.h"
#include "common/wallpaper.h"

// The pill itself. The WIDTH is a fraction of the screen (already in screen
// units, so it does not convert); the other two are Apple's 5pt bar sitting 8pt
// off the bottom edge, and they do — a 5-unit pill in a 62-unit strip is a
// hairline, which is what it looked like when the strip grew.
#define PILL_W_FRAC 0.36f       // pill spans ~a third of the width, as on iOS
#define PILL_H ((float)Z_PT(5))
#define PILL_BOTTOM ((float)Z_PT(8))   // gap under the pill, inside the strip
#define LAUNCHER_APP_ID "os.zelto.launcher"
#define RECENTS_BIN "/usr/bin/zelto-recents"

// Gesture thresholds. SWIPE_MIN keeps a stray graze from navigating; FLICK_V is
// the release speed above which the swipe reads as a throw; HOLD_DIST is how far
// a SLOW drag must travel before it means the switcher rather than home.
#define SWIPE_MIN 40.0f         // px of travel before anything happens
#define FLICK_V (-900.0f)       // px/s upward: faster than this = go home
#define HOLD_DIST (-200.0f)     // px: slow drag at least this far = switcher
#define SIDE_MIN 80.0f          // px of sideways travel = adjacent app

typedef struct HomeBarState {
    ZAnimated *grip;            // 0 idle .. 1 finger down on the strip
} HomeBarState;

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
static void go_home(ZApp *app) {
    int n = 0;
    const ZTask *t = z_running_apps(app, &n);
    for (int i = 0; i < n; i++) {
        if (t[i].app_id && strcmp(t[i].app_id, LAUNCHER_APP_ID) == 0) {
            z_task_activate(app, &t[i]);
            return;
        }
    }
}

// App switcher: open the task-overview overlay. The binary is
// /usr/bin/zelto-recents on the device; the desktop simulator runs uninstalled
// build-host binaries, so it points ZELTO_RECENTS_BIN at the actual build path
// (the same env-override idiom as ZELTO_DATA_DIR/ZELTO_FONT).
static void open_switcher(void) {
    const char *bin = getenv("ZELTO_RECENTS_BIN");
    spawn(bin && *bin ? bin : RECENTS_BIN);
}

// Sideways swipe: step to the app next to this one in the running list, the way
// dragging across the iOS home indicator walks the recently-used stack. The
// launcher is skipped — it is Home, reachable by flicking up, and leaving it in
// the ring would make every second sideways swipe land on the wallpaper. With
// nothing to step to (only the launcher is running) the gesture is inert.
static void switch_adjacent(ZApp *app, bool forward) {
    int n = 0;
    const ZTask *t = z_running_apps(app, &n);

    const ZTask *ring[16];
    int count = 0, active = -1;
    for (int i = 0; i < n && count < (int)(sizeof ring / sizeof ring[0]); i++) {
        if (t[i].app_id && strcmp(t[i].app_id, LAUNCHER_APP_ID) == 0) {
            continue;
        }
        if (t[i].active) {
            active = count;
        }
        ring[count++] = &t[i];
    }
    if (count == 0) {
        return;
    }
    // Coming from Home there is no active entry in the ring, so either direction
    // lands on the most recent app — which is what "swipe back into what I was
    // doing" should do.
    int next = (active < 0) ? 0
                            : (active + (forward ? 1 : count - 1)) % count;
    z_task_activate(app, ring[next]);
}

static void on_pan(ZApp *app, void *state, const ZPanEvent *e) {
    HomeBarState *s = state;
    if (!s->grip) {
        return;
    }
    if (e->phase == Z_PAN_BEGIN) {
        z_animated_spring_with(s->grip, 1.0f, Z_SPRING_PRESS);
        return;
    }
    if (e->phase != Z_PAN_END) {
        return;
    }
    z_animated_spring_with(s->grip, 0.0f, Z_SPRING_PRESS);

    float dx = e->translation_x, dy = e->translation_y;
    if (fabsf(dx) > fabsf(dy) && fabsf(dx) > SIDE_MIN) {
        switch_adjacent(app, dx < 0.0f);   // drag left = forward, as on iOS
        return;
    }
    if (dy > -SWIPE_MIN) {
        return;                            // a graze, not a swipe
    }
    if (e->velocity_y <= FLICK_V) {
        go_home(app);                      // thrown: decisive, go home
        return;
    }
    if (dy <= HOLD_DIST) {
        open_switcher();                   // dragged slowly and far: overview
        return;
    }
    go_home(app);
}

static ZView bar_body(ZApp *app, HomeBarState *s) {
    if (!s->grip) {
        s->grip = z_animated_value(app, 0.0f);
    }
    // Transparent surface over live wallpaper/app pixels: nothing erases the
    // stale ones, so the whole strip repaints every frame.
    z_full_repaint(app);

    float w = (float)z_app_width(app);
    float g = z_animated_get(s->grip);

    // Under the finger the pill draws in a touch tighter and brighter — the only
    // feedback the indicator gives, and enough to say "yes, I have you".
    float pill_w = w * PILL_W_FRAC * (1.0f - 0.10f * g);
    // THE INK COMES FROM THE WALLPAPER (P54), measured over the bottom strip
    // this surface occupies. The pill sits directly on the picture, and a
    // picture does not change when the appearance does — taking Z_COLOR_TEXT
    // here made the indicator dark-on-dark the moment a light appearance
    // existed. Held, it goes to full opacity rather than to a second colour.
    float sh = (float)z_screen_height(app);
    if (sh < 1.0f) {
        sh = 1440.0f;
    }
    // The pill is centred and spans about a third of the width, so the patch
    // it stands on is the middle of the bottom strip, not the whole of it.
    ZColor wink = zelto_wallpaper_ink(
        app, 0.5f - PILL_W_FRAC * 0.5f, 1.0f - (float)ZELTO_HOMEBAR_H / sh,
        0.5f + PILL_W_FRAC * 0.5f, 1.0f);
    ZColor idle = z_fade(wink, 0xd8);
    ZColor held = wink;

    // The pill carries a shadow rather than a plate: the indicator has to stay
    // legible over an arbitrary wallpaper, and a soft dark penumbra does that
    // without putting a visible bar back on the screen we just cleared.
    ZView pill = Shadow(Z_ELEV_1,
        Rect(.color = z_color_lerp(idle, held, g), .width = pill_w,
             .height = PILL_H, .radius = PILL_H * 0.5f));

    // The whole strip is the gesture target, not just the pill — a 5px-tall hit
    // area would be unusable, and on iOS the grab zone is the full inset too.
    return OnPan(on_pan,
        Fill(VStack(
            Spacer(),
            pill,
            Frame(1.0f, PILL_BOTTOM, Rect(.color = z_rgba(0, 0, 0, 0))),
            .spacing = 0, .align = Z_ALIGN_CENTER)));
}

Z_LAYER_APP(HomeBarState, bar_body,
            .layer = Z_LAYER_TOP,
            .anchor = Z_ANCHOR_BOTTOM | Z_ANCHOR_LEFT | Z_ANCHOR_RIGHT,
            .exclusive_zone = ZELTO_HOMEBAR_H,
            .height = ZELTO_HOMEBAR_H,
            // No text on this surface at all — a pill in a 62-unit strip whose
            // height is an exclusive zone. Declared rather than left to fall out
            // of "it happens to have no Text node", so a label added here later
            // cannot silently start resizing the safe area.
            .fixed_type = true)
