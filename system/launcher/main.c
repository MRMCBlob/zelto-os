// Zelto System UI — app launcher.
//
// A normal xdg_toplevel app (app_id "os.zelto.launcher") that the compositor
// keeps at the back of the app stack and sizes to the usable area below the
// status bar. It shows a tile per installed app; tapping a tile fork()+exec()s
// that app binary, which connects to the same Wayland socket and maps its own
// toplevel in front. The compositor stays oblivious to "apps" — there is no new
// protocol, just process launch. The Home chord (handled in zcomp) brings this
// launcher back to the front. See docs/platform/app-lifecycle.md.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <zelto/ui.h>

// Static registry of launchable apps: a label, a tile colour, and the binary to
// exec. zcomp assigns each launched toplevel the usable area below the bar.
typedef struct AppEntry {
    const char *name;
    const char *subtitle;
    const char *exec_path;
    ZColor color;
} AppEntry;

static const AppEntry g_apps[] = {
    {"Rows", "Scrollable list + navigation", "/usr/bin/zelto-hello",
     {0xfa, 0x66, 0x26, 0xff}},
    {"Cards", "Tap to count", "/usr/bin/zelto-cards",
     {0x2e, 0x9b, 0xff, 0xff}},
};
#define N_APPS ((int)(sizeof(g_apps) / sizeof(g_apps[0])))

typedef struct LauncherState {
    int unused;
} LauncherState;

// Tile tap: spawn the app binary. The child inherits WAYLAND_DISPLAY/ZELTO_FONT
// from the launcher's environment (set by the initramfs init).
static void launch_app(ZApp *app, void *state, void *data) {
    (void)app;
    (void)state;
    const AppEntry *e = data;
    pid_t pid = fork();
    if (pid == 0) {
        // Detach from the launcher's session so it outlives a relaunch.
        setsid();
        execlp(e->exec_path, e->exec_path, (char *)NULL);
        _exit(127);  // exec failed
    }
}

static ZView tile(const AppEntry *e) {
    return OnTapData(launch_app, (void *)e,
        Background(e->color,
            HStack(
                // App "icon": a rounded white-ish square.
                Frame(56.0f, 56.0f,
                    Rect(.color = z_rgba(0xff, 0xff, 0xff, 0x33), .radius = 14)),
                VStack(
                    Foreground(Z_COLOR_TEXT_INV,
                        Font(Z_FONT_CALLOUT, Text("%s", e->name))),
                    Foreground(z_rgba(0xff, 0xff, 0xff, 0xcc),
                        Text("%s", e->subtitle)),
                    .spacing = 4, .align = Z_ALIGN_LEADING),
                Spacer(),
                .padding = 16, .spacing = 16, .align = Z_ALIGN_CENTER)));
}

static ZView launcher_body(ZApp *app, LauncherState *state) {
    (void)app;
    (void)state;
    return Background(z_rgba(0x0b, 0x0e, 0x13, 0xff),
        VStack(
            Foreground(Z_COLOR_TEXT_INV,
                Font(Z_FONT_TITLE, Text("Apps"))),
            tile(&g_apps[0]),
            tile(&g_apps[1]),
            Spacer(),
            .padding = 24, .spacing = 18, .align = Z_ALIGN_LEADING));
}

Z_APP_ID(LauncherState, launcher_body, "os.zelto.launcher")
