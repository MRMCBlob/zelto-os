// Zelto System UI — status bar.
//
// A wlr-layer-shell client anchored to the top edge in the TOP layer with an
// exclusive zone, so the compositor keeps the strip below it clear for apps.
// Same libzelto declarative loop as any app; only the surface role differs
// (Z_LAYER_APP). Shows a title on the left and a clock + a live system-status
// cluster on the right. See docs/contributing/compositor-internals.md.
//
// THE STATUS CLUSTER IS THE THIRD BROKER READER (P19). Alongside the shade and
// the Settings app, the bar z_settings_observe()s the zsysd settings store and
// renders glyphs from the sys.* keys — a Wi-Fi dot, an airplane dot, and a
// brightness pip whose width tracks sys.brightness. Flipping a toggle in ANY of
// the three updates all three live (the broker fans settings_changed out to every
// subscriber); the bar is a passive Z_LAYER_TOP surface, so the observe just
// rides its persistent ctrl_fd like the shade's does.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zelto/ui.h>

#define BAR_H 40

typedef struct BarState {
    bool inited;
    bool wifi;
    bool airplane;
    int64_t brightness;   // 1..5
} BarState;

// A small filled status dot (signal / mode indicator).
static ZView dot(ZColor c) {
    return Frame(10.0f, 10.0f, Rect(.color = c, .radius = 5));
}

// A setting changed (here, the shade, or the Settings app): re-read the field it
// maps to and repaint. Idempotent — the bar never writes settings, it only
// reflects them, so observing any change is a pure read.
static void on_changed(ZApp *app, const char *key, const char *value, void *ud) {
    BarState *s = ud;
    int v = atoi(value);
    if (strcmp(key, "sys.wifi") == 0) {
        s->wifi = v != 0;
    } else if (strcmp(key, "sys.airplane") == 0) {
        s->airplane = v != 0;
    } else if (strcmp(key, "sys.brightness") == 0) {
        s->brightness = v;
    }
    z_invalidate(app);
}

static ZView bar_body(ZApp *app, BarState *state) {
    // First build: read the system state from the broker and subscribe for live
    // updates (the ctrl_fd is up by now, like the shade).
    if (!state->inited) {
        state->inited = true;
        state->wifi = z_setting_get_int("sys.wifi", 1) != 0;
        state->airplane = z_setting_get_int("sys.airplane", 0) != 0;
        state->brightness = z_setting_get_int("sys.brightness", 3);
        z_settings_observe(app, on_changed, state);
    }

    // Clock from wall time (updates whenever the bar rebuilds).
    char clock[8] = "--:--";
    time_t t = time(NULL);
    struct tm tmv;
    if (gmtime_r(&t, &tmv)) {
        snprintf(clock, sizeof(clock), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    }

    // Wi-Fi reads as connected only when it is on AND not overridden by airplane
    // mode (airplane forces the radios off — the actuation the bar surfaces).
    bool wifi_live = state->wifi && !state->airplane;
    ZColor wifi_col = wifi_live ? z_rgba(0x3d, 0xc7, 0x8c, 0xff)   // green: on
                                : z_rgba(0x4a, 0x53, 0x5c, 0xff);  // grey: off
    // Brightness pip: a little bar whose width grows with the level (1..5), so a
    // step in Settings visibly changes the bar too.
    int64_t lvl = state->brightness < 1 ? 1 : (state->brightness > 5 ? 5
                                                                      : state->brightness);
    float pip_w = 6.0f + (float)lvl * 4.0f;   // level 1 -> 10px, level 5 -> 26px

    // Build the right cluster dynamically so the airplane dot only appears when
    // airplane mode is on.
    ZStackOpts cluster = {.spacing = 10, .align = Z_ALIGN_CENTER};
    int k = 0;
    cluster.children[k++] = dot(wifi_col);                       // Wi-Fi
    if (state->airplane) {
        cluster.children[k++] = dot(z_rgba(0xf0, 0xa0, 0x30, 0xff));  // airplane
    }
    cluster.children[k++] = Frame(pip_w, 10.0f,
        Rect(.color = z_rgba(0xc6, 0xcf, 0xd8, 0xff), .radius = 3));  // brightness
    cluster.children[k++] = Foreground(Z_COLOR_TEXT_INV, Text("%s", clock));

    return Background(z_rgba(0x10, 0x14, 0x1a, 0xff),
        HStack(
            Foreground(Z_COLOR_TEXT_INV,
                Font(Z_FONT_BODY, Text("Zelto"))),
            Spacer(),
            z_stack(Z_AXIS_HORIZONTAL, &cluster),
            .padding = 12, .spacing = 10, .align = Z_ALIGN_CENTER));
}

Z_LAYER_APP(BarState, bar_body,
            .layer = Z_LAYER_TOP,
            .anchor = Z_ANCHOR_TOP | Z_ANCHOR_LEFT | Z_ANCHOR_RIGHT,
            .exclusive_zone = BAR_H,
            .height = BAR_H)
