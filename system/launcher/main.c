// Zelto System UI — app launcher.
//
// A normal xdg_toplevel app (app_id "os.zelto.launcher") that the compositor
// keeps at the back of the app stack and sizes to the usable area below the
// status bar (and above the bottom nav bar). It is a phone-style home screen: a
// grid of square app icons, one per app discovered on disk. App metadata is read
// from /usr/share/zelto/apps/<id>.app manifests (a dead-simple key=value text
// file) plus $ZELTO_DATA_DIR/apps/manifests at startup, not a hardcoded C array.
// Tapping an icon fork()+exec()s the app's `exec=` binary, which connects to the
// same Wayland socket and maps its own toplevel in front.
//
// The live task switcher that used to share this surface (the "Running" section)
// now lives in the Recents overlay (zelto-recents), reached from the bottom nav
// bar; the home screen is just the grid. The Home button (and the Home chord in
// zcomp) brings this launcher back to the front via foreign-toplevel activate;
// the compositor broadcasts the xdg activated state so the front app is Active
// and the rest are paused. See docs/platform/app-lifecycle.md.
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

// --- home grid ------------------------------------------------------------
// Icon tap: spawn the app binary. The child inherits WAYLAND_DISPLAY/ZELTO_FONT
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

#define GRID_COLS 4
#define ICON_SIZE 104.0f
#define ICON_RADIUS 24.0f

// One phone-style home-screen cell: a square coloured icon (the app's initial
// centred in it) with the app name as a caption below. No PNG assets yet —
// real assets/icon.png rendering is Planned; the coloured rounded square + glyph
// stands in. The whole cell is the tap target (fork/exec of exec=). Grow(1) so a
// row of GRID_COLS cells splits the width evenly (empty trailing slots are
// Spacers, which grow the same, keeping real cells at one column wide).
static ZView grid_cell(const AppEntry *e) {
    char glyph = e->name[0] ? e->name[0] : '?';
    if (glyph >= 'a' && glyph <= 'z') {
        glyph = (char)(glyph - 'a' + 'A');
    }
    return Grow(1.0f,
        OnTapData(launch_app, (void *)e,
            VStack(
                Frame(ICON_SIZE, ICON_SIZE,
                    Background(e->color,
                        CornerRadius(ICON_RADIUS,
                            ZStack(
                                Foreground(Z_COLOR_TEXT_INV,
                                    Font(Z_FONT_LARGE_TITLE,
                                        Text("%c", glyph))),
                                .align = Z_ALIGN_CENTER)))),
                Foreground(Z_COLOR_TEXT_INV,
                    Font(Z_FONT_CAPTION, Text("%s", e->name))),
                .spacing = 8, .align = Z_ALIGN_CENTER)));
}

// The home screen: a grid of square icons (GRID_COLS per row). The live task
// switcher that used to live here has moved to the Recents overlay (zelto-recents,
// reached from the bottom nav bar) — the home screen is now just the app grid.
static ZView launcher_body(ZApp *app, LauncherState *state) {
    (void)state;
    (void)app;
    ensure_apps();

    ZStackOpts grid = {.padding = 20, .spacing = 18, .align = Z_ALIGN_LEADING};
    int k = 0;

    grid.children[k++] =
        Foreground(Z_COLOR_TEXT_INV, Font(Z_FONT_TITLE, Text("Apps")));

    // One HStack row per GRID_COLS apps; pad the final row with Spacers so its
    // cells stay one column wide instead of stretching to fill.
    for (int i = 0; i < g_n_apps && k < Z_MAX_CHILDREN - 1; i += GRID_COLS) {
        ZStackOpts row = {.spacing = 16, .align = Z_ALIGN_LEADING};
        for (int c = 0; c < GRID_COLS; c++) {
            int idx = i + c;
            row.children[c] =
                idx < g_n_apps ? grid_cell(&g_apps[idx]) : Spacer();
        }
        grid.children[k++] = z_stack(Z_AXIS_HORIZONTAL, &row);
    }

    grid.children[k++] = Spacer();

    return Background(z_rgba(0x0b, 0x0e, 0x13, 0xff),
                      z_stack(Z_AXIS_VERTICAL, &grid));
}

Z_APP_ID(LauncherState, launcher_body, "os.zelto.launcher")
