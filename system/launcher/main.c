// Zelto System UI — app launcher / home screen.
//
// A normal xdg_toplevel app (app_id "os.zelto.launcher") that the compositor
// keeps at the back of the app stack and sizes to the usable area below the
// status bar (and above the bottom nav bar). It is a phone-style home screen
// split the way a real phone does:
//
//   - a HOME surface: a drawn wallpaper, a small grid of *favourite* apps, and a
//     visible drawer handle; and
//   - an APP DRAWER: the *complete* installed-app list in a scroll, on an opaque
//     panel that slides up over the home surface.
//
// Both live in ONE launcher body as the two children of a depth ZStack: the home
// surface at the back, the drawer (offset off the bottom of the screen) in front.
// The drawer is opened/closed by an up/down swipe (OnPan, live finger-tracking
// with a velocity/threshold settle) or by tapping the home handle / the drawer's
// Close — its vertical position is an Offset bound to a persistent animated value
// (0 = hidden below the fold, 1 = covering the home surface), settled with a
// spring. The bottom nav bar's Back (zelto-nav) stays the *app-level* Back; the
// launcher owns its own drawer open/close (a cross-process Back→drawer signal is
// out of scope — see docs/platform/app-lifecycle.md).
//
// App metadata is read from /usr/share/zelto/apps/<id>.app manifests (a simple
// key=value text file) plus $ZELTO_DATA_DIR/apps/manifests at startup. The set of
// favourites is persisted as a prefs string (home.favorites=<id>,<id>,...) in the
// launcher's private storage on /var/zelto, so it survives a reboot; absent a
// stored set the launcher seeds a default (the first few apps) and writes it.
// Tapping any icon fork()+exec()s the app's `exec=` binary.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <zelto/ui.h>

#define MANIFEST_DIR "/usr/share/zelto/apps"
#define MAX_APPS 32
#define DEFAULT_FAVS 4   // home shows this many apps until the user curates them

// One installed app, parsed from a .app manifest. `id` is the manifest basename
// (minus ".app"), e.g. "os.zelto.cards" — the stable key used for favourites.
typedef struct AppEntry {
    char id[96];
    char name[64];
    char subtitle[96];
    char exec_path[160];
    ZColor color;
} AppEntry;

static AppEntry g_apps[MAX_APPS];
static int g_n_apps;
static bool g_scanned;

// Favourites: indices into g_apps shown on the home surface (the drawer shows
// all of g_apps). Resolved once from the persisted prefs string (or a default).
static int g_favs[MAX_APPS];
static int g_n_favs;
static bool g_home_scanned;

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
            // id = basename minus ".app" (the manifest is named after the app id).
            size_t idlen = n - 4;
            if (idlen >= sizeof(g_apps[g_n_apps].id)) {
                idlen = sizeof(g_apps[g_n_apps].id) - 1;
            }
            memcpy(g_apps[g_n_apps].id, name, idlen);
            g_apps[g_n_apps].id[idlen] = '\0';
            g_n_apps++;
        }
    }
    closedir(d);
}

// Scan the manifest directories once into g_apps. Two sources: the baked-in
// /usr/share/zelto/apps, plus $ZELTO_DATA_DIR/apps/manifests where zelto-install
// registers runtime-installed .zap packages (P13). A package installed on a prior
// boot therefore appears on the next startup scan.
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
    // readdir order is filesystem-dependent; sort by name so the layout is
    // deterministic — the headless harness reads fixed coordinates off a frame.
    qsort(g_apps, (size_t)g_n_apps, sizeof(g_apps[0]), app_name_cmp);
}

// Find an app index by id, or -1.
static int find_app(const char *id) {
    for (int i = 0; i < g_n_apps; i++) {
        if (strcmp(g_apps[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

// Seed g_favs with the first DEFAULT_FAVS apps and persist that set as the
// home.favorites prefs string, so a later "add to home" can mutate it and so the
// choice survives a reboot. Called only when no set is stored yet.
static void seed_default_favs(void) {
    g_n_favs = 0;
    char csv[512];
    size_t off = 0;
    int n = g_n_apps < DEFAULT_FAVS ? g_n_apps : DEFAULT_FAVS;
    csv[0] = '\0';
    for (int i = 0; i < n; i++) {
        g_favs[g_n_favs++] = i;
        int m = snprintf(csv + off, sizeof(csv) - off, "%s%s",
                         i ? "," : "", g_apps[i].id);
        if (m > 0 && (size_t)m < sizeof(csv) - off) {
            off += (size_t)m;
        }
    }
    z_prefs_set_str("home.favorites", csv);
    fprintf(stderr, "launcher: wrote default favorites: %s\n", csv);
}

// Resolve the favourites set once: read the persisted home.favorites string and
// map each id to a g_apps index; if none is stored (or none resolve), seed and
// persist the default set. Storage is direct filesystem on /var/zelto (P11), no
// zsysd round-trip, scoped to this app_id (os.zelto.launcher).
static void ensure_home(void) {
    if (g_home_scanned) {
        return;
    }
    g_home_scanned = true;
    ensure_apps();

    const char *pref = z_prefs_get_str("home.favorites", NULL);
    if (pref && pref[0]) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s", pref);
        g_n_favs = 0;
        for (char *tok = strtok(buf, ","); tok && g_n_favs < MAX_APPS;
             tok = strtok(NULL, ",")) {
            int idx = find_app(tok);
            if (idx >= 0) {
                g_favs[g_n_favs++] = idx;
            }
        }
        if (g_n_favs > 0) {
            fprintf(stderr, "launcher: favorites loaded from prefs: %s\n", pref);
            return;
        }
        // Stored set resolved to nothing (apps changed) — fall back in-memory.
    }
    seed_default_favs();
}

// Launcher persistent state. drawer_anim (0 hidden .. 1 covering) drives the
// drawer's vertical Offset; surface_h is the launcher's pixel height, both
// refreshed every build. drawer_anim is a retained hook: it MUST be allocated at
// the same call-order position every rebuild (the first z_animated_value in this
// body) or its identity shifts and the animation state corrupts.
typedef struct LauncherState {
    ZAnimated *drawer_anim;
    float surface_h;
} LauncherState;

// --- shared cell ----------------------------------------------------------
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

// One phone-style cell: a square coloured icon (the app's initial centred in it)
// with the app name as a caption below. No PNG assets yet — real assets/icon.png
// rendering is Planned; the coloured rounded square + glyph stands in. The whole
// cell is the tap target. Grow(1) so a row of GRID_COLS cells splits the width
// evenly (empty trailing slots are Spacers, which grow the same).
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

// A grid of square icons (GRID_COLS per row). `idx` selects which g_apps entries
// to show (NULL = all g_apps, in order); a partial last row is padded with
// Spacers so real cells stay one column wide.
static ZView app_grid(const int *idx, int n) {
    ZStackOpts grid = {.spacing = 18, .align = Z_ALIGN_LEADING};
    int k = 0;
    for (int i = 0; i < n && k < Z_MAX_CHILDREN; i += GRID_COLS) {
        ZStackOpts row = {.spacing = 16, .align = Z_ALIGN_LEADING};
        for (int c = 0; c < GRID_COLS; c++) {
            int j = i + c;
            row.children[c] =
                j < n ? grid_cell(&g_apps[idx ? idx[j] : j]) : Spacer();
        }
        grid.children[k++] = z_stack(Z_AXIS_HORIZONTAL, &row);
    }
    return z_stack(Z_AXIS_VERTICAL, &grid);
}

// --- wallpaper ------------------------------------------------------------
// The software renderer can't decode PNGs yet (real assets/wallpaper.png is
// Planned), so the wallpaper is *drawn*: a vertical gradient of stacked equal
// Rect bands from a deep indigo at the top to near-black at the bottom. Nicer
// than the old flat #0b0e13 fill, and gives the favourites something to sit over.
#define WALL_BANDS 10

static ZView wallpaper(void) {
    ZStackOpts col = {0};
    const int top[3] = {0x16, 0x1d, 0x38};
    const int bot[3] = {0x05, 0x07, 0x0e};
    for (int i = 0; i < WALL_BANDS; i++) {
        float t = (float)i / (float)(WALL_BANDS - 1);
        uint8_t r = (uint8_t)(top[0] + (bot[0] - top[0]) * t);
        uint8_t g = (uint8_t)(top[1] + (bot[1] - top[1]) * t);
        uint8_t b = (uint8_t)(top[2] + (bot[2] - top[2]) * t);
        col.children[i] = Rect(.color = z_rgba(r, g, b, 0xff), .grow = 1.0f);
    }
    return Fill(z_stack(Z_AXIS_VERTICAL, &col));
}

// --- drawer open/close ----------------------------------------------------
// Spring the drawer to a target. These run under z_with_animation so the open/
// close taps settle with the standard spring (gesture releases settle the same
// via z_animated_spring, which defaults to the standard profile).
static void anim_open(ZApp *app, void *state) {
    (void)app;
    LauncherState *s = state;
    if (s->drawer_anim) {
        z_animated_spring(s->drawer_anim, 1.0f);
    }
}
static void anim_close(ZApp *app, void *state) {
    (void)app;
    LauncherState *s = state;
    if (s->drawer_anim) {
        z_animated_spring(s->drawer_anim, 0.0f);
    }
}
static void open_drawer(ZApp *app, void *state) {
    (void)state;
    z_with_animation(app, Z_SPRING_STANDARD, anim_open);
}
static void close_drawer(ZApp *app, void *state) {
    (void)state;
    z_with_animation(app, Z_SPRING_STANDARD, anim_close);
}

// Up-swipe on the home surface opens the drawer; the drag tracks the finger live
// (z_animated_set jumps, no spring) and the release settles by position/velocity.
// translation_y is negative dragging up; velocity_y is negative for an up-fling.
static void on_home_pan(ZApp *app, void *state, const ZPanEvent *e) {
    (void)app;
    LauncherState *s = state;
    if (!s->drawer_anim) {
        return;
    }
    float h = s->surface_h > 1.0f ? s->surface_h : 1.0f;
    if (e->phase == Z_PAN_CHANGED) {
        float a = -e->translation_y / h;
        if (a < 0.0f) {
            a = 0.0f;
        } else if (a > 1.0f) {
            a = 1.0f;
        }
        z_animated_set(s->drawer_anim, a);   // live finger-tracking
    } else if (e->phase == Z_PAN_END) {
        float a = z_animated_get(s->drawer_anim);
        bool open = a > 0.35f || e->velocity_y < -500.0f;
        z_animated_spring(s->drawer_anim, open ? 1.0f : 0.0f);
    }
}

// Down-swipe on the drawer's top grabber closes it (mirror of on_home_pan).
static void on_drawer_pan(ZApp *app, void *state, const ZPanEvent *e) {
    (void)app;
    LauncherState *s = state;
    if (!s->drawer_anim) {
        return;
    }
    float h = s->surface_h > 1.0f ? s->surface_h : 1.0f;
    if (e->phase == Z_PAN_CHANGED) {
        float a = 1.0f - e->translation_y / h;
        if (a < 0.0f) {
            a = 0.0f;
        } else if (a > 1.0f) {
            a = 1.0f;
        }
        z_animated_set(s->drawer_anim, a);
    } else if (e->phase == Z_PAN_END) {
        float a = z_animated_get(s->drawer_anim);
        bool close = a < 0.65f || e->velocity_y > 500.0f;
        z_animated_spring(s->drawer_anim, close ? 0.0f : 1.0f);
    }
}

// --- body -----------------------------------------------------------------
static ZView launcher_body(ZApp *app, LauncherState *state) {
    // Retained hook, allocated FIRST at a fixed position every rebuild so its
    // identity is stable (the drawer is always built — never conditionally — so
    // its Scroll cell, allocated below, is likewise stable).
    state->drawer_anim = z_animated_value(app, 0.0f);
    state->surface_h = (float)z_app_height(app);
    ensure_home();

    // (1) HOME surface: wallpaper + favourites grid + a drawer handle, the whole
    // thing an OnPan target so an up-swipe anywhere opens the drawer.
    ZView home = OnPan(on_home_pan, Fill(
        ZStack(
            wallpaper(),
            Fill(VStack(
                app_grid(g_favs, g_n_favs),
                Spacer(),
                OnTap(open_drawer,
                    VStack(
                        Rect(.color = z_rgba(0x8a, 0x93, 0x9e, 0xff),
                             .width = 56, .height = 5, .radius = 3),
                        Foreground(Z_COLOR_TEXT_INV,
                            Font(Z_FONT_CALLOUT, Text("^"))),
                        Foreground(z_rgba(0xb6, 0xbe, 0xc8, 0xff),
                            Font(Z_FONT_CAPTION, Text("All apps"))),
                        .spacing = 4, .align = Z_ALIGN_CENTER)),
                .spacing = 16, .padding = 24, .align = Z_ALIGN_CENTER)),
            .align = Z_ALIGN_CENTER)));

    // (2) APP DRAWER: an opaque dark panel (the renderer can't do real
    // translucency — same constraint as the shade) holding the full app list in
    // a Scroll, slid up from below by an Offset bound to drawer_anim. Reading the
    // animated value as the static y each rebuild animates it (the spring tick
    // invalidates → rebuild → new y). 0 = parked one full height below the fold.
    float drawer_v = z_animated_get(state->drawer_anim);
    // While the drawer is off the fully-closed rest position it is sliding/open
    // over the home surface; force a full repaint so the moving panel doesn't
    // leave stale pixels from the under-damaging partial-repaint path.
    if (drawer_v > 0.001f) {
        z_full_repaint(app);
    }
    float slide = (1.0f - drawer_v) * state->surface_h;
    ZView grabber = OnPan(on_drawer_pan,
        VStack(
            Rect(.color = z_rgba(0x8a, 0x93, 0x9e, 0xff),
                 .width = 56, .height = 5, .radius = 3),
            HStack(
                Foreground(Z_COLOR_TEXT_INV,
                    Font(Z_FONT_TITLE, Text("All apps"))),
                Spacer(),
                OnTap(close_drawer,
                    Foreground(Z_COLOR_TEXT_INV,
                        Font(Z_FONT_TITLE, Text("X")))),
                .align = Z_ALIGN_CENTER),
            .spacing = 10, .align = Z_ALIGN_CENTER));

    ZView drawer = Offset(NULL, slide, Fill(
        Background(z_rgba(0x06, 0x08, 0x0d, 0xff),
            VStack(
                grabber,
                Grow(1.0f, Scroll(app, app_grid(NULL, g_n_apps),
                                  .axis = Z_AXIS_VERTICAL)),
                .spacing = 12, .padding = 20, .align = Z_ALIGN_LEADING))));

    return ZStack(home, drawer, .align = Z_ALIGN_CENTER);
}

Z_APP_ID(LauncherState, launcher_body, "os.zelto.launcher")
