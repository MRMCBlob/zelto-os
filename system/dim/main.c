// Zelto System UI — brightness dim overlay (P19).
//
// The first whole-screen actuation of a system setting. An always-mapped OVERLAY
// layer-shell surface that observes the brokered `sys.brightness` setting (1..5)
// and paints a full-screen translucent BLACK scrim whose alpha = (5 - level) *
// STEP: level 5 = no dim (fully transparent, nothing painted), level 1 = heavily
// dimmed. Flipping the Brightness stepper in Settings (or anywhere the broker is
// written) recolours this live and dims the whole screen — not just a chip.
//
// HOW THE DIM ACTUALLY DIMS (the P10 renderer fact, resolved in P19). The
// software renderer historically forced every painted pixel to opaque alpha, so a
// separate overlay surface could only ever COVER the app, never tint it. P19 gave
// fill_round_rect a real source-over path that PRESERVES per-pixel alpha, so this
// surface can paint genuinely translucent black; the compositor's wlr_scene then
// alpha-blends it over the app beneath (it already honours per-pixel alpha — the
// shade relies on alpha-0 pixels being see-through). A moving/!opaque surface
// needs a full repaint, so we z_full_repaint whenever we paint a tint.
//
// INPUT. The scrim is visually full-screen but must steal NO taps — they have to
// reach the app, the status bar and the shade below it. So it sets an EMPTY input
// region (z_layer_set_input_none): every pointer event falls through. (Note this
// is the OPPOSITE of z_layer_set_input_region(app,0,0,0,0), which means "whole
// surface".)
//
// TRADEOFF / decision. We anchor BELOW the status bar (margin_top = BAR_H) so the
// bar — which is the live status indicator for these very settings (its Wi-Fi /
// airplane glyphs + a brightness pip) — stays at full brightness and legible,
// while the whole app area visibly dims. A real phone dims the bar too; here the
// bar doubling as the state read-out wins.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zelto/ui.h>

#include "common/settings_defaults.h"

#define BAR_H 40
// Per-level alpha step. Level 5 -> 0 (no dim); level 1 -> 4*STEP = 192 (~75%
// black, the app still faintly visible). Strong enough to read off a screendump.
#define DIM_STEP 48

typedef struct DimState {
    bool inited;
    bool subscribed;
    int64_t brightness;   // 1..5 (5 = brightest / no dim)
} DimState;

// Map a brightness level (1..5) to the scrim's alpha (0..255).
static int dim_alpha(int64_t level) {
    if (level < 1) {
        level = 1;
    } else if (level > 5) {
        level = 5;
    }
    int a = (int)(5 - level) * DIM_STEP;
    return a > 255 ? 255 : a;
}

// A setting changed: re-read brightness and repaint. Only sys.brightness affects
// us; ignore the rest. Idempotent (observing our own non-existent writes is moot).
static void on_changed(ZApp *app, const char *key, const char *value, void *ud) {
    DimState *s = ud;
    if (key && value && strcmp(key, "sys.brightness") == 0) {
        s->brightness = (int64_t)atoll(value);
        z_invalidate(app);
    }
}

static ZView dim_body(ZApp *app, DimState *s) {
    if (!s->inited) {
        s->inited = true;
        s->brightness =
            z_setting_get_int("sys.brightness", ZELTO_DEFAULT_BRIGHTNESS);
    }
    // Subscribe once (ctrl_fd is up by the first build), like the shade.
    if (!s->subscribed) {
        s->subscribed = true;
        z_settings_observe(app, on_changed, s);
    }
    // Never catch input — the scrim is purely visual; taps fall through.
    z_layer_set_input_none(app);

    int a = dim_alpha(s->brightness);
    if (a <= 0) {
        // Level 5: paint nothing, so the surface is fully transparent (no dim,
        // no cost). Unpainted pixels stay alpha 0 -> the app shows through.
        return Fill(Spacer());
    }
    // A translucent black scrim filling the whole surface. The new source-over
    // renderer keeps the alpha, so this tints (not covers) the app below; a
    // tint over transparent needs a full repaint.
    z_full_repaint(app);
    return Background(z_scrim((uint8_t)a), Fill(Spacer()));
}

// OVERLAY (above the app + status bar), anchored to all four edges below the bar
// so it fills the app area without ever resizing. exclusive_zone 0 reserves
// nothing; not keyboard-interactive. Its empty input region (set each build) makes
// it input-transparent regardless of the full-screen footprint.
Z_LAYER_APP(DimState, dim_body,
            .layer = Z_LAYER_OVERLAY,
            .anchor = Z_ANCHOR_TOP | Z_ANCHOR_BOTTOM | Z_ANCHOR_LEFT |
                      Z_ANCHOR_RIGHT,
            .exclusive_zone = 0,
            .margin_top = BAR_H,
            .keyboard = false)
