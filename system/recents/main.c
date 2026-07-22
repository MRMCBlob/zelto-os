// Zelto System UI — the App Switcher (P40 stage 2).
//
// Reached by dragging up from the home indicator and pausing (system/homebar).
// An OVERLAY layer-shell modal over a blurred backdrop, listing every running app
// window from z_running_apps() (wlr-foreign-toplevel-management, P7).
//
// This used to be an Android-style overview: a vertical list of 96px rows, each
// with a red X on the right. That shape is wrong for the gesture that reaches it.
// You arrive here already dragging, with a thumb halfway up the screen, and what
// you want is the app you were in two apps ago — a decision you make from the
// SHAPE of the thing, not from reading a row of labels. So the switcher is now
// what iOS's is: a horizontal deck of large cards, one per app, filling most of
// the screen, that you swipe through with the same thumb that opened them, and
// FLICK UP to close. The red X is gone; a close button is a mouse affordance, and
// spending a 56px target on "destroy" next to the target for "open" is how you
// close the app you meant to resume.
//
// THE THREE GESTURES, all read off one OnPan over the whole surface, with the
// axis latched on the first unambiguous motion (the two must not fight):
//   - horizontal : page the deck. The scroll is in CARD-INDEX units, so one
//                  card's travel is one unit; past either end it RUBBER-BANDS
//                  (z_rubber_band, the same Apple curve the shade over-pull and
//                  the launcher carousel use) instead of hard-stopping, and on
//                  release it settles with z_animated_spring_velocity carrying the
//                  finger's speed — a flick hands off to the spring instead of
//                  stopping dead and restarting, exactly as the volume HUD's
//                  dismiss does.
//   - vertical up: lift the CENTRED card 1:1 with the finger and, past a
//                  threshold or on a fast flick, close that window (z_task_close).
//                  A short release springs it back.
//   - tap a card : z_task_activate(that window) + dismiss (the overlay quits so
//                  the activated app shows). Tap the backdrop / Escape: dismiss.
//
// WHAT IS ON A CARD. A picture of the WINDOW, as it looked when you last left
// it (P42). wlr-screencopy can only capture an OUTPUT, and a backgrounded window
// is by definition not on the output, so this needed a protocol of its own:
// zcomp photographs each window at the moment it stops being the foreground one
// and keeps the image, and z_snapshot() hands it over here (compositor/src/
// capture.c, protocols/zelto-toplevel-capture-v1.xml).
//
// The picture is deliberately STALE — the window is not being drawn while it is
// backgrounded — which is exactly what makes it useful: you recognise the thing
// you were doing. A window that has never been backgrounded has no picture yet,
// and neither does anything running on a compositor without the protocol, so the
// old icon poster is still here as a live fallback rather than an error path.
//
// The home launcher is filtered out of the deck (it is Home, one flick away), so
// the switcher shows only app windows. See docs/platform/app-lifecycle.md.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zelto/ui.h>

#include "common/app_icons.h"

#define LAUNCHER_APP_ID "os.zelto.launcher"
#define XKB_KEY_Escape 0xff1b

#define MAX_CARDS 16
#define CARD_W_FRAC 0.74f    // a card spans most of the screen; neighbours peek in
#define CARD_H_FRAC 0.62f
#define CARD_GAP 26.0f       // px between two cards
#define HEADER_H 30.0f       // the icon + name strip above a card
#define HEADER_GAP 10.0f

// Flick-up-to-close. THRESH px of lift (or a fast up-fling) commits; DIST is the
// travel the card fades out over as it leaves.
#define CLOSE_THRESH 90.0f
#define CLOSE_DIST 260.0f
#define AXIS_SLOP 10.0f      // px before the drag commits to an axis

typedef struct RecentsState {
    bool inited;
    // The deck's position in CARD-INDEX units (0 = the first card centred), and
    // the centred card's live vertical drag in px (<= 0 = lifted toward closing).
    // Allocated first and unconditionally every rebuild for a stable identity.
    ZAnimated *scroll;
    ZAnimated *lift;
    float scroll_base;   // scroll captured at a drag's begin
    int axis;            // 0 undecided, 1 paging, 2 closing
    int focus;           // which card the close drag applies to
    bool dragging;
} RecentsState;

// The deck as it stands THIS build: the filtered running-app snapshot and the
// card pitch. The pan handler needs both, and it can fire before (or between)
// rebuilds, so they are file-statics set once per build — the same idiom the
// shade uses for its pull distance. The ZTask pointers are into z_running_apps'
// stable snapshot.
static const ZTask *g_cards[MAX_CARDS];
static int g_n_cards;
static float g_pitch = 1.0f;

// Map the RAW scroll (a drag may push it past either end) to the DISPLAYED one,
// rubber-banding the overshoot so the deck resists at the ends instead of
// sliding off into empty space. `dim` is 1 card, so a hard pull past the last
// card yields well under one card of slack. In range this is the identity.
static float deck_display(float raw, int n) {
    float max = n > 0 ? (float)(n - 1) : 0.0f;
    if (raw > max) {
        return max + z_rubber_band(raw - max, 1.0f);
    }
    if (raw < 0.0f) {
        return z_rubber_band(raw, 1.0f);
    }
    return raw;
}

// Tap a card: switch to that window, then close the overlay so it appears.
static void on_pick(ZApp *app, void *state, void *data) {
    (void)state;
    z_task_activate(app, (const ZTask *)data);
    z_app_quit(app);
}

// Dismiss without switching (backdrop tap / Escape).
static void on_dismiss(ZApp *app, void *state) {
    (void)state;
    z_app_quit(app);
}

static void on_key(ZApp *app, void *state, uint32_t keysym) {
    (void)state;
    if (keysym == XKB_KEY_Escape) {
        z_app_quit(app);
    }
}

// The flick-up committed: close the centred window. The card drops out of
// z_running_apps on the next build and the deck closes up behind it; closing the
// LAST one dismisses the switcher, because an empty deck is not a screen anyone
// wants to be looking at.
static void close_focused(ZApp *app, RecentsState *s) {
    bool was_last = g_n_cards <= 1;
    if (s->focus >= 0 && s->focus < g_n_cards && g_cards[s->focus]) {
        z_task_close(app, g_cards[s->focus]);
    }
    z_animated_set(s->lift, 0.0f);
    if (was_last) {
        z_app_quit(app);
        return;
    }
    // Closing the last card in the deck leaves the scroll one past the end; walk
    // it back so the deck does not settle in empty space.
    if (z_animated_get(s->scroll) > (float)(g_n_cards - 2)) {
        z_animated_spring_with(s->scroll, (float)(g_n_cards - 2), Z_SPRING_SNAPPY);
    }
    z_invalidate(app);
}

// One pan drives both the paging and the close, so the axis is latched from the
// first motion past the slop and held for the rest of the gesture — deciding it
// per-event would make a diagonal drag stutter between the two.
static void on_deck_pan(ZApp *app, void *state, const ZPanEvent *e) {
    RecentsState *s = state;
    if (!s->scroll || !s->lift) {
        return;
    }
    if (e->phase == Z_PAN_BEGIN) {
        s->axis = 0;
        s->dragging = true;
        // Grab both springs so the finger takes over from their live values with
        // no jump — the deck is interruptible mid-fling.
        s->scroll_base = z_animated_grab(s->scroll);
        z_animated_grab(s->lift);
        int f = (int)lroundf(s->scroll_base);
        s->focus = f < 0 ? 0 : (f >= g_n_cards ? g_n_cards - 1 : f);
        return;
    }
    if (s->axis == 0) {
        float ax = fabsf(e->translation_x), ay = fabsf(e->translation_y);
        if (ax < AXIS_SLOP && ay < AXIS_SLOP) {
            return;                       // still ambiguous
        }
        s->axis = ax >= ay ? 1 : 2;
    }

    if (s->axis == 1) {                   // ---- paging ----
        if (e->phase == Z_PAN_CHANGED) {
            // Drag left moves the deck FORWARD, so the scroll runs against x.
            z_animated_set(s->scroll,
                           s->scroll_base - e->translation_x / g_pitch);
        } else {
            s->dragging = false;
            float v = z_animated_get(s->scroll);
            float vel = -e->velocity_x / g_pitch;   // cards/s
            // A flick commits to the NEXT card in its direction even from a
            // standing start; a slow release just snaps to the nearest.
            float target = (vel < -0.8f)   ? floorf(v)
                           : (vel > 0.8f)  ? ceilf(v)
                                           : roundf(v);
            float max = g_n_cards > 0 ? (float)(g_n_cards - 1) : 0.0f;
            if (target < 0.0f) {
                target = 0.0f;
            } else if (target > max) {
                target = max;
            }
            z_animated_spring_velocity(s->scroll, target, Z_SPRING_SNAPPY, vel);
        }
    } else {                              // ---- close ----
        if (e->phase == Z_PAN_CHANGED) {
            // Up only: a downward drag on a card means nothing here, and letting
            // it push the card down would read as a second, fake gesture.
            z_animated_set(s->lift,
                           e->translation_y < 0.0f ? e->translation_y : 0.0f);
        } else {
            s->dragging = false;
            float d = z_animated_get(s->lift);
            if (d < -CLOSE_THRESH || e->velocity_y < -700.0f) {
                close_focused(app, s);
            } else {
                z_animated_spring_velocity(s->lift, 0.0f, Z_SPRING_STANDARD,
                                           e->velocity_y);   // snap back
            }
        }
    }
    z_invalidate(app);
}

// One card: an icon + name + lifecycle state strip, over a large rounded poster.
// The name is the manifest DISPLAY name ("Fetch"), not the window title, which
// for a libzelto app is its body-function symbol ("fetch_body"), and not the
// app_id, which is a database key.
static ZView switch_card(ZApp *app, const ZTask *t, float cw, float ch) {
    char name[128];
    const char *title =
        (t->app_id && zelto_name_for_app_id(t->app_id, name, sizeof(name)))
            ? name
            : (t->title ? t->title : (t->app_id ? t->app_id : "App"));

    char buf[192];
    const char *icon = (t->app_id &&
                        zelto_icon_for_app_id(t->app_id, buf, sizeof(buf)) &&
                        z_image_loads(buf))
                           ? buf
                           : zelto_placeholder_icon();

    ZView header = Frame(cw, HEADER_H,
        HStack(
            Frame(22.0f, 22.0f,
                CornerRadius(22.0f * Z_RADIUS_ICON, Image(icon))),
            Weight(Z_WEIGHT_MEDIUM,
                Foreground(Z_COLOR_TEXT, Font(Z_FONT_SUBHEAD,
                    Text("%s", title)))),
            Spacer(),
            Foreground(t->active ? Z_COLOR_TEXT_MUTED : Z_COLOR_TEXT_FAINT,
                Font(Z_FONT_CAPTION2, Text("%s", t->active ? "Active"
                                                           : "Paused"))),
            .spacing = 8, .align = Z_ALIGN_CENTER));

    // The card face. A picture of the WINDOW when the compositor has one — which
    // is the whole point of a switcher: you recognise the thing you were doing,
    // not which app you were doing it in. z_snapshot returns NULL until the
    // compositor has actually photographed that window (it does so when the
    // window leaves the foreground) and on any compositor without
    // zelto-toplevel-capture-v1, so the icon poster below is a live fallback,
    // not an error path — every card renders it at least once, on the frame
    // before the picture arrives.
    //
    // Cover, not fit: the snapshot has the SCREEN's aspect and the card is a
    // slightly different shape, so aspect-fit would letterbox it inside its own
    // rounded plate. Cover crops to fill, which is what a window preview should
    // do. Its corner radius is the SHEET radius, not the card radius: at this
    // size a 16px corner reads as a rectangle with the corners filed off, while
    // the sheet radius is the continuous curve every other full-screen surface
    // in the system uses.
    const char *shot = z_snapshot(app, t);
    float art = cw * 0.44f;
    ZView face =
        shot ? Frame(cw, ch, Cover(Image(shot)))
             : Frame(cw, ch,
                   ZStack(
                       Frame(art, art,
                           CornerRadius(art * Z_RADIUS_ICON, Image(icon))),
                       .align = Z_ALIGN_CENTER));
    ZView poster = Shadow(Z_ELEV_3,
        Background(Z_COLOR_SURFACE,
            CornerRadius(Z_RADIUS_SHEET, face)));

    return OnTapData(on_pick, (void *)t,
        VStack(header, poster, .spacing = HEADER_GAP, .align = Z_ALIGN_LEADING));
}

static ZView recents_body(ZApp *app, RecentsState *s) {
    // Retained cells first + unconditionally (call-order identity).
    s->scroll = z_animated_value(app, 0.0f);
    s->lift = z_animated_value(app, 0.0f);

    int n = 0;
    const ZTask *t = z_running_apps(app, &n);
    g_n_cards = 0;
    for (int i = 0; i < n && g_n_cards < MAX_CARDS; i++) {
        if (t[i].app_id && strcmp(t[i].app_id, LAUNCHER_APP_ID) == 0) {
            continue;   // Home is a flick away, not a card
        }
        g_cards[g_n_cards++] = &t[i];
    }

    float sw = (float)z_app_width(app);
    float sh = (float)z_app_height(app);
    float card_w = sw * CARD_W_FRAC;
    float card_h = sh * CARD_H_FRAC;
    float unit_h = HEADER_H + HEADER_GAP + card_h;
    g_pitch = card_w + CARD_GAP;

    // Headless test hooks. ZELTO_SWITCHER_SCROLL=<cards> pins the deck at a
    // position (1.5 = exactly between two cards; 2.4 past the last card shows the
    // rubber-band resisting), ZELTO_SWITCHER_LIFT=<px> pins the centred card at a
    // held upward drag — so both gestures are shot-verifiable with no injected
    // pointer input, like every other surface's freeze-frame.
    if (!s->inited) {
        s->inited = true;
        const char *sc = getenv("ZELTO_SWITCHER_SCROLL");
        if (sc && sc[0]) {
            z_animated_pin(s->scroll, (float)atof(sc));
        }
        const char *lf = getenv("ZELTO_SWITCHER_LIFT");
        if (lf && lf[0]) {
            z_animated_pin(s->lift, (float)atof(lf));
            s->axis = 2;
        }
    }

    float raw = z_animated_get(s->scroll);
    float disp = deck_display(raw, g_n_cards);
    float lift = z_animated_get(s->lift);
    float lprog = lift < 0.0f ? (-lift / CLOSE_DIST) : 0.0f;
    if (lprog > 1.0f) {
        lprog = 1.0f;
    }
    int focus = (int)lroundf(disp);
    if (focus < 0) {
        focus = 0;
    } else if (focus >= g_n_cards) {
        focus = g_n_cards - 1;
    }
    if (!s->dragging) {
        s->focus = focus;
    }

    // A moving deck over a blurred backdrop: partial damage under-repairs a big
    // translated subtree, so repaint in full while anything is in flight.
    z_full_repaint(app);
    // The switcher is a MATERIAL over whatever was on screen: ask the compositor
    // to blur the whole scene beneath us, then wash it with the scrim below. The
    // cards then read as objects lifted off a defocused background rather than as
    // tiles pasted onto a black sheet.
    z_backdrop(app, 0.0f, 0.0f, sw, sh, 0.0f);

    ZStackOpts deck = {.align = Z_ALIGN_CENTER};
    int k = 0;
    for (int i = 0; i < g_n_cards && k < Z_MAX_CHILDREN - 1; i++) {
        float dx = ((float)i - disp) * g_pitch;
        if (dx < -sw * 1.6f || dx > sw * 1.6f) {
            continue;   // fully off-screen: don't build it
        }
        // Only the centred card lifts. The neighbours sit back a touch so the
        // one under the thumb is unambiguous — the deck has no selection
        // highlight, so contrast IS the selection.
        float dy = (i == s->focus) ? lift : 0.0f;
        ZView cell = OffsetXY(dx, dy,
            Frame(card_w, unit_h,
                  switch_card(app, g_cards[i], card_w, card_h)));
        if (i == s->focus) {
            if (lprog > 0.001f) {
                cell = Opacity(1.0f - lprog, cell);   // fades out as it leaves
            }
        } else {
            cell = Opacity(0.68f, cell);
        }
        deck.children[k++] = cell;
    }
    if (k == 0) {
        deck.children[k++] = Foreground(Z_COLOR_TEXT_MUTED,
            Font(Z_FONT_CALLOUT, Text("No running apps")));
    }

    // The backdrop takes the tap that dismisses and the pan that drives both
    // gestures; the cards are deeper tap targets inside it (deepest wins).
    return OnPan(on_deck_pan,
        OnTap(on_dismiss,
            OnKey(on_key,
                Background(Z_COLOR_SCRIM,
                    Fill(z_stack(Z_AXIS_DEPTH, &deck))))));
}

Z_LAYER_APP(RecentsState, recents_body,
            .layer = Z_LAYER_OVERLAY,
            .anchor = Z_ANCHOR_TOP | Z_ANCHOR_BOTTOM | Z_ANCHOR_LEFT |
                      Z_ANCHOR_RIGHT,
            .exclusive_zone = 0,
            .width = 0,
            .height = 0,
            .keyboard = true)
