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

typedef struct SettingsState {
    bool inited;
    bool wifi, mute, bright, airplane;
    int64_t brightness;   // 1..5
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
    z_settings_observe(app, on_changed, s);
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

// A small rounded chip (the On/Off toggle face or a stepper button).
static ZView chip(ZColor bg, const char *label) {
    return Background(bg,
        CornerRadius(16.0f,
            Padding(14.0f,
                Foreground(Z_COLOR_TEXT_INV,
                    Font(Z_FONT_CALLOUT, Text("%s", label))))));
}

// One label + On/Off toggle row.
static ZView toggle_row(const char *label, bool on, ZAction act) {
    ZColor bg = on ? z_rgba(0x2e, 0x9b, 0xff, 0xff)
                   : z_rgba(0x1b, 0x22, 0x2c, 0xff);
    return HStack(
        Foreground(Z_COLOR_TEXT_INV, Font(Z_FONT_CALLOUT, Text("%s", label))),
        Spacer(),
        OnTap(act, chip(bg, on ? "On" : "Off")),
        .spacing = 12, .align = Z_ALIGN_CENTER);
}

static ZView settings_body(ZApp *app, SettingsState *state) {
    ensure_init(app, state);

    ZColor dark = z_rgba(0x1b, 0x22, 0x2c, 0xff);
    ZStackOpts col = {.spacing = 14, .align = Z_ALIGN_LEADING};
    int k = 0;
    col.children[k++] = Foreground(Z_COLOR_TEXT_INV,
        Font(Z_FONT_TITLE, Text("Settings")));
    col.children[k++] = toggle_row("Wi-Fi", state->wifi, t_wifi);
    col.children[k++] = toggle_row("Mute", state->mute, t_mute);
    col.children[k++] = toggle_row("Brightness boost", state->bright, t_bright);
    col.children[k++] = toggle_row("Airplane mode", state->airplane, t_airplane);
    // Brightness level stepper (the "at least one more control").
    col.children[k++] = HStack(
        Foreground(Z_COLOR_TEXT_INV, Font(Z_FONT_CALLOUT, Text("Brightness"))),
        Spacer(),
        OnTap(bright_dec, chip(dark, "-")),
        Foreground(Z_COLOR_TEXT_INV,
            Font(Z_FONT_TITLE, Text("%lld", (long long)state->brightness))),
        OnTap(bright_inc, chip(dark, "+")),
        .spacing = 14, .align = Z_ALIGN_CENTER);
    col.children[k++] = Foreground(z_rgba(0x9a, 0xc4, 0xf0, 0xff),
        Font(Z_FONT_CAPTION,
            Text("Shared with the shade — changes apply live, and persist")));

    return Background(z_rgba(0x14, 0x18, 0x24, 0xff),
        VStack(
            Spacer(),
            Frame(560.0f, 0.0f, z_stack(Z_AXIS_VERTICAL, &col)),
            Spacer(),
            .padding = 24, .spacing = 14, .align = Z_ALIGN_CENTER));
}

Z_APP_ID(SettingsState, settings_body, "os.zelto.settings")
