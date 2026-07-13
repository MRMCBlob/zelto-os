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

// A small rounded chip (the On/Off toggle face or a stepper button).
static ZView chip(ZColor bg, const char *label) {
    return Background(bg,
        CornerRadius(16.0f,
            Padding(14.0f,
                Foreground(Z_COLOR_TEXT_INV,
                    Font(Z_FONT_CALLOUT, Text("%s", label))))));
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
    ZColor bg = z_color_lerp(Z_COLOR_SURFACE_3, Z_COLOR_PRIMARY,
                             z_animated_get(t));
    return HStack(
        Foreground(Z_COLOR_TEXT_INV, Font(Z_FONT_CALLOUT, Text("%s", label))),
        Spacer(),
        OnTap(act, chip(bg, on ? "On" : "Off")),
        .spacing = 12, .align = Z_ALIGN_CENTER);
}

// A label + [-] value [+] stepper row (idle timeout controls).
static ZView stepper_row(const char *label, int64_t val, const char *unit,
                         ZAction dec, ZAction inc) {
    ZColor dark = Z_COLOR_SURFACE_3;
    return HStack(
        Foreground(Z_COLOR_TEXT_INV, Font(Z_FONT_CALLOUT, Text("%s", label))),
        Spacer(),
        OnTap(dec, chip(dark, "-")),
        Foreground(Z_COLOR_TEXT_INV,
            Font(Z_FONT_CALLOUT, Text("%lld%s", (long long)val, unit))),
        OnTap(inc, chip(dark, "+")),
        .spacing = 10, .align = Z_ALIGN_CENTER);
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
#define WP_THUMB_W 150.0f
#define WP_THUMB_H 96.0f
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

// A grid (WP_COLS per row) of wallpaper thumbnails, padding a partial last row
// with Spacers so real tiles keep their column width.
static ZView wp_grid(SettingsState *s) {
    ZStackOpts grid = {.spacing = 12, .align = Z_ALIGN_LEADING};
    int k = 0;
    for (int i = 0; i < s->wp_count && k < Z_MAX_CHILDREN; i += WP_COLS) {
        ZStackOpts row = {.spacing = 12, .align = Z_ALIGN_CENTER};
        for (int c = 0; c < WP_COLS; c++) {
            int j = i + c;
            row.children[c] = j < s->wp_count ? wp_thumb(s, j) : Spacer();
        }
        grid.children[k++] = z_stack(Z_AXIS_HORIZONTAL, &row);
    }
    return z_stack(Z_AXIS_VERTICAL, &grid);
}

static ZView settings_body(ZApp *app, SettingsState *state) {
    ensure_init(app, state);

    ZColor dark = Z_COLOR_SURFACE_3;
    ZStackOpts col = {.spacing = 14, .align = Z_ALIGN_LEADING};
    int k = 0;
    col.children[k++] = Foreground(Z_COLOR_TEXT_INV,
        Font(Z_FONT_TITLE, Text("Settings")));
    col.children[k++] = toggle_row(app, 0x5E7101u, "Wi-Fi", state->wifi, t_wifi);
    col.children[k++] = toggle_row(app, 0x5E7102u, "Mute", state->mute, t_mute);
    col.children[k++] = toggle_row(app, 0x5E7103u, "Brightness boost",
                                   state->bright, t_bright);
    col.children[k++] = toggle_row(app, 0x5E7104u, "Airplane mode",
                                   state->airplane, t_airplane);
    // Brightness level stepper (the "at least one more control").
    col.children[k++] = HStack(
        Foreground(Z_COLOR_TEXT_INV, Font(Z_FONT_CALLOUT, Text("Brightness"))),
        Spacer(),
        OnTap(bright_dec, chip(dark, "-")),
        Foreground(Z_COLOR_TEXT_INV,
            Font(Z_FONT_TITLE, Text("%lld", (long long)state->brightness))),
        OnTap(bright_inc, chip(dark, "+")),
        .spacing = 14, .align = Z_ALIGN_CENTER);

    // --- P25 wallpaper section ---
    col.children[k++] = Foreground(Z_COLOR_TEXT_MUTED,
        Font(Z_FONT_CAPTION, Text("WALLPAPER")));
    if (state->wp_count > 0) {
        col.children[k++] = wp_grid(state);
    } else {
        col.children[k++] = Foreground(Z_COLOR_TEXT_FAINT,
            Text("No wallpapers found"));
    }

    // --- P20 lock screen section ---
    col.children[k++] = Foreground(Z_COLOR_TEXT_MUTED,
        Font(Z_FONT_CAPTION, Text("LOCK SCREEN")));
    col.children[k++] = toggle_row(app, 0x5E7105u, "Lock screen",
                                   state->lock_enabled, t_lock);
    col.children[k++] = stepper_row("Dim after", state->dim_s, "s",
                                    dim_dec, dim_inc);
    col.children[k++] = stepper_row("Lock after", state->lock_s, "s",
                                    lock_dec, lock_inc);
    col.children[k++] = stepper_row("Screen off after", state->off_s, "s",
                                    off_dec, off_inc);
    col.children[k++] = toggle_row(app, 0x5E7106u, "Passcode (1234)",
                                   state->passcode_set, t_passcode);
    col.children[k++] = OnTap(lock_now,
        Background(Z_COLOR_PRIMARY,
            CornerRadius(14.0f,
                Padding(14.0f,
                    Foreground(Z_COLOR_ON_PRIMARY,
                        Font(Z_FONT_CALLOUT, Text("Lock now")))))));
    col.children[k++] = Foreground(Z_COLOR_TEXT_MUTED,
        Font(Z_FONT_CAPTION,
            Text("Shared with the shade — changes apply live, and persist")));

    // Scroll so the whole list stays reachable in the usable area (top bar +
    // bottom nav shrink it below the content height once the lock section is in).
    return Background(Z_COLOR_BG,
        Fill(Scroll(app,
            VStack(
                Frame(560.0f, 0.0f, z_stack(Z_AXIS_VERTICAL, &col)),
                .padding = 24, .spacing = 0, .align = Z_ALIGN_CENTER),
            .axis = Z_AXIS_VERTICAL)));
}

Z_APP_ID(SettingsState, settings_body, "os.zelto.settings")
