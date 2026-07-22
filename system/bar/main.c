// Zelto System UI — status bar.
//
// A wlr-layer-shell client anchored to the top edge in the TOP layer with an
// exclusive zone, so the compositor keeps the strip below it clear for apps.
// Same libzelto declarative loop as any app; only the surface role differs
// (Z_LAYER_APP). Shows a title on the left and a clock + a live system-status
// cluster on the right. See docs/contributing/compositor-internals.md.
//
// THE STATUS CLUSTER IS THE THIRD BROKER READER (P19). Alongside the Control
// Center and the Settings app, the bar z_settings_observe()s the zsysd settings
// store and renders marks from the sys.* keys — cellular bars (or the airplane),
// Wi-Fi, an optional padlock, and the battery. Flipping a toggle in ANY of the
// three updates all three live (the broker fans settings_changed out to every
// subscriber); the bar is a passive Z_LAYER_TOP surface, so the observe just
// rides its persistent ctrl_fd like the Control Center's does. The marks
// themselves live in system/common/glyphs.h, shared with the Control Center, so
// the toggle you flip and the indicator it changes are literally the same shape.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zelto/ui.h>

#include "common/glyphs.h"
#include "common/safe_areas.h"
#include "common/settings_defaults.h"

// A FIXED horizontal gap. Not Frame(w, h, Spacer()): a Spacer carries grow, so
// Frame'ing one still lets it eat every spare pixel in the stack and the "12px"
// gap swallows the row. An empty (fully transparent) Rect has no grow.
static ZView hgap(float w) {
    return Frame(w, 1.0f, Rect(.color = z_rgba(0, 0, 0, 0)));
}

typedef struct BarState {
    bool inited;
    bool wifi;
    bool airplane;
    int64_t signal;       // sys.signal: cellular bars lit, 0..4
    int64_t brightness;   // 1..5
    bool lock_enabled;    // P20: show a padlock glyph when the lock screen is on
    int64_t lock_now;     // sys.lock_now counter (a long-press on the bar bumps it)
    int64_t battery_pct;  // P23: 0..100 (from the zsysd power source)
    bool charging;        // P23: sys.battery_charging
} BarState;

// Long-press anywhere on the bar: manually lock now (bump sys.lock_now, which
// zelto-lock observes). A nice-to-have manual affordance (P20).
static void bar_longpress(ZApp *app, void *state, void *data, float x, float y) {
    (void)data;
    (void)x;
    (void)y;
    BarState *s = state;
    s->lock_now++;
    z_setting_set_int("sys.lock_now", s->lock_now);
    z_invalidate(app);
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
    } else if (strcmp(key, "sys.signal") == 0) {
        s->signal = v;
    } else if (strcmp(key, "sys.brightness") == 0) {
        s->brightness = v;
    } else if (strcmp(key, "sys.lock_enabled") == 0) {
        s->lock_enabled = v != 0;
    } else if (strcmp(key, "sys.battery_pct") == 0) {
        s->battery_pct = v;
    } else if (strcmp(key, "sys.battery_charging") == 0) {
        s->charging = v != 0;
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
        // Cellular strength. There is no modem behind this yet, so it reads a
        // brokered key like every other indicator here and defaults to full — a
        // real radio port fills sys.signal from the modem the same way the P23
        // power source fills sys.battery_pct, and the bar needs no change.
        state->signal = z_setting_get_int("sys.signal", 4);
        state->brightness =
            z_setting_get_int("sys.brightness", ZELTO_DEFAULT_BRIGHTNESS);
        state->lock_enabled = z_setting_get_int("sys.lock_enabled", 0) != 0;
        state->lock_now = z_setting_get_int("sys.lock_now", 0);
        state->battery_pct = z_setting_get_int("sys.battery_pct", 100);
        state->charging = z_setting_get_int("sys.battery_charging", 0) != 0;
        z_settings_observe(app, on_changed, state);
    }

    // The bar paints NO background, so its surface is transparent everywhere it
    // does not draw — and the partial-damage path has nothing to erase stale pixels
    // WITH: a repainted clock composites on top of the previous minute's glyphs.
    // A surface this small repaints in full for a pittance.
    z_full_repaint(app);

    // Clock from wall time (updates whenever the bar rebuilds).
    char clock[8] = "--:--";
    time_t t = time(NULL);
    struct tm tmv;
    // LOCAL time, not UTC: this is the clock the user reads. gmtime_r here put the
    // status bar an offset out of step with the home screen's clock widget and the
    // lock screen, both of which have always used localtime_r.
    if (localtime_r(&t, &tmv)) {
        snprintf(clock, sizeof(clock), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    }

    // Wi-Fi reads as connected only when it is on AND not overridden by airplane
    // mode (airplane forces the radios off — the actuation the bar surfaces).
    bool wifi_live = state->wifi && !state->airplane;

    // The right cluster, in the order every phone puts it: cellular, Wi-Fi,
    // charge. The brightness pip is gone — brightness is not a STATUS you monitor,
    // it is a setting you already see the result of (the screen is dimmer), and a
    // bar that grows and shrinks as you drag a slider is just noise at the top of
    // every screen. The "Zelto" wordmark is gone too: a phone does not print its
    // own brand across the status bar.
    //
    // AIRPLANE MODE REPLACES THE BARS with the plane, rather than adding an amber
    // dot beside them: the mode's entire meaning is "the radios are off", so
    // showing signal strength next to it states the opposite of the truth. That is
    // also what the mode does functionally here — airplane gates the network stack
    // (P19, z_net_send/z_ws_open) and forces Wi-Fi to read as down below.
    ZStackOpts cluster = {.spacing = 8, .align = Z_ALIGN_CENTER};
    int k = 0;
    if (state->airplane) {
        cluster.children[k++] = zelto_glyph_airplane(15.0f, Z_COLOR_TEXT);
    } else {
        cluster.children[k++] = zelto_glyph_cellular(
            15.0f, (int)state->signal, Z_COLOR_TEXT, Z_COLOR_TEXT_FAINT);
    }
    if (state->lock_enabled) {
        cluster.children[k++] = zelto_glyph_lock(15.0f, Z_COLOR_TEXT);
    }
    cluster.children[k++] = zelto_glyph_wifi(
        18.0f, wifi_live ? Z_COLOR_TEXT : Z_COLOR_TEXT_FAINT);
    ZColor bat = state->charging ? Z_COLOR_SUCCESS
               : (state->battery_pct <= 20 ? Z_COLOR_DANGER : Z_COLOR_TEXT);
    cluster.children[k++] =
        zelto_glyph_battery(state->battery_pct, bat, Z_COLOR_BORDER);

    // TRANSPARENT. The bar paints no background, so on the home the wallpaper runs
    // right up under the clock (the way it does on a phone) instead of being cut
    // off by a black band across the top of every screen. Its exclusive zone still
    // keeps app windows clear of it, and app backgrounds are dark, so the ink stays
    // legible over both; the text shadow covers a bright wallpaper.
    //
    // A long-press on the bar manually locks now (bumps sys.lock_now).
    // Caption (22 units = 12pt) is the status-bar step, and it stays there after
    // P44 grew the strip from 40 units to ZELTO_BAR_H. It was chosen in P43 for
    // the wrong reason — as the largest step that would fit 40 units — and the
    // note said "12pt in a 20pt bar, exactly what the phone this copies puts
    // there", which was true of the type and an admission about the bar: 12pt IS
    // Apple's status-bar size, but it sits in a 44pt inset there, not a 20pt one.
    // The strip moved; the type was already right.
    //
    // Padding is 4, not 14: the uniform .padding is BOTH axes. The horizontal
    // inset comes from explicit end gaps instead, so the two axes can differ.
    return OnLongPress(bar_longpress, NULL,
        HStack(
            hgap(12.0f),
            // No TextShadow: the dropped copy lands a pixel or two off the ink and
            // at this size just reads as a smeared double-strike, not as depth.
            // Small text wants contrast, not a shadow.
            Weight(Z_WEIGHT_SEMIBOLD,
                Foreground(Z_COLOR_TEXT, Font(Z_FONT_CAPTION,
                    Text("%s", clock)))),
            Spacer(),
            z_stack(Z_AXIS_HORIZONTAL, &cluster),
            hgap(12.0f),
            .padding = 4, .spacing = 10, .align = Z_ALIGN_CENTER));
}

Z_LAYER_APP(BarState, bar_body,
            .layer = Z_LAYER_TOP,
            .anchor = Z_ANCHOR_TOP | Z_ANCHOR_LEFT | Z_ANCHOR_RIGHT,
            .exclusive_zone = ZELTO_BAR_H,
            .height = ZELTO_BAR_H)
