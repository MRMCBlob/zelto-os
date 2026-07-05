// Zelto System UI — permission consent dialog.
//
// A modal overlay shown by zsysd when an app requests a permission that has no
// cached decision. It is a libzelto layer-shell app in the OVERLAY layer,
// stretched over the whole output (a dim backdrop) with a centred card and
// Allow / Deny buttons. It requests EXCLUSIVE keyboard interactivity so the
// compositor routes the keyboard to it (a real modal); pointer taps reach it
// because the overlay composites on top of every app and the bar.
//
// zsysd fork/execs it with argv = <app_id> <perm> and waits for it: the process
// exits 0 for Allow, 1 for Deny, and exiting tears down the surface (the dialog
// dismisses). See docs/platform/permissions.md.
#include <stdlib.h>

#include <zelto/ui.h>

// Flick-to-Deny (P33). The modal stays button-driven for Allow, but a DOWNWARD
// drag on the card past this threshold (or a strong down-fling) resolves to Deny —
// the safe default: an accidental swipe denies rather than grants. Dragging the
// card up "the wrong way" resists with the rubber-band. DIST is the travel the card
// fades out over as it leaves.
#define CONSENT_DENY_THRESH 90.0f
#define CONSENT_DENY_DIST 220.0f

typedef struct ConsentState {
    const char *app_id;
    const char *perm;
    bool armed;   // headless auto-resolve scheduled once
    // Entrance motion (P32). `enter` springs 0 (below + transparent) -> 1 (centred
    // + opaque) on the first build. There is NO exit transition: dismissal is a
    // process exit() that zsysd reads as the Allow/Deny result, and animating
    // before exit() races the surface teardown against the grant round-trip
    // (it blocks the requester's synchronous z_perm/z_notify_post) — so the modal
    // resolves immediately on tap (and on a committed flick — see below).
    ZAnimated *enter;
    // Flick-to-Deny drag (P33). `drag` is the card's live downward translation,
    // driven 1:1 while held and sprung back on a short release. A commit past the
    // threshold calls exit(1) IMMEDIATELY with no release animation — the same
    // constraint the buttons honour (animating before exit races the grant
    // round-trip). So the drag is direct-manipulation, but the resolution is instant.
    ZAnimated *drag;
    bool dragging;
} ConsentState;

// Buttons resolve the dialog by the process exit code zsysd reads (0 = Allow).
static void on_allow(ZApp *app, void *state) {
    (void)app;
    (void)state;
    exit(0);
}

// Headless test hook: ZELTO_CONSENT_AUTO=allow|deny resolves the dialog on its own
// a beat after it renders, so the post->grant->banner path completes without a real
// Allow tap. Unset (the default) leaves the dialog up — that IS the consent shot.
static void auto_resolve(ZApp *app, void *ud) {
    (void)app;
    const char *mode = ud;
    exit(mode && mode[0] == 'd' ? 1 : 0);
}
static void on_deny(ZApp *app, void *state) {
    (void)app;
    (void)state;
    exit(1);
}

// Flick-to-Deny gesture (P33): the card follows a downward drag 1:1; a release past
// the threshold/velocity resolves to Deny by exiting immediately (no pre-exit
// animation — that would race zsysd's grant round-trip), a short release snaps the
// card back. An upward drag resists (handled in the body via the rubber-band).
static void on_consent_pan(ZApp *app, void *state, const ZPanEvent *e) {
    ConsentState *s = state;
    if (!s->drag) {
        return;
    }
    if (e->phase == Z_PAN_BEGIN) {
        s->dragging = true;
        z_animated_grab(s->drag);
    } else if (e->phase == Z_PAN_CHANGED) {
        z_animated_set(s->drag, e->translation_y > 0.0f ? e->translation_y : 0.0f);
    } else {   // Z_PAN_END
        s->dragging = false;
        float d = z_animated_get(s->drag);
        if (d > CONSENT_DENY_THRESH || e->velocity_y > 700.0f) {
            exit(1);   // Deny — immediate, no animation (grant-round-trip safe)
        }
        z_animated_spring_velocity(s->drag, 0.0f, Z_SPRING_STANDARD, e->velocity_y);
    }
    z_invalidate(app);
}

static ZView consent_body(ZApp *app, ConsentState *s) {
    // The entrance spring + the flick-to-Deny drag, allocated first +
    // unconditionally (call-order cells) for a stable identity across rebuilds.
    s->enter = z_animated_value(app, 0.0f);
    s->drag = z_animated_value(app, 0.0f);
    if (!s->armed) {
        s->armed = true;
        // Freeze-frame hook (P32): ZELTO_CONSENT_ENTER=<0..1> pins the entrance
        // mid-flight; otherwise spring it in on the first build.
        const char *en = getenv("ZELTO_CONSENT_ENTER");
        if (en && en[0]) {
            z_animated_pin(s->enter, (float)atof(en));
        } else {
            z_animated_spring_with(s->enter, 1.0f, Z_SPRING_STANDARD);
        }
        // Flick-to-Deny freeze-frame (P33): ZELTO_CONSENT_DRAG=<px> pins the card at
        // a held downward drag (toward Deny), seating the entrance so the mid-drag
        // frame is shot-verifiable with no injected input.
        const char *dr = getenv("ZELTO_CONSENT_DRAG");
        if (dr && dr[0]) {
            z_animated_set(s->enter, 1.0f);
            z_animated_pin(s->drag, (float)atof(dr));
            s->dragging = true;
        }
        char *mode = getenv("ZELTO_CONSENT_AUTO");
        if (mode && mode[0]) {
            z_after(app, 400, auto_resolve, mode);
        }
    }
    z_full_repaint(app);   // full-screen modal fading/moving over the app
    // The modal card: title, the "<app> wants to use the <perm>" line, and the
    // Deny / Allow actions (tinted via Background over the default button fill).
    ZView card = Shadow(Z_ELEV_3, Background(Z_COLOR_SURFACE,
        CornerRadius(20,
            Frame(580.0f, 300.0f,
                VStack(
                    Weight(Z_WEIGHT_SEMIBOLD, Foreground(Z_COLOR_TEXT_INV,
                        Font(Z_FONT_TITLE, Text("Permission request")))),
                    Foreground(Z_COLOR_TEXT_MUTED,
                        Font(Z_FONT_CALLOUT,
                            Text("\"%s\" wants to use the %s", s->app_id,
                                 s->perm))),
                    Spacer(),
                    HStack(
                        Background(Z_COLOR_SURFACE_3,
                            Button(on_deny, "Deny")),
                        Spacer(),
                        Background(Z_COLOR_SUCCESS,
                            Button(on_allow, "Allow")),
                        .spacing = 16, .align = Z_ALIGN_CENTER),
                    .padding = 28, .spacing = 18, .align = Z_ALIGN_LEADING)))));

    // Entrance (P32): the whole modal (dim backdrop + card) fades on `enter` via one
    // Opacity, and the card additionally rises into place — a faked scale-up (the
    // toolkit has no scale primitive) reading as a gentle lift.
    float e = z_animated_get(s->enter);
    // Flick-to-Deny (P33): the card follows a downward drag 1:1 (an upward drag
    // resists via the rubber-band) and fades as it nears the Deny threshold. OnPan
    // sits on the offset node, so the draggable card stays hit-testable where it
    // lives. The DIM BACKDROP does not move — only the card leaves.
    float d = z_animated_get(s->drag);
    float dy = d > 0.0f ? d : z_rubber_band(d, (float)z_app_height(app));
    float dprog = d > 0.0f ? d / CONSENT_DENY_DIST : 0.0f;
    if (dprog > 1.0f) {
        dprog = 1.0f;
    }
    ZView risen = Opacity(1.0f - dprog,
        OnPan(on_consent_pan, OffsetXY(0.0f, (1.0f - e) * 24.0f + dy, card)));
    // Dim full-screen backdrop with the card centred in it.
    return Opacity(e, Background(Z_COLOR_SCRIM,
        VStack(
            Spacer(),
            HStack(Spacer(), risen, Spacer(), .align = Z_ALIGN_CENTER),
            Spacer(),
            .align = Z_ALIGN_CENTER)));
}

static ZView body_tr(ZApp *app, void *state) {
    return consent_body(app, (ConsentState *)state);
}

int main(int argc, char **argv) {
    static ConsentState s;
    s.app_id = argc > 1 ? argv[1] : "An app";
    s.perm = argc > 2 ? argv[2] : "camera";

    ZLayerOpts opts = {
        .layer = Z_LAYER_OVERLAY,
        .anchor = Z_ANCHOR_TOP | Z_ANCHOR_BOTTOM | Z_ANCHOR_LEFT | Z_ANCHOR_RIGHT,
        .exclusive_zone = 0,
        .width = 0,
        .height = 0,
        .keyboard = true,
    };
    return z_layer_app_main(&s, body_tr, "Permission", &opts);
}
