// Zelto demo app — "Settings" (the brokered-settings demo, P18).
//
// A normal xdg toplevel that reads and writes system toggles through the zsysd
// settings broker (z_setting_get/set_int on the sys.* keys) — the SAME source of
// truth the quick-settings shade uses. Flipping Wi-Fi here persists it (the
// broker writes through to /var/zelto) AND broadcasts the change, so the shade's
// chip recolours live without a reboot; flipping it in the shade broadcasts back
// here. The app z_settings_observe()s so a change made anywhere updates this UI
// from the app loop. Beyond the three shade toggles it adds an Airplane toggle
// and a Brightness stepper, proving the broker carries arbitrary sys.* settings.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zelto/ui.h>

#include "common/settings_defaults.h"
#include "common/wallpaper.h"

typedef struct SettingsState {
    bool inited;
    bool wifi, mute, bright, airplane;
    int64_t brightness;   // 1..5
    // The Brightness row's slider. Retained (it holds the drag's origin value
    // across the per-frame rebuild); its `value` is kept in step with
    // `brightness` above, which the broker may change under us.
    ZSlider bright_slider;
    // P20 lock screen / idle lifecycle.
    bool lock_enabled;
    int64_t dim_s, lock_s, off_s;
    bool passcode_set;
    bool test_set_done;   // ZELTO_SETTINGS_SET applied once
    int64_t lock_now;
    bool nav_seeded;      // ZELTO_SETTINGS_SCREEN applied (once, on first build)

    // P25 wallpaper picker. The wallpaper dir listed once into wp_paths (kept in
    // state so the OnTapData pointer we hand each thumbnail stays valid across the
    // per-frame rebuild), and wp_current mirrors the brokered sys.wallpaper so the
    // selected tile is highlighted and updates live when it changes.
    char wp_paths[ZELTO_WALLPAPER_MAX][ZELTO_WALLPAPER_PATH_MAX];
    int wp_count;
    char wp_current[ZELTO_WALLPAPER_PATH_MAX];

    // P49: the keyboard's learned dictionary, as seen from here. The keyboard
    // publishes how many words it has learned; this app never sees the words
    // themselves, and deliberately — see the privacy note in system/keyboard/
    // main.c. Clearing is an EPOCH the keyboard compares against one it stored,
    // not an event it has to be running to hear, so a clear made while the
    // keyboard is dead is still honoured on its next boot.
    int64_t kbd_learned;
    int64_t kbd_forget_epoch;

    // P50 Accessibility. text_size is the STEP INDEX (0..11), not a size: the
    // ladder is the toolkit's business (z_font_units), and a Settings app that
    // stored point values would be a second opinion about the type scale.
    int64_t text_size;
    bool bold_text;
    bool reduce_motion;
    bool increase_contrast;   // P51
    ZSlider text_slider;
} SettingsState;

// A setting changed (here or in the shade): re-read the field it maps to and
// repaint. Idempotent, so observing our own writes never loops.
static void on_changed(ZApp *app, const char *key, const char *value, void *ud) {
    SettingsState *s = ud;
    int v = atoi(value);
    if (strcmp(key, "sys.wifi") == 0) {
        s->wifi = v != 0;
    } else if (strcmp(key, "sys.mute") == 0) {
        s->mute = v != 0;
    } else if (strcmp(key, "sys.bright") == 0) {
        s->bright = v != 0;
    } else if (strcmp(key, "sys.airplane") == 0) {
        s->airplane = v != 0;
    } else if (strcmp(key, "sys.brightness") == 0) {
        s->brightness = v;
    } else if (strcmp(key, "sys.lock_enabled") == 0) {
        s->lock_enabled = v != 0;
    } else if (strcmp(key, "sys.idle_dim_s") == 0) {
        s->dim_s = v;
    } else if (strcmp(key, "sys.idle_lock_s") == 0) {
        s->lock_s = v;
    } else if (strcmp(key, "sys.idle_off_s") == 0) {
        s->off_s = v;
    } else if (strcmp(key, "sys.passcode") == 0) {
        s->passcode_set = value[0] != '\0';
    } else if (strcmp(key, ZELTO_KEY_KBD_LEARNED) == 0) {
        s->kbd_learned = v;
    } else if (strcmp(key, ZELTO_KEY_TEXT_SIZE) == 0) {
        s->text_size = v;
    } else if (strcmp(key, ZELTO_KEY_BOLD_TEXT) == 0) {
        s->bold_text = v != 0;
    } else if (strcmp(key, "sys.reduce_motion") == 0) {
        s->reduce_motion = v != 0;
    } else if (strcmp(key, ZELTO_KEY_INCREASE_CONTRAST) == 0) {
        s->increase_contrast = v != 0;
    } else if (strcmp(key, ZELTO_WALLPAPER_KEY) == 0) {
        snprintf(s->wp_current, sizeof(s->wp_current), "%s", value ? value : "");
    }
    z_invalidate(app);
}

static void bright_slide(ZApp *app, void *state, float v);
static void text_slide(ZApp *app, void *state, float v);

// First build: read every toggle from the broker (with defaults) and subscribe
// for live updates, so the first frame reflects the shared state.
static void ensure_init(ZApp *app, SettingsState *s) {
    if (s->inited) {
        return;
    }
    s->inited = true;
    s->wifi = z_setting_get_int("sys.wifi", 1) != 0;
    s->mute = z_setting_get_int("sys.mute", 0) != 0;
    s->bright = z_setting_get_int("sys.bright", 1) != 0;
    s->airplane = z_setting_get_int("sys.airplane", 0) != 0;
    s->brightness = z_setting_get_int("sys.brightness", ZELTO_DEFAULT_BRIGHTNESS);
    s->bright_slider.on_change = bright_slide;
    s->lock_enabled = z_setting_get_int("sys.lock_enabled", 0) != 0;
    s->dim_s = z_setting_get_int("sys.idle_dim_s", 8);
    s->lock_s = z_setting_get_int("sys.idle_lock_s", 20);
    s->off_s = z_setting_get_int("sys.idle_off_s", 120);
    s->passcode_set = z_setting_get_str("sys.passcode", "")[0] != '\0';
    s->lock_now = z_setting_get_int("sys.lock_now", 0);
    s->kbd_learned = z_setting_get_int(ZELTO_KEY_KBD_LEARNED, 0);
    s->kbd_forget_epoch = z_setting_get_int(ZELTO_KEY_KBD_FORGET, 0);
    s->text_size = z_setting_get_int(ZELTO_KEY_TEXT_SIZE, Z_TEXT_SIZE_DEFAULT);
    s->bold_text = z_setting_get_int(ZELTO_KEY_BOLD_TEXT, 0) != 0;
    s->reduce_motion = z_setting_get_int("sys.reduce_motion", 0) != 0;
    s->increase_contrast =
        z_setting_get_int(ZELTO_KEY_INCREASE_CONTRAST, 0) != 0;
    s->text_slider.on_change = text_slide;
    // Wallpaper picker: enumerate the directory once and record the active choice.
    s->wp_count = zelto_wallpaper_list(s->wp_paths, ZELTO_WALLPAPER_MAX);
    snprintf(s->wp_current, sizeof(s->wp_current), "%s",
             z_setting_get_str(ZELTO_WALLPAPER_KEY, ""));
    z_settings_observe(app, on_changed, s);
}

// Tap a wallpaper thumbnail: write its absolute path to the broker (which
// persists + fans out, so the home + lock screen rebuild live). `data` points at
// the state-owned wp_paths entry, stable across rebuilds.
static void pick_wallpaper(ZApp *app, void *state, void *data) {
    (void)state;
    z_setting_set_str(ZELTO_WALLPAPER_KEY, (const char *)data);
    z_invalidate(app);
}

// Toggle handlers: flip the bool locally and write it through the broker (which
// persists + broadcasts; the broadcast comes back to on_changed as a no-op).
static void t_wifi(ZApp *app, void *state) {
    SettingsState *s = state;
    s->wifi = !s->wifi;
    z_setting_set_int("sys.wifi", s->wifi);
    z_invalidate(app);
}
static void t_mute(ZApp *app, void *state) {
    SettingsState *s = state;
    s->mute = !s->mute;
    z_setting_set_int("sys.mute", s->mute);
    z_invalidate(app);
}
static void t_bright(ZApp *app, void *state) {
    SettingsState *s = state;
    s->bright = !s->bright;
    z_setting_set_int("sys.bright", s->bright);
    z_invalidate(app);
}
static void t_airplane(ZApp *app, void *state) {
    SettingsState *s = state;
    s->airplane = !s->airplane;
    z_setting_set_int("sys.airplane", s->airplane);
    z_invalidate(app);
}
// Brightness is a SLIDER now, not a [-|+] stepper (P42). Brightness is not a
// count you nudge, it is a level you hunt for by looking at the screen while you
// move it — the stepper made "a bit dimmer" a two-tap round trip through a
// control that showed you a number instead of the result.
//
// The brokered key stays an integer 1..5 (zelto-dim maps it to a scrim alpha),
// so the 0..1 slider position is quantised on the way in and expanded on the way
// out. That keeps the storage contract identical: nothing else in the system
// learns that the control changed shape.
static int64_t bright_from_slider(float v) {
    int level = 1 + (int)(v * 4.0f + 0.5f);
    return level < 1 ? 1 : (level > 5 ? 5 : level);
}

static void bright_slide(ZApp *app, void *state, float v) {
    SettingsState *s = state;
    int64_t level = bright_from_slider(v);
    if (level == s->brightness) {
        return;   // same step: don't spam the broker on every pixel of drag
    }
    s->brightness = level;
    z_setting_set_int("sys.brightness", s->brightness);
    z_invalidate(app);
}

// --- P50 accessibility handlers ---
// TEXT SIZE IS A SLIDER, for the same reason Brightness became one in P42 and
// then some. Brightness is a level you hunt for by LOOKING at the result rather
// than a count you nudge; text size is that argument at its strongest, because
// the control is made of the thing it changes — the labels on this screen resize
// under the finger while it moves, so the slider is its own preview.
//
// It is a QUANTISED slider (seven steps), not a continuous one, and the two are
// not the same control wearing different clothes: a continuous size would put
// the whole system's type on a value no step of the ladder was designed for, and
// the ladder is the thing that keeps Caption smaller than Body everywhere.
static int64_t text_from_slider(float v) {
    int step = (int)(v * (float)(Z_TEXT_SIZE_STEPS - 1) + 0.5f);
    return step < 0 ? 0 : (step >= Z_TEXT_SIZE_STEPS ? Z_TEXT_SIZE_STEPS - 1
                                                     : step);
}

static void text_slide(ZApp *app, void *state, float v) {
    SettingsState *s = state;
    int64_t step = text_from_slider(v);
    if (step == s->text_size) {
        return;   // same step: don't spam the broker on every pixel of drag
    }
    s->text_size = step;
    z_setting_set_int(ZELTO_KEY_TEXT_SIZE, s->text_size);
    fprintf(stderr, "[settings] text size -> %lld (%s)\n", (long long)step,
            z_text_size_name((int)step));
    fflush(stderr);
    z_invalidate(app);
}

static void t_bold(ZApp *app, void *state) {
    SettingsState *s = state;
    s->bold_text = !s->bold_text;
    z_setting_set_int(ZELTO_KEY_BOLD_TEXT, s->bold_text);
    z_invalidate(app);
}

static void t_increase_contrast(ZApp *app, void *state) {
    SettingsState *s = state;
    s->increase_contrast = !s->increase_contrast;
    z_setting_set_int(ZELTO_KEY_INCREASE_CONTRAST, s->increase_contrast);
    z_invalidate(app);
}

static void t_reduce_motion(ZApp *app, void *state) {
    SettingsState *s = state;
    s->reduce_motion = !s->reduce_motion;
    z_setting_set_int("sys.reduce_motion", s->reduce_motion);
    z_invalidate(app);
}

// --- P20 lock-screen handlers ---
static void t_lock(ZApp *app, void *state) {
    SettingsState *s = state;
    s->lock_enabled = !s->lock_enabled;
    z_setting_set_int("sys.lock_enabled", s->lock_enabled);
    z_invalidate(app);
}
static void dim_dec(ZApp *app, void *state) {
    SettingsState *s = state;
    if (s->dim_s > 1) {
        s->dim_s -= 2;
    }
    if (s->dim_s < 1) {
        s->dim_s = 1;
    }
    z_setting_set_int("sys.idle_dim_s", s->dim_s);
    z_invalidate(app);
}
static void dim_inc(ZApp *app, void *state) {
    SettingsState *s = state;
    s->dim_s += 2;
    z_setting_set_int("sys.idle_dim_s", s->dim_s);
    z_invalidate(app);
}
static void lock_dec(ZApp *app, void *state) {
    SettingsState *s = state;
    if (s->lock_s > 2) {
        s->lock_s -= 2;
    }
    z_setting_set_int("sys.idle_lock_s", s->lock_s);
    z_invalidate(app);
}
static void lock_inc(ZApp *app, void *state) {
    SettingsState *s = state;
    s->lock_s += 2;
    z_setting_set_int("sys.idle_lock_s", s->lock_s);
    z_invalidate(app);
}
static void off_dec(ZApp *app, void *state) {
    SettingsState *s = state;
    if (s->off_s > 5) {
        s->off_s -= 5;
    }
    z_setting_set_int("sys.idle_off_s", s->off_s);
    z_invalidate(app);
}
static void off_inc(ZApp *app, void *state) {
    SettingsState *s = state;
    s->off_s += 5;
    z_setting_set_int("sys.idle_off_s", s->off_s);
    z_invalidate(app);
}
// Toggle a demo passcode (1234) on/off, so the lock screen exercises the keypad.
static void t_passcode(ZApp *app, void *state) {
    SettingsState *s = state;
    s->passcode_set = !s->passcode_set;
    z_setting_set_str("sys.passcode", s->passcode_set ? "1234" : "");
    z_invalidate(app);
}
// Manual "lock now": bump the sys.lock_now counter zelto-lock observes.
static void lock_now(ZApp *app, void *state) {
    SettingsState *s = state;
    s->lock_now++;
    z_setting_set_int("sys.lock_now", s->lock_now);
    z_invalidate(app);
}

// --- the grouped inset list ------------------------------------------------
// The shape of this screen is the iOS grouped inset list, and every constant
// below serves that one idea: a settings screen is not a page of controls, it is
// a set of BOUNDED GROUPS, each of which is a sentence — a header saying what the
// group is for, rows that are all the same kind of thing, and a footer saying
// what the group does when you change it. The rows carry no explanation of their
// own, which is what lets them stay one line each.
#define LIST_W 640.0f     // the inset column (a ~40px gutter each side of 720)
// A row is Z_ROW_H TALL and inset ROW_PAD on the LEADING/TRAILING edges. Both of
// those changed meaning in P44 and it is worth saying why, because the numbers
// they replace were right by accident.
//
// ROW_H was 44 — Apple's row height, in POINTS, spent as screen units — and the
// row still measured 76 units because `Padding` insets BOTH axes, so the 16 above
// and below made up most of the difference (44 + 32 = 76, against the 81 that
// 44pt actually converts to). Two wrong numbers landing near the right answer.
// Now ROW_H is the row's TOTAL height and comes from the toolkit's Z_ROW_H, and
// the inset is horizontal only — which is also what fixes the LEADING edge, where
// nothing was making up any difference: 16 units is 8.6pt where the list it
// copies uses 16pt, so every label in the app sat half as far from the card's
// edge as it should.
// P50 measured that the 81-unit floor won across the seven standard steps and
// left the literal here, with a note saying "the day the accessibility sizes are
// shipped, this line becomes z_row_h(app) and the rows below it take an app
// pointer". P51 shipped them, so it did. `ROW_H` is gone: a row's height is
// z_row_h(app) — the larger of the 44pt touch target and one Body line plus its
// breathing room — which is still exactly 81 at the default size, so no shot of
// an unconfigured device moved.
#define ROW_PAD ((float)Z_SPACE_L)     // 29 — leading/trailing inset inside a card
// Between one group's footer and the next group's header. P52: this was a bare
// 30.0f — 16.2pt, off the grid, and the ONLY thing separating two cards that are
// otherwise identical surfaces. iOS's grouped list runs ~35pt between sections
// (the default grouped section-header height); Z_SPACE_XL is 20pt = 37 units,
// which is the largest step of the scale that does not push the root list's last
// group off the fold. The 35pt reading is recorded in the dossier as the upper
// bound to test against a shot rather than adopted at a desk.
#define SEC_GAP ((float)Z_SPACE_XL)
// The gap between a wrapped label and the control that moved under it when the
// row reflowed. Half the row's own vertical breathing room: the two belong to
// each other and must read as one row, not as two.
#define REFLOW_GAP ((float)Z_ROW_VPAD * 0.5f)

// A fixed gap. NOT Frame(w, h, Spacer()) — a Spacer keeps its grow flag through
// Frame and eats the stack's spare space; an empty transparent Rect does not.
static ZView gap(float h) {
    return Frame(1.0f, h, Rect(.color = z_rgba(0, 0, 0, 0)));
}

// A header or footer, inset to sit under the card's text column rather than
// under the card's edge — the alignment is what makes the three parts read as one
// group instead of as three stacked objects.
//
// WRAPPED, as of P44. This prose used to be hand-broken on '\n' by the author,
// because the toolkit had no text wrapping — layout is one intrinsic-size pass,
// so a Text measures to one line and a caption wider than the column runs off the
// right edge (which is what the P43 type rescale did to every footer here: at
// 13px they fit, at 24px they did not, and P43 broke them by hand). WrapText now
// does the split at build time against the real font, so the strings below are
// written as plain sentences and a longer one cannot silently overflow. The
// column is LIST_W less the leading/trailing insets that hold it under the card's
// text.
#define INSET_TEXT_W (LIST_W - 2.0f * ROW_PAD)

static ZView inset_text(ZApp *app, const char *s, ZFont size, ZColor ink,
                        ZWeight w) {
    return HStack(
        Frame(ROW_PAD, 1.0f, Rect(.color = z_rgba(0, 0, 0, 0))),
        WrapText(app, s, .width = INSET_TEXT_W, .size = size, .weight = w,
                 .color = ink, .line_gap = 4.0f),
        Frame(ROW_PAD, 1.0f, Rect(.color = z_rgba(0, 0, 0, 0))),
        .spacing = 0, .align = Z_ALIGN_LEADING);
}

// A MINIMUM height — the primitive the toolkit does not have and this file needs
// three times over once rows stop having a knowable height. A ZStack takes the
// size of its largest child (and centres children at their own size unless
// Fill()), so an invisible Frame of the floor height beside the real content IS
// max(floor, content), with no measuring and no new toolkit node.
static ZView min_height(float h, ZView content) {
    return ZStack(Frame(1.0f, h, Rect(.color = z_rgba(0, 0, 0, 0))), content,
                  .align = Z_ALIGN_CENTER);
}

// One list row: a height so every row in every group shares a baseline rhythm (a
// row that sizes to its control makes a switch row and a stepper row different
// heights, and the list stops looking like a list).
//
// The inset is two explicit end gaps rather than Padding, because Padding is BOTH
// axes: as padding it would add its 29 above and below the fixed height and the
// row would be 139 tall. The content Grow(1)s between them so the label column
// still spans the card.
//
// REFLOWED, the height goes away entirely. It has to: the whole reason the row
// reflowed is that its content no longer has a knowable height — a label that
// wraps is one line or three depending on the string and the size, and a Frame
// that pinned it would clip exactly the text somebody asked to be able to read.
// The vertical rhythm becomes explicit padding instead, so the row still breathes
// the same amount at both ends.
static ZView list_row(ZApp *app, ZView content) {
    ZView inner = HStack(Frame(ROW_PAD, 1.0f, Rect(.color = z_rgba(0, 0, 0, 0))),
                         Grow(1.0f, content),
                         Frame(ROW_PAD, 1.0f, Rect(.color = z_rgba(0, 0, 0, 0))),
                         .spacing = 0, .align = Z_ALIGN_CENTER);
    if (z_text_size_reflows()) {
        return VStack(gap((float)Z_ROW_VPAD), inner, gap((float)Z_ROW_VPAD),
                      .spacing = 0, .align = Z_ALIGN_LEADING);
    }
    return Frame(0.0f, z_row_h(app), inner);
}

// A label on the left, its control on the right. This is the whole grammar of the
// screen: the left column is scannable prose, the right column is the state.
//
// AND AT THE ACCESSIBILITY SIZES IT IS THE WHOLE GRAMMAR OF THE SCREEN TURNED
// NINETY DEGREES. Past Z_TEXT_SIZE_REFLOW_FIRST the label alone is wider than the
// card, so:
//
//   - the label becomes a WrapText at the card's own text column. Text never
//     wraps and never truncates — it measures to one line however long and the
//     glyphs paint straight through their box — so a Text here is not "a label
//     that overflows a bit", it is a label running off the screen.
//   - the control moves onto its own line, leading-aligned. NOT trailing: at
//     these sizes the label's last line ends anywhere, and a control pinned to
//     the right edge of a card floats away from the words it belongs to. Every
//     row starting its control at the same x is what keeps the list a list.
//
// One function, so a row cannot opt out of the reflow by being written later.
static ZView labelled(ZApp *app, const char *label, ZView control) {
    if (z_text_size_reflows()) {
        return list_row(app, VStack(
            WrapText(app, label, .width = INSET_TEXT_W, .size = Z_FONT_BODY,
                     .color = Z_COLOR_TEXT, .line_gap = 4.0f),
            gap(REFLOW_GAP),
            HStack(control, Spacer(), .spacing = 0, .align = Z_ALIGN_CENTER),
            .spacing = 0, .align = Z_ALIGN_LEADING));
    }
    return list_row(app, HStack(
        Foreground(Z_COLOR_TEXT, Font(Z_FONT_BODY, Text("%s", label))),
        Spacer(),
        control,
        .spacing = Z_SPACE_S, .align = Z_ALIGN_CENTER));
}

// An ACTION row: a full-width card with a centred label and no control. It does
// something now rather than holding a state, and centring it is how iOS says so.
//
// It never reflows — there is no control to move under anything — but it is a
// fixed height holding text like every other row, so it takes the derived one;
// and its label wraps, because "Clear Learned Words" measures over 1000 units at
// AX5 in a 640-unit card.
static ZView action_row(ZApp *app, const char *label, ZColor ink) {
    return Background(Z_COLOR_SURFACE,
        CornerRadius(Z_RADIUS_CARD,
            min_height(z_row_h(app),
                VStack(gap((float)Z_ROW_VPAD),
                       WrapText(app, label, .width = INSET_TEXT_W,
                                .size = Z_FONT_BODY, .weight = Z_WEIGHT_SEMIBOLD,
                                .color = ink, .line_gap = 4.0f),
                       gap((float)Z_ROW_VPAD),
                       .spacing = 0, .align = Z_ALIGN_CENTER))));
}

// One label + On/Off toggle row. The On/Off face CROSS-FADES between off
// (SURFACE_3) and on (PRIMARY) on a spring-backed, identity-keyed value (P32)
// instead of hard-swapping colour — the same motion the shade's quick-settings
// chips use, so a flip reads consistently in both places. ZELTO_QS_ANIM=<0..1>
// pins the cross-fade mid-flight for a still shot.
static ZView toggle_row(ZApp *app, uint64_t key, const char *label, bool on,
                        ZAction act) {
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

    // A real SWITCH, not a box that says "On".
    //
    // The old control was a chip whose fill cross-faded to PRIMARY with a TEXT_INV
    // label — and PRIMARY is now near-white, so an "On" toggle rendered as white
    // text on a white plate: a blank box. But the deeper problem is that a word is
    // the wrong control. A switch shows its state by POSITION (the knob is left or
    // right) as well as by fill, so it reads at a glance and while it animates,
    // and it is the single most recognisable control on a phone.
    const float TRACK_W = 52.0f, TRACK_H = 32.0f, KNOB = 26.0f;
    ZColor track = z_color_lerp(Z_COLOR_SURFACE_3, Z_COLOR_PRIMARY, v);
    float travel = (TRACK_W - KNOB - 6.0f);      // 3px inset at each end
    float knob_x = -travel * 0.5f + travel * v;  // slides left -> right with `v`
    ZView sw = Frame(TRACK_W, TRACK_H,
        Background(track,
            CornerRadius(TRACK_H * 0.5f,
                ZStack(
                    OffsetXY(knob_x, 0.0f,
                        Shadow(Z_ELEV_1,
                            Frame(KNOB, KNOB,
                                Rect(.color = Z_COLOR_TEXT,
                                     .radius = KNOB * 0.5f)))),
                    .align = Z_ALIGN_CENTER))));

    return labelled(app, label, OnTap(act, sw));
}

// One half of the stepper pill. The press veil is masked to the TAPPED node's own
// radius, so each half carries the pill radius itself: a square veil inside a
// rounded pill pokes white corners out past the fill.
static ZView step_key(ZAction act, const char *mark) {
    return OnTap(act,
        CornerRadius(Z_RADIUS_CHIP,
            Frame(52.0f, 36.0f,
                ZStack(
                    Weight(Z_WEIGHT_MEDIUM,
                        Foreground(Z_COLOR_TEXT,
                            Font(Z_FONT_CALLOUT, Text("%s", mark)))),
                    .align = Z_ALIGN_CENTER))));
}

// A label + value + [-|+] stepper row. The two keys are ONE pill split by a
// hairline, not two floating chips: they are a single control with two ends, and
// drawing them apart makes the row read as three unrelated objects. The value
// sits to the LEFT of the control in muted ink — the iOS "detail" column, where
// every read-only right-hand value on the screen lives — and is tabular-width so
// stepping it does not shuffle the row.
static ZView stepper_row(ZApp *app, const char *label, int64_t val,
                         const char *unit, ZAction dec, ZAction inc) {
    ZView pill = Background(Z_COLOR_SURFACE_3,
        CornerRadius(Z_RADIUS_CHIP,
            HStack(
                step_key(dec, "\xe2\x88\x92"),   // a real minus, not a hyphen
                Frame(1.0f, 22.0f, Rect(.color = Z_COLOR_BORDER)),
                step_key(inc, "+"),
                .spacing = 0, .align = Z_ALIGN_CENTER)));
    // The value's column is TABULAR: stepping 8s to 9s must not shuffle the pill.
    // It was a literal 56, which is a fixed WIDTH HOLDING TEXT — the same class
    // of bug as a fixed height holding text, and it was already wrong at the
    // DEFAULT size: '120s' measures 147 units at Body 73.6 (step 9), so a digit
    // in this face advances almost exactly half its em, and the four-glyph widest
    // value wants 2.0 x Body — 62 units where the column gave 56.
    //
    // So the column is that expression, at whatever Body is now. This is the
    // number the reflow break was re-measured against: until the column scaled,
    // the value silently painted outside it and the row only APPEARED to fit.
    ZView value = Frame(2.0f * z_font_units(Z_FONT_BODY), 0.0f,
        HStack(Spacer(),
            Foreground(Z_COLOR_TEXT_MUTED,
                Font(Z_FONT_BODY, Text("%lld%s", (long long)val, unit))),
            .spacing = 0, .align = Z_ALIGN_CENTER));
    return labelled(app, label,
        HStack(value, pill, .spacing = Z_SPACE_S, .align = Z_ALIGN_CENTER));
}

// The disclosure chevron: the mark that says "this row is a DOOR, not a
// control". Every row in the grouped-list vocabulary reads left-to-right as
// label / value / affordance, and this is the affordance that distinguishes a
// row you tap to go somewhere from a row you tap to change something.
static ZView chevron(void) {
    static const float pts[] = {0.35f, 0.22f, 0.65f, 0.5f, 0.35f, 0.78f};
    return Frame(16.0f, 16.0f,
        Stroke(.points = pts, .count = 3, .thickness = 2.5f,
               .color = Z_COLOR_TEXT_FAINT));
}

// Tapping a detail row pushes the screen it names.
//
// OnTapData binds ONE void* per view, and the natural thing to bind is the
// screen function — but ISO C does not let a function pointer round-trip through
// void* (the build is -Wpedantic -Werror, and rightly: it is undefined). So each
// row points at a static ROUTE record instead, which is an ordinary object.
//
// Every screen takes the same props, the app state, held in a file-static
// because a Navigator's ROOT screen is invoked with no props of its own.
typedef struct SettingsRoute {
    ZScreenFn screen;
} SettingsRoute;

static SettingsState *g_state;

static void push_screen(ZApp *app, void *state, void *data) {
    (void)state;
    const SettingsRoute *r = data;
    if (r && r->screen) {
        z_nav_push(z_navigation(app), r->screen, g_state);
    }
}

// A DETAIL ROW: label on the left, its current value in the muted detail column,
// a chevron, and a tap that pushes a screen. This is the row the root list is
// made of, and the value is the point of it — "Brightness  4  >" tells you the
// state without drilling in, so the root stays a status read-out rather than a
// bare table of contents.
static ZView detail_row(ZApp *app, const char *label, const char *value,
                        const SettingsRoute *route) {
    return OnTapData(push_screen, (void *)route,
        labelled(app, label,
            HStack(
                Foreground(Z_COLOR_TEXT_MUTED,
                    Font(Z_FONT_BODY, Text("%s", value ? value : ""))),
                chevron(),
                .spacing = Z_SPACE_S, .align = Z_ALIGN_CENTER)));
}

// A label + slider row. Unlike the stepper this row has NO numeric read-out: the
// slider's own fill is the value, and a level whose whole point is "how bright
// does that look" does not gain anything from also being told it is a 4.
static ZView slider_row(ZApp *app, const char *label, ZSlider *sl) {
    // Reflowed, the slider gets the whole column rather than the 300 units left
    // over beside a label — the one control on this screen that is BETTER for
    // having reflowed, because a slider's precision is its length.
    float len = z_text_size_reflows() ? INSET_TEXT_W : 300.0f;
    return labelled(app, label,
        Slider(app, sl, .length = len, .thickness = 6.0f));
}

// A GROUP: the inset, rounded card that a run of settings rows lives in, with a
// hairline between rows. This is the shape of every settings screen on every
// phone — it turns a loose column of labels into a bounded, scannable region (Law
// of Common Region), and it is what makes a settings screen look like a settings
// screen rather than a debug panel.
//
// The separator is INSET to the row's text column, not full-bleed. That is not a
// detail: a hairline running edge to edge cuts the card into stacked slabs, while
// one that starts where the labels start reads as a rule BETWEEN rows of a single
// list — and the un-ruled left margin is what visually holds the card together.
static ZView group(ZView *rows, int n) {
    ZStackOpts col = {.spacing = 0, .align = Z_ALIGN_LEADING};
    int k = 0;
    for (int i = 0; i < n && k < Z_MAX_CHILDREN - 1; i++) {
        if (i > 0) {
            col.children[k++] = Frame(0.0f, 1.0f,
                HStack(Frame(ROW_PAD, 1.0f, Rect(.color = z_rgba(0, 0, 0, 0))),
                       Rect(.color = Z_COLOR_BORDER, .grow = 1.0f),
                       .spacing = 0, .align = Z_ALIGN_CENTER));
        }
        col.children[k++] = rows[i];
    }
    return Background(Z_COLOR_SURFACE,
        CornerRadius(Z_RADIUS_CARD, z_stack(Z_AXIS_VERTICAL, &col)));
}

// A whole SECTION: header, card, footer, as one unit with its own internal
// rhythm (tight to its card, loose to its neighbours), so the outer column only
// has to space sections apart. Either label may be NULL.
static ZView section_block(ZApp *app, const char *header, ZView card,
                           const char *foot) {
    ZStackOpts col = {.spacing = 0, .align = Z_ALIGN_LEADING};
    int k = 0;
    if (header) {
        col.children[k++] = inset_text(app, header, Z_FONT_FOOTNOTE,
                                       Z_COLOR_TEXT_MUTED, Z_WEIGHT_SEMIBOLD);
        col.children[k++] = gap(8.0f);
    }
    col.children[k++] = card;
    if (foot) {
        col.children[k++] = gap(8.0f);
        col.children[k++] = inset_text(app, foot, Z_FONT_FOOTNOTE,
                                       Z_COLOR_TEXT_FAINT, Z_WEIGHT_REGULAR);
    }
    return z_stack(Z_AXIS_VERTICAL, &col);
}

// One wallpaper thumbnail: a cover-fit rounded preview of the PNG; the currently
// selected one gets an accent ring (a padded primary plate behind it). The whole
// tile is the tap target. NB: every thumbnail decodes the full-resolution
// wallpaper — the P24 image cache is keyed by path only, so a thumbnail and the
// full-screen wallpaper SHARE one cache entry. That is deliberate: a size-keyed
// downscaled decode would *duplicate* the decode (a small thumb bitmap AND the
// big one), whereas sharing means picking a wallpaper is instant (its bitmap is
// already warm) at the cost of holding the handful of demo wallpapers at full res
// — well within the 64-entry cache. See docs + the P25 memory note.
// The tile is DERIVED, not declared. 186 x 118 was a literal that happened to
// fit — 3 x 186 + 2 x 12 = 582, exactly the card's inner column — which means the
// grid silently depended on the gutter never changing. It does change here (the
// gutter is a step of the spacing scale now), so the tile is the expression it
// always was: the column, less the gutters, over the columns. The 0.634 is the
// tile's own proportion, kept as it shipped; a wallpaper thumb that showed the
// screen's real 1:2 portrait would be a different grid and is not this phase.
#define WP_COLS 3
#define WP_GUTTER ((float)Z_SPACE_S)
#define WP_THUMB_W ((INSET_TEXT_W - (float)(WP_COLS - 1) * WP_GUTTER) / (float)WP_COLS)
#define WP_THUMB_H (WP_THUMB_W * 0.6344f)

static ZView wp_thumb(SettingsState *s, int i) {
    bool cur = strcmp(s->wp_paths[i], s->wp_current) == 0;
    ZView preview = Frame(WP_THUMB_W, WP_THUMB_H,
        CornerRadius(14.0f, Cover(Image(s->wp_paths[i]))));
    ZView tile = cur
        ? Background(Z_COLOR_PRIMARY, CornerRadius(18.0f, Padding(4.0f, preview)))
        : preview;
    return OnTapData(pick_wallpaper, s->wp_paths[i], tile);
}

// A grid (WP_COLS per row) of wallpaper thumbnails. A partial last row is padded
// with a FIXED-WIDTH empty cell, never a Spacer: a grow-weighted filler makes the
// row redistribute by child count, so a row of two tiles would sit at different
// x's than a row of three and the columns would visibly shift.
static ZView wp_grid(SettingsState *s) {
    ZStackOpts grid = {.spacing = WP_GUTTER, .align = Z_ALIGN_LEADING};
    int k = 0;
    for (int i = 0; i < s->wp_count && k < Z_MAX_CHILDREN; i += WP_COLS) {
        ZStackOpts row = {.spacing = WP_GUTTER, .align = Z_ALIGN_CENTER};
        for (int c = 0; c < WP_COLS; c++) {
            int j = i + c;
            row.children[c] = j < s->wp_count
                ? wp_thumb(s, j)
                : Frame(WP_THUMB_W, WP_THUMB_H,
                        Rect(.color = z_rgba(0, 0, 0, 0)));
        }
        grid.children[k++] = z_stack(Z_AXIS_HORIZONTAL, &row);
    }
    return z_stack(Z_AXIS_VERTICAL, &grid);
}

// Every screen on this stack has the same shape: a large title, then a column of
// groups, in a scroll. Factored out so a detail screen cannot drift from the root
// — the whole point of the restructure is that they are the same kind of thing.
//
// The large title scrolls WITH the list (it is the list's first item, not a fixed
// chrome bar). iOS's large title collapses into the navigation bar as you scroll;
// with no navigation bar to collapse into, the honest version is to let it leave.
static ZView settings_screen(ZApp *app, const char *title, ZView *blocks, int n) {
    ZStackOpts col = {.spacing = SEC_GAP, .align = Z_ALIGN_LEADING};
    int k = 0;
    // WRAPPED, as of P51. A Large Title is 74 units at the default size and 141
    // at AX5, where "Display & Sound" measures 798 on a 720 screen — a screen
    // title running off its own screen. It is the same Text-never-wraps trap the
    // footers hit in P44, at the one string on the page nobody thought could be
    // too long, because at the size it was written it never was.
    //
    // AND IT DROPS A STEP WHEN THE ROWS REFLOW. <zelto/gfx.h>'s additive ladder
    // is exact for Body and 10pt generous for the hero style, because Apple
    // compresses Large Title at the accessibility sizes and one additive rule
    // cannot; the note over Z_TEXT_SIZE_OFFSETS says the ceiling belongs in the
    // LAYOUT rather than in a second table, and this is the layout. Title still
    // wraps here — no size a person asked to be able to read fits
    // "Accessibility" on one 582-unit line — but the hero stops being three
    // times the height of the rows under it.
    col.children[k++] = HStack(
        Frame(ROW_PAD, 1.0f, Rect(.color = z_rgba(0, 0, 0, 0))),
        Weight(Z_WEIGHT_BOLD,
            WrapText(app, title, .width = LIST_W - 2.0f * ROW_PAD,
                     .size = z_text_size_reflows() ? Z_FONT_TITLE
                                                   : Z_FONT_LARGE_TITLE,
                     .weight = Z_WEIGHT_BOLD,
                     .color = Z_COLOR_TEXT, .line_gap = 4.0f)),
        .spacing = 0, .align = Z_ALIGN_CENTER);
    for (int i = 0; i < n && k < Z_MAX_CHILDREN - 2; i++) {
        col.children[k++] = blocks[i];
    }
    col.children[k++] = gap(24.0f);

    return Background(Z_COLOR_BG,
        Fill(Scroll(app,
            VStack(
                Frame(LIST_W, 0.0f, z_stack(Z_AXIS_VERTICAL, &col)),
                .padding = 20, .spacing = 0, .align = Z_ALIGN_CENTER),
            .axis = Z_AXIS_VERTICAL)));
}

// --- the detail screens -----------------------------------------------------
// Each is one subject's worth of controls: exactly the groups that used to be
// stacked into the single flat scroll, now reachable one drill at a time.

static ZView screen_network(ZApp *app, void *props) {
    SettingsState *s = props;
    ZView rows[] = {
        toggle_row(app, 0x5E7104u, "Airplane Mode", s->airplane, t_airplane),
        toggle_row(app, 0x5E7101u, "Wi-Fi", s->wifi, t_wifi),
    };
    ZView blocks[] = {
        section_block(app, NULL, group(rows, 2),
            "Airplane Mode turns the radios off. Network calls fail while it "
            "is on."),
    };
    return settings_screen(app, "Network", blocks, 1);
}

static ZView screen_display(ZApp *app, void *props) {
    SettingsState *s = props;
    ZView rows[] = {
        slider_row(app, "Brightness", &s->bright_slider),
        toggle_row(app, 0x5E7103u, "Brightness Boost", s->bright, t_bright),
        toggle_row(app, 0x5E7102u, "Silent", s->mute, t_mute),
    };
    ZView blocks[] = {
        section_block(app, NULL, group(rows, 3),
            "Brightness runs 1 to 5 and dims the screen with a scrim."),
    };
    return settings_screen(app, "Display & Sound", blocks, 1);
}

static ZView screen_wallpaper(ZApp *app, void *props) {
    SettingsState *s = props;
    // The picker lives INSIDE a card like every other group, rather than floating
    // on the background: a bare grid between two cards reads as a different
    // screen that got pasted in.
    ZView card = s->wp_count > 0
        ? Background(Z_COLOR_SURFACE,
              CornerRadius(Z_RADIUS_CARD, Padding(ROW_PAD, wp_grid(s))))
        : group((ZView[]){labelled(app, "Wallpaper",
                    Foreground(Z_COLOR_TEXT_MUTED,
                        Font(Z_FONT_BODY, Text("None found"))))}, 1);
    ZView blocks[] = {
        section_block(app, NULL, card, "Shown on the Home and Lock screens."),
    };
    return settings_screen(app, "Wallpaper", blocks, 1);
}

static ZView screen_lock(ZApp *app, void *props) {
    SettingsState *s = props;
    ZView rows[] = {
        toggle_row(app, 0x5E7105u, "Lock Screen", s->lock_enabled, t_lock),
        toggle_row(app, 0x5E7106u, "Passcode (1234)", s->passcode_set,
                   t_passcode),
        stepper_row(app, "Dim After", s->dim_s, "s", dim_dec, dim_inc),
        stepper_row(app, "Lock After", s->lock_s, "s", lock_dec, lock_inc),
        stepper_row(app, "Screen Off After", s->off_s, "s", off_dec, off_inc),
    };
    ZView act = action_row(app, "Lock Now", Z_COLOR_TEXT);
    ZView blocks[] = {
        section_block(app, NULL, group(rows, 5),
            "Each delay is measured from your last touch."),
        section_block(app, NULL, OnTap(lock_now, act), NULL),
    };
    return settings_screen(app, "Lock Screen", blocks, 2);
}

// ACCESSIBILITY (P50) — the screen the OS had settings for and no door to.
//
// Reduce Motion has existed since P31 and has been reachable by nothing but a
// test env var: sys.reduce_motion was implemented, honoured by every spring in
// the toolkit, photographed in the shot catalogue, and could not be turned on by
// a person. Text Size and Bold Text arrive with a screen rather than after one.
//
// THE SLIDER IS ITS OWN PREVIEW. Every label on this screen resizes under the
// finger as it moves — the group header, the footer, the two toggle rows below —
// because they are drawn by the same seam the slider is writing to. That is why
// the sample line above the slider is deliberately SHORT: it is not the preview
// (the screen is), it is a reminder of what the ladder does to a paragraph.
//
// The two end caps are the small and the large A, which is the control's own
// legend and is what every phone puts there. They are drawn at FIXED STEPS
// (Caption2 and Title) — and P50 wrote that as "rather than at the live size, or
// the legend would resize with the thing it is labelling", which was wrong about
// the mechanism. Font() routes through z_font_units() like everything else, so
// the caps DO grow: at AX5 the Title 'A' wants a 148-unit line box. What a fixed
// STEP preserves is not the size, it is the CONTRAST — the small A stays five
// steps below the large one wherever the ladder sits, and that ratio is the
// entire information content of a legend. The row it sits in is then the one
// that has to give (see text_size_row).
#define TEXT_SAMPLE "The quick brown fox."

// The end caps are drawn at FIXED steps and the row they sit in is not: a
// Z_FONT_TITLE 'A' wants 148 units of line box at AX5 and Z_ROW_H is 81. The
// caps are the one thing on this screen that must NOT resize, so the ROW has to
// be the one that gives — it is the taller of the row height and the taller cap,
// which is an expression over the two things in it rather than a number.
static ZView text_size_row(ZApp *app, SettingsState *s) {
    float caps_h = z_line_height(app, Z_FONT_TITLE) + 2.0f * (float)Z_ROW_VPAD;
    float row_h = z_row_h(app);
    float h = caps_h > row_h ? caps_h : row_h;
    // NOT list_row(): this row is the slider's own legend, it never reflows (the
    // caps ARE the horizontal scale — stacking them destroys the control), and
    // its height is the caps', not the body text's.
    return Frame(0.0f, h,
        HStack(Frame(ROW_PAD, 1.0f, Rect(.color = z_rgba(0, 0, 0, 0))),
               Grow(1.0f, HStack(
                   Foreground(Z_COLOR_TEXT_MUTED, Font(Z_FONT_CAPTION2, Text("A"))),
                   Slider(app, &s->text_slider, .length = 380.0f,
                          .thickness = 6.0f),
                   Foreground(Z_COLOR_TEXT, Font(Z_FONT_TITLE, Text("A"))),
                   .spacing = Z_SPACE_S, .align = Z_ALIGN_CENTER)),
               Frame(ROW_PAD, 1.0f, Rect(.color = z_rgba(0, 0, 0, 0))),
               .spacing = 0, .align = Z_ALIGN_CENTER));
}

static ZView screen_accessibility(ZApp *app, void *props) {
    SettingsState *s = props;

    // The sample sits in its own card above the slider, at BODY — the step the
    // ladder is anchored on, so what it shows is what reading text will look
    // like. WrapText, not Text: at the largest size this sentence is wider than
    // the column, and a preview that ran off the card would be demonstrating the
    // wrong thing rather convincingly.
    ZView sample = Background(Z_COLOR_SURFACE,
        CornerRadius(Z_RADIUS_CARD,
            HStack(
                Frame(ROW_PAD, 1.0f, Rect(.color = z_rgba(0, 0, 0, 0))),
                WrapText(app, TEXT_SAMPLE, .width = INSET_TEXT_W,
                         .size = Z_FONT_BODY, .color = Z_COLOR_TEXT,
                         .line_gap = 4.0f),
                Frame(ROW_PAD, 1.0f, Rect(.color = z_rgba(0, 0, 0, 0))),
                .spacing = 0, .align = Z_ALIGN_CENTER,
                // The card's own breathing room. Its HORIZONTAL inset is the two
                // ROW_PAD frames above — `.padding` insets BOTH axes, so this
                // step is chosen for the vertical and lands on top of them.
                .padding = Z_SPACE_S)));

    ZView size_rows[] = {text_size_row(app, s)};
    ZView vis_rows[] = {
        toggle_row(app, 0x5E7107u, "Bold Text", s->bold_text, t_bold),
        toggle_row(app, 0x5E7109u, "Increase Contrast", s->increase_contrast,
                   t_increase_contrast),
        toggle_row(app, 0x5E7108u, "Reduce Motion", s->reduce_motion,
                   t_reduce_motion),
    };
    // The footer names the SETTING'S OWN LIMIT, which is the honest thing for an
    // accessibility screen to do: the three surfaces that do not follow it are
    // the ones a person will notice first, and finding that out by squinting at
    // the keyboard is worse than being told.
    ZView blocks[] = {
        section_block(app, "TEXT SIZE", sample, NULL),
        section_block(app, NULL, group(size_rows, 1),
            "Apps and system screens use this size. The status bar and the "
            "keyboard keep theirs."),
        section_block(app, NULL, group(vis_rows, 3),
            "Bold Text thickens every weight. Increase Contrast lightens "
            "secondary text and the lines between rows. Reduce Motion replaces "
            "slides and springs with instant changes."),
    };
    return settings_screen(app, "Accessibility", blocks, 3);
}

// KEYBOARD — and the only control on it is a DELETE (P49).
//
// The keyboard learns words you type that its shipped dictionary does not carry
// (system/keyboard/main.c has the full decision: what, where, and what never gets
// in). Two things make shipping that defensible rather than merely convenient,
// and both are on this screen: the user can see THAT there is a store and how big
// it is, and the user can empty it. A store with no way out of it is not a
// feature, it is a leak with a nice name.
//
// WHAT IS DELIBERATELY NOT HERE: the words. A screen listing what somebody typed
// is a shoulder-surfing surface of its own, and it is not needed to answer "what
// do you have, and get rid of it".
static void forget_learned(ZApp *app, void *state) {
    SettingsState *s = state;
    // An EPOCH, not a command. The keyboard may not be running; it compares this
    // against one it persisted and honours any value it has not seen, so a clear
    // requested now is applied on its next boot if it misses the broadcast.
    s->kbd_forget_epoch++;
    z_setting_set_int(ZELTO_KEY_KBD_FORGET, s->kbd_forget_epoch);
    s->kbd_learned = 0;
    fprintf(stderr, "[settings] clear learned words (epoch %lld)\n",
            (long long)s->kbd_forget_epoch);
    fflush(stderr);
    z_invalidate(app);
}

static ZView screen_keyboard(ZApp *app, void *props) {
    SettingsState *s = props;
    char count[32];
    snprintf(count, sizeof(count), "%lld", (long long)s->kbd_learned);
    ZView rows[] = {
        labelled(app, "Learned Words",
                 Foreground(Z_COLOR_TEXT_MUTED,
                            Font(Z_FONT_BODY, Text("%s", count)))),
    };
    // The DESTRUCTIVE action row: the same shape as "Lock Now" on the Lock
    // screen, in the danger colour, because it throws data away.
    ZView act = action_row(app, "Clear Learned Words", Z_COLOR_DANGER);
    ZView blocks[] = {
        section_block(app, NULL, group(rows, 1),
            "The keyboard learns a word after you have typed it a few times "
            "without correcting it, so it stops correcting it. Nothing typed "
            "into a password field is ever learned."),
        section_block(app, NULL, OnTap(forget_learned, act),
            "Forgets every learned word and deletes them from this device."),
    };
    return settings_screen(app, "Keyboard", blocks, 2);
}

// --- the root ---------------------------------------------------------------
// A SHORT list of doors, not a long list of switches.
//
// Settings used to be one flat scroll: every group in the app stacked into a
// single column you paged through to find anything, which is a settings DUMP
// rather than a settings app. A phone's Settings is a drill-down, and the reason
// is that the root is then scannable in one screenful — you read four labels
// instead of fifteen controls, and the thing you came for is one tap away
// instead of somewhere in a scroll.
//
// Each row carries its subject's current state in the detail column, so the root
// is still a status read-out: you can see Wi-Fi is on without opening Network.
static ZView screen_root(ZApp *app, void *props) {
    SettingsState *s = props ? props : g_state;

    static const SettingsRoute r_network = {screen_network};
    static const SettingsRoute r_display = {screen_display};
    static const SettingsRoute r_wallpaper = {screen_wallpaper};
    static const SettingsRoute r_lock = {screen_lock};
    static const SettingsRoute r_keyboard = {screen_keyboard};
    static const SettingsRoute r_access = {screen_accessibility};

    char bright[16];
    snprintf(bright, sizeof(bright), "%lld", (long long)s->brightness);
    char learned[32];
    snprintf(learned, sizeof(learned), "%lld learned",
             (long long)s->kbd_learned);

    ZView rows[] = {
        detail_row(app, "Network",
                   s->airplane ? "Airplane" : (s->wifi ? "Wi-Fi" : "Off"),
                   &r_network),
        detail_row(app, "Display & Sound", bright, &r_display),
        detail_row(app, "Wallpaper", s->wp_count > 0 ? "" : "None", &r_wallpaper),
        detail_row(app, "Lock Screen", s->lock_enabled ? "On" : "Off", &r_lock),
        detail_row(app, "Keyboard", learned, &r_keyboard),
        // The detail column carries the SIZE'S NAME, not its index: "4 of 7" is
        // a fact about the implementation, and the root list is meant to read as
        // a status line.
        detail_row(app, "Accessibility", z_text_size_name((int)s->text_size),
                   &r_access),
    };
    ZView blocks[] = {
        section_block(app, NULL, group(rows, 6),
            "These settings are shared with Control Center. Changes apply live "
            "and persist."),
    };
    return settings_screen(app, "Settings", blocks, 1);
}

static ZView settings_body(ZApp *app, SettingsState *state) {
    ensure_init(app, state);
    g_state = state;

    // Test hook (P45): ZELTO_SETTINGS_SET="key:value,key:value" writes those
    // settings through the broker on the first build.
    //
    // WHY A HOOK AND NOT A TAP. The SETTINGS harness this replaces navigated the
    // app drawer P40 deleted and tapped toggle chips at coordinates read off a
    // screenshot; it ended in an unconditional exit 0 and was disabled in P44.
    // What it was actually testing is that a brokered write reaches OTHER
    // PROCESSES — the shade, the bar, the dim scrim and the lock all observe the
    // same keys — and none of that needs a finger. This drives the write; zsysd's
    // own "settings_set K=V -> N subscriber(s)" reports the fan-out.
    if (!state->test_set_done) {
        state->test_set_done = true;
        const char *spec = getenv("ZELTO_SETTINGS_SET");
        if (spec && spec[0]) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s", spec);
            char *save = NULL;
            for (char *tok = strtok_r(buf, ",", &save); tok;
                 tok = strtok_r(NULL, ",", &save)) {
                char *colon = strchr(tok, ':');
                if (!colon) {
                    continue;
                }
                *colon = '\0';
                z_setting_set_int(tok, (int64_t)atoll(colon + 1));
            }
        }
    }

    // Pull the slider back in line with the setting, EXCEPT while it is being
    // dragged. The broker echoes every write back through on_changed, and it also
    // changes brightness on its own (zsysd nudges it down on low battery), so the
    // control has to follow the value — but doing that mid-drag would quantise
    // the finger's position to the nearest of five steps and make the knob stick.
    if (!state->bright_slider.dragging) {
        state->bright_slider.value = (float)(state->brightness - 1) / 4.0f;
    }
    // The same for the text-size slider: the broker echoes our own write back,
    // and a device seeded at another step must open with the knob where the
    // setting is. Not while dragging, or the finger would be quantised to the
    // nearest of seven steps and the knob would stick.
    if (!state->text_slider.dragging) {
        state->text_slider.value =
            (float)state->text_size / (float)(Z_TEXT_SIZE_STEPS - 1);
    }

    // Test hook: open straight onto a detail screen, so the shot catalogue can
    // photograph one without injecting a tap. Every other state in the catalogue
    // is reached by an env hook for the same reason — a screenshot that depends
    // on a pointer landing in the right place is a screenshot that silently
    // photographs the wrong screen when the layout moves.
    //
    // Built BEFORE the push: z_navigation(app) only exists once the Navigator
    // has been constructed, so seeding the stack first would push onto nothing.
    // The push lands on the next build, which a screenshot's settle delay covers.
    ZView nav = Navigator(app, .root = screen_root);

    if (!state->nav_seeded) {
        state->nav_seeded = true;
        const char *want = getenv("ZELTO_SETTINGS_SCREEN");
        if (want && want[0]) {
            ZScreenFn s = NULL;
            if (strcmp(want, "network") == 0) {
                s = screen_network;
            } else if (strcmp(want, "display") == 0) {
                s = screen_display;
            } else if (strcmp(want, "wallpaper") == 0) {
                s = screen_wallpaper;
            } else if (strcmp(want, "lock") == 0) {
                s = screen_lock;
            } else if (strcmp(want, "keyboard") == 0) {
                s = screen_keyboard;
            } else if (strcmp(want, "accessibility") == 0) {
                s = screen_accessibility;
            }
            if (s) {
                z_nav_push(z_navigation(app), s, state);
            }
        }
    }

    // Back out of a detail screen with the system back gesture (edge-swipe) or
    // Escape — the Navigator handles both, so no screen needs a back button.
    return nav;
}

Z_APP_ID(SettingsState, settings_body, "os.zelto.settings")
