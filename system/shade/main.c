// Zelto System UI — notification + quick-settings shade.
//
// A system-wide pull-down panel (the way Android's notification shade works): an
// always-mapped OVERLAY layer-shell app anchored to the top edge that catches a
// down-swipe over ANY running app, expands into a full quick-settings +
// notifications panel, and merges in the heads-up banner sink. One surface owns
// three jobs:
//
//   1. HEADS-UP BANNERS (P10). It subscribes to zsysd as the single notification
//      sink (z_notify_subscribe); a posted notification pops as a banner card
//      strip across the top, over whatever app is in front.
//   2. THE PULL-DOWN. A thin always-present grab strip at the very top catches a
//      down-swipe and pulls the panel down over the app; an up-drag / scrim tap
//      retracts it. The QUICK SETTINGS (clock + Wi-Fi/Mute/Bright toggle chips,
//      migrated out of the launcher) sit at the top of the panel; the current +
//      recently-dismissed notifications list below them.
//
// THE SURFACE-FOOTPRINT CHOREOGRAPHY (the real layer-shell problem). The software
// renderer cannot produce a translucent SURFACE — any node it paints writes
// opaque final alpha, and any pixel it leaves untouched stays fully transparent
// (the compositor's wlr_scene then blends it as see-through to the app beneath).
// So "reserve/cover nothing when idle, take input + cover when expanded" is done
// by RESIZING the surface (z_layer_resize), not by painting transparent — a layer
// surface's footprint is simultaneously what it covers AND its input region. We
// drive three footprints every rebuild:
//
//   - IDLE (no banner, not pulled): a GRAB_H thin strip. It covers only the top
//     gesture inset of the app and catches the down-swipe; everything below is the
//     app, untouched. (The app's very top GRAB_H px can't be tapped while idle —
//     the documented cost of a top-edge gesture region, like Android's.)
//   - BANNER (a heads-up is up, not pulled): BANNER_STRIP_H, the cards strip.
//   - EXPANDED (being pulled / open): the FULL area below the status bar. The
//     opaque QS+notifications panel is pinned to the top and slid down by an
//     Offset bound to a spring `pull` value; the region the panel has not reached
//     is left UNPAINTED so the app shows through there, and a bg-less full-surface
//     scrim catches the tap/drag that closes it (input without cover).
//
// Because the panel is in motion and the surface mixes opaque + transparent, a
// frame mid-pull forces a full repaint (the partial-repaint path under-damages a
// big translated subtree and re-blends transparent regions wrong).
//
// QUICK-SETTINGS TOGGLE STATE now lives in the zsysd-brokered settings store
// (z_setting_get/set_int on the sys.* keys), NOT the shade's private prefs. A
// second reader/writer appeared (the Settings app, os.zelto.settings), so the
// toggles were promoted from the shade's own storage to a single brokered source
// of truth (P18): the shade z_settings_observe()s, so a flip in Settings
// recolours the chip here live (and a flip here is broadcast back to Settings),
// and the broker persists every change to /var/zelto so it survives a reboot.
// See docs/guides/settings.md + the P17/P18 memory notes.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zelto/ui.h>

#include "common/app_icons.h"

#define MAX_BANNERS 8
#define MAX_HISTORY 6   // recently-dismissed notifications kept for the panel list

// Footprints (px). The status bar owns the top BAR_H (we float below it via
// margin_top so it stays visible); GRAB_H is the idle down-swipe gesture strip;
// BANNER_STRIP_H is the heads-up cards strip. EXPANDED resizes to the full area
// below the bar (computed from the screen height at runtime).
#define BAR_H 40
#define GRAB_H 72   // generous top-edge gesture inset so a down-swipe reliably
                    // starts on the strip and the expand-to-full happens before
                    // the finger leaves it (input then stays over the full surface)
#define BANNER_STRIP_H 150

// Heads-up swipe-to-dismiss (P33): an upward drag past THRESH px (or a strong
// up-fling) hides the pop-over; DIST is the travel the strip fades out over.
#define BANNER_DISMISS_THRESH 30.0f
#define BANNER_DISMISS_DIST 100.0f

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
    // unconditionally every rebuild so its retained identity is stable.
    ZAnimated *pull;
    float pull_base;                 // pull value captured at a drag's begin
    bool dragging;                   // a pull/close drag is in flight

    // Heads-up banner entrance (P32). Springs 0 -> 1 when the first banner
    // arrives (the cards strip slides down + fades in) and resets to 0 once the
    // last banner clears, so the next heads-up enters fresh. Bound to the strip's
    // Offset + Opacity in the collapsed-banner branch.
    ZAnimated *banner_enter;

    // Heads-up swipe-to-dismiss (P33). `banner_drag` is the finger's live vertical
    // translation of the cards strip (<=0 dragged up toward dismissal), sprung back
    // on a short release. `banner_mode` latches the drag's intent on the first real
    // motion: an UP drag dismisses the heads-up, a DOWN drag opens the shade (the
    // same surface hosts both gestures). `heads_up_hidden` collapses the strip after
    // a dismiss — the notifications stay active in the panel (swiping a heads-up away
    // only hides the pop-over, the Android/iOS way); a new post clears it.
    ZAnimated *banner_drag;
    int banner_mode;   // 0 undecided, 1 pull-shade, 2 dismiss-heads-up
    bool heads_up_hidden;

    // Quick-settings toggles, loaded once from prefs and persisted on flip.
    bool qs_loaded;
    bool qs_wifi, qs_mute, qs_bright;
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

// Load the quick-settings toggles once from the brokered settings store
// (defaults Wi-Fi + bright on, mute off). These are the SAME sys.* keys the
// Settings app reads/writes — one source of truth. z_settings_observe (in
// shade_body) then keeps them live: a flip in Settings recolours the chip here
// without a reboot, and a flip here is broadcast back to Settings.
static void ensure_qs(ShadeState *s) {
    if (s->qs_loaded) {
        return;
    }
    s->qs_loaded = true;
    s->qs_wifi = z_setting_get_int("sys.wifi", 1) != 0;
    s->qs_mute = z_setting_get_int("sys.mute", 0) != 0;
    s->qs_bright = z_setting_get_int("sys.bright", 1) != 0;
}

// A setting changed somewhere (this shade or the Settings app): re-read the
// quick-settings bool it maps to and repaint the chip. Idempotent — applying a
// value the shade just set is a harmless no-op, so observing our own set (the
// broker fans out to every subscriber) neither loops nor double-toggles.
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

// One notification card: app id caption + title + body and, when present and the
// card is interactive, an action button. The whole card is the body-tap target;
// the action button is a deeper tap target nested inside (deepest handler wins).
// A non-interactive (history) card carries no handlers and renders dimmer.
static ZView notif_card(Banner *b, bool interactive) {
    ZColor cap = interactive ? Z_COLOR_TEXT_MUTED
                             : Z_COLOR_TEXT_FAINT;
    ZColor bodyc = interactive ? Z_COLOR_TEXT
                               : Z_COLOR_TEXT_MUTED;
    ZColor bg = interactive ? Z_COLOR_SURFACE_2
                            : Z_COLOR_SURFACE;

    // The posting app's icon (resolved from app_id via its manifest, like
    // Recents), falling back to the shared Placeholder if it has none or it won't
    // load. App icons are square, so a plain aspect-fit Image is right here.
    char ipath[256];
    const char *icon =
        (zelto_icon_for_app_id(b->app_id, ipath, sizeof(ipath)) &&
         z_image_loads(ipath))
            ? ipath
            : zelto_placeholder_icon();

    ZStackOpts row = {.padding = 16, .spacing = 16, .align = Z_ALIGN_CENTER};
    int k = 0;
    row.children[k++] = Frame(40.0f, 40.0f, CornerRadius(10.0f, Image(icon)));
    row.children[k++] =
        VStack(
            Foreground(cap, Font(Z_FONT_CAPTION, Text("%s", b->app_id))),
            Foreground(Z_COLOR_TEXT_INV,
                Font(Z_FONT_CALLOUT, Text("%s", b->title))),
            Foreground(bodyc, Text("%s", b->body)),
            .spacing = 4, .align = Z_ALIGN_LEADING);
    row.children[k++] = Spacer();
    if (interactive && b->action_id[0]) {
        row.children[k++] = OnTapData(tap_action, b,
            Background(Z_COLOR_PRIMARY,
                CornerRadius(12,
                    Padding(14,
                        Foreground(Z_COLOR_TEXT_INV,
                            Font(Z_FONT_CALLOUT,
                                Text("%s", b->action_title)))))));
    }

    ZView card = Shadow(interactive ? Z_ELEV_2 : Z_ELEV_1, Background(bg,
        CornerRadius(16, z_stack(Z_AXIS_HORIZONTAL, &row))));
    return interactive ? OnTapData(tap_body, b, card) : card;
}

// --- quick-settings ---------------------------------------------------------
// Toggle handlers: flip the bool, persist it, recolour next rebuild.
static void toggle_wifi(ZApp *app, void *state) {
    ShadeState *s = state;
    s->qs_wifi = !s->qs_wifi;
    z_setting_set_int("sys.wifi", s->qs_wifi);   // broker persists + broadcasts
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

// One quick-settings toggle chip: a rounded label whose fill CROSS-FADES between
// off (SURFACE_3) and on (PRIMARY) on a spring-backed value (P32) instead of
// hard-swapping, and flips on tap. The spring is IDENTITY-keyed (`key`) so each
// chip keeps its own animation across rebuilds. ZELTO_QS_ANIM=<0..1> pins every
// chip's cross-fade mid-flight for a still shot.
static ZView qs_chip(ZApp *app, uint64_t key, ZAction on_tap, const char *label,
                     bool on) {
    ZAnimated *t = z_animated_keyed(app, key, on ? 1.0f : 0.0f);
    float goal = on ? 1.0f : 0.0f;
    if (z_animated_target(t) != goal) {
        z_animated_spring_with(t, goal, Z_SPRING_STANDARD);
    }
    const char *qa = getenv("ZELTO_QS_ANIM");
    if (qa && qa[0]) {
        z_animated_pin(t, (float)atof(qa));
    }
    ZColor bg = z_color_lerp(Z_COLOR_SURFACE_3, Z_COLOR_PRIMARY,
                             z_animated_get(t));
    return Grow(1.0f,
        OnTap(on_tap,
            Background(bg,
                CornerRadius(16.0f,
                    Padding(16.0f,
                        Foreground(Z_COLOR_TEXT_INV,
                            Font(Z_FONT_CALLOUT,
                                Text("%s %s", label, on ? "On" : "Off"))))))));
}

// The quick-settings block: a big clock + a row of toggle chips.
static ZView qs_block(ZApp *app, ShadeState *s) {
    char clock[16] = "--:--";
    time_t t = time(NULL);
    struct tm tmv;
    if (localtime_r(&t, &tmv)) {
        strftime(clock, sizeof(clock), "%H:%M", &tmv);
    }
    return VStack(
        Foreground(Z_COLOR_TEXT_INV,
            Font(Z_FONT_LARGE_TITLE, Text("%s", clock))),
        HStack(
            qs_chip(app, 0x7135F1u, toggle_wifi, "Wi-Fi", s->qs_wifi),
            qs_chip(app, 0x7135F2u, toggle_mute, "Mute", s->qs_mute),
            qs_chip(app, 0x7135F3u, toggle_bright, "Bright", s->qs_bright),
            .spacing = 12, .align = Z_ALIGN_CENTER),
        .spacing = 16, .align = Z_ALIGN_CENTER);
}

// --- pull gesture -----------------------------------------------------------
// The same handler drives both directions: a down-drag from the idle grab strip
// (or a banner) opens, an up-drag on the open panel/scrim closes. It tracks the
// pull value from whatever it was at the drag's begin, so it composes regardless
// of where the surface started. `dist` (the panel slide distance) is captured in
// a file-static set each rebuild from the screen height — the surface itself may
// still be the collapsed strip when the drag begins.
static float g_pull_dist = 1.0f;

static void on_shade_pan(ZApp *app, void *state, const ZPanEvent *e) {
    (void)app;
    ShadeState *s = state;
    if (!s->pull) {
        return;
    }
    float dist = g_pull_dist > 1.0f ? g_pull_dist : 1.0f;
    if (e->phase == Z_PAN_BEGIN) {
        // Grab the (possibly still-settling) spring so the finger takes over from
        // its live value with no jump — the pull is interruptible mid-animation.
        s->pull_base = z_animated_grab(s->pull);
        s->dragging = true;
    } else if (e->phase == Z_PAN_CHANGED) {
        // Store the RAW pull (unclamped); the render rubber-bands the over-pull.
        z_animated_set(s->pull, s->pull_base + e->translation_y / dist);
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
                                   e->velocity_y / dist);
    }
}

// Heads-up banner gesture (P33). The cards strip hosts BOTH the shade-pull and a
// swipe-to-dismiss, so latch the intent from the first real motion: a DOWN drag
// opens the shade (delegate to the pull handler), an UP drag lifts the strip 1:1
// and dismisses the heads-up past a threshold/velocity (snapping back if short).
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

// Tap the dimmed area outside the panel: spring the shade closed.
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
        // app recolours our quick-settings chip live (both subscribe on the same
        // ctrl_fd; the broker fans settings_changed out to every observer).
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

    // Headless test hook: ZELTO_SHADE_OPEN=1 seeds the panel fully pulled down on
    // the first build, so the expanded quick-settings + notifications shade is
    // screenshot-verifiable without driving a (flaky) down-swipe. Read once.
    static bool shade_open_applied = false;
    if (!shade_open_applied) {
        shade_open_applied = true;
        const char *so = getenv("ZELTO_SHADE_OPEN");
        if (so && so[0] == '1') {
            z_animated_set(s->pull, 1.0f);
        }
        // Over-pull freeze-frame (P33): ZELTO_SHADE_PULL=<val> pins the RAW pull at
        // a value past 1 so the rubber-banded resistance past fully-open is
        // shot-verifiable (the render damps it via shade_display_pull).
        const char *sp = getenv("ZELTO_SHADE_PULL");
        if (sp && sp[0]) {
            z_animated_pin(s->pull, (float)atof(sp));
        }
    }

    float pull_v = z_animated_get(s->pull);
    // The surface is ALWAYS the full area below the bar (anchored top+bottom), so
    // its size never changes mid-gesture — idle pass-through is done by shrinking
    // the INPUT region, not the surface, which would race pointer delivery.
    int full_h = z_app_height(app);
    if (full_h < GRAB_H + 1) {
        full_h = z_screen_height(app) - BAR_H;   // before the first configure
    }
    int w = z_app_width(app);
    int panel_h = (int)((float)full_h * 0.85f);   // panel covers most of it
    g_pull_dist = (float)panel_h;
    int n_active = active_count(s);
    // The heads-up pop-over shows only while there are active banners AND it has
    // not been swiped away (a dismiss hides the pop-over but keeps the banners in
    // the panel). banner_mode 2 keeps it up through the dismiss drag itself.
    bool show_heads_up = n_active > 0 &&
                         (!s->heads_up_hidden || s->banner_mode == 2);
    bool expanded = pull_v > 0.001f || (s->dragging && s->banner_mode != 2);

    // ----- COLLAPSED: idle grab strip or heads-up banners -----
    if (!expanded) {
        // Catch input only in the top strip; the rest of the full-height surface
        // is input-transparent, so taps below fall through to the app. (An
        // in-flight pull keeps its events via the compositor's pointer grab, so
        // the region narrowing here never interrupts a drag.)
        int strip_h = !show_heads_up ? GRAB_H : BANNER_STRIP_H;
        z_layer_set_input_region(app, 0, 0, w, strip_h);
        if (!show_heads_up) {
            // Idle: a thin top strip with a faint centred grab handle, the rest
            // transparent. The OnPan strip catches the down-swipe.
            ZView strip = OnPan(on_shade_pan,
                Frame((float)w, (float)GRAB_H,
                    ZStack(Rect(.color = Z_COLOR_TEXT_MUTED,
                                .width = 64, .height = 5, .radius = 3),
                           .align = Z_ALIGN_CENTER)));
            return Fill(VStack(strip, Spacer(),
                               .spacing = 0, .align = Z_ALIGN_CENTER));
        }
        // Banners up: the heads-up cards strip (opaque cards over transparent),
        // sliding down + fading in on the entrance spring (P32).
        z_full_repaint(app);
        ZStackOpts col = {.padding = 8, .spacing = 8, .align = Z_ALIGN_LEADING};
        int k = 0;
        for (int i = 0; i < MAX_BANNERS && k < Z_MAX_CHILDREN - 1; i++) {
            if (s->banners[i].used) {
                col.children[k++] = notif_card(&s->banners[i], true);
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

    // The panel content: quick settings, a section header, then the active
    // notifications and a little dismissed history. A Spacer pushes a close
    // handle to the panel's bottom edge. The list is capped (no Scroll yet) so a
    // drag anywhere on the panel still controls the pull, not a scroll.
    ZStackOpts list = {.spacing = 10, .padding = 22, .align = Z_ALIGN_LEADING};
    int li = 0;
    list.children[li++] = qs_block(app, s);
    list.children[li++] = Foreground(Z_COLOR_TEXT_MUTED,
        Font(Z_FONT_CAPTION, Text("NOTIFICATIONS")));
    bool any = false;
    for (int i = 0; i < MAX_BANNERS && li < Z_MAX_CHILDREN - 4; i++) {
        if (s->banners[i].used) {
            list.children[li++] = notif_card(&s->banners[i], true);
            any = true;
        }
    }
    for (int i = 0; i < s->n_history && li < Z_MAX_CHILDREN - 3; i++) {
        list.children[li++] = notif_card(&s->history[i], false);
        any = true;
    }
    if (!any) {
        list.children[li++] = Foreground(Z_COLOR_TEXT_FAINT,
            Text("No notifications"));
    }
    list.children[li++] = Spacer();
    list.children[li++] = OnTap(close_shade,
        Rect(.color = Z_COLOR_TEXT_MUTED,
             .width = 64, .height = 5, .radius = 3));

    // The opaque panel, fixed to (full width x panel_h). Frame sizes the depth
    // wrapper (no padding -> no inflation); the inner VStack fills it and insets
    // its own content. absorb_tap keeps a panel-background tap from closing.
    ZView panel = Frame((float)w, (float)panel_h,
        OnTap(absorb_tap,
            Background(Z_COLOR_BG,
                ZStack(
                    Fill(z_stack(Z_AXIS_VERTICAL, &list)),
                    .align = Z_ALIGN_CENTER))));

    // Rubber-band the over-pull: past fully-open the panel resists instead of
    // sliding off the bottom, and snaps back on release (raw pull -> display pull).
    float slide = (shade_display_pull(pull_v) - 1.0f) * (float)panel_h;

    // Back: a bg-less full-surface scrim — drag controls the pull, tap closes,
    // and it paints nothing so the app shows through where the panel isn't.
    ZView scrim = OnPan(on_shade_pan, OnTap(close_shade, Fill(Spacer())));
    // Front: the panel pinned to the top over a transparent filler, slid by the
    // pull Offset; a drag on it also controls the pull (Android-style).
    ZView front = Offset(NULL, slide,
        OnPan(on_shade_pan, Fill(VStack(panel, Spacer(),
                                        .spacing = 0, .align = Z_ALIGN_CENTER))));

    return ZStack(scrim, front, .align = Z_ALIGN_CENTER);
}

// OVERLAY layer anchored to ALL FOUR edges (so it always fills the area below the
// BAR_H status bar — margin_top keeps the bar visible — without ever resizing),
// NOT keyboard-exclusive. It is visually transparent except where it paints (the
// grab handle / banners / pulled-down panel); the app shows through everywhere
// else. What it CATCHES is set by its input region each rebuild (shade_body via
// z_layer_set_input_region): just the top strip when idle, the whole surface once
// pulled — so when idle it covers only the top gesture inset and the app owns the
// rest. exclusive_zone 0 reserves nothing.
Z_LAYER_APP(ShadeState, shade_body,
            .layer = Z_LAYER_OVERLAY,
            .anchor = Z_ANCHOR_TOP | Z_ANCHOR_BOTTOM | Z_ANCHOR_LEFT |
                      Z_ANCHOR_RIGHT,
            .exclusive_zone = 0,
            .margin_top = BAR_H,
            .keyboard = false)
