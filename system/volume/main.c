// Zelto System UI — volume-rocker overlay (P23).
//
// A transient OVERLAY layer-shell HUD, sibling to zelto-dim / zelto-lock. It
// observes the brokered sys.volume (0..10) and sys.mute keys and, on a CHANGE,
// pops a centred volume slider for ~1.5s, then fades to nothing — exactly the
// heads-up rocker a phone shows when you press volume up/down. The keys are
// driven by the compositor's volume keybinds (zcomp writes settings_set on
// XF86AudioRaise/Lower/Mute) or by the Settings app; either way the broker fans
// the change out here.
//
// SELF-DISMISS. There is no "hide" event — the HUD hides itself on a timer. It
// uses the SDK one-shot z_after(ms): each change (re)arms it, so a rapid burst of
// presses keeps the HUD up and it disappears ~1.5s after the LAST one. When the
// timer fires it flips visible=false and repaints to a fully transparent surface.
//
// RENDER / INPUT (the P16/P19 overlay facts). While visible it paints a
// translucent scrim panel; a tint over transparent needs z_full_repaint, and the
// software renderer's source-over path (P19) keeps the alpha so it composites over
// the app. It sets an EMPTY input region every build (z_layer_set_input_none) so
// it steals no taps outside itself — the whole surface is input-transparent (the
// slider is display-only; volume is driven by keys, not by dragging this HUD).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zelto/ui.h>

#define BAR_H 40
#define VOL_MAX 10
#define VOL_DEFAULT 5
// How long the HUD stays up after the last change. Overridable so a screenshot
// harness can widen the window (the native sim/QEMU capture may lag the change).
#define VOL_SHOW_MS_DEFAULT 1500

// How long the exit slide+fade runs before the surface is torn down transparent.
#define VOL_EXIT_MS 340

typedef struct VolState {
    bool inited;
    bool subscribed;
    bool visible;         // is the HUD currently shown (or animating out)?
    int64_t volume;       // 0..VOL_MAX
    bool mute;
    int show_ms;
    // Entrance/exit motion (P32). `enter` springs 0 (off-anchor, transparent) ->
    // 1 (seated, opaque) on show and back to 0 before the surface hides; the panel
    // Offset + Opacity are bound to it. `exiting` marks the fade-out leg so the
    // body can tear the surface down once the spring has settled at 0.
    ZAnimated *enter;
    bool exiting;
} VolState;

// The exit slide+fade has run its course: drop the now-transparent surface.
static void hud_finish_hide(ZApp *app, void *ud) {
    VolState *s = ud;
    s->visible = false;
    s->exiting = false;
    z_full_repaint(app);   // repaint the now-transparent surface over the app
    z_invalidate(app);
}

// The dwell elapsed with no further change: start the exit (spring `enter` back to
// 0) and, once it has settled, hide. Reduce Motion collapses the spring, so the
// panel simply vanishes when hud_finish_hide fires.
static void hud_timeout(ZApp *app, void *ud) {
    VolState *s = ud;
    s->exiting = true;
    if (s->enter) {
        z_animated_spring_with(s->enter, 0.0f, Z_SPRING_STANDARD);
    }
    z_after(app, VOL_EXIT_MS, hud_finish_hide, s);
    z_invalidate(app);
}

// A setting changed. Only sys.volume / sys.mute concern us; either one pops the
// HUD (spring it in) and (re)arms the dwell timer. Ignore everything else.
static void on_changed(ZApp *app, const char *key, const char *value, void *ud) {
    VolState *s = ud;
    if (!key || !value) {
        return;
    }
    if (strcmp(key, "sys.volume") == 0) {
        s->volume = (int64_t)atoll(value);
    } else if (strcmp(key, "sys.mute") == 0) {
        s->mute = atoi(value) != 0;
    } else {
        return;
    }
    s->visible = true;
    s->exiting = false;
    if (s->enter) {
        z_animated_spring_with(s->enter, 1.0f, Z_SPRING_SNAPPY);  // slide+fade in
    }
    z_after(app, s->show_ms, hud_timeout, s);
    z_invalidate(app);
}

// A speaker glyph: a small body + cone, tinted muted (danger) or normal.
static ZView speaker(bool mute) {
    ZColor c = mute ? Z_COLOR_DANGER : Z_COLOR_TEXT;
    return Frame(20.0f, 20.0f,
        HStack(Frame(6.0f, 10.0f, Rect(.color = c, .radius = 2)),
               Frame(10.0f, 18.0f, Rect(.color = c, .radius = 3)),
               .spacing = 0, .align = Z_ALIGN_CENTER));
}

// The rocker panel: speaker + a horizontal track whose fill tracks the level
// (empty when muted). A depth stack overlays the fill on the track groove.
static ZView rocker(VolState *s) {
    int64_t vol = s->volume < 0 ? 0 : (s->volume > VOL_MAX ? VOL_MAX : s->volume);
    float track_w = 220.0f;
    float fill_w = s->mute ? 0.0f : track_w * (float)vol / (float)VOL_MAX;
    if (!s->mute && fill_w < 4.0f) {
        fill_w = 4.0f;
    }
    ZView fill = s->mute
        ? (ZView)Spacer()
        : Frame(fill_w, 10.0f, Rect(.color = Z_COLOR_ACCENT, .radius = 5));
    ZView track = ZStack(
        Frame(track_w, 10.0f, Rect(.color = Z_COLOR_SURFACE_3, .radius = 5)),
        HStack(fill, Spacer(), .spacing = 0),
        .align = Z_ALIGN_CENTER);

    return Shadow(Z_ELEV_3, CornerRadius(20.0f, Background(Z_COLOR_SURFACE,
        Padding(16.0f,
            HStack(speaker(s->mute), track,
                   .spacing = 16, .align = Z_ALIGN_CENTER)))));
}

static ZView vol_body(ZApp *app, VolState *s) {
    if (!s->inited) {
        s->inited = true;
        s->volume = z_setting_get_int("sys.volume", VOL_DEFAULT);
        s->mute = z_setting_get_int("sys.mute", 0) != 0;
        const char *ms = getenv("ZELTO_VOLUME_MS");
        s->show_ms = (ms && atoi(ms) > 0) ? atoi(ms) : VOL_SHOW_MS_DEFAULT;
    }
    // The entrance/exit spring, allocated first + unconditionally for a stable id.
    s->enter = z_animated_value(app, 0.0f);
    if (!s->subscribed) {
        s->subscribed = true;
        z_settings_observe(app, on_changed, s);
    }
    // Headless test hook: ZELTO_VOLUME_SHOW=1 pops the HUD on the first build (at
    // the seeded sys.volume/mute level) without needing a media-key/settings change
    // to fire the observer, so the rocker is screenshot-verifiable deterministically.
    // ZELTO_VOLUME_ENTER=<0..1> instead pins the entrance spring mid-flight (a
    // frozen slide+fade frame) — the P32 transition freeze-frame hook.
    static bool vol_show_applied = false;
    if (!vol_show_applied) {
        vol_show_applied = true;
        const char *en = getenv("ZELTO_VOLUME_ENTER");
        const char *vs = getenv("ZELTO_VOLUME_SHOW");
        if (en && en[0]) {
            s->visible = true;
            // Under Reduce Motion the entrance spring is suppressed (collapses to
            // an instant jump), so the "mid-transition" freeze has no mid-state to
            // show — pin it fully seated instead. This makes the reduce-motion shot
            // an A/B against 42 (same ENTER=0.5 request, but seated not half-faded),
            // a still proof that the spring was collapsed.
            float ev = z_setting_get_int("sys.reduce_motion", 0) ? 1.0f
                                                                 : (float)atof(en);
            z_animated_pin(s->enter, ev);
        } else if (vs && vs[0] == '1') {
            s->visible = true;
            z_animated_set(s->enter, 1.0f);              // fully seated
        }
    }
    // Never catch input — the HUD is display-only; taps fall through.
    z_layer_set_input_none(app);

    if (!s->visible) {
        // Nothing painted -> fully transparent surface, the app shows through.
        return Fill(Spacer());
    }
    // Centre the panel horizontally, a little below the status bar. A tint over
    // transparent (and a moving/fading subtree) needs a full repaint.
    z_full_repaint(app);
    float e = z_animated_get(s->enter);
    float slide = (1.0f - e) * -26.0f;   // slides down into place from above
    ZView panel = Opacity(e, OffsetXY(0.0f, slide, rocker(s)));
    return Fill(VStack(
        Frame(0.0f, 80.0f, Spacer()),
        HStack(Spacer(), panel, Spacer(), .spacing = 0),
        Spacer(),
        .spacing = 0));
}

// OVERLAY (above the app + status bar) filling the app area below the bar, never
// resizing; input-transparent via its per-build empty region. Started by init
// after zelto-lock, like the other overlays.
Z_LAYER_APP(VolState, vol_body,
            .layer = Z_LAYER_OVERLAY,
            .anchor = Z_ANCHOR_TOP | Z_ANCHOR_BOTTOM | Z_ANCHOR_LEFT |
                      Z_ANCHOR_RIGHT,
            .exclusive_zone = 0,
            .margin_top = BAR_H,
            .keyboard = false)
