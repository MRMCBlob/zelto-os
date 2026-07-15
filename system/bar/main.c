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
    bool lock_enabled;    // P20: show a padlock glyph when the lock screen is on
    int64_t lock_now;     // sys.lock_now counter (a long-press on the bar bumps it)
    int64_t battery_pct;  // P23: 0..100 (from the zsysd power source)
    bool charging;        // P23: sys.battery_charging
} BarState;

// A small filled status dot (signal / mode indicator).
static ZView dot(ZColor c) {
    return Frame(10.0f, 10.0f, Rect(.color = c, .radius = 5));
}

// The Wi-Fi mark: two arcs over a dot, drawn as polylines in the unit box. Every
// phone draws connectivity this way, and a coloured DOT (what the bar used) says
// only "something is on" — it does not say what, and a green dot in a status bar
// reads as a recording indicator.
static ZView wifi_glyph(bool on) {
    ZColor c = on ? Z_COLOR_TEXT : Z_COLOR_TEXT_FAINT;
    static const float outer[] = {0.06f, 0.42f, 0.20f, 0.28f, 0.38f, 0.21f,
                                  0.62f, 0.21f, 0.80f, 0.28f, 0.94f, 0.42f};
    static const float inner[] = {0.26f, 0.58f, 0.38f, 0.47f, 0.50f, 0.44f,
                                  0.62f, 0.47f, 0.74f, 0.58f};
    return Frame(18.0f, 14.0f,
        ZStack(
            Frame(18.0f, 14.0f,
                Stroke(.points = outer, .count = 6, .thickness = 2.0f,
                       .color = c)),
            Frame(18.0f, 14.0f,
                Stroke(.points = inner, .count = 5, .thickness = 2.0f,
                       .color = c)),
            OffsetXY(0.0f, 4.5f, Frame(3.5f, 3.5f,
                Rect(.color = c, .radius = 1.75f))),
            .align = Z_ALIGN_CENTER));
}

// A tiny padlock glyph (shackle over a body). Shown when the lock screen is
// enabled — the bar's 4th brokered indicator (P20), alongside Wi-Fi/airplane/pip.
static ZView lock_glyph(ZColor c) {
    return Frame(12.0f, 16.0f,
        VStack(
            Frame(8.0f, 6.0f, Rect(.color = c, .radius = 3)),   // shackle
            Frame(12.0f, 9.0f, Rect(.color = c, .radius = 2)),  // body
            .spacing = 0, .align = Z_ALIGN_CENTER));
}

// Battery glyph (P23): a small horizontal cell whose interior fill tracks the
// charge level, plus a terminal nub. Green while charging, red at/below 20%,
// otherwise the normal text colour. A depth stack overlays the fill on the
// bordered shell (left-aligned so it grows rightward with the level).
static ZView battery_glyph(int64_t pct, bool charging) {
    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }
    ZColor col = charging ? Z_COLOR_SUCCESS
               : (pct <= 20 ? Z_COLOR_DANGER : Z_COLOR_TEXT);
    float inner = 20.0f;                       // usable fill width inside the cell
    float fill = inner * (float)pct / 100.0f;
    if (fill < 2.0f) {
        fill = 2.0f;                           // always a sliver so 1% is visible
    }
    return HStack(
        ZStack(
            Frame(24.0f, 12.0f, Rect(.color = Z_COLOR_BORDER, .radius = 3)),
            Frame(24.0f, 12.0f,
                Padding(2.0f,
                    HStack(Frame(fill, 8.0f, Rect(.color = col, .radius = 1)),
                           Spacer(), .spacing = 0))),
            .align = Z_ALIGN_CENTER),
        Frame(3.0f, 6.0f, Rect(.color = Z_COLOR_BORDER, .radius = 1)),  // nub
        .spacing = 1, .align = Z_ALIGN_CENTER);
}

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
        state->brightness = z_setting_get_int("sys.brightness", 3);
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
    if (gmtime_r(&t, &tmv)) {
        snprintf(clock, sizeof(clock), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    }

    // Wi-Fi reads as connected only when it is on AND not overridden by airplane
    // mode (airplane forces the radios off — the actuation the bar surfaces).
    bool wifi_live = state->wifi && !state->airplane;

    // The right cluster: connectivity, then charge. The brightness pip is gone —
    // brightness is not a STATUS you monitor, it is a setting you already see the
    // result of (the screen is dimmer), and a bar that grows and shrinks as you
    // drag a slider is just noise at the top of every screen. The "Zelto" wordmark
    // is gone too: a phone does not print its own brand across the status bar.
    ZStackOpts cluster = {.spacing = 8, .align = Z_ALIGN_CENTER};
    int k = 0;
    if (state->airplane) {
        cluster.children[k++] = dot(Z_COLOR_WARN);
    }
    if (state->lock_enabled) {
        cluster.children[k++] = lock_glyph(Z_COLOR_TEXT);
    }
    cluster.children[k++] = wifi_glyph(wifi_live);
    cluster.children[k++] = battery_glyph(state->battery_pct, state->charging);

    // TRANSPARENT. The bar paints no background, so on the home the wallpaper runs
    // right up under the clock (the way it does on a phone) instead of being cut
    // off by a black band across the top of every screen. Its exclusive zone still
    // keeps app windows clear of it, and app backgrounds are dark, so the ink stays
    // legible over both; the text shadow covers a bright wallpaper.
    //
    // A long-press on the bar manually locks now (bumps sys.lock_now).
    return OnLongPress(bar_longpress, NULL,
        HStack(
            // No TextShadow: at 15px the dropped copy lands a pixel or two off the
            // ink and just reads as a smeared double-strike, not as depth. Small
            // text wants contrast, not a shadow.
            Weight(Z_WEIGHT_SEMIBOLD,
                Foreground(Z_COLOR_TEXT, Font(Z_FONT_SUBHEAD,
                    Text("%s", clock)))),
            Spacer(),
            z_stack(Z_AXIS_HORIZONTAL, &cluster),
            .padding = 14, .spacing = 10, .align = Z_ALIGN_CENTER));
}

Z_LAYER_APP(BarState, bar_body,
            .layer = Z_LAYER_TOP,
            .anchor = Z_ANCHOR_TOP | Z_ANCHOR_LEFT | Z_ANCHOR_RIGHT,
            .exclusive_zone = BAR_H,
            .height = BAR_H)
