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

#include "common/safe_areas.h"

#define VOL_MAX 10
#define VOL_DEFAULT 5
// How long the HUD stays up after the last change. Overridable so a screenshot
// harness can widen the window (the native sim/QEMU capture may lag the change).
#define VOL_SHOW_MS_DEFAULT 1500

// How long the exit slide+fade runs before the surface is torn down transparent.
#define VOL_EXIT_MS 340

// Swipe-to-dismiss (P33). The HUD sits near the top, so an UPWARD drag dismisses it
// (a downward drag resists with the rubber-band). DISMISS_THRESH px up (or a strong
// up-fling) past release commits; DISMISS_DIST is the travel over which the panel
// fades fully out as it leaves. INPUT_BAND is the top strip the surface catches
// input in while shown (so the drag reaches the panel; taps below fall through).
#define VOL_DISMISS_THRESH 36.0f
#define VOL_DISMISS_DIST 110.0f
#define VOL_INPUT_BAND 320

// The left-edge capsule. Tall and thin, held off the edge by a margin so it
// reads as floating over the app rather than welded to the screen border, and
// started below the status bar so it never fouls the clock.
#define VOL_PILL_W 34.0f
#define VOL_PILL_H 170.0f
#define VOL_PILL_LEFT 14.0f
#define VOL_PILL_TOP 56.0f

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
    // Swipe-to-dismiss (P33). `drag` is the finger's live vertical translation (px),
    // driven 1:1 while a finger is down and sprung back to 0 on a short release. A
    // release past the threshold/velocity dismisses via the P32 exit spring — the
    // enter/exit spring is now the RELEASE animation, not the whole story. `dragging`
    // suppresses the self-dismiss timer while the finger holds the panel.
    ZAnimated *drag;
    bool dragging;
} VolState;

// The exit slide+fade has run its course: drop the now-transparent surface.
static void hud_finish_hide(ZApp *app, void *ud) {
    VolState *s = ud;
    s->visible = false;
    s->exiting = false;
    z_full_repaint(app);   // repaint the now-transparent surface over the app
    z_invalidate(app);
}

// Begin the exit slide+fade (spring `enter` back to 0, carrying an optional release
// velocity for a swipe-dismiss hand-off) and schedule the teardown once it settles.
// Reduce Motion collapses the spring, so the panel just vanishes when the timer
// fires. Shared by the dwell timeout and swipe-to-dismiss.
static void start_exit(ZApp *app, VolState *s, float velocity) {
    s->exiting = true;
    if (s->enter) {
        z_animated_spring_velocity(s->enter, 0.0f, Z_SPRING_STANDARD, velocity);
    }
    z_after(app, VOL_EXIT_MS, hud_finish_hide, s);
    z_invalidate(app);
}

// The dwell elapsed with no further change: fade out.
static void hud_timeout(ZApp *app, void *ud) {
    start_exit(app, ud, 0.0f);
}

// Swipe-to-dismiss the HUD (P33): a drag moves the panel 1:1 with the finger; an
// upward release past the threshold (or a strong up-fling) dismisses via the exit
// spring, a short release snaps back and re-arms the dwell timer. A downward drag
// resists (dismiss is upward), handled in the body via the rubber-band.
static void on_vol_pan(ZApp *app, void *state, const ZPanEvent *e) {
    VolState *s = state;
    if (!s->drag) {
        return;
    }
    if (e->phase == Z_PAN_BEGIN) {
        s->dragging = true;
        z_after_cancel(app);           // hold: don't self-dismiss under the finger
        z_animated_grab(s->drag);      // take control of any snap-back in flight
    } else if (e->phase == Z_PAN_CHANGED) {
        z_animated_set(s->drag, e->translation_y);
    } else {                            // Z_PAN_END
        s->dragging = false;
        float d = z_animated_get(s->drag);
        bool dismiss = d < -VOL_DISMISS_THRESH || e->velocity_y < -700.0f;
        if (dismiss) {
            start_exit(app, s, e->velocity_y);   // continue up + fade, from the hand
        } else {
            z_animated_spring_velocity(s->drag, 0.0f, Z_SPRING_STANDARD,
                                       e->velocity_y);   // snap back
            z_after(app, s->show_ms, hud_timeout, s);    // re-arm the dwell
        }
    }
    z_full_repaint(app);
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
    ZColor c = mute ? Z_COLOR_DANGER : Z_COLOR_ON_PRIMARY;
    return Frame(18.0f, 18.0f,
        HStack(Frame(5.0f, 9.0f, Rect(.color = c, .radius = 2)),
               Frame(9.0f, 16.0f, Rect(.color = c, .radius = 3)),
               .spacing = 0, .align = Z_ALIGN_CENTER));
}

// The rocker: a VERTICAL capsule pinned to the left edge, the level rising from
// its bottom, with the speaker glyph riding inside the base.
//
// It used to be a wide horizontal card floating in the middle of the screen —
// the shape a desktop OSD uses, and the single most intrusive thing the system
// drew: it landed on top of whatever you were reading every time your thumb
// brushed a volume key. A tall thin capsule against the edge you are already
// pressing puts the readout NEXT TO the control that changes it, and leaves the
// centre of the screen — the part you are actually looking at — alone.
//
// The capsule is a MATERIAL, not a solid card: the compositor blurs what is
// behind it (z_backdrop) and this tints it, so it reads as a pane of frosted
// glass over the app rather than a slab dropped on top of it. The fill is
// PRIMARY (the near-white "lit" token), which is the level indicator's whole job
// — it is the one thing in the capsule that must read at a glance.
static ZView rocker(VolState *s) {
    int64_t vol = s->volume < 0 ? 0 : (s->volume > VOL_MAX ? VOL_MAX : s->volume);
    float fill_h = s->mute ? 0.0f
                           : VOL_PILL_H * (float)vol / (float)VOL_MAX;
    // Never let a non-zero level collapse to nothing: the fill has to stay
    // visible as a sliver or "quiet" is indistinguishable from "muted".
    if (!s->mute && fill_h < VOL_PILL_W) {
        fill_h = VOL_PILL_W;
    }

    // Bottom-anchored fill: a spacer eats the empty head of the capsule so the
    // level grows UPWARD from the base, the way a gauge does.
    ZView column = VStack(
        Spacer(),
        s->mute ? (ZView)Spacer()
                : Frame(VOL_PILL_W, fill_h,
                        Rect(.color = Z_COLOR_PRIMARY,
                             .radius = VOL_PILL_W * 0.5f)),
        .spacing = 0, .align = Z_ALIGN_CENTER);

    // The glyph sits in the base of the capsule, over the fill — so it inverts
    // against the lit level (ON_PRIMARY ink) exactly as iOS's does.
    ZView glyph = VStack(Spacer(), speaker(s->mute),
                         Frame(1.0f, 10.0f, Rect(.color = z_rgba(0, 0, 0, 0))),
                         .spacing = 0, .align = Z_ALIGN_CENTER);

    float radius = VOL_PILL_W * 0.5f;
    return Shadow(Z_ELEV_3,
        CornerRadius(radius,
            Background(Z_COLOR_MATERIAL_THICK,
                ZStack(
                    Frame(VOL_PILL_W, VOL_PILL_H, column),
                    Frame(VOL_PILL_W, VOL_PILL_H, glyph),
                    .align = Z_ALIGN_CENTER))));
}

static ZView vol_body(ZApp *app, VolState *s) {
    if (!s->inited) {
        s->inited = true;
        s->volume = z_setting_get_int("sys.volume", VOL_DEFAULT);
        s->mute = z_setting_get_int("sys.mute", 0) != 0;
        const char *ms = getenv("ZELTO_VOLUME_MS");
        s->show_ms = (ms && atoi(ms) > 0) ? atoi(ms) : VOL_SHOW_MS_DEFAULT;
    }
    // The entrance/exit spring + the swipe drag value, allocated first +
    // unconditionally (call-order cells) for a stable identity across rebuilds.
    s->enter = z_animated_value(app, 0.0f);
    s->drag = z_animated_value(app, 0.0f);
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
        // Swipe-dismiss freeze-frame: ZELTO_VOLUME_DRAG=<px> pins the panel at a
        // held finger translation (negative = dragged up toward dismissal), so the
        // mid-drag frame is shot-verifiable with no injected pointer input.
        const char *dr = getenv("ZELTO_VOLUME_DRAG");
        if (dr && dr[0]) {
            s->visible = true;
            if (z_animated_get(s->enter) < 0.5f) {
                z_animated_set(s->enter, 1.0f);          // seated, then held
            }
            z_animated_pin(s->drag, (float)atof(dr));
            s->dragging = true;
        }
    }

    if (!s->visible) {
        // Nothing painted -> fully transparent surface, the app shows through.
        z_layer_set_input_none(app);
        return Fill(Spacer());
    }
    // Catch input only in the left column the capsule occupies, so the drag
    // reaches it while every tap outside still falls through to the app — the
    // HUD is a readout, and it must not swallow the screen it floats over. A
    // tint over transparent (and a moving/fading subtree) needs a full repaint.
    z_layer_set_input_region(app, 0, 0,
                             (int)(VOL_PILL_LEFT + VOL_PILL_W + 24.0f),
                             VOL_INPUT_BAND);
    z_full_repaint(app);
    float e = z_animated_get(s->enter);
    float d = z_animated_get(s->drag);
    // Slides in from the LEFT EDGE it lives on, rather than dropping from above:
    // a panel should enter from the side it belongs to.
    float slide = (1.0f - e) * -(VOL_PILL_LEFT + VOL_PILL_W);
    // An upward drag (d < 0) carries the panel 1:1 and fades it toward dismissal; a
    // downward drag resists elastically (dismiss is upward, so down is "wrong way").
    float dy = d < 0.0f ? d : z_rubber_band(d, (float)z_app_height(app));
    float prog = d < 0.0f ? (-d / VOL_DISMISS_DIST) : 0.0f;
    if (prog > 1.0f) {
        prog = 1.0f;
    }
    // The entrance now travels on X (in from the edge) and the dismiss drag on Y,
    // so the two compose instead of fighting over one axis.
    ZView panel = Opacity(e * (1.0f - prog),
                          OffsetXY(slide, dy, rocker(s)));
    // NB: the fixed gaps are empty Rects, NOT Frame(w,h,Spacer()) — a Spacer
    // carries grow and Frame only sets a size, so a "14px" gap built that way
    // eats every spare pixel in the stack and shoves the capsule off-screen.
    return OnPan(on_vol_pan, Fill(VStack(
        Frame(1.0f, VOL_PILL_TOP, Rect(.color = z_rgba(0, 0, 0, 0))),
        HStack(Frame(VOL_PILL_LEFT, 1.0f, Rect(.color = z_rgba(0, 0, 0, 0))),
               panel, Spacer(), .spacing = 0),
        Spacer(),
        .spacing = 0)));
}

// OVERLAY (above the app + status bar) filling the app area below the bar, never
// resizing; input-transparent via its per-build empty region. Started by init
// after zelto-lock, like the other overlays.
Z_LAYER_APP(VolState, vol_body,
            .layer = Z_LAYER_OVERLAY,
            .anchor = Z_ANCHOR_TOP | Z_ANCHOR_BOTTOM | Z_ANCHOR_LEFT |
                      Z_ANCHOR_RIGHT,
            .exclusive_zone = 0,
            .margin_top = ZELTO_BAR_H,
            .keyboard = false)
