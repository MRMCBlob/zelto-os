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

#include "common/wallpaper.h"

typedef struct SettingsState {
    bool inited;
    bool wifi, mute, bright, airplane;
    int64_t brightness;   // 1..5
    // P20 lock screen / idle lifecycle.
    bool lock_enabled;
    int64_t dim_s, lock_s, off_s;
    bool passcode_set;
    int64_t lock_now;

    // P25 wallpaper picker. The wallpaper dir listed once into wp_paths (kept in
    // state so the OnTapData pointer we hand each thumbnail stays valid across the
    // per-frame rebuild), and wp_current mirrors the brokered sys.wallpaper so the
    // selected tile is highlighted and updates live when it changes.
    char wp_paths[ZELTO_WALLPAPER_MAX][ZELTO_WALLPAPER_PATH_MAX];
    int wp_count;
    char wp_current[ZELTO_WALLPAPER_PATH_MAX];
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
    } else if (strcmp(key, ZELTO_WALLPAPER_KEY) == 0) {
        snprintf(s->wp_current, sizeof(s->wp_current), "%s", value ? value : "");
    }
    z_invalidate(app);
}

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
    s->brightness = z_setting_get_int("sys.brightness", 3);
    s->lock_enabled = z_setting_get_int("sys.lock_enabled", 0) != 0;
    s->dim_s = z_setting_get_int("sys.idle_dim_s", 8);
    s->lock_s = z_setting_get_int("sys.idle_lock_s", 20);
    s->off_s = z_setting_get_int("sys.idle_off_s", 120);
    s->passcode_set = z_setting_get_str("sys.passcode", "")[0] != '\0';
    s->lock_now = z_setting_get_int("sys.lock_now", 0);
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
static void bright_dec(ZApp *app, void *state) {
    SettingsState *s = state;
    if (s->brightness > 1) {
        s->brightness--;
    }
    z_setting_set_int("sys.brightness", s->brightness);
    z_invalidate(app);
}
static void bright_inc(ZApp *app, void *state) {
    SettingsState *s = state;
    if (s->brightness < 5) {
        s->brightness++;
    }
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
#define ROW_H 44.0f       // a row's CONTENT height; ROW_PAD adds the air around it
#define ROW_PAD 16.0f     // a row's leading/trailing inset inside its card
#define SEC_GAP 30.0f     // between one group's footer and the next group's header

// A fixed gap. NOT Frame(w, h, Spacer()) — a Spacer keeps its grow flag through
// Frame and eats the stack's spare space; an empty transparent Rect does not.
static ZView gap(float h) {
    return Frame(1.0f, h, Rect(.color = z_rgba(0, 0, 0, 0)));
}

// A header or footer line, inset to sit under the card's text column rather than
// under the card's edge — the alignment is what makes the three parts read as one
// group instead of as three stacked objects.
static ZView inset_text(const char *s, ZFont size, ZColor ink, ZWeight w) {
    return HStack(
        Frame(ROW_PAD, 1.0f, Rect(.color = z_rgba(0, 0, 0, 0))),
        Weight(w, Foreground(ink, Font(size, Text("%s", s)))),
        .spacing = 0, .align = Z_ALIGN_CENTER);
}

// One list row: a fixed height so every row in every group shares a baseline
// rhythm (a row that sizes to its control makes a switch row and a stepper row
// different heights, and the list stops looking like a list).
static ZView list_row(ZView content) {
    return Frame(0.0f, ROW_H, Padding(ROW_PAD, content));
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
static ZView section_block(const char *header, ZView card, const char *foot) {
    ZStackOpts col = {.spacing = 0, .align = Z_ALIGN_LEADING};
    int k = 0;
    if (header) {
        col.children[k++] = inset_text(header, Z_FONT_FOOTNOTE,
                                       Z_COLOR_TEXT_MUTED, Z_WEIGHT_SEMIBOLD);
        col.children[k++] = gap(8.0f);
    }
    col.children[k++] = card;
    if (foot) {
        col.children[k++] = gap(8.0f);
        col.children[k++] = inset_text(foot, Z_FONT_FOOTNOTE, Z_COLOR_TEXT_FAINT,
                                       Z_WEIGHT_REGULAR);
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

static ZView settings_body(ZApp *app, SettingsState *state) {
    ensure_init(app, state);

    ZStackOpts col = {.spacing = SEC_GAP, .align = Z_ALIGN_LEADING};
    int k = 0;
    // A large title, the way a phone's settings screen opens. It scrolls with the
    // list (it is the list's first item, not a fixed chrome bar) — the iOS large
    // title collapses into the bar as you scroll, and the honest version of that
    // with no navigation bar to collapse into is simply to let it leave.
    col.children[k++] = HStack(
        Frame(ROW_PAD, 1.0f, Rect(.color = z_rgba(0, 0, 0, 0))),
        Weight(Z_WEIGHT_BOLD,
            Foreground(Z_COLOR_TEXT, Font(Z_FONT_LARGE_TITLE, Text("Settings")))),
        .spacing = 0, .align = Z_ALIGN_CENTER);

    // GROUPING IS THE DESIGN. The old screen was one card of five unrelated
    // switches followed by another of five: Wi-Fi sat next to Brightness because
    // they were both booleans, which is a programmer's taxonomy. These groups are
    // by SUBJECT — what you came here to change — and each one's footer says what
    // the group actually does, so no row has to explain itself.
    ZView net_rows[] = {
        toggle_row(app, 0x5E7104u, "Airplane Mode", state->airplane, t_airplane),
        toggle_row(app, 0x5E7101u, "Wi-Fi", state->wifi, t_wifi),
    };
    col.children[k++] = section_block("NETWORK", group(net_rows, 2),
        "Airplane Mode turns the radios off. Network calls fail while it is on.");

    ZView disp_rows[] = {
        stepper_row("Brightness", state->brightness, "", bright_dec, bright_inc),
        toggle_row(app, 0x5E7103u, "Brightness Boost", state->bright, t_bright),
        toggle_row(app, 0x5E7102u, "Silent", state->mute, t_mute),
    };
    col.children[k++] = section_block("DISPLAY & SOUND", group(disp_rows, 3),
        "Brightness runs 1 to 5 and dims the screen with a scrim.");

    // --- P25 wallpaper section ---
    // The picker lives INSIDE a card like every other group, rather than floating
    // on the background: a bare grid between two cards reads as a different screen
    // that got pasted in.
    ZView wp_card = state->wp_count > 0
        ? Background(Z_COLOR_SURFACE,
              CornerRadius(Z_RADIUS_CARD, Padding(ROW_PAD, wp_grid(state))))
        : group((ZView[]){labelled("Wallpaper",
                    Foreground(Z_COLOR_TEXT_MUTED,
                        Font(Z_FONT_BODY, Text("None found"))))}, 1);
    col.children[k++] = section_block("WALLPAPER", wp_card,
        "Shown on the Home and Lock screens.");

    // --- P20 lock screen section ---
    ZView lock_rows[] = {
        toggle_row(app, 0x5E7105u, "Lock Screen", state->lock_enabled, t_lock),
        toggle_row(app, 0x5E7106u, "Passcode (1234)", state->passcode_set,
                   t_passcode),
        stepper_row("Dim After", state->dim_s, "s", dim_dec, dim_inc),
        stepper_row("Lock After", state->lock_s, "s", lock_dec, lock_inc),
        stepper_row("Screen Off After", state->off_s, "s", off_dec, off_inc),
    };
    col.children[k++] = section_block("LOCK SCREEN", group(lock_rows, 5),
        "Each delay is measured from your last touch.");

    // The one ACTION on the screen. An action row is a full-width card with a
    // centred label and no control: it does something now, rather than holding a
    // state, and centring it is how iOS says so.
    ZView act = Background(Z_COLOR_SURFACE,
        CornerRadius(Z_RADIUS_CARD,
            Frame(0.0f, ROW_H,
                ZStack(
                    Weight(Z_WEIGHT_SEMIBOLD,
                        Foreground(Z_COLOR_TEXT,
                            Font(Z_FONT_BODY, Text("Lock Now")))),
                    .align = Z_ALIGN_CENTER))));
    col.children[k++] = section_block(NULL, OnTap(lock_now, act),
        "These settings are shared with Control Center. Changes apply live and "
        "persist.");
    col.children[k++] = gap(24.0f);

    // Scroll so the whole list stays reachable in the usable area (top bar +
    // home indicator shrink it below the content height once every group is in).
    return Background(Z_COLOR_BG,
        Fill(Scroll(app,
            VStack(
                Frame(LIST_W, 0.0f, z_stack(Z_AXIS_VERTICAL, &col)),
                .padding = 20, .spacing = 0, .align = Z_ALIGN_CENTER),
            .axis = Z_AXIS_VERTICAL)));
}

Z_APP_ID(SettingsState, settings_body, "os.zelto.settings")
