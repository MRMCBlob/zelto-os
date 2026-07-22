// Zelto System UI — Control Center + Notification Center (P40 stage 2).
//
// This surface used to be ONE Android-style unified pull-down: any down-swipe
// from the top edge brought down a single panel holding the clock, three
// quick-settings chips and the notification list. iOS splits that panel in two,
// and splits it BY WHERE YOUR THUMB LANDS on the top edge:
//
//   - top RIGHT  -> CONTROL CENTER, a grid of round toggle buttons.
//   - top LEFT   -> NOTIFICATION CENTER, notification cards over the (blurred)
//                   wallpaper, with the clock and date above them.
//
// The split is not decoration. A unified shade makes every glance at a
// notification a trip past the controls, and every toggle flip a trip past the
// notifications; separating them means each pull has ONE destination and the
// muscle memory is a half-screen apart. The status bar already teaches the
// mapping for free: the clock sits top-left (Notification Center) and the
// battery/Wi-Fi cluster top-right (Control Center), so the thing you reach for
// is under the thing you are looking at.
//
// WHY ONE BINARY AND NOT TWO LAYER-SHELL CLIENTS. The obvious split is two
// processes, one per panel, each with a half-width grab strip. It does not pay:
//
//   1. The panels are MUTUALLY EXCLUSIVE, and enforcing that across processes
//      means putting a broker round-trip inside the gesture path — the open
//      panel must take the whole surface's input while its sibling drops to
//      none, or the sibling's idle strip eats the open panel's top corner. In
//      one process that is a bool read in the same rebuild; across two it is a
//      settings_set/fan-out per pan BEGIN, and the finger's first CHANGED event
//      can beat the sibling's narrowing.
//   2. Both would be OVERLAY surfaces fighting over the same top edge, so their
//      stacking order silently decides which one wins a press on the boundary.
//   3. There is exactly ONE notification sink subscription (z_notify_subscribe)
//      and one heads-up banner strip. A two-process split either leaves the sink
//      in the Notification Center (fine) or duplicates the banner logic (not).
//
// So: one surface, one input region, one pull spring, and the panel identity is
// latched at Z_PAN_BEGIN from the touch's x (e->x < w/2 -> Notification Center).
// The binary keeps the name zelto-shade because init, run-sim.sh and the
// initramfs all start it by that path; what it hosts is now two panels.
//
// The three jobs this surface still owns:
//
//   1. HEADS-UP BANNERS (P10). It subscribes to zsysd as the single notification
//      sink; a posted notification pops as a banner card strip across the top,
//      over whatever app is in front. Swiping one UP hides the pop-over (the
//      notification stays in the Notification Center); dragging DOWN on it opens
//      the Notification Center, since that is where it came from.
//   2. THE TWO PULL-DOWNS, above.
//   3. The brokered quick-settings state the Control Center toggles.
//
// THE SURFACE-FOOTPRINT CHOREOGRAPHY (the real layer-shell problem). The software
// renderer cannot produce a translucent SURFACE — any node it paints writes
// opaque final alpha, and any pixel it leaves untouched stays fully transparent
// (the compositor's wlr_scene then blends it as see-through to the app beneath).
// The surface is ALWAYS the full area below the status bar and never resizes;
// what it CATCHES is set by its INPUT REGION every rebuild:
//
//   - IDLE (no banner, not pulled): a GRAB_H thin strip across the top. It covers
//     only the top gesture inset of the app and catches the down-swipe;
//     everything below is the app, untouched. It paints NOTHING — iOS shows no
//     handle up there either, and the status bar is the affordance.
//   - BANNER (a heads-up is up, not pulled): BANNER_STRIP_H, the cards strip.
//   - EXPANDED (being pulled / open): the whole surface. The panel is pinned to
//     the top and slid down by an Offset bound to a spring `pull` value; the
//     region the panel has not reached is left UNPAINTED so the app shows through
//     there, and a bg-less full-surface scrim catches the tap/drag that closes it
//     (input without cover).
//
// Because the panel is in motion and the surface mixes opaque + transparent, a
// frame mid-pull forces a full repaint (the partial-repaint path under-damages a
// big translated subtree and re-blends transparent regions wrong).
//
// TOGGLE STATE lives in the zsysd-brokered settings store (z_setting_get/set_int
// on the sys.* keys), NOT this surface's private prefs, because the Settings app
// reads and writes the same keys: one source of truth. We z_settings_observe(),
// so a flip in Settings recolours the toggle here live (and a flip here is
// broadcast back), and the broker persists every change to /var/zelto.
// See docs/guides/settings.md + the P17/P18 memory notes.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zelto/ui.h>

#include "common/app_icons.h"
#include "common/glyphs.h"
#include "common/notif_card.h"
#include "common/safe_areas.h"
#include "common/settings_defaults.h"

#define MAX_BANNERS 8
#define MAX_HISTORY 6   // recently-dismissed notifications kept for the panel list

// Footprints (px). The status bar owns the top ZELTO_BAR_H (we float below it via
// margin_top so it stays visible); GRAB_H is the idle down-swipe gesture strip;
// BANNER_STRIP_H is the heads-up cards strip.
#define GRAB_H 72   // generous top-edge gesture inset so a down-swipe reliably
                    // starts on the strip and the expand-to-full happens before
                    // the finger leaves it (input then stays over the full surface)
#define BANNER_STRIP_H 150

// Heads-up swipe-to-dismiss (P33): an upward drag past THRESH px (or a strong
// up-fling) hides the pop-over; DIST is the travel the strip fades out over.
#define BANNER_DISMISS_THRESH 30.0f
#define BANNER_DISMISS_DIST 100.0f

// Panel geometry. The Control Center is an INSET floating sheet (it is a slab of
// controls, and the inset says "this is an object over your screen"); the
// Notification Center is FULL-BLEED (it is your screen's content, not a widget).
//
// The Control Center's height is its CONTENT's height, not a fraction of the
// screen: six toggles in a fixed 3x2 grid occupy ~250px, and stretching the sheet
// to 60% of the screen to hold them leaves 600px of empty material that reads as
// a panel that failed to load. A short sheet also makes its pull SHORT, which is
// right — a control you reach for reflexively should not need a full-screen drag.
// The Notification Center is a list of unknown length, so it does take a fraction.
#define CC_MARGIN 12.0f
// Two toggle rows (~100 each) + the tall slider pair + the gaps between them.
// Grown in P42 when brightness and volume became real sliders instead of a round
// toggle and nothing at all; still CONTENT height, not a fraction of the screen.
#define CC_H 560.0f
#define NC_FRAC 0.85f
#define CC_BTN 68.0f      // the round toggle's diameter
#define CC_GLYPH 30.0f    // the mark inside it
#define CC_SLIDER_W 150.0f  // the tall brightness/volume slab
#define CC_SLIDER_H 260.0f

// Which panel a pull is bringing down.
enum { PANEL_NONE = 0, PANEL_CC, PANEL_NC };

// One stored notification (active banner or history). The pushed
// ZShownNotification strings are valid only during the show callback, so copy.
typedef struct Banner {
    bool used;
    int64_t id;
    char app_id[96];
    char title[128];
    char body[192];
    char tap_route[256];
    char action_id[64];
    char action_title[64];
} Banner;

typedef struct ShadeState {
    bool subscribed;
    Banner banners[MAX_BANNERS];     // active notifications (heads-up + panel)
    Banner history[MAX_HISTORY];     // recently dismissed, most-recent-first
    int n_history;

    // Pull-down state. `pull` is the spring-backed 0 (closed strip) .. 1 (fully
    // pulled down) value bound to the panel's slide Offset; allocated first and
    // unconditionally every rebuild so its retained identity is stable. `panel`
    // is latched at the drag's begin from which half of the top edge it started
    // in, and held until the pull settles back to closed.
    ZAnimated *pull;
    float pull_base;                 // pull value captured at a drag's begin
    bool dragging;                   // a pull/close drag is in flight
    int panel;                       // PANEL_NONE / _CC / _NC

    // Heads-up banner entrance (P32). Springs 0 -> 1 when the first banner
    // arrives (the cards strip slides down + fades in) and resets to 0 once the
    // last banner clears, so the next heads-up enters fresh. Bound to the strip's
    // Offset + Opacity in the collapsed-banner branch.
    ZAnimated *banner_enter;

    // Heads-up swipe-to-dismiss (P33). `banner_drag` is the finger's live vertical
    // translation of the cards strip (<=0 dragged up toward dismissal), sprung back
    // on a short release. `banner_mode` latches the drag's intent on the first real
    // motion: an UP drag dismisses the heads-up, a DOWN drag opens the Notification
    // Center (the same surface hosts both gestures). `heads_up_hidden` collapses the
    // strip after a dismiss — the notifications stay active in the panel (swiping a
    // heads-up away only hides the pop-over, the iOS way); a new post clears it.
    ZAnimated *banner_drag;
    int banner_mode;   // 0 undecided, 1 pull-shade, 2 dismiss-heads-up
    bool heads_up_hidden;

    // Control Center toggles, loaded once from the broker and persisted on flip.
    bool qs_loaded;
    bool qs_wifi, qs_mute, qs_bright, qs_airplane, qs_lock, qs_motion;

    // The two CONTINUOUS controls (P42). Brightness (sys.brightness, 1..5) and
    // volume (sys.volume, 0..10) are levels, not switches, and they are the two
    // controls a Control Center exists to put a thumb on. Their brokered integer
    // values are mirrored here and the sliders' 0..1 positions derived from them.
    int64_t qs_brightness;
    int64_t qs_volume;
    ZSlider bright_slider;
    ZSlider volume_slider;
} ShadeState;

// The pull value is stored RAW (a drag may push it past either end); this maps it
// to the DISPLAYED pull, rubber-banding the over-pull so dragging past fully-open
// (or above fully-closed) resists with diminishing returns instead of running off
// the panel. In [0,1] it is the identity. dim is in pull-units (a fraction of the
// panel travel), tuned so a hard over-pull adds only a little slack (P33).
static float shade_display_pull(float raw) {
    if (raw > 1.0f) {
        return 1.0f + z_rubber_band(raw - 1.0f, 0.4f);
    }
    if (raw < 0.0f) {
        return z_rubber_band(raw, 0.4f);
    }
    return raw;
}

// Load the toggles once from the brokered settings store. These are the SAME
// sys.* keys the Settings app reads/writes — one source of truth.
// z_settings_observe (in shade_body) then keeps them live: a flip in Settings
// recolours the toggle here without a reboot, and a flip here is broadcast back.
static void ensure_qs(ShadeState *s) {
    if (s->qs_loaded) {
        return;
    }
    s->qs_loaded = true;
    s->qs_wifi = z_setting_get_int("sys.wifi", 1) != 0;
    s->qs_mute = z_setting_get_int("sys.mute", 0) != 0;
    s->qs_bright = z_setting_get_int("sys.bright", 1) != 0;
    s->qs_airplane = z_setting_get_int("sys.airplane", 0) != 0;
    s->qs_lock = z_setting_get_int("sys.lock_enabled", 0) != 0;
    s->qs_motion = z_setting_get_int("sys.reduce_motion", 0) != 0;
    s->qs_brightness =
        z_setting_get_int("sys.brightness", ZELTO_DEFAULT_BRIGHTNESS);
    s->qs_volume = z_setting_get_int("sys.volume", ZELTO_DEFAULT_VOLUME);
}

// A setting changed somewhere (this surface or the Settings app): re-read the
// bool it maps to and repaint the toggle. Idempotent — applying a value we just
// set is a harmless no-op, so observing our own set (the broker fans out to every
// subscriber) neither loops nor double-toggles.
static void on_qs_setting(ZApp *app, const char *key, const char *value,
                          void *ud) {
    ShadeState *s = ud;
    bool v = atoi(value) != 0;
    if (strcmp(key, "sys.wifi") == 0) {
        s->qs_wifi = v;
    } else if (strcmp(key, "sys.mute") == 0) {
        s->qs_mute = v;
    } else if (strcmp(key, "sys.bright") == 0) {
        s->qs_bright = v;
    } else if (strcmp(key, "sys.airplane") == 0) {
        s->qs_airplane = v;
    } else if (strcmp(key, "sys.lock_enabled") == 0) {
        s->qs_lock = v;
    } else if (strcmp(key, "sys.reduce_motion") == 0) {
        s->qs_motion = v;
    } else if (strcmp(key, "sys.brightness") == 0) {
        // Not a bool: these two are levels. atoi, not the != 0 above.
        s->qs_brightness = atoll(value);
    } else if (strcmp(key, "sys.volume") == 0) {
        s->qs_volume = atoll(value);
    }
    z_invalidate(app);
}

// --- notification sink (zsysd push) ---------------------------------------
static int active_count(ShadeState *s);   // defined below; used by on_show

// Find an active banner by id (NULL if none).
static Banner *banner_by_id(ShadeState *s, int64_t id) {
    for (int i = 0; i < MAX_BANNERS; i++) {
        if (s->banners[i].used && s->banners[i].id == id) {
            return &s->banners[i];
        }
    }
    return NULL;
}

// zsysd pushed a new notification: store it (replacing one with the same id).
static void on_show(ZApp *app, const ZShownNotification *n, void *ud) {
    (void)app;
    ShadeState *s = ud;
    bool was_empty = active_count(s) == 0;
    Banner *b = banner_by_id(s, n->id);
    if (!b) {
        for (int i = 0; i < MAX_BANNERS; i++) {
            if (!s->banners[i].used) {
                b = &s->banners[i];
                break;
            }
        }
    }
    if (!b) {
        return;   // strip full
    }
    // A new post re-pops the heads-up even if the last one was swiped away.
    s->heads_up_hidden = false;
    if (s->banner_drag) {
        z_animated_set(s->banner_drag, 0.0f);
    }
    // First banner of a fresh heads-up: spring the cards strip in (slide+fade).
    if (was_empty && s->banner_enter) {
        z_animated_set(s->banner_enter, 0.0f);
        z_animated_spring_with(s->banner_enter, 1.0f, Z_SPRING_SNAPPY);
    }
    b->used = true;
    b->id = n->id;
    snprintf(b->app_id, sizeof(b->app_id), "%s", n->app_id ? n->app_id : "");
    snprintf(b->title, sizeof(b->title), "%s", n->title ? n->title : "");
    snprintf(b->body, sizeof(b->body), "%s", n->body ? n->body : "");
    snprintf(b->tap_route, sizeof(b->tap_route), "%s",
             n->tap_route ? n->tap_route : "");
    snprintf(b->action_id, sizeof(b->action_id), "%s",
             n->action_id ? n->action_id : "");
    snprintf(b->action_title, sizeof(b->action_title), "%s",
             n->action_title ? n->action_title : "");
}

// zsysd dropped a notification (cancel / tap / action): move its banner into the
// recently-dismissed history (most-recent-first, capped) and free the slot.
static void on_hide(ZApp *app, int64_t id, void *ud) {
    (void)app;
    ShadeState *s = ud;
    Banner *b = banner_by_id(s, id);
    if (!b) {
        return;
    }
    // Shift history down one and copy this banner to the front.
    int keep = s->n_history < MAX_HISTORY ? s->n_history : MAX_HISTORY - 1;
    for (int i = keep; i > 0; i--) {
        s->history[i] = s->history[i - 1];
    }
    s->history[0] = *b;
    if (s->n_history < MAX_HISTORY) {
        s->n_history++;
    }
    b->used = false;
    // Last banner cleared: reset the entrance spring so the next heads-up enters
    // fresh from off-anchor.
    if (active_count(s) == 0 && s->banner_enter) {
        z_animated_set(s->banner_enter, 0.0f);
    }
}

// Count active banners.
static int active_count(ShadeState *s) {
    int n = 0;
    for (int i = 0; i < MAX_BANNERS; i++) {
        if (s->banners[i].used) {
            n++;
        }
    }
    return n;
}

// --- notification card taps -----------------------------------------------
// Body tap: route the deep link ourselves (P9 intents) and drop the banner.
static void tap_body(ZApp *app, void *state, void *data) {
    (void)app;
    (void)state;
    Banner *b = data;
    if (b->tap_route[0]) {
        z_open_url(b->tap_route);
    }
    z_notify_report_tap(b->id);
}
// Action tap: report it so zsysd routes it to the poster, then drops the banner.
static void tap_action(ZApp *app, void *state, void *data) {
    (void)app;
    (void)state;
    Banner *b = data;
    z_notify_report_action(b->id, b->action_id);
}

// One notification card: the SHARED mark (system/common/notif_card.h), which the
// lock screen draws too, wrapped in this surface's own handlers. The whole card
// is the body-tap target; the action button is a deeper tap target nested inside
// (deepest handler wins). A non-interactive (history) card carries no handlers
// and renders dimmer.
// `card_w` is the width the caller will frame this card at — the notification's
// title and body are wrapped to it (they are app-supplied strings and were
// running off the edge before P45), and the action button is pinned to
// ZELTO_NOTIF_ACTION_W so the column beside it is known at build time.
static ZView notif_card(ZApp *app, float card_w, Banner *b, bool interactive) {
    ZView action = NULL;
    if (interactive && b->action_id[0]) {
        action = Frame(ZELTO_NOTIF_ACTION_W, 0.0f,
            OnTapData(tap_action, b,
                Background(Z_COLOR_PRIMARY,
                    CornerRadius(Z_RADIUS_CHIP,
                        Padding(12,
                            Foreground(Z_COLOR_ON_PRIMARY,
                                Weight(Z_WEIGHT_SEMIBOLD,
                                    Font(Z_FONT_SUBHEAD,
                                        Text("%s", b->action_title)))))))));
    }
    ZView card = zelto_notif_card(app, card_w, b->app_id, b->title, b->body,
                                  action, interactive);
    return interactive ? OnTapData(tap_body, b, card) : card;
}

// --- Control Center ---------------------------------------------------------
// Toggle handlers: flip the bool, persist it via the broker, recolour next
// rebuild (the broker echoes the set back through on_qs_setting too).
static void toggle_wifi(ZApp *app, void *state) {
    ShadeState *s = state;
    s->qs_wifi = !s->qs_wifi;
    z_setting_set_int("sys.wifi", s->qs_wifi);
    z_invalidate(app);
}
static void toggle_mute(ZApp *app, void *state) {
    ShadeState *s = state;
    s->qs_mute = !s->qs_mute;
    z_setting_set_int("sys.mute", s->qs_mute);
    z_invalidate(app);
}
static void toggle_bright(ZApp *app, void *state) {
    ShadeState *s = state;
    s->qs_bright = !s->qs_bright;
    z_setting_set_int("sys.bright", s->qs_bright);
    z_invalidate(app);
}
static void toggle_airplane(ZApp *app, void *state) {
    ShadeState *s = state;
    s->qs_airplane = !s->qs_airplane;
    z_setting_set_int("sys.airplane", s->qs_airplane);
    z_invalidate(app);
}
static void toggle_lock(ZApp *app, void *state) {
    ShadeState *s = state;
    s->qs_lock = !s->qs_lock;
    z_setting_set_int("sys.lock_enabled", s->qs_lock);
    z_invalidate(app);
}
static void toggle_motion(ZApp *app, void *state) {
    ShadeState *s = state;
    s->qs_motion = !s->qs_motion;
    z_setting_set_int("sys.reduce_motion", s->qs_motion);
    z_invalidate(app);
}

// The two continuous controls. Both quantise the slider's 0..1 position back to
// the integer the broker stores, and both short-circuit when the step has not
// changed — a drag fires on_change on every frame, and writing the same value to
// the broker fifty times a second would fan it out to every subscriber that many
// times.
static void slide_bright(ZApp *app, void *state, float v) {
    ShadeState *s = state;
    int64_t level = 1 + (int64_t)(v * 4.0f + 0.5f);
    level = level < 1 ? 1 : (level > 5 ? 5 : level);
    if (level == s->qs_brightness) {
        return;
    }
    s->qs_brightness = level;
    z_setting_set_int("sys.brightness", level);
    z_invalidate(app);
}

static void slide_volume(ZApp *app, void *state, float v) {
    ShadeState *s = state;
    int64_t vol = (int64_t)(v * 10.0f + 0.5f);
    vol = vol < 0 ? 0 : (vol > 10 ? 10 : vol);
    if (vol == s->qs_volume) {
        return;
    }
    s->qs_volume = vol;
    z_setting_set_int("sys.volume", vol);
    z_invalidate(app);
}

// The colours one toggle wears right now. The fill CROSS-FADES between off
// (SURFACE_3) and on (PRIMARY) on a spring-backed value (P32) rather than
// hard-swapping, and the INK travels with it: an "on" toggle is a LIGHT disc
// (Z_COLOR_PRIMARY is near-white, not a hue), so its mark has to go from light
// ink on a dark disc to DARK ink on a light one — fading only the fill leaves
// white-on-white the moment it lights up. The spring is IDENTITY-keyed so each
// toggle keeps its own animation across rebuilds. ZELTO_QS_ANIM=<0..1> pins
// every cross-fade mid-flight for a still shot.
typedef struct CcTint {
    ZColor bg, ink, label;
} CcTint;

static CcTint cc_tint(ZApp *app, uint64_t key, bool on) {
    ZAnimated *t = z_animated_keyed(app, key, on ? 1.0f : 0.0f);
    float goal = on ? 1.0f : 0.0f;
    if (z_animated_target(t) != goal) {
        z_animated_spring_with(t, goal, Z_SPRING_STANDARD);
    }
    const char *qa = getenv("ZELTO_QS_ANIM");
    if (qa && qa[0]) {
        z_animated_pin(t, (float)atof(qa));
    }
    float v = z_animated_get(t);
    CcTint c;
    c.bg = z_color_lerp(Z_COLOR_SURFACE_3, Z_COLOR_PRIMARY, v);
    c.ink = z_color_lerp(Z_COLOR_TEXT, Z_COLOR_ON_PRIMARY, v);
    c.label = z_color_lerp(Z_COLOR_TEXT_MUTED, Z_COLOR_TEXT, v);
    return c;
}

// One Control Center cell: a round toggle with its name under it. The mark is
// built by the caller (each glyph takes different arguments) with the ink this
// tint hands back. The label is OUTSIDE the disc, the way iOS labels the ones it
// labels — putting "Wi-Fi On" INSIDE a 68px circle is how you get a chip that
// says everything and shows nothing.
static ZView cc_cell(ZAction on_tap, CcTint t, ZView mark, const char *label) {
    // OnTap sits on the DISC, not on the disc-plus-label column: the press veil is
    // masked to the tapped node's own corner radius, so a handler on the column
    // paints a rounded BOX over a round button. The disc is the target on iOS too.
    return Grow(1.0f,
        VStack(
            OnTap(on_tap,
                Frame(CC_BTN, CC_BTN,
                    CornerRadius(CC_BTN * 0.5f,
                        Background(t.bg,
                            ZStack(mark, .align = Z_ALIGN_CENTER))))),
            Foreground(t.label,
                Weight(Z_WEIGHT_MEDIUM,
                    Font(Z_FONT_CAPTION2, Text("%s", label)))),
            .spacing = 8, .align = Z_ALIGN_CENTER));
}

// The toggle grid: 3 across, 2 down. Every one of these is a REAL brokered key
// that something in the system actuates — Wi-Fi and airplane gate the network
// stack (P19), brightness drives the dim scrim, lock arms the lock screen,
// reduce-motion collapses every spring. There are no decorative toggles here.
static ZView cc_grid(ZApp *app, ShadeState *s) {
    // Bind the handlers once, and pull each slider back in line with its brokered
    // value EXCEPT while it is being dragged — the broker echoes our own writes
    // back, and re-deriving the position from a quantised level mid-drag would
    // make the fill jump between steps under the finger.
    s->bright_slider.on_change = slide_bright;
    s->volume_slider.on_change = slide_volume;
    if (!s->bright_slider.dragging) {
        s->bright_slider.value = (float)(s->qs_brightness - 1) / 4.0f;
    }
    if (!s->volume_slider.dragging) {
        s->volume_slider.value = (float)s->qs_volume / 10.0f;
    }

    CcTint wifi = cc_tint(app, 0x7135F1u, s->qs_wifi);
    CcTint mute = cc_tint(app, 0x7135F2u, s->qs_mute);
    CcTint bright = cc_tint(app, 0x7135F3u, s->qs_bright);
    CcTint plane = cc_tint(app, 0x7135F4u, s->qs_airplane);
    CcTint lock = cc_tint(app, 0x7135F5u, s->qs_lock);
    CcTint motion = cc_tint(app, 0x7135F6u, s->qs_motion);

    ZView row1 = HStack(
        cc_cell(toggle_airplane, plane,
                zelto_glyph_airplane(CC_GLYPH, plane.ink), "Airplane"),
        cc_cell(toggle_wifi, wifi,
                zelto_glyph_wifi(CC_GLYPH, wifi.ink), "Wi-Fi"),
        cc_cell(toggle_mute, mute,
                zelto_glyph_speaker(CC_GLYPH, mute.ink, s->qs_mute), "Silent"),
        .spacing = 10, .align = Z_ALIGN_CENTER);
    ZView row2 = HStack(
        cc_cell(toggle_bright, bright,
                zelto_glyph_sun(CC_GLYPH, bright.ink), "Bright"),
        cc_cell(toggle_lock, lock,
                zelto_glyph_lock(CC_GLYPH, lock.ink), "Lock"),
        cc_cell(toggle_motion, motion,
                zelto_glyph_motion(CC_GLYPH, motion.ink), "Motion"),
        .spacing = 10, .align = Z_ALIGN_CENTER);

    // The two TALL sliders, below the toggles. This is the pair iOS leads its
    // Control Center with, and until P42 Zelto had neither: brightness was a
    // round on/off toggle (which cannot express a level at all) and volume was
    // not here. They are the tall shape rather than a thin rail because at this
    // size the control IS the target — you grab the slab anywhere and push.
    //
    // Flanked by GROWING spacers rather than relying on the parent VStack's
    // cross-axis align: the two toggle rows above are full-width (their cells are
    // Grow(1)), so the column is as wide as the sheet, and a content-sized row
    // dropped into it sits at the leading edge instead of under the toggles.
    ZView sliders = HStack(
        Grow(1.0f, Spacer()),
        Slider(app, &s->bright_slider,
               .tall = true, .length = CC_SLIDER_H, .thickness = CC_SLIDER_W,
               .glyph = zelto_glyph_sun(CC_GLYPH, Z_COLOR_TEXT_MUTED)),
        Slider(app, &s->volume_slider,
               .tall = true, .length = CC_SLIDER_H, .thickness = CC_SLIDER_W,
               .glyph = zelto_glyph_speaker(CC_GLYPH, Z_COLOR_TEXT_MUTED,
                                            s->qs_volume == 0)),
        Grow(1.0f, Spacer()),
        .spacing = 18, .align = Z_ALIGN_CENTER);

    return VStack(row1, row2, sliders, .spacing = 22, .align = Z_ALIGN_CENTER);
}

// --- Notification Center ----------------------------------------------------
// The label above a group of cards. Small, heavy, muted — the same "section
// header" every phone uses.
static ZView section_header(const char *text) {
    return Weight(Z_WEIGHT_SEMIBOLD,
        Foreground(Z_COLOR_TEXT_MUTED,
            Font(Z_FONT_FOOTNOTE, Text("%s", text))));
}

// The clock + date that head the Notification Center. This is where the time
// belongs now: the Control Center is a slab of controls and a clock on it is
// furniture, while the Notification Center is the "what happened while I was
// away" screen, and the first thing you want there is when.
static ZView nc_clock(void) {
    char clock[16] = "--:--";
    char date[32] = "";
    time_t t = time(NULL);
    struct tm tmv;
    if (localtime_r(&t, &tmv)) {
        strftime(clock, sizeof(clock), "%H:%M", &tmv);
        strftime(date, sizeof(date), "%A %d %B", &tmv);
    }
    return VStack(
        Weight(Z_WEIGHT_BOLD,
            Foreground(Z_COLOR_TEXT,
                Font(Z_FONT_LARGE_TITLE, Text("%s", clock)))),
        Foreground(Z_COLOR_TEXT_MUTED,
            Font(Z_FONT_SUBHEAD, Text("%s", date))),
        .spacing = 2, .align = Z_ALIGN_LEADING);
}

// --- pull gesture -----------------------------------------------------------
// The same handler drives both panels and both directions: a down-drag from the
// idle grab strip (or a banner) opens, an up-drag on the open panel/scrim closes.
// It tracks the pull value from whatever it was at the drag's begin, so it
// composes regardless of where the surface started. The travel distances are
// file-statics set each rebuild from the screen height — the surface may still be
// showing nothing when the drag begins, and each panel has its own height.
static float g_dist_cc = 1.0f;
static float g_dist_nc = 1.0f;

static float panel_dist(int panel) {
    float d = panel == PANEL_CC ? g_dist_cc : g_dist_nc;
    return d > 1.0f ? d : 1.0f;
}

static void on_shade_pan(ZApp *app, void *state, const ZPanEvent *e) {
    (void)app;
    ShadeState *s = state;
    if (!s->pull) {
        return;
    }
    if (e->phase == Z_PAN_BEGIN) {
        // THE SPLIT. While the panels are closed, which half of the top edge the
        // finger landed in decides which one comes down; once one is on its way
        // (or open) every further drag belongs to it, so a close-drag that starts
        // on the other half does not swap panels out from under the finger.
        if (s->panel == PANEL_NONE) {
            float w = (float)z_app_width(app);
            s->panel = (w > 1.0f && e->x >= w * 0.5f) ? PANEL_CC : PANEL_NC;
        }
        // Grab the (possibly still-settling) spring so the finger takes over from
        // its live value with no jump — the pull is interruptible mid-animation.
        s->pull_base = z_animated_grab(s->pull);
        s->dragging = true;
    } else if (e->phase == Z_PAN_CHANGED) {
        // Store the RAW pull (unclamped); the render rubber-bands the over-pull.
        z_animated_set(s->pull,
                       s->pull_base + e->translation_y / panel_dist(s->panel));
    } else {   // Z_PAN_END
        s->dragging = false;
        float v = z_animated_get(s->pull);
        bool open = v > 0.4f;
        if (e->velocity_y > 600.0f) {
            open = true;            // strong down-fling opens
        } else if (e->velocity_y < -600.0f) {
            open = false;           // strong up-fling closes
        }
        // Settle to the chosen end, carrying the finger velocity (px/s -> pull/s).
        z_animated_spring_velocity(s->pull, open ? 1.0f : 0.0f, Z_SPRING_STANDARD,
                                   e->velocity_y / panel_dist(s->panel));
    }
}

// Heads-up banner gesture (P33). The cards strip hosts BOTH a shade-pull and a
// swipe-to-dismiss, so latch the intent from the first real motion: a DOWN drag
// opens the NOTIFICATION CENTER (a banner is a notification — it belongs to that
// panel regardless of which half of the strip you grabbed), an UP drag lifts the
// strip 1:1 and dismisses the heads-up past a threshold/velocity (snapping back
// if short).
static void on_banner_pan(ZApp *app, void *state, const ZPanEvent *e) {
    ShadeState *s = state;
    if (!s->banner_drag) {
        return;
    }
    if (e->phase == Z_PAN_BEGIN) {
        s->banner_mode = 0;
        s->pull_base = s->pull ? z_animated_get(s->pull) : 0.0f;
        s->dragging = true;
        z_animated_grab(s->banner_drag);
        return;
    }
    // Latch direction once the finger has moved enough to be unambiguous.
    if (s->banner_mode == 0 && (e->translation_y > 6.0f || e->translation_y < -6.0f)) {
        s->banner_mode = e->translation_y > 0.0f ? 1 : 2;
        if (s->banner_mode == 1 && s->panel == PANEL_NONE) {
            s->panel = PANEL_NC;
        }
    }
    if (s->banner_mode == 1) {
        on_shade_pan(app, state, e);   // downward: this is a shade pull
        return;
    }
    if (s->banner_mode != 2) {
        return;                        // not yet decided (tiny motion)
    }
    // Upward: 1:1 lift toward dismissal.
    if (e->phase == Z_PAN_CHANGED) {
        z_animated_set(s->banner_drag, e->translation_y < 0.0f ? e->translation_y
                                                               : 0.0f);
    } else {   // Z_PAN_END
        s->dragging = false;
        float d = z_animated_get(s->banner_drag);
        if (d < -BANNER_DISMISS_THRESH || e->velocity_y < -700.0f) {
            // Hide the pop-over: the notifications stay active in the panel.
            s->heads_up_hidden = true;
            if (s->banner_enter) {
                z_animated_set(s->banner_enter, 0.0f);
            }
            z_animated_set(s->banner_drag, 0.0f);
        } else {
            z_animated_spring_velocity(s->banner_drag, 0.0f, Z_SPRING_STANDARD,
                                       e->velocity_y);   // snap back
        }
        s->banner_mode = 0;
    }
    z_invalidate(app);
}

// Tap the area outside the panel: spring it closed.
static void close_shade(ZApp *app, void *state) {
    ShadeState *s = state;
    if (s->pull) {
        z_animated_spring(s->pull, 0.0f);
    }
    z_invalidate(app);
}
// Absorb taps that land on the panel's own background (between widgets) so they
// do not fall through to the close scrim behind it.
static void absorb_tap(ZApp *app, void *state) {
    (void)app;
    (void)state;
}

// --- body -------------------------------------------------------------------
static ZView shade_body(ZApp *app, ShadeState *s) {
    // Subscribe as the notification sink on the first build (ctrl_fd exists by
    // now). Retained pull value allocated first + unconditionally for a stable id.
    if (!s->subscribed) {
        s->subscribed = true;
        z_notify_subscribe(app, on_show, on_hide, s);
        // Observe the brokered settings store so a toggle flipped in the Settings
        // app recolours our toggle live (both subscribe on the same ctrl_fd; the
        // broker fans settings_changed out to every observer).
        z_settings_observe(app, on_qs_setting, s);
    }
    s->pull = z_animated_value(app, 0.0f);
    s->banner_enter = z_animated_value(app, 0.0f);
    s->banner_drag = z_animated_value(app, 0.0f);
    ensure_qs(s);

    // Freeze-frame hook (P32): ZELTO_BANNER_ENTER=<0..1> pins the heads-up strip's
    // entrance spring, so a banner shot can be captured mid slide+fade regardless
    // of when the (spawn-driven) notification actually lands.
    const char *be_env = getenv("ZELTO_BANNER_ENTER");
    if (be_env && be_env[0]) {
        z_animated_pin(s->banner_enter, (float)atof(be_env));
    }
    // Swipe-dismiss freeze-frame (P33): ZELTO_BANNER_DRAG=<px> pins the strip at a
    // held upward drag (negative = toward dismissal), seating the entrance first so
    // the mid-drag lift+fade is shot-verifiable with no injected input.
    const char *bd_env = getenv("ZELTO_BANNER_DRAG");
    if (bd_env && bd_env[0]) {
        z_animated_set(s->banner_enter, 1.0f);
        z_animated_pin(s->banner_drag, (float)atof(bd_env));
        s->banner_mode = 2;
    }
    // ZELTO_BANNER_DEMO=1 fabricates a heads-up banner directly in the sink on the
    // first build, so the entrance transition is deterministically shot-verifiable
    // WITHOUT the flaky multi-process post->consent->grant->deliver path.
    static bool banner_demo_applied = false;
    if (!banner_demo_applied) {
        banner_demo_applied = true;
        if (getenv("ZELTO_BANNER_DEMO") && !s->banners[0].used) {
            Banner *b = &s->banners[0];
            b->used = true;
            b->id = 1;
            snprintf(b->app_id, sizeof(b->app_id), "os.zelto.pinger");
            snprintf(b->title, sizeof(b->title), "Ping");
            snprintf(b->body, sizeof(b->body), "You have a new ping");
            snprintf(b->action_id, sizeof(b->action_id), "ack");
            snprintf(b->action_title, sizeof(b->action_title), "Ack");
        }
    }

    // Headless test hook: ZELTO_SHADE_OPEN seeds a panel fully pulled down on the
    // first build, so each pull-down is screenshot-verifiable without driving a
    // (flaky) down-swipe. `cc` / `nc` pick the panel; a bare `1` means the Control
    // Center. Read once.
    static bool shade_open_applied = false;
    if (!shade_open_applied) {
        shade_open_applied = true;
        const char *so = getenv("ZELTO_SHADE_OPEN");
        if (so && so[0]) {
            s->panel = so[0] == 'n' ? PANEL_NC : PANEL_CC;
            z_animated_set(s->pull, 1.0f);
        }
        // Over-pull freeze-frame (P33): ZELTO_SHADE_PULL=<val> pins the RAW pull at
        // a value past 1 so the rubber-banded resistance past fully-open is
        // shot-verifiable (the render damps it via shade_display_pull).
        const char *sp = getenv("ZELTO_SHADE_PULL");
        if (sp && sp[0]) {
            if (s->panel == PANEL_NONE) {
                s->panel = PANEL_CC;
            }
            z_animated_pin(s->pull, (float)atof(sp));
        }
    }

    float pull_v = z_animated_get(s->pull);
    // The surface is ALWAYS the full area below the bar (anchored top+bottom), so
    // its size never changes mid-gesture — idle pass-through is done by shrinking
    // the INPUT region, not the surface, which would race pointer delivery.
    int full_h = z_app_height(app);
    if (full_h < GRAB_H + 1) {
        full_h = z_screen_height(app) - ZELTO_BAR_H;   // before the first configure
    }
    int w = z_app_width(app);
    g_dist_cc = CC_H;
    g_dist_nc = (float)full_h * NC_FRAC;
    int n_active = active_count(s);
    // The heads-up pop-over shows only while there are active banners AND it has
    // not been swiped away (a dismiss hides the pop-over but keeps the banners in
    // the panel). banner_mode 2 keeps it up through the dismiss drag itself.
    bool show_heads_up = n_active > 0 &&
                         (!s->heads_up_hidden || s->banner_mode == 2);
    bool expanded = pull_v > 0.001f || (s->dragging && s->banner_mode != 2);

    // ----- COLLAPSED: idle grab strip or heads-up banners -----
    if (!expanded) {
        s->panel = PANEL_NONE;   // settled shut: the next pull picks a panel again
        // Catch input only in the top strip; the rest of the full-height surface
        // is input-transparent, so taps below fall through to the app. (An
        // in-flight pull keeps its events via the compositor's pointer grab, so
        // the region narrowing here never interrupts a drag.)
        int strip_h = !show_heads_up ? GRAB_H : BANNER_STRIP_H;
        z_layer_set_input_region(app, 0, 0, w, strip_h);
        // Closed: no material. (The surface stays full-height and transparent, so
        // without this the compositor would keep blurring the whole screen behind
        // a panel that is no longer there.)
        z_backdrop(app, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        if (!show_heads_up) {
            // Idle: an INVISIBLE full-width grab strip. The old unified shade drew
            // a centred handle up here; two panels cannot share one centred handle
            // without lying about where the split is, and iOS draws nothing at all
            // — the status bar is the affordance (clock left = notifications,
            // status cluster right = controls). So the strip paints nothing and
            // only listens.
            ZView strip = OnPan(on_shade_pan,
                Frame((float)w, (float)GRAB_H,
                      Rect(.color = z_rgba(0, 0, 0, 0))));
            return Fill(VStack(strip, Spacer(),
                               .spacing = 0, .align = Z_ALIGN_CENTER));
        }
        // Banners up: the heads-up cards strip (opaque cards over transparent),
        // sliding down + fading in on the entrance spring (P32).
        z_full_repaint(app);
        ZStackOpts col = {.padding = 8, .spacing = 8, .align = Z_ALIGN_LEADING};
        // The strip's cards span the surface less its own 8px padding on each
        // side. WrapText needs this at BUILD time, so it is derived here rather
        // than left to layout.
        float card_w = (float)w - 2.0f * 8.0f;
        int k = 0;
        for (int i = 0; i < MAX_BANNERS && k < Z_MAX_CHILDREN - 1; i++) {
            if (s->banners[i].used) {
                col.children[k++] = notif_card(app, card_w, &s->banners[i], true);
            }
        }
        col.children[k++] = Spacer();
        float be = z_animated_get(s->banner_enter);
        float bslide = (1.0f - be) * -24.0f;   // slides down from behind the bar
        // Swipe-to-dismiss (P33): an upward drag lifts the strip 1:1 and fades it
        // out toward dismissal; combined with the entrance slide/fade above.
        float bd = z_animated_get(s->banner_drag);
        float dprog = bd < 0.0f ? (-bd / BANNER_DISMISS_DIST) : 0.0f;
        if (dprog > 1.0f) {
            dprog = 1.0f;
        }
        return OnPan(on_banner_pan,
            Opacity(be * (1.0f - dprog),
                OffsetXY(0.0f, bslide + bd,
                    Fill(z_stack(Z_AXIS_VERTICAL, &col)))));
    }

    // ----- EXPANDED: the whole surface takes input; the panel slides down -----
    z_layer_set_input_region(app, 0, 0, 0, 0);   // whole surface
    z_full_repaint(app);   // moving opaque panel over transparent: full repaint

    bool cc = s->panel == PANEL_CC;
    float panel_h = cc ? g_dist_cc : g_dist_nc;
    float panel_x = cc ? CC_MARGIN : 0.0f;
    float panel_w = cc ? (float)w - 2.0f * CC_MARGIN : (float)w;

    // The panel's contents. The Control Center is a toggle grid and nothing else;
    // the Notification Center is the clock, then the active notifications and a
    // little recently-dismissed history. Neither list scrolls yet (no Scroll
    // here), so a drag anywhere on the panel still controls the pull.
    ZStackOpts list = {.spacing = 12, .padding = 22, .align = Z_ALIGN_LEADING};
    // Same derivation as the banner strip: the panel's width less the list's own
    // padding is the width a notification card is laid out at.
    float card_w = panel_w - 2.0f * 22.0f;
    int li = 0;
    if (cc) {
        list.children[li++] = cc_grid(app, s);
    } else {
        list.children[li++] = nc_clock();
        list.children[li++] = Frame(1.0f, 6.0f, Rect(.color = z_rgba(0, 0, 0, 0)));
        list.children[li++] = section_header("Notifications");
        bool any = false;
        for (int i = 0; i < MAX_BANNERS && li < Z_MAX_CHILDREN - 4; i++) {
            if (s->banners[i].used) {
                list.children[li++] = notif_card(app, card_w, &s->banners[i], true);
                any = true;
            }
        }
        for (int i = 0; i < s->n_history && li < Z_MAX_CHILDREN - 3; i++) {
            list.children[li++] = notif_card(app, card_w, &s->history[i], false);
            any = true;
        }
        if (!any) {
            list.children[li++] = Foreground(Z_COLOR_TEXT_FAINT,
                Text("No notifications"));
        }
    }
    list.children[li++] = Spacer();
    list.children[li++] = OnTap(close_shade,
        HStack(Spacer(),
               Rect(.color = Z_COLOR_TEXT_MUTED,
                    .width = 64, .height = 5, .radius = 3),
               Spacer(), .spacing = 0, .align = Z_ALIGN_CENTER));

    // Rubber-band the over-pull: past fully-open the panel resists instead of
    // sliding off the bottom, and snaps back on release (raw pull -> display pull).
    float slide = (shade_display_pull(pull_v) - 1.0f) * panel_h;

    // The panel is a MATERIAL, not an opaque box: ask the compositor to blur the
    // scene beneath the rectangle the panel occupies (it moves with the pull, so
    // this is re-declared every frame of the drag — z_backdrop dedups a still one),
    // then paint the translucent tint over that blur. Without a compositor that
    // implements it, the tint alone still reads as a panel. The Control Center is
    // the denser material of the two: it is an object you operate, while the
    // Notification Center is a view of your screen with cards on it.
    z_backdrop(app, panel_x, slide, panel_w, panel_h, Z_RADIUS_SHEET);

    ZView panel = Frame(panel_w, panel_h,
        OnTap(absorb_tap,
            CornerRadius(Z_RADIUS_SHEET,
                Background(cc ? Z_COLOR_MATERIAL_THICK : Z_COLOR_MATERIAL_REGULAR,
                    ZStack(
                        Fill(z_stack(Z_AXIS_VERTICAL, &list)),
                        .align = Z_ALIGN_CENTER)))));
    if (cc) {
        // Inset from the edges: the Control Center floats, so it needs a gutter on
        // both sides. A fixed gutter is an empty Rect, NOT Frame(w, h, Spacer()) —
        // a Spacer keeps its grow flag through Frame and would eat the row.
        panel = HStack(Frame(CC_MARGIN, 1.0f, Rect(.color = z_rgba(0, 0, 0, 0))),
                       panel,
                       Frame(CC_MARGIN, 1.0f, Rect(.color = z_rgba(0, 0, 0, 0))),
                       .spacing = 0, .align = Z_ALIGN_LEADING);
    }

    // Back: a bg-less full-surface scrim — drag controls the pull, tap closes,
    // and it paints nothing so the app shows through where the panel isn't.
    ZView scrim = OnPan(on_shade_pan, OnTap(close_shade, Fill(Spacer())));
    // Front: the panel pinned to the top over a transparent filler, slid by the
    // pull Offset; a drag on it also controls the pull.
    ZView front = Offset(NULL, slide,
        OnPan(on_shade_pan, Fill(VStack(panel, Spacer(),
                                        .spacing = 0, .align = Z_ALIGN_LEADING))));

    return ZStack(scrim, front, .align = Z_ALIGN_CENTER);
}

// OVERLAY layer anchored to ALL FOUR edges (so it always fills the area below the
// status bar — margin_top keeps the bar visible — without ever resizing),
// NOT keyboard-exclusive. It is visually transparent except where it paints (the
// banners / a pulled-down panel); the app shows through everywhere else. What it
// CATCHES is set by its input region each rebuild (shade_body via
// z_layer_set_input_region): just the top strip when idle, the whole surface once
// pulled — so when idle it covers only the top gesture inset and the app owns the
// rest. exclusive_zone 0 reserves nothing.
Z_LAYER_APP(ShadeState, shade_body,
            .layer = Z_LAYER_OVERLAY,
            .anchor = Z_ANCHOR_TOP | Z_ANCHOR_BOTTOM | Z_ANCHOR_LEFT |
                      Z_ANCHOR_RIGHT,
            .exclusive_zone = 0,
            .margin_top = ZELTO_BAR_H,
            .keyboard = false)
