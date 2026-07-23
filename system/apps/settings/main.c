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
    } else if (strcmp(key, "sys.kbd_learned") == 0) {
        s->kbd_learned = v;
    } else if (strcmp(key, ZELTO_WALLPAPER_KEY) == 0) {
        snprintf(s->wp_current, sizeof(s->wp_current), "%s", value ? value : "");
    }
    z_invalidate(app);
}

static void bright_slide(ZApp *app, void *state, float v);

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
    s->kbd_learned = z_setting_get_int("sys.kbd_learned", 0);
    s->kbd_forget_epoch = z_setting_get_int("sys.kbd_forget_learned", 0);
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
#define ROW_H ((float)Z_ROW_H)         // 81 — the row's TOTAL height (Apple's 44pt)
#define ROW_PAD ((float)Z_PT(16))      // 29 — leading/trailing inset inside a card
#define SEC_GAP 30.0f     // between one group's footer and the next group's header

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

// One list row: a fixed height so every row in every group shares a baseline
// rhythm (a row that sizes to its control makes a switch row and a stepper row
// different heights, and the list stops looking like a list).
//
// The inset is two explicit end gaps rather than Padding, because Padding is BOTH
// axes: as padding it would add its 29 above and below the fixed height and the
// row would be 139 tall. The content Grow(1)s between them so the label column
// still spans the card.
static ZView list_row(ZView content) {
    return Frame(0.0f, ROW_H,
        HStack(Frame(ROW_PAD, 1.0f, Rect(.color = z_rgba(0, 0, 0, 0))),
               Grow(1.0f, content),
               Frame(ROW_PAD, 1.0f, Rect(.color = z_rgba(0, 0, 0, 0))),
               .spacing = 0, .align = Z_ALIGN_CENTER));
}

// A label on the left, its control on the right. This is the whole grammar of the
// screen: the left column is scannable prose, the right column is the state.
static ZView labelled(const char *label, ZView control) {
    return list_row(HStack(
        Foreground(Z_COLOR_TEXT, Font(Z_FONT_BODY, Text("%s", label))),
        Spacer(),
        control,
        .spacing = 12, .align = Z_ALIGN_CENTER));
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

    return labelled(label, OnTap(act, sw));
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
static ZView stepper_row(const char *label, int64_t val, const char *unit,
                         ZAction dec, ZAction inc) {
    ZView pill = Background(Z_COLOR_SURFACE_3,
        CornerRadius(Z_RADIUS_CHIP,
            HStack(
                step_key(dec, "\xe2\x88\x92"),   // a real minus, not a hyphen
                Frame(1.0f, 22.0f, Rect(.color = Z_COLOR_BORDER)),
                step_key(inc, "+"),
                .spacing = 0, .align = Z_ALIGN_CENTER)));
    return list_row(HStack(
        Foreground(Z_COLOR_TEXT, Font(Z_FONT_BODY, Text("%s", label))),
        Spacer(),
        Frame(56.0f, 0.0f,
            HStack(Spacer(),
                Foreground(Z_COLOR_TEXT_MUTED,
                    Font(Z_FONT_BODY,
                         Text("%lld%s", (long long)val, unit))),
                .spacing = 0, .align = Z_ALIGN_CENTER)),
        pill,
        .spacing = 12, .align = Z_ALIGN_CENTER));
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
static ZView detail_row(const char *label, const char *value,
                        const SettingsRoute *route) {
    return OnTapData(push_screen, (void *)route,
        list_row(HStack(
            Foreground(Z_COLOR_TEXT, Font(Z_FONT_BODY, Text("%s", label))),
            Spacer(),
            Foreground(Z_COLOR_TEXT_MUTED,
                Font(Z_FONT_BODY, Text("%s", value ? value : ""))),
            chevron(),
            .spacing = 10, .align = Z_ALIGN_CENTER)));
}

// A label + slider row. Unlike the stepper this row has NO numeric read-out: the
// slider's own fill is the value, and a level whose whole point is "how bright
// does that look" does not gain anything from also being told it is a 4.
static ZView slider_row(ZApp *app, const char *label, ZSlider *sl) {
    return list_row(HStack(
        Foreground(Z_COLOR_TEXT, Font(Z_FONT_BODY, Text("%s", label))),
        Spacer(),
        Slider(app, sl, .length = 300.0f, .thickness = 6.0f),
        .spacing = 12, .align = Z_ALIGN_CENTER));
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
#define WP_THUMB_W 186.0f
#define WP_THUMB_H 118.0f
#define WP_COLS 3

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
    ZStackOpts grid = {.spacing = 12, .align = Z_ALIGN_LEADING};
    int k = 0;
    for (int i = 0; i < s->wp_count && k < Z_MAX_CHILDREN; i += WP_COLS) {
        ZStackOpts row = {.spacing = 12, .align = Z_ALIGN_CENTER};
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
    col.children[k++] = HStack(
        Frame(ROW_PAD, 1.0f, Rect(.color = z_rgba(0, 0, 0, 0))),
        Weight(Z_WEIGHT_BOLD,
            Foreground(Z_COLOR_TEXT, Font(Z_FONT_LARGE_TITLE, Text("%s", title)))),
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
        : group((ZView[]){labelled("Wallpaper",
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
        stepper_row("Dim After", s->dim_s, "s", dim_dec, dim_inc),
        stepper_row("Lock After", s->lock_s, "s", lock_dec, lock_inc),
        stepper_row("Screen Off After", s->off_s, "s", off_dec, off_inc),
    };
    // The one ACTION here. An action row is a full-width card with a centred
    // label and no control: it does something now rather than holding a state,
    // and centring it is how iOS says so.
    ZView act = Background(Z_COLOR_SURFACE,
        CornerRadius(Z_RADIUS_CARD,
            Frame(0.0f, ROW_H,
                ZStack(
                    Weight(Z_WEIGHT_SEMIBOLD,
                        Foreground(Z_COLOR_TEXT,
                            Font(Z_FONT_BODY, Text("Lock Now")))),
                    .align = Z_ALIGN_CENTER))));
    ZView blocks[] = {
        section_block(app, NULL, group(rows, 5),
            "Each delay is measured from your last touch."),
        section_block(app, NULL, OnTap(lock_now, act), NULL),
    };
    return settings_screen(app, "Lock Screen", blocks, 2);
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
    z_setting_set_int("sys.kbd_forget_learned", s->kbd_forget_epoch);
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
        labelled("Learned Words",
                 Foreground(Z_COLOR_TEXT_MUTED,
                            Font(Z_FONT_BODY, Text("%s", count)))),
    };
    // The DESTRUCTIVE action row: the same shape as "Lock Now" on the Lock
    // screen, in the danger colour, because it throws data away.
    ZView act = Background(Z_COLOR_SURFACE,
        CornerRadius(Z_RADIUS_CARD,
            Frame(0.0f, ROW_H,
                ZStack(
                    Weight(Z_WEIGHT_SEMIBOLD,
                        Foreground(Z_COLOR_DANGER,
                            Font(Z_FONT_BODY, Text("Clear Learned Words")))),
                    .align = Z_ALIGN_CENTER))));
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

    char bright[16];
    snprintf(bright, sizeof(bright), "%lld", (long long)s->brightness);
    char learned[32];
    snprintf(learned, sizeof(learned), "%lld learned",
             (long long)s->kbd_learned);

    ZView rows[] = {
        detail_row("Network",
                   s->airplane ? "Airplane" : (s->wifi ? "Wi-Fi" : "Off"),
                   &r_network),
        detail_row("Display & Sound", bright, &r_display),
        detail_row("Wallpaper", s->wp_count > 0 ? "" : "None", &r_wallpaper),
        detail_row("Lock Screen", s->lock_enabled ? "On" : "Off", &r_lock),
        detail_row("Keyboard", learned, &r_keyboard),
    };
    ZView blocks[] = {
        section_block(app, NULL, group(rows, 5),
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
