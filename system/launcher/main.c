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
// These live in ONE launcher body as the children of a depth ZStack: the home
// surface at the back, then the app drawer (offset off the BOTTOM, opened by an
// up-swipe), then — only while open — a long-press curate menu and a toast. The
// drawer's position is an Offset bound to a persistent animated value (0 = parked
// off-screen, 1 = fully out), settled with a spring. The bottom nav bar's Back
// (zelto-nav) stays the *app-level* Back; the launcher owns its own drawer (a
// cross-process Back→drawer signal is out of scope — see app-lifecycle.md).
//
// HOME EDITING: long-pressing any icon (press-and-hold past the SDK threshold)
// opens a small context menu to add/remove that app from the home favourites,
// capped at one screen (MAX_FAVS); the change rewrites the home.favorites CSV and
// the grid + drawer rebuild live.
//
// QUICK SETTINGS moved OUT of the launcher in P17. The P16 quick-settings shade
// lived in this body (reachable only on the home screen); it is now a real
// system-wide pull-down owned by the OVERLAY layer-shell client zelto-shade, so
// it works over any running app. The launcher therefore no longer handles the
// top-edge down-swipe (the shade catches it above the launcher) and no longer
// stores the toggle bools. See system/shade/main.c + the P17 memory note.
//
// App metadata is read from /usr/share/zelto/apps/<id>.app manifests (a simple
// key=value text file) plus $ZELTO_DATA_DIR/apps/manifests at startup. The set of
// favourites is persisted as a prefs string (home.favorites=<id>,<id>,...) in the
// launcher's private storage on /var/zelto, so it survives a reboot; absent a
// stored set the launcher seeds a default (the first few apps) and writes it.
// Tapping any icon fork()+exec()s the app's `exec=` binary.
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <zelto/ui.h>

#include "common/app_icons.h"
#include "common/wallpaper.h"

#define MANIFEST_DIR "/usr/share/zelto/apps"
#define MAX_APPS 32
#define DEFAULT_FAVS 4   // home shows this many apps until the user curates them
#define MAX_FAVS 8       // home holds one screen of favourites; adding past this toasts

// One installed app, parsed from a .app manifest. `id` is the manifest basename
// (minus ".app"), e.g. "os.zelto.cards" — the stable key used for favourites.
typedef struct AppEntry {
    char id[96];
    char name[64];
    char subtitle[96];
    char exec_path[160];
    char icon_path[192];   // manifest icon= (PNG/SVG); empty -> placeholder
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
        return Z_COLOR_SURFACE_3;
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
    e->color = Z_COLOR_SURFACE_3;
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
        } else if (strcmp(key, "icon") == 0) {
            snprintf(e->icon_path, sizeof(e->icon_path), "%s", val);
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

// Serialise the current g_favs[] (as app ids) into the home.favorites prefs
// string, so the home grid survives a reboot. Called after every curate (add/
// remove) and once when seeding the default set. Direct fs on /var/zelto (P11).
static void write_favs_csv(void) {
    char csv[512];
    size_t off = 0;
    csv[0] = '\0';
    for (int i = 0; i < g_n_favs; i++) {
        int m = snprintf(csv + off, sizeof(csv) - off, "%s%s",
                         i ? "," : "", g_apps[g_favs[i]].id);
        if (m > 0 && (size_t)m < sizeof(csv) - off) {
            off += (size_t)m;
        }
    }
    z_prefs_set_str("home.favorites", csv);
    fprintf(stderr, "launcher: wrote favorites: %s\n", csv);
}

// Seed g_favs with the first DEFAULT_FAVS apps and persist that set. Called only
// when no set is stored yet.
static void seed_default_favs(void) {
    g_n_favs = 0;
    int n = g_n_apps < DEFAULT_FAVS ? g_n_apps : DEFAULT_FAVS;
    for (int i = 0; i < n; i++) {
        g_favs[g_n_favs++] = i;
    }
    write_favs_csv();
}

// Position of g_apps index `idx` in g_favs, or -1 if it is not a favourite.
static int fav_pos(int idx) {
    for (int i = 0; i < g_n_favs; i++) {
        if (g_favs[i] == idx) {
            return i;
        }
    }
    return -1;
}

// Append a favourite (no-op if already present); caller enforces the cap.
static void add_fav(int idx) {
    if (fav_pos(idx) >= 0 || g_n_favs >= MAX_FAVS) {
        return;
    }
    g_favs[g_n_favs++] = idx;
    write_favs_csv();
}

// Remove a favourite, compacting the list (no-op if absent).
static void remove_fav(int idx) {
    int p = fav_pos(idx);
    if (p < 0) {
        return;
    }
    for (int i = p; i < g_n_favs - 1; i++) {
        g_favs[i] = g_favs[i + 1];
    }
    g_n_favs--;
    write_favs_csv();
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

// --- home-screen widgets (P26) --------------------------------------------
// A widget is an SDK primitive (z_widget): a titled card whose live content a
// callback builds, self-refreshing on a declared cadence. The launcher ships
// three built-ins and hosts them on the home surface above the favourites grid.
// Each reads its live source directly — the clock ticks on its own z_tick_every
// cadence; the status + notifications glances re-read the brokered sys.* keys on
// every rebuild and stay current because the launcher's settings observer (which
// already watches sys.wallpaper) repaints on any change. The ACTIVE set + order
// is curated like favourites and persisted as a home.widgets CSV of widget ids.

// The clock/date card: big time over a muted date. Ticks once a second.
static ZView w_clock(ZApp *app, void *state) {
    (void)app;
    (void)state;
    char hhmm[8] = "--:--";
    char date[32] = "";
    time_t t = time(NULL);
    struct tm tmv;
    if (gmtime_r(&t, &tmv)) {
        snprintf(hhmm, sizeof(hhmm), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        strftime(date, sizeof(date), "%a %d %b", &tmv);
    }
    return VStack(
        Foreground(Z_COLOR_TEXT, Font(Z_FONT_LARGE_TITLE, Text("%s", hhmm))),
        Foreground(Z_COLOR_TEXT_MUTED, Font(Z_FONT_CAPTION, Text("%s", date))),
        .spacing = 2, .align = Z_ALIGN_LEADING);
}

// The status glance: battery % (green charging / red low) + the connectivity
// mode, both from the brokered sys.* keys the P23 power source + P18 settings own.
static ZView w_battery(ZApp *app, void *state) {
    (void)app;
    (void)state;
    int pct = (int)z_setting_get_int("sys.battery_pct", 100);
    bool charging = z_setting_get_int("sys.battery_charging", 0) != 0;
    bool wifi = z_setting_get_int("sys.wifi", 1) != 0;
    bool airplane = z_setting_get_int("sys.airplane", 0) != 0;
    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }
    ZColor pc = charging ? Z_COLOR_SUCCESS
              : (pct <= 20 ? Z_COLOR_DANGER : Z_COLOR_TEXT);
    const char *net = airplane ? "Airplane" : (wifi ? "Wi-Fi" : "Offline");
    return VStack(
        Foreground(pc, Font(Z_FONT_TITLE, Text("%d%%", pct))),
        Foreground(Z_COLOR_TEXT_MUTED,
            Font(Z_FONT_CAPTION,
                 Text("%s%s", charging ? "Charging \xc2\xb7 " : "", net))),
        .spacing = 2, .align = Z_ALIGN_LEADING);
}

// The notifications glance: the live count of stored notifications, read from the
// brokered sys.notif_count key zsysd publishes off its notification store.
static ZView w_notifs(ZApp *app, void *state) {
    (void)app;
    (void)state;
    int n = (int)z_setting_get_int("sys.notif_count", 0);
    return VStack(
        Foreground(n > 0 ? Z_COLOR_ACCENT : Z_COLOR_TEXT_MUTED,
                   Font(Z_FONT_LARGE_TITLE, Text("%d", n))),
        Foreground(Z_COLOR_TEXT_MUTED,
            Font(Z_FONT_CAPTION,
                 Text("%s", n == 1 ? "notification" : "notifications"))),
        .spacing = 2, .align = Z_ALIGN_LEADING);
}

typedef struct WidgetDef {
    const char *id;        // stable key persisted in the home.widgets CSV
    const char *title;     // card caption
    ZWidgetFn build;       // content builder (z_widget body)
    int refresh_ms;        // self-refresh cadence (0 = source-driven only)
} WidgetDef;

static const WidgetDef g_widget_defs[] = {
    {"clock",   "Clock",         w_clock,   1000},
    {"battery", "Status",        w_battery, 0},
    {"notifs",  "Notifications", w_notifs,  0},
};
#define N_WIDGET_DEFS ((int)(sizeof(g_widget_defs) / sizeof(g_widget_defs[0])))

// The active widgets: indices into g_widget_defs, in display order, resolved from
// the persisted home.widgets CSV (or seeded to all built-ins on first run).
static int g_widgets[N_WIDGET_DEFS];
static int g_n_widgets;
static bool g_widgets_scanned;

static int find_widget_def(const char *id) {
    for (int i = 0; i < N_WIDGET_DEFS; i++) {
        if (strcmp(g_widget_defs[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

// Position of def index `di` in g_widgets, or -1 if inactive.
static int widget_pos(int di) {
    for (int i = 0; i < g_n_widgets; i++) {
        if (g_widgets[i] == di) {
            return i;
        }
    }
    return -1;
}

// Serialise g_widgets (as widget ids) into the home.widgets prefs string so the
// set + order survive a reboot. Mirrors write_favs_csv.
static void write_widgets_csv(void) {
    char csv[256];
    size_t off = 0;
    csv[0] = '\0';
    for (int i = 0; i < g_n_widgets; i++) {
        int m = snprintf(csv + off, sizeof(csv) - off, "%s%s",
                         i ? "," : "", g_widget_defs[g_widgets[i]].id);
        if (m > 0 && (size_t)m < sizeof(csv) - off) {
            off += (size_t)m;
        }
    }
    z_prefs_set_str("home.widgets", csv);
    fprintf(stderr, "launcher: wrote widgets: %s\n", csv);
}

// Resolve the active widget set once from home.widgets, else seed all built-ins.
static void ensure_widgets(void) {
    if (g_widgets_scanned) {
        return;
    }
    g_widgets_scanned = true;
    const char *pref = z_prefs_get_str("home.widgets", NULL);
    if (pref && pref[0]) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", pref);
        g_n_widgets = 0;
        for (char *tok = strtok(buf, ","); tok && g_n_widgets < N_WIDGET_DEFS;
             tok = strtok(NULL, ",")) {
            int di = find_widget_def(tok);
            if (di >= 0 && widget_pos(di) < 0) {
                g_widgets[g_n_widgets++] = di;
            }
        }
        fprintf(stderr, "launcher: widgets loaded from prefs: %s\n", pref);
        return;   // an empty stored set is legitimate (user removed them all)
    }
    g_n_widgets = 0;
    for (int i = 0; i < N_WIDGET_DEFS; i++) {
        g_widgets[g_n_widgets++] = i;
    }
    write_widgets_csv();
}

// Curate ops (persist on each change). Add appends a built-in; remove compacts;
// move-up swaps a card with its predecessor (the reorder affordance).
static void widget_add(int di) {
    if (di < 0 || di >= N_WIDGET_DEFS || widget_pos(di) >= 0 ||
        g_n_widgets >= N_WIDGET_DEFS) {
        return;
    }
    g_widgets[g_n_widgets++] = di;
    write_widgets_csv();
}
static void widget_remove_at(int pos) {
    if (pos < 0 || pos >= g_n_widgets) {
        return;
    }
    for (int i = pos; i < g_n_widgets - 1; i++) {
        g_widgets[i] = g_widgets[i + 1];
    }
    g_n_widgets--;
    write_widgets_csv();
}
static void widget_move_up(int pos) {
    if (pos <= 0 || pos >= g_n_widgets) {
        return;
    }
    int tmp = g_widgets[pos - 1];
    g_widgets[pos - 1] = g_widgets[pos];
    g_widgets[pos] = tmp;
    write_widgets_csv();
}

// Launcher persistent state. drawer_anim (0 hidden .. 1 covering) drives the
// drawer's vertical Offset; surface_h is the launcher's pixel height, both
// refreshed every build. drawer_anim is a retained hook: it MUST be allocated at
// the same call-order position every rebuild (the first z_animated_value in this
// body) or its identity shifts and the animation state corrupts.
typedef struct LauncherState {
    ZAnimated *drawer_anim;   // 0 hidden below the fold .. 1 covering home
    float surface_h;

    // Wallpaper (P25). The active wallpaper's absolute path, resolved from the
    // brokered sys.wallpaper key; wp_ok records whether it decodes (else the home
    // surface falls back to the drawn gradient). wp_subscribed gates the one-time
    // observe + default-seed. Cached in state (not re-read every build) and
    // refreshed only when the broker fans out a sys.wallpaper change.
    bool wp_subscribed;
    bool wp_ok;
    char wp_path[ZELTO_WALLPAPER_PATH_MAX];

    // Long-press context menu. menu_idx is a g_apps index; menu_open gates the
    // overlay. A short-lived toast (cap-reached feedback) shows until toast_until.
    bool menu_open;
    int menu_idx;
    double toast_until;       // monotonic seconds; 0 = no toast
    char toast[64];

    // Widget curate (P26). A long-press on a widget card opens a per-card menu
    // (remove / move up) keyed by its position in g_widgets; the "+ Add widget"
    // pill opens a sheet listing the inactive built-ins. Both are modal overlays
    // built only while open (no retained hooks), like the app curate menu.
    bool wmenu_open;
    int wmenu_pos;            // position in g_widgets the card menu acts on
    bool wadd_open;
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

// Monotonic seconds (the SDK's z_now_seconds is internal; the launcher only
// links the public API, so use the clock directly for the toast timer).
static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// Pop a transient toast (cap-reached feedback). It is rendered by body() while
// now < toast_until; body re-invalidates each frame until then so it self-clears.
static void show_toast(LauncherState *s, const char *msg) {
    snprintf(s->toast, sizeof(s->toast), "%s", msg);
    s->toast_until = now_s() + 2.0;
}

// Long-press an icon (home or drawer) -> open the curate context menu for it.
// `data` is the AppEntry* the cell was built with; recover its g_apps index.
static void on_icon_longpress(ZApp *app, void *state, void *data, float x,
                              float y) {
    (void)x;
    (void)y;
    LauncherState *s = state;
    const AppEntry *e = data;
    s->menu_idx = (int)(e - g_apps);
    s->menu_open = true;
    z_invalidate(app);
}

#define GRID_COLS 4
#define ICON_SIZE 104.0f
#define ICON_RADIUS 24.0f
#define ICON_INSET 20.0f   // padding of an emblem within its coloured tile

// One phone-style cell: the app's icon over a rounded tile, with the app name as
// a caption below. The icon is the manifest `icon=` PNG/SVG; when the app has no
// icon (or it fails to load) the shared Placeholder image stands in — no more
// letter-square. An app's own emblem (a transparent line/PNG icon) is inset over
// the app's colour tile; the placeholder is a self-contained image that fills
// the rounded tile. The whole cell is the tap target. Grow(1) so a row of
// GRID_COLS cells splits the width evenly (empty trailing slots are Spacers).
static ZView app_icon_tile(const AppEntry *e) {
    bool own = e->icon_path[0] && z_image_loads(e->icon_path);
    const char *icon = own ? e->icon_path : zelto_placeholder_icon();

    // Own emblem: inset over the app colour tile. Placeholder: fills the tile,
    // rounded to match (it carries its own art, so it needs no colour behind).
    ZView art = own
        ? Frame(ICON_SIZE - 2.0f * ICON_INSET, ICON_SIZE - 2.0f * ICON_INSET,
                Image(icon))
        : Frame(ICON_SIZE, ICON_SIZE, CornerRadius(ICON_RADIUS, Image(icon)));

    return Frame(ICON_SIZE, ICON_SIZE,
        Background(e->color,
            CornerRadius(ICON_RADIUS,
                ZStack(art, .align = Z_ALIGN_CENTER))));
}

static ZView grid_cell(const AppEntry *e) {
    return Grow(1.0f,
        OnLongPress(on_icon_longpress, (void *)e,
        OnTapData(launch_app, (void *)e,
            VStack(
                app_icon_tile(e),
                Foreground(Z_COLOR_TEXT_INV,
                    Font(Z_FONT_CAPTION, Text("%s", e->name))),
                .spacing = 8, .align = Z_ALIGN_CENTER))));
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
// The home surface's full-screen backdrop: a real photo (the active sys.wallpaper
// PNG), cover-fit so it fills the screen with no letterbox bars, with a subtle
// dark legibility scrim over it so the white favourites captions stay readable. If
// the wallpaper file is missing or won't decode, we fall back to a *drawn*
// vertical gradient (the pre-P25 look) — a self-contained background that never
// depends on an asset being present.
#define WALL_BANDS 10
#define SCRIM_BANDS 8

// A top+bottom darkening gradient (clear through the middle) painted over the
// photo. The renderer alpha-blends source-over, and both the photo and this scrim
// are static, so a partial repaint re-paints them together for each damage rect —
// no cumulative darkening, no forced full repaint. eased (squared) so the falloff
// is smooth rather than a hard band.
static ZView wallpaper_scrim(void) {
    ZStackOpts col = {0};
    for (int i = 0; i < SCRIM_BANDS; i++) {
        float t = (float)i / (float)(SCRIM_BANDS - 1);   // 0 top .. 1 bottom
        float edge = t < 0.5f ? (1.0f - t * 2.0f) : ((t - 0.5f) * 2.0f);
        uint8_t a = (uint8_t)(edge * edge * 130.0f);
        col.children[i] = Rect(.color = z_rgba(0, 0, 0, a), .grow = 1.0f);
    }
    return Fill(z_stack(Z_AXIS_VERTICAL, &col));
}

// The drawn-gradient fallback (deep indigo -> near-black), used when no wallpaper
// photo is available. Nicer than a flat fill and gives the favourites contrast.
static ZView wallpaper_gradient(void) {
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

static ZView wallpaper(LauncherState *s) {
    if (s->wp_ok) {
        return Fill(ZStack(
            Fill(Cover(Image(s->wp_path))),
            wallpaper_scrim(),
            .align = Z_ALIGN_CENTER));
    }
    return wallpaper_gradient();
}

// A brokered setting changed. Re-resolve the wallpaper when it is sys.wallpaper
// (idempotent — re-reading the value we just set is a harmless no-op), and repaint
// on ANY change so the home widgets that read brokered keys directly (the status +
// notifications glances read sys.battery_*/sys.notif_count each rebuild) update
// live — a battery drain, a Settings toggle, or a new notification re-renders the
// glance with no reboot. The rebuild is cheap and the reads are pure, so observing
// every key rather than a curated subset is simplest and never loops.
static void on_wp_setting(ZApp *app, const char *key, const char *value,
                          void *ud) {
    (void)value;
    LauncherState *s = ud;
    if (strcmp(key, ZELTO_WALLPAPER_KEY) == 0) {
        s->wp_ok = zelto_wallpaper_active(s->wp_path, sizeof(s->wp_path));
    }
    z_invalidate(app);
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

// --- curate context menu --------------------------------------------------
static void close_menu(ZApp *app, void *state) {
    LauncherState *s = state;
    s->menu_open = false;
    z_invalidate(app);
}
// "Add to home": append the menu's app to the favourites (toast if at the cap),
// rewrite the persisted CSV, and close. The home grid + drawer rebuild live.
static void do_add_fav(ZApp *app, void *state) {
    LauncherState *s = state;
    if (g_n_favs >= MAX_FAVS) {
        show_toast(s, "Home is full (8)");
    } else {
        add_fav(s->menu_idx);
    }
    s->menu_open = false;
    z_invalidate(app);
}
// "Remove from home": drop the menu's app from the favourites + rewrite the CSV.
static void do_remove_fav(ZApp *app, void *state) {
    LauncherState *s = state;
    remove_fav(s->menu_idx);
    s->menu_open = false;
    z_invalidate(app);
}

// --- widget curate --------------------------------------------------------
// Long-press a widget card -> open its per-card menu (remove / move up). `data`
// is the card's position in g_widgets, encoded in the pointer (like OnLongPress
// on the app cells, but a small integer identity rather than an AppEntry*).
static void on_widget_longpress(ZApp *app, void *state, void *data, float x,
                                float y) {
    (void)x;
    (void)y;
    LauncherState *s = state;
    s->wmenu_pos = (int)(intptr_t)data;
    s->wmenu_open = true;
    z_invalidate(app);
}
static void close_wmenu(ZApp *app, void *state) {
    LauncherState *s = state;
    s->wmenu_open = false;
    z_invalidate(app);
}
static void do_widget_remove(ZApp *app, void *state) {
    LauncherState *s = state;
    widget_remove_at(s->wmenu_pos);
    s->wmenu_open = false;
    z_invalidate(app);
}
static void do_widget_move_up(ZApp *app, void *state) {
    LauncherState *s = state;
    widget_move_up(s->wmenu_pos);
    s->wmenu_open = false;
    z_invalidate(app);
}
// The "+ Add widget" pill opens the add sheet; a row in it adds one built-in.
static void open_widget_add(ZApp *app, void *state) {
    LauncherState *s = state;
    s->wadd_open = true;
    z_invalidate(app);
}
static void close_widget_add(ZApp *app, void *state) {
    LauncherState *s = state;
    s->wadd_open = false;
    z_invalidate(app);
}
// `data` is the WidgetDef* of the inactive built-in to add (recover its index).
static void do_widget_add(ZApp *app, void *state, void *data) {
    LauncherState *s = state;
    const WidgetDef *d = data;
    widget_add((int)(d - g_widget_defs));
    s->wadd_open = false;
    z_invalidate(app);
}

static float clamp01(float a) {
    return a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
}

// The home surface's up-swipe pulls the app drawer up (drawer_anim 0->1); the
// value tracks the finger live (z_animated_set) and the release settles it by
// position/velocity. translation_y/velocity_y are negative dragging up. A
// down-drag is ignored here — the top-edge pull-down is owned by the system
// shade (zelto-shade), which sits above the launcher and catches it first.
static void on_home_pan(ZApp *app, void *state, const ZPanEvent *e) {
    (void)app;
    LauncherState *s = state;
    if (!s->drawer_anim) {
        return;
    }
    float h = s->surface_h > 1.0f ? s->surface_h : 1.0f;
    if (e->phase == Z_PAN_CHANGED) {
        z_animated_set(s->drawer_anim, clamp01(-e->translation_y / h));
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

// --- widget host ----------------------------------------------------------
// One widget card: the SDK z_widget wrapped in a long-press target (its position
// carries the curate identity) and Grow(1) so a pair splits the row evenly.
static ZView widget_card(ZApp *app, int pos) {
    const WidgetDef *d = &g_widget_defs[g_widgets[pos]];
    ZView w = Widget(app, .title = d->title, .body = d->build,
                     .refresh_ms = d->refresh_ms);
    return Grow(1.0f,
        OnLongPress(on_widget_longpress, (void *)(intptr_t)pos, w));
}

// The widget area above the favourites grid: the active cards in a two-column
// grid (a partial last row padded with a Spacer so a card stays one column wide),
// then — while any built-in is inactive — a dashed "+ Add widget" pill. Returns
// NULL when there is nothing to show (no active widgets AND all built-ins active,
// which can't both hold, but guard anyway); the caller drops a NULL child.
#define WIDGET_COLS 2
static ZView widget_host(ZApp *app) {
    ZStackOpts grid = {.spacing = 12, .align = Z_ALIGN_LEADING};
    int k = 0;
    for (int i = 0; i < g_n_widgets && k < Z_MAX_CHILDREN; i += WIDGET_COLS) {
        ZStackOpts row = {.spacing = 12, .align = Z_ALIGN_LEADING};
        for (int c = 0; c < WIDGET_COLS; c++) {
            int j = i + c;
            row.children[c] = j < g_n_widgets ? widget_card(app, j) : Spacer();
        }
        grid.children[k++] = z_stack(Z_AXIS_HORIZONTAL, &row);
    }
    // Add-widget pill (only when something can be added).
    if (g_n_widgets < N_WIDGET_DEFS && k < Z_MAX_CHILDREN) {
        grid.children[k++] = OnTap(open_widget_add,
            Background(Z_COLOR_SURFACE_2,
                CornerRadius(14.0f,
                    Padding(12.0f,
                        Foreground(Z_COLOR_TEXT_MUTED,
                            Font(Z_FONT_CALLOUT, Text("+ Add widget")))))));
    }
    if (k == 0) {
        return NULL;
    }
    return z_stack(Z_AXIS_VERTICAL, &grid);
}

// --- body -----------------------------------------------------------------
static ZView launcher_body(ZApp *app, LauncherState *state) {
    // Retained hooks, allocated FIRST + unconditionally every rebuild so their
    // identity is stable: drawer_anim is the body's single z_animated_value (anim
    // cell 0). The drawer is always built (never conditionally), so its Scroll
    // cell stays stable too; it is merely translated off-screen when at rest.
    state->drawer_anim = z_animated_value(app, 0.0f);
    state->surface_h = (float)z_app_height(app);
    ensure_home();
    ensure_widgets();

    // Wallpaper: on the first build observe the brokered sys.wallpaper key (so a
    // pick in Settings updates the home surface live) and, as its single owner,
    // seed a default if none is stored yet. Then resolve the active path once;
    // on_wp_setting refreshes it on any later change.
    if (!state->wp_subscribed) {
        state->wp_subscribed = true;
        z_settings_observe(app, on_wp_setting, state);
        const char *cur = z_setting_get_str(ZELTO_WALLPAPER_KEY, "");
        if (!cur || !cur[0]) {
            char def[ZELTO_WALLPAPER_PATH_MAX];
            if (zelto_wallpaper_default(def, sizeof(def))) {
                z_setting_set_str(ZELTO_WALLPAPER_KEY, def);
            }
        }
        state->wp_ok = zelto_wallpaper_active(state->wp_path,
                                              sizeof(state->wp_path));
    }

    // (1) HOME surface: wallpaper + a widget area + the favourites grid + a
    // drawer handle, the whole thing an OnPan target so an up-swipe anywhere opens
    // the drawer. The content column is assembled explicitly because the widget
    // host is an optional (possibly NULL) leading child — a NULL positional child
    // would truncate a stack literal's list.
    ZView drawer_handle = OnTap(open_drawer,
        VStack(
            Rect(.color = Z_COLOR_TEXT_MUTED,
                 .width = 56, .height = 5, .radius = 3),
            Foreground(Z_COLOR_TEXT_INV,
                Font(Z_FONT_CALLOUT, Text("^"))),
            Foreground(Z_COLOR_TEXT_MUTED,
                Font(Z_FONT_CAPTION, Text("All apps"))),
            .spacing = 4, .align = Z_ALIGN_CENTER));

    ZStackOpts hcol = {.spacing = 16, .padding = 24, .align = Z_ALIGN_CENTER};
    int hk = 0;
    ZView wh = widget_host(app);
    if (wh) {
        hcol.children[hk++] = wh;
    }
    hcol.children[hk++] = app_grid(g_favs, g_n_favs);
    hcol.children[hk++] = Spacer();
    hcol.children[hk++] = drawer_handle;

    ZView home = OnPan(on_home_pan, Fill(
        ZStack(
            wallpaper(state),
            Fill(z_stack(Z_AXIS_VERTICAL, &hcol)),
            .align = Z_ALIGN_CENTER)));

    // (2) APP DRAWER: an opaque dark panel (the renderer can't do real
    // translucency — same constraint as the shade) holding the full app list in
    // a Scroll, slid up from below by an Offset bound to drawer_anim. Reading the
    // animated value as the static y each rebuild animates it (the spring tick
    // invalidates → rebuild → new y). 0 = parked one full height below the fold.
    float drawer_v = z_animated_get(state->drawer_anim);
    float slide = (1.0f - drawer_v) * state->surface_h;
    ZView grabber = OnPan(on_drawer_pan,
        VStack(
            Rect(.color = Z_COLOR_TEXT_MUTED,
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
        Background(Z_COLOR_BG,
            VStack(
                grabber,
                Grow(1.0f, Scroll(app, app_grid(NULL, g_n_apps),
                                  .axis = Z_AXIS_VERTICAL)),
                .spacing = 12, .padding = 20, .align = Z_ALIGN_LEADING))));

    // (3) CURATE CONTEXT MENU (long-press): a dimmed modal over everything with
    // an Add/Remove-from-home action for the long-pressed app. Built only while
    // open; carries no retained hooks, so the cell order above stays fixed.
    ZView menu = NULL;
    if (state->menu_open && state->menu_idx >= 0 &&
        state->menu_idx < g_n_apps) {
        const AppEntry *e = &g_apps[state->menu_idx];
        bool fav = fav_pos(state->menu_idx) >= 0;
        menu = Fill(ZStack(
            OnTap(close_menu, Fill(Background(Z_COLOR_SCRIM,
                                              Fill(Spacer())))),
            Background(Z_COLOR_SURFACE,
                CornerRadius(20.0f,
                    Padding(24.0f,
                        VStack(
                            Foreground(Z_COLOR_TEXT_INV,
                                Font(Z_FONT_TITLE, Text("%s", e->name))),
                            Button(fav ? do_remove_fav : do_add_fav, "%s",
                                   fav ? "Remove from home" : "Add to home"),
                            Button(close_menu, "Cancel"),
                            .spacing = 14, .align = Z_ALIGN_CENTER)))),
            .align = Z_ALIGN_CENTER));
    }

    // (4) TOAST: transient cap-reached feedback near the bottom. While it is up we
    // re-invalidate so the loop keeps ticking and the toast self-clears at its
    // deadline (a rare, brief busy-repaint — only on a no-op "home is full").
    ZView toast = NULL;
    if (state->toast_until > 0.0 && now_s() < state->toast_until) {
        z_invalidate(app);
        toast = Fill(VStack(
            Spacer(),
            Background(Z_COLOR_SURFACE_2,
                CornerRadius(12.0f,
                    Padding(14.0f,
                        Foreground(Z_COLOR_TEXT_INV,
                            Font(Z_FONT_BODY, Text("%s", state->toast)))))),
            .spacing = 0, .padding = 48, .align = Z_ALIGN_CENTER));
    }

    // (5) WIDGET CURATE MENU (long-press a card): a dimmed modal with Remove and
    // — for any card but the first — Move up (the reorder affordance). Built with
    // an explicit child list so the conditional Move up doesn't truncate the stack.
    ZView wmenu = NULL;
    if (state->wmenu_open && state->wmenu_pos >= 0 &&
        state->wmenu_pos < g_n_widgets) {
        const WidgetDef *d = &g_widget_defs[g_widgets[state->wmenu_pos]];
        ZStackOpts col = {.spacing = 14, .align = Z_ALIGN_CENTER};
        int c = 0;
        col.children[c++] = Foreground(Z_COLOR_TEXT_INV,
            Font(Z_FONT_TITLE, Text("%s", d->title)));
        if (state->wmenu_pos > 0) {
            col.children[c++] = Button(do_widget_move_up, "Move up");
        }
        col.children[c++] = Button(do_widget_remove, "Remove");
        col.children[c++] = Button(close_wmenu, "Cancel");
        wmenu = Fill(ZStack(
            OnTap(close_wmenu, Fill(Background(Z_COLOR_SCRIM, Fill(Spacer())))),
            Background(Z_COLOR_SURFACE,
                CornerRadius(20.0f,
                    Padding(24.0f, z_stack(Z_AXIS_VERTICAL, &col)))),
            .align = Z_ALIGN_CENTER));
    }

    // (6) ADD-WIDGET SHEET: a dimmed modal listing each inactive built-in as a
    // tappable row (OnTapData carries the WidgetDef*). Explicit child list.
    ZView wadd = NULL;
    if (state->wadd_open) {
        ZStackOpts col = {.spacing = 12, .align = Z_ALIGN_CENTER};
        int c = 0;
        col.children[c++] = Foreground(Z_COLOR_TEXT_INV,
            Font(Z_FONT_TITLE, Text("Add widget")));
        for (int i = 0; i < N_WIDGET_DEFS && c < Z_MAX_CHILDREN - 1; i++) {
            if (widget_pos(i) >= 0) {
                continue;   // already on the home screen
            }
            col.children[c++] = OnTapData(do_widget_add,
                (void *)&g_widget_defs[i],
                Background(Z_COLOR_SURFACE_2,
                    CornerRadius(12.0f,
                        Padding(14.0f,
                            Foreground(Z_COLOR_TEXT_INV,
                                Font(Z_FONT_BODY,
                                     Text("%s", g_widget_defs[i].title)))))));
        }
        col.children[c++] = Button(close_widget_add, "Cancel");
        wadd = Fill(ZStack(
            OnTap(close_widget_add,
                  Fill(Background(Z_COLOR_SCRIM, Fill(Spacer())))),
            Background(Z_COLOR_SURFACE,
                CornerRadius(20.0f,
                    Padding(24.0f, z_stack(Z_AXIS_VERTICAL, &col)))),
            .align = Z_ALIGN_CENTER));
    }

    // A layer in motion (the sliding drawer) or a translucent overlay (any menu
    // scrim / toast) must paint as a full repaint, or the partial-repaint path
    // re-blends over already-correct pixels and darkens them cumulatively. Force
    // it whenever the drawer is off its rest position or an overlay is up.
    if (drawer_v > 0.001f || menu || toast || wmenu || wadd) {
        z_full_repaint(app);
    }

    // Assemble the depth stack back-to-front, skipping absent optional layers (a
    // NULL child would truncate the positional list, so build it explicitly).
    ZStackOpts root = {.align = Z_ALIGN_CENTER};
    int k = 0;
    root.children[k++] = home;
    root.children[k++] = drawer;
    if (menu) {
        root.children[k++] = menu;
    }
    if (wmenu) {
        root.children[k++] = wmenu;
    }
    if (wadd) {
        root.children[k++] = wadd;
    }
    if (toast) {
        root.children[k++] = toast;
    }
    return z_stack(Z_AXIS_DEPTH, &root);
}

Z_APP_ID(LauncherState, launcher_body, "os.zelto.launcher")
