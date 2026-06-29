// Zelto System UI — app launcher.
//
// A normal xdg_toplevel app (app_id "os.zelto.launcher") that the compositor
// keeps at the back of the app stack and sizes to the usable area below the
// status bar. It shows two things:
//
//   1. Install tiles — one per app discovered on disk. App metadata is read from
//      /usr/share/zelto/apps/<id>.app manifests (a dead-simple key=value text
//      file) at startup, not a hardcoded C array. Tapping a tile fork()+exec()s
//      the app's `exec=` binary, which connects to the same Wayland socket and
//      maps its own toplevel in front.
//
//   2. A "Running" section — a live task switcher built from
//      z_running_apps() (wlr-foreign-toplevel-management). Each card taps to
//      switch to that app (z_task_activate) or close it (the × button →
//      z_task_close). The list updates as apps open, change focus, and close.
//
// The Home chord (handled in zcomp) brings this launcher back to the front; the
// compositor broadcasts the xdg activated state so the front app is Active and
// the rest are paused. See docs/platform/app-lifecycle.md.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <zelto/ui.h>

#define MANIFEST_DIR "/usr/share/zelto/apps"
#define MAX_APPS 32

// One installed app, parsed from a .app manifest.
typedef struct AppEntry {
    char name[64];
    char subtitle[96];
    char exec_path[160];
    ZColor color;
} AppEntry;

static AppEntry g_apps[MAX_APPS];
static int g_n_apps;
static bool g_scanned;

// Parse "RRGGBB" hex into an opaque ZColor (defaults grey on a bad value).
static ZColor parse_color(const char *hex) {
    if (!hex || strlen(hex) < 6) {
        return z_rgba(0x3a, 0x42, 0x4c, 0xff);
    }
    long v = strtol(hex, NULL, 16);
    return z_rgba((uint8_t)((v >> 16) & 0xff), (uint8_t)((v >> 8) & 0xff),
                  (uint8_t)(v & 0xff), 0xff);
}

// Strip a trailing newline/CR in place.
static void chomp(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

// Parse one manifest file into `e`. Returns true if it has at least name+exec.
static bool parse_manifest(const char *path, AppEntry *e) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }
    memset(e, 0, sizeof(*e));
    e->color = z_rgba(0x3a, 0x42, 0x4c, 0xff);
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        chomp(line);
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') {
            continue;
        }
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;
        if (strcmp(key, "name") == 0) {
            snprintf(e->name, sizeof(e->name), "%s", val);
        } else if (strcmp(key, "subtitle") == 0) {
            snprintf(e->subtitle, sizeof(e->subtitle), "%s", val);
        } else if (strcmp(key, "exec") == 0) {
            snprintf(e->exec_path, sizeof(e->exec_path), "%s", val);
        } else if (strcmp(key, "color") == 0) {
            e->color = parse_color(val);
        }
    }
    fclose(f);
    return e->name[0] && e->exec_path[0];
}

// Order tiles by display name (case-insensitive), for a stable layout.
static int app_name_cmp(const void *a, const void *b) {
    const AppEntry *ea = a, *eb = b;
    return strcasecmp(ea->name, eb->name);
}

// Scan one manifest dir, appending each parseable <id>.app to g_apps.
static void scan_apps_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) && g_n_apps < MAX_APPS) {
        const char *name = de->d_name;
        size_t n = strlen(name);
        if (n < 5 || strcmp(name + n - 4, ".app") != 0) {
            continue;
        }
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        if (parse_manifest(path, &g_apps[g_n_apps])) {
            g_n_apps++;
        }
    }
    closedir(d);
}

// Scan the manifest directories once into g_apps. Two sources: the baked-in
// /usr/share/zelto/apps, plus $ZELTO_DATA_DIR/apps/manifests where zelto-install
// registers runtime-installed .zap packages (P13). A package installed on a prior
// boot therefore appears as a tile on the next startup scan.
static void ensure_apps(void) {
    if (g_scanned) {
        return;
    }
    g_scanned = true;
    scan_apps_dir(MANIFEST_DIR);
    const char *data = getenv("ZELTO_DATA_DIR");
    if (data && data[0]) {
        char runtime_dir[256];
        snprintf(runtime_dir, sizeof(runtime_dir), "%s/apps/manifests", data);
        scan_apps_dir(runtime_dir);
    }
    // readdir order is filesystem-dependent (and differs between the baked-in and
    // runtime dirs); sort by name so the tile layout is deterministic — the
    // headless harness reads fixed tile coordinates off a captured frame.
    qsort(g_apps, (size_t)g_n_apps, sizeof(g_apps[0]), app_name_cmp);
}

typedef struct LauncherState {
    int unused;
} LauncherState;

// --- install tiles --------------------------------------------------------
// Tile tap: spawn the app binary. The child inherits WAYLAND_DISPLAY/ZELTO_FONT
// from the launcher's environment (set by the initramfs init).
static void launch_app(ZApp *app, void *state, void *data) {
    (void)app;
    (void)state;
    const AppEntry *e = data;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();  // detach so the app outlives a launcher relaunch
        execlp(e->exec_path, e->exec_path, (char *)NULL);
        _exit(127);  // exec failed
    }
}

// Tiles are kept compact so the full app list (now ~9 entries incl. the P13
// Store + a runtime-installed Widget) fits on one screen without overflowing
// past the bottom edge — every tile must be tappable by the headless harness.
static ZView tile(const AppEntry *e) {
    return OnTapData(launch_app, (void *)e,
        Background(e->color,
            HStack(
                Frame(40.0f, 40.0f,
                    Rect(.color = z_rgba(0xff, 0xff, 0xff, 0x33), .radius = 12)),
                VStack(
                    Foreground(Z_COLOR_TEXT_INV,
                        Font(Z_FONT_CALLOUT, Text("%s", e->name))),
                    Foreground(z_rgba(0xff, 0xff, 0xff, 0xcc),
                        Text("%s", e->subtitle)),
                    .spacing = 2, .align = Z_ALIGN_LEADING),
                Spacer(),
                .padding = 10, .spacing = 12, .align = Z_ALIGN_CENTER)));
}

// --- running cards (task switcher) ----------------------------------------
// Tapping a card switches to that app; tapping its × closes it. Both read the
// ZTask from the stable z_running_apps snapshot bound at build time.
static void activate_task(ZApp *app, void *state, void *data) {
    (void)state;
    z_task_activate(app, (const ZTask *)data);
}
static void close_task(ZApp *app, void *state, void *data) {
    (void)state;
    z_task_close(app, (const ZTask *)data);
}

static ZView running_card(const ZTask *t) {
    const char *title = t->title ? t->title
                                 : (t->app_id ? t->app_id : "App");
    // Active = green, paused = amber, so the foreground app is obvious in a frame.
    ZColor bg = t->active ? z_rgba(0x1d, 0x5e, 0x3a, 0xff)
                          : z_rgba(0x5e, 0x49, 0x1d, 0xff);
    return OnTapData(activate_task, (void *)t,
        Background(bg,
            HStack(
                VStack(
                    Foreground(Z_COLOR_TEXT_INV,
                        Font(Z_FONT_CALLOUT, Text("%s", title))),
                    Foreground(z_rgba(0xff, 0xff, 0xff, 0xcc),
                        Text("%s", t->active ? "Active" : "Paused")),
                    .spacing = 4, .align = Z_ALIGN_LEADING),
                Spacer(),
                // Close button: a big red square so it is an easy tap target.
                OnTapData(close_task, (void *)t,
                    Background(z_rgba(0xc8, 0x3a, 0x3a, 0xff),
                        Frame(52.0f, 52.0f,
                            CornerRadius(12,
                                Foreground(Z_COLOR_TEXT_INV,
                                    Font(Z_FONT_TITLE, Text("X"))))))),
                .padding = 14, .spacing = 12, .align = Z_ALIGN_CENTER)));
}

static ZView launcher_body(ZApp *app, LauncherState *state) {
    (void)state;
    ensure_apps();

    ZStackOpts opts = {.padding = 14, .spacing = 10, .align = Z_ALIGN_LEADING};
    int k = 0;

    opts.children[k++] =
        Foreground(Z_COLOR_TEXT_INV, Font(Z_FONT_TITLE, Text("Apps")));

    for (int i = 0; i < g_n_apps && k < Z_MAX_CHILDREN - 2; i++) {
        opts.children[k++] = tile(&g_apps[i]);
    }

    // Running section: a live list of the other app windows.
    int n_running = 0;
    const ZTask *running = z_running_apps(app, &n_running);
    if (n_running > 0) {
        opts.children[k++] = Foreground(z_rgba(0x9a, 0xa4, 0xad, 0xff),
            Font(Z_FONT_CALLOUT, Text("Running")));
        for (int i = 0; i < n_running && k < Z_MAX_CHILDREN - 1; i++) {
            opts.children[k++] = running_card(&running[i]);
        }
    }

    opts.children[k++] = Spacer();

    return Background(z_rgba(0x0b, 0x0e, 0x13, 0xff),
                      z_stack(Z_AXIS_VERTICAL, &opts));
}

Z_APP_ID(LauncherState, launcher_body, "os.zelto.launcher")
