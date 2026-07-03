// Zelto System UI — app launcher / home screen.
//
// A normal xdg_toplevel app (app_id "os.zelto.launcher") that the compositor
// keeps at the back of the app stack and sizes to the usable area below the
// status bar (and above the bottom nav bar). It is a phone-style home screen
// split the way a real phone does:
//
//   - a HOME surface: a drawn/photo wallpaper and a directly-manipulable BENTO
//     GRID holding BOTH app icons and widgets in one ordered sequence; and
//   - an APP DRAWER: the *complete* installed-app list in a scroll, on an opaque
//     panel that slides up over the home surface.
//
// UNIFIED BENTO GRID (this iteration). The home surface is one ordered sequence
// of typed entries — an app icon (1x1 cell) or a widget (a cw x ch cell span,
// e.g. the clock at 2x2). The sequence is flow-packed into a uniform square-cell
// grid (GRID_COLS columns) by a first-fit occupancy scan, so an icon takes one
// cell and a widget takes its rectangle, wrapping to keep whole cells aligned.
// The grid is a pure function of the sequence: reorder is a 1-D splice, layout
// is 2-D packing. Cells are placed absolutely inside one depth ZStack via the
// SDK's OffsetXY (a plain ZStack only centres its children).
//
// DIRECT MANIPULATION. Press-and-hold any item to enter *rearrange mode*: a
// faint alignment raster appears over the grid, the pressed item lifts under the
// finger (a ghost drawn on top), tap-to-launch is suppressed, and each item
// carries a remove badge. Dragging recomputes the target insertion index every
// frame from the ghost's centre against the cell grid; the sequence is spliced
// live so the other items reflow to open the hovered slot (iOS-style spring-
// loaded reorder). Release commits the new order; a Done bar (or a tap on empty
// space) leaves the mode. All persisted as ONE home.layout CSV of typed tokens
// ("app:os.zelto.cards,widget:clock,...") so the arrangement survives a reboot.
//
// RETAINED-CELL IDENTITY. z_animated_value cells are keyed by CALL ORDER, and
// the sequence's call order changes every reorder — so only the single lifted
// ghost is animated (one z_animated_value at a FIXED position alongside
// drawer_anim, both unconditional and first); the other items express their
// reflow purely through their packed grid position each rebuild. The whole home
// surface's pan is routed through ONE stable handler (on_home_pan) that resolves
// "which item is held" + "target index" from launcher state, never by
// dereferencing an arena node (the P17 shade bug).
//
// App metadata is read from /usr/share/zelto/apps/<id>.app manifests plus
// $ZELTO_DATA_DIR/apps/manifests at startup. The home sequence is persisted in
// the launcher's private storage on /var/zelto; on first run (no home.layout) it
// migrates from the older home.widgets + home.favorites prefs, else seeds a
// default. Tapping any icon fork()+exec()s the app's `exec=` binary.
#include <dirent.h>
#include <math.h>
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
#define DEFAULT_FAVS 4   // home seeds this many apps on first run (pre-layout)

// One installed app, parsed from a .app manifest. `id` is the manifest basename
// (minus ".app"), e.g. "os.zelto.cards" — the stable key used in the layout.
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

// Scan the manifest directories once into g_apps (baked-in + runtime-installed).
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

// --- home-screen widgets --------------------------------------------------
// A widget is an SDK primitive (z_widget): a titled card whose live content a
// callback builds, self-refreshing on a declared cadence. The launcher ships
// three built-ins. In the bento grid each declares a cell span (cw x ch).

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

// The status glance: battery % + the connectivity mode, from brokered sys.* keys.
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

// The notifications glance: the live count of stored notifications.
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
    const char *id;        // stable key persisted in the home.layout CSV
    const char *title;     // card caption
    ZWidgetFn build;       // content builder (z_widget body)
    int refresh_ms;        // self-refresh cadence (0 = source-driven only)
    int cw, ch;            // bento cell span (columns x rows)
} WidgetDef;

static const WidgetDef g_widget_defs[] = {
    {"clock",   "Clock",         w_clock,   1000, 2, 2},
    {"battery", "Status",        w_battery, 0,    2, 1},
    {"notifs",  "Notifications", w_notifs,  0,    2, 1},
};
#define N_WIDGET_DEFS ((int)(sizeof(g_widget_defs) / sizeof(g_widget_defs[0])))

static int find_widget_def(const char *id) {
    for (int i = 0; i < N_WIDGET_DEFS; i++) {
        if (strcmp(g_widget_defs[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

// --- unified home sequence (the bento model) ------------------------------
// One ordered list of typed entries. HE_APP.ref indexes g_apps; HE_WIDGET.ref
// indexes g_widget_defs. Persisted as the home.layout CSV.
typedef enum HomeKind { HE_APP = 0, HE_WIDGET } HomeKind;
typedef struct HomeEntry {
    HomeKind kind;
    int ref;
} HomeEntry;

#define MAX_HOME 32
static HomeEntry g_home[MAX_HOME];
static int g_n_home;
static bool g_home_scanned;

static bool entry_eq(const HomeEntry *e, HomeKind k, int ref) {
    return e->kind == k && e->ref == ref;
}
static int home_find(HomeKind k, int ref) {
    for (int i = 0; i < g_n_home; i++) {
        if (entry_eq(&g_home[i], k, ref)) {
            return i;
        }
    }
    return -1;
}

// Serialise g_home into the home.layout prefs string ("app:id,widget:id,...").
static void write_home_csv(void) {
    char csv[1024];
    size_t off = 0;
    csv[0] = '\0';
    for (int i = 0; i < g_n_home; i++) {
        const char *tok;
        char buf[128];
        if (g_home[i].kind == HE_WIDGET) {
            snprintf(buf, sizeof(buf), "widget:%s",
                     g_widget_defs[g_home[i].ref].id);
        } else {
            snprintf(buf, sizeof(buf), "app:%s", g_apps[g_home[i].ref].id);
        }
        tok = buf;
        int m = snprintf(csv + off, sizeof(csv) - off, "%s%s", i ? "," : "", tok);
        if (m > 0 && (size_t)m < sizeof(csv) - off) {
            off += (size_t)m;
        }
    }
    z_prefs_set_str("home.layout", csv);
    fprintf(stderr, "launcher: wrote home.layout: %s\n", csv);
}

static void home_push_app(int ai) {
    if (ai < 0 || ai >= g_n_apps || g_n_home >= MAX_HOME ||
        home_find(HE_APP, ai) >= 0) {
        return;
    }
    g_home[g_n_home++] = (HomeEntry){HE_APP, ai};
}
static void home_push_widget(int di) {
    if (di < 0 || di >= N_WIDGET_DEFS || g_n_home >= MAX_HOME ||
        home_find(HE_WIDGET, di) >= 0) {
        return;
    }
    g_home[g_n_home++] = (HomeEntry){HE_WIDGET, di};
}

// Resolve g_home once: prefer the home.layout CSV; else migrate from the older
// home.widgets + home.favorites prefs (widgets first, then favourite icons);
// else seed a default (all widgets + the first few apps). Persist on migration.
static void ensure_home_layout(void) {
    if (g_home_scanned) {
        return;
    }
    g_home_scanned = true;
    ensure_apps();

    const char *layout = z_prefs_get_str("home.layout", NULL);
    if (layout && layout[0]) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s", layout);
        g_n_home = 0;
        for (char *tok = strtok(buf, ","); tok && g_n_home < MAX_HOME;
             tok = strtok(NULL, ",")) {
            char *colon = strchr(tok, ':');
            if (!colon) {
                continue;
            }
            *colon = '\0';
            const char *kind = tok;
            const char *id = colon + 1;
            if (strcmp(kind, "widget") == 0) {
                home_push_widget(find_widget_def(id));
            } else if (strcmp(kind, "app") == 0) {
                home_push_app(find_app(id));
            }
        }
        if (g_n_home > 0) {
            fprintf(stderr, "launcher: home.layout loaded (%d)\n", g_n_home);
            return;
        }
    }

    // Migration / seed.
    g_n_home = 0;
    const char *wp = z_prefs_get_str("home.widgets", NULL);
    if (wp && wp[0]) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", wp);
        for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
            home_push_widget(find_widget_def(tok));
        }
    } else {
        for (int i = 0; i < N_WIDGET_DEFS; i++) {
            home_push_widget(i);
        }
    }
    const char *fp = z_prefs_get_str("home.favorites", NULL);
    if (fp && fp[0]) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s", fp);
        for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
            home_push_app(find_app(tok));
        }
    } else {
        int n = g_n_apps < DEFAULT_FAVS ? g_n_apps : DEFAULT_FAVS;
        for (int i = 0; i < n; i++) {
            home_push_app(i);
        }
    }
    write_home_csv();
    fprintf(stderr, "launcher: home.layout migrated/seeded (%d)\n", g_n_home);
}

// --- bento grid geometry --------------------------------------------------
#define GRID_COLS 4
#define GRID_GAP 16.0f
#define GRID_PAD 20.0f
#define GRID_TOP 20.0f
#define MAX_ROWS 20
#define ICON_SIZE 104.0f
#define ICON_RADIUS 24.0f
#define ICON_INSET 20.0f

static float cell_side(float surface_w) {
    float w = surface_w > 1.0f ? surface_w : 720.0f;
    float inner = w - 2.0f * GRID_PAD;
    return (inner - (GRID_COLS - 1) * GRID_GAP) / (float)GRID_COLS;
}

static void entry_span(const HomeEntry *e, int *cw, int *ch) {
    if (e->kind == HE_WIDGET) {
        *cw = g_widget_defs[e->ref].cw;
        *ch = g_widget_defs[e->ref].ch;
    } else {
        *cw = 1;
        *ch = 1;
    }
    if (*cw < 1) *cw = 1;
    if (*cw > GRID_COLS) *cw = GRID_COLS;
    if (*ch < 1) *ch = 1;
}

// A packed cell: which sequence entry, and where (in cells).
typedef struct Placed {
    int col, row, cw, ch;
} Placed;

// First-fit occupancy pack of `seq` into the column grid. Fills `out[i]` per
// entry; returns the number of rows used.
static int pack_home(const HomeEntry *seq, int n, Placed *out) {
    static bool occ[MAX_ROWS][GRID_COLS];
    memset(occ, 0, sizeof(occ));
    int used = 0;
    for (int i = 0; i < n; i++) {
        int cw, ch;
        entry_span(&seq[i], &cw, &ch);
        int pr = -1, pc = -1;
        for (int r = 0; r + ch <= MAX_ROWS && pr < 0; r++) {
            for (int c = 0; c + cw <= GRID_COLS; c++) {
                bool ok = true;
                for (int dr = 0; dr < ch && ok; dr++) {
                    for (int dc = 0; dc < cw && ok; dc++) {
                        if (occ[r + dr][c + dc]) {
                            ok = false;
                        }
                    }
                }
                if (ok) {
                    pr = r;
                    pc = c;
                    break;
                }
            }
        }
        if (pr < 0) {
            pr = 0;
            pc = 0;
        }
        for (int dr = 0; dr < ch; dr++) {
            for (int dc = 0; dc < cw; dc++) {
                if (pr + dr < MAX_ROWS && pc + dc < GRID_COLS) {
                    occ[pr + dr][pc + dc] = true;
                }
            }
        }
        out[i] = (Placed){pc, pr, cw, ch};
        if (pr + ch > used) {
            used = pr + ch;
        }
    }
    return used;
}

// Pixel rect of a packed cell within the surface (top-left origin).
static ZRect cell_rect(float sw, int col, int row, int cw, int ch) {
    float s = cell_side(sw);
    ZRect r;
    r.x = GRID_PAD + col * (s + GRID_GAP);
    r.y = GRID_TOP + row * (s + GRID_GAP);
    r.w = cw * s + (cw - 1) * GRID_GAP;
    r.h = ch * s + (ch - 1) * GRID_GAP;
    return r;
}

// The single grid slot (row*COLS+col) the ghost centre currently hovers.
static int ghost_slot(float sw, float gx, float gy) {
    float s = cell_side(sw);
    int col = (int)floorf((gx - GRID_PAD) / (s + GRID_GAP));
    int row = (int)floorf((gy - GRID_TOP) / (s + GRID_GAP));
    if (col < 0) col = 0;
    if (col > GRID_COLS - 1) col = GRID_COLS - 1;
    if (row < 0) row = 0;
    if (row > MAX_ROWS - 1) row = MAX_ROWS - 1;
    return row * GRID_COLS + col;
}

// Insertion index for the held item in a reduced (held-removed) sequence whose
// packed cells are `pl`: count the items whose start slot precedes the ghost's.
static int target_index(const Placed *pl, int n, int gslot) {
    int idx = 0;
    for (int i = 0; i < n; i++) {
        int s = pl[i].row * GRID_COLS + pl[i].col;
        if (s < gslot) {
            idx = i + 1;
        } else {
            break;
        }
    }
    return idx;
}

// --- launcher state -------------------------------------------------------
typedef struct LauncherState {
    ZAnimated *drawer_anim;   // 0 hidden below the fold .. 1 covering home (cell 0)
    ZAnimated *ghost_anim;    // lifted ghost's x (cell 1) — the ONLY reorder anim
    float surface_w, surface_h;

    // Wallpaper (P25).
    bool wp_subscribed;
    bool wp_ok;
    char wp_path[ZELTO_WALLPAPER_PATH_MAX];

    // Rearrange mode.
    bool rearrange;
    bool held;                // an item is lifted under the finger
    HomeKind held_kind;
    int held_ref;
    float ghost_x, ghost_y;   // ghost centre (surface-local)
    int target_index;         // last-computed insertion index (in the reduced seq)

    // Transient toast.
    double toast_until;
    char toast[64];

    bool test_applied;        // env test-hook armed once
} LauncherState;

// --- helpers --------------------------------------------------------------
static void launch_app(ZApp *app, void *state, void *data) {
    (void)app;
    (void)state;
    const AppEntry *e = data;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execlp(e->exec_path, e->exec_path, (char *)NULL);
        _exit(127);
    }
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void show_toast(LauncherState *s, const char *msg) {
    snprintf(s->toast, sizeof(s->toast), "%s", msg);
    s->toast_until = now_s() + 2.0;
}

// Encode/decode an entry identity in a data pointer (badge + long-press carry
// it). +1 so an (app, index 0) identity is never the NULL pointer.
static void *pack_id(HomeKind k, int ref) {
    intptr_t v = ((((k == HE_WIDGET) ? 1 : 0) << 20) | (ref & 0xffff)) + 1;
    return (void *)v;
}
static void unpack_id(void *p, HomeKind *k, int *ref) {
    intptr_t v = (intptr_t)p - 1;
    *k = ((v >> 20) & 1) ? HE_WIDGET : HE_APP;
    *ref = (int)(v & 0xffff);
}

// --- rearrange handlers ---------------------------------------------------
static void enter_rearrange(ZApp *app, void *state, void *data, float x,
                            float y) {
    LauncherState *s = state;
    HomeKind k;
    int ref;
    unpack_id(data, &k, &ref);
    s->rearrange = true;
    s->held = true;
    s->held_kind = k;
    s->held_ref = ref;
    s->ghost_x = x;
    s->ghost_y = y;
    if (s->ghost_anim) {
        z_animated_set(s->ghost_anim, x);
    }
    z_full_repaint(app);
    z_invalidate(app);
}

static void exit_rearrange(ZApp *app, void *state) {
    LauncherState *s = state;
    s->rearrange = false;
    s->held = false;
    write_home_csv();
    z_full_repaint(app);
    z_invalidate(app);
}

// Remove badge tap: drop this entry from the sequence (stays in rearrange).
static void on_remove_entry(ZApp *app, void *state, void *data) {
    LauncherState *s = state;
    HomeKind k;
    int ref;
    unpack_id(data, &k, &ref);
    int p = home_find(k, ref);
    if (p >= 0) {
        for (int i = p; i < g_n_home - 1; i++) {
            g_home[i] = g_home[i + 1];
        }
        g_n_home--;
        write_home_csv();
    }
    if (s->held && s->held_kind == k && s->held_ref == ref) {
        s->held = false;
    }
    z_full_repaint(app);
    z_invalidate(app);
}

// Drawer long-press: add this app to the home sequence.
static void on_drawer_add(ZApp *app, void *state, void *data, float x,
                          float y) {
    (void)x;
    (void)y;
    LauncherState *s = state;
    const AppEntry *e = data;
    int ai = (int)(e - g_apps);
    if (home_find(HE_APP, ai) >= 0) {
        show_toast(s, "Already on Home");
    } else if (g_n_home >= MAX_HOME) {
        show_toast(s, "Home is full");
    } else {
        home_push_app(ai);
        write_home_csv();
        show_toast(s, "Added to Home");
    }
    z_invalidate(app);
}

// Hit-test a surface point to a g_home index (for drag-any in rearrange mode).
static int hit_test_home(LauncherState *s, float x, float y) {
    static Placed pl[MAX_HOME];
    pack_home(g_home, g_n_home, pl);
    for (int i = 0; i < g_n_home; i++) {
        ZRect r = cell_rect(s->surface_w, pl[i].col, pl[i].row, pl[i].cw,
                            pl[i].ch);
        if (x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h) {
            return i;
        }
    }
    return -1;
}

// Commit the current drag: recompute the reduced sequence + target insertion
// and rewrite g_home with the held item spliced into its new slot.
static void commit_reorder(LauncherState *s) {
    if (!s->held) {
        return;
    }
    HomeEntry reduced[MAX_HOME] = {0};
    int nr = 0;
    HomeEntry held = {s->held_kind, s->held_ref};
    for (int i = 0; i < g_n_home; i++) {
        if (!entry_eq(&g_home[i], s->held_kind, s->held_ref)) {
            reduced[nr++] = g_home[i];
        }
    }
    static Placed pl[MAX_HOME];
    pack_home(reduced, nr, pl);
    int gs = ghost_slot(s->surface_w, s->ghost_x, s->ghost_y);
    int ti = target_index(pl, nr, gs);
    if (ti > nr) {
        ti = nr;
    }
    int n = 0;
    for (int i = 0; i < nr; i++) {
        if (i == ti) {
            g_home[n++] = held;
        }
        g_home[n++] = reduced[i];
    }
    if (ti >= nr) {
        g_home[n++] = held;
    }
    g_n_home = n;
    write_home_csv();
}

static float clamp01(float a) {
    return a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
}

// --- pan: the single home-surface handler ---------------------------------
// Not in rearrange mode: an up-swipe pulls the app drawer up (as before). In
// rearrange mode: a drag lifts/moves the item under the finger (ghost follows),
// recomputing the target slot; release commits. Which item is held + the target
// are resolved from state, never from an arena node (the P17 shade bug).
static void on_home_pan(ZApp *app, void *state, const ZPanEvent *e) {
    LauncherState *s = state;

    if (s->rearrange) {
        if (e->phase == Z_PAN_BEGIN) {
            if (!s->held) {
                int hi = hit_test_home(s, e->x, e->y);
                if (hi >= 0) {
                    s->held = true;
                    s->held_kind = g_home[hi].kind;
                    s->held_ref = g_home[hi].ref;
                }
            }
            s->ghost_x = e->x;
            s->ghost_y = e->y;
            if (s->ghost_anim) {
                z_animated_set(s->ghost_anim, e->x);
            }
            z_full_repaint(app);
            z_invalidate(app);
        } else if (e->phase == Z_PAN_CHANGED) {
            s->ghost_x = e->x;
            s->ghost_y = e->y;
            if (s->ghost_anim) {
                z_animated_set(s->ghost_anim, e->x);
            }
            z_full_repaint(app);
            z_invalidate(app);
        } else if (e->phase == Z_PAN_END) {
            if (s->held) {
                commit_reorder(s);
                s->held = false;
            }
            z_full_repaint(app);
            z_invalidate(app);
        }
        return;
    }

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

// --- wallpaper ------------------------------------------------------------
#define WALL_BANDS 10
#define SCRIM_BANDS 8

static ZView wallpaper_scrim(void) {
    ZStackOpts col = {0};
    for (int i = 0; i < SCRIM_BANDS; i++) {
        float t = (float)i / (float)(SCRIM_BANDS - 1);
        float edge = t < 0.5f ? (1.0f - t * 2.0f) : ((t - 0.5f) * 2.0f);
        uint8_t a = (uint8_t)(edge * edge * 130.0f);
        col.children[i] = Rect(.color = z_rgba(0, 0, 0, a), .grow = 1.0f);
    }
    return Fill(z_stack(Z_AXIS_VERTICAL, &col));
}

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

static void on_wp_setting(ZApp *app, const char *key, const char *value,
                          void *ud) {
    (void)value;
    LauncherState *s = ud;
    if (strcmp(key, ZELTO_WALLPAPER_KEY) == 0) {
        s->wp_ok = zelto_wallpaper_active(s->wp_path, sizeof(s->wp_path));
    }
    z_invalidate(app);
}

// --- cell content ---------------------------------------------------------
// An app icon over its rounded tile with the app name below.
static ZView app_icon_tile(const AppEntry *e) {
    bool own = e->icon_path[0] && z_image_loads(e->icon_path);
    const char *icon = own ? e->icon_path : zelto_placeholder_icon();
    ZView art = own
        ? Frame(ICON_SIZE - 2.0f * ICON_INSET, ICON_SIZE - 2.0f * ICON_INSET,
                Image(icon))
        : Frame(ICON_SIZE, ICON_SIZE, CornerRadius(ICON_RADIUS, Image(icon)));
    return Frame(ICON_SIZE, ICON_SIZE,
        Background(e->color,
            CornerRadius(ICON_RADIUS,
                ZStack(art, .align = Z_ALIGN_CENTER))));
}

static ZView app_cell_content(const AppEntry *e) {
    return VStack(
        app_icon_tile(e),
        Foreground(Z_COLOR_TEXT_INV, Font(Z_FONT_CAPTION, Text("%s", e->name))),
        .spacing = 8, .align = Z_ALIGN_CENTER);
}

static ZView widget_cell_content(ZApp *app, int di) {
    const WidgetDef *d = &g_widget_defs[di];
    return Widget(app, .title = d->title, .body = d->build,
                  .refresh_ms = d->refresh_ms);
}

// A small round remove badge pinned to a cell's top-left corner.
static ZView remove_badge(HomeKind k, int ref) {
    ZView dot = Frame(30.0f, 30.0f,
        Background(Z_COLOR_DANGER,
            CornerRadius(15.0f,
                ZStack(
                    Foreground(Z_COLOR_TEXT_INV,
                        Font(Z_FONT_CALLOUT, Text("\xc3\x97"))),
                    .align = Z_ALIGN_CENTER))));
    return OnTapData(on_remove_entry, pack_id(k, ref), dot);
}

// The view for one placed entry `e` occupying pixel rect `r`. In rearrange mode
// the cell is non-launching and carries a remove badge; otherwise it launches
// (app) and long-press arms rearrange.
static ZView entry_view(ZApp *app, LauncherState *s, const HomeEntry *e,
                        ZRect r) {
    ZView content = (e->kind == HE_WIDGET)
        ? widget_cell_content(app, e->ref)
        : app_cell_content(&g_apps[e->ref]);

    if (s->rearrange) {
        // Non-interactive cell + a corner remove badge (pinned top-left).
        ZView badged = ZStack(
            Fill(content),
            Fill(VStack(
                HStack(Spacer(), remove_badge(e->kind, e->ref),
                       .align = Z_ALIGN_CENTER),
                Spacer(),
                .align = Z_ALIGN_LEADING)),
            .align = Z_ALIGN_CENTER);
        return Frame(r.w, r.h, badged);
    }

    if (e->kind == HE_WIDGET) {
        return Frame(r.w, r.h,
            OnLongPress(enter_rearrange, pack_id(e->kind, e->ref), content));
    }
    return Frame(r.w, r.h,
        OnLongPress(enter_rearrange, pack_id(e->kind, e->ref),
            OnTapData(launch_app, (void *)&g_apps[e->ref], content)));
}

// Place a fixed-size cell view absolutely at rect `r` inside a full-surface
// depth ZStack (which otherwise centres its children).
static ZView placed(float sw, float sh, ZRect r, ZView cell) {
    return OffsetXY(r.x - (sw - r.w) / 2.0f, r.y - (sh - r.h) / 2.0f, cell);
}

// The faint alignment raster: a low-alpha rounded tile at every 1x1 slot across
// the used rows (plus one spare row to drop into).
static ZView raster_layer(LauncherState *s, int rows) {
    ZStackOpts st = {.align = Z_ALIGN_LEADING};
    int k = 0;
    int rr = rows + 1;
    if (rr > MAX_ROWS) {
        rr = MAX_ROWS;
    }
    for (int row = 0; row < rr && k < Z_MAX_CHILDREN; row++) {
        for (int col = 0; col < GRID_COLS && k < Z_MAX_CHILDREN; col++) {
            ZRect r = cell_rect(s->surface_w, col, row, 1, 1);
            ZView tile = Frame(r.w, r.h,
                CornerRadius(ICON_RADIUS,
                    Rect(.color = z_rgba(0xf4, 0xf7, 0xfb, 0x14),
                         .radius = ICON_RADIUS)));
            st.children[k++] = placed(s->surface_w, s->surface_h, r, tile);
        }
    }
    return Fill(z_stack(Z_AXIS_DEPTH, &st));
}

// --- body -----------------------------------------------------------------
static ZView launcher_body(ZApp *app, LauncherState *state) {
    // Retained hooks FIRST + unconditionally so their identity is stable:
    // drawer_anim (cell 0) then ghost_anim (cell 1, the only reorder anim).
    state->drawer_anim = z_animated_value(app, 0.0f);
    state->ghost_anim = z_animated_value(app, 0.0f);
    state->surface_w = (float)z_app_width(app);
    state->surface_h = (float)z_app_height(app);
    ensure_home_layout();

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

    // Test hook: force rearrange (+ held + ghost) once, so the reflow/snap is
    // screenshot-verifiable headlessly without driving a relative-pointer drag.
    //   ZELTO_HOME_REARRANGE=1   arm rearrange mode
    //   ZELTO_HOME_HELD=<i>      lift g_home entry i (default 0)
    //   ZELTO_HOME_GHOST_X/Y=<px> ghost centre (default: held cell centre)
    if (!state->test_applied) {
        state->test_applied = true;
        const char *rr = getenv("ZELTO_HOME_REARRANGE");
        if (rr && rr[0] == '1' && g_n_home > 0) {
            state->rearrange = true;
            const char *hi = getenv("ZELTO_HOME_HELD");
            int h = hi ? atoi(hi) : 0;
            if (h < 0 || h >= g_n_home) {
                h = 0;
            }
            state->held = true;
            state->held_kind = g_home[h].kind;
            state->held_ref = g_home[h].ref;
            static Placed tpl[MAX_HOME];
            pack_home(g_home, g_n_home, tpl);
            ZRect hr = cell_rect(state->surface_w, tpl[h].col, tpl[h].row,
                                 tpl[h].cw, tpl[h].ch);
            const char *gx = getenv("ZELTO_HOME_GHOST_X");
            const char *gy = getenv("ZELTO_HOME_GHOST_Y");
            state->ghost_x = gx ? (float)atof(gx) : hr.x + hr.w / 2.0f;
            state->ghost_y = gy ? (float)atof(gy) : hr.y + hr.h / 2.0f;
            z_animated_set(state->ghost_anim, state->ghost_x);
        }
    }

    // Build the DISPLAY sequence: in rearrange with a held item, splice it out
    // and re-insert at the ghost's target index so the others reflow live.
    HomeEntry disp[MAX_HOME] = {0};
    int nd = 0;
    int held_disp = -1;
    HomeEntry held = {state->held_kind, state->held_ref};
    if (state->rearrange && state->held) {
        HomeEntry reduced[MAX_HOME] = {0};
        int nr = 0;
        for (int i = 0; i < g_n_home; i++) {
            if (!entry_eq(&g_home[i], state->held_kind, state->held_ref)) {
                reduced[nr++] = g_home[i];
            }
        }
        static Placed rpl[MAX_HOME];
        pack_home(reduced, nr, rpl);
        int gs = ghost_slot(state->surface_w, state->ghost_x, state->ghost_y);
        int ti = target_index(rpl, nr, gs);
        if (ti > nr) {
            ti = nr;
        }
        state->target_index = ti;
        for (int i = 0; i < nr; i++) {
            if (i == ti) {
                held_disp = nd;
                disp[nd++] = held;
            }
            disp[nd++] = reduced[i];
        }
        if (ti >= nr) {
            held_disp = nd;
            disp[nd++] = held;
        }
    } else {
        for (int i = 0; i < g_n_home; i++) {
            disp[nd++] = g_home[i];
        }
    }

    static Placed pl[MAX_HOME];
    int rows = pack_home(disp, nd, pl);

    // The bento cells layer (absolute placement in one depth ZStack). The held
    // item's slot renders as a faint placeholder; the ghost is drawn on top.
    ZStackOpts cells = {.align = Z_ALIGN_LEADING};
    int ck = 0;
    for (int i = 0; i < nd && ck < Z_MAX_CHILDREN; i++) {
        ZRect r = cell_rect(state->surface_w, pl[i].col, pl[i].row, pl[i].cw,
                            pl[i].ch);
        ZView cell;
        if (i == held_disp) {
            cell = Frame(r.w, r.h,
                CornerRadius(ICON_RADIUS,
                    Rect(.color = z_rgba(0xf4, 0xf7, 0xfb, 0x22),
                         .radius = ICON_RADIUS)));
        } else {
            cell = entry_view(app, state, &disp[i], r);
        }
        cells.children[ck++] = placed(state->surface_w, state->surface_h, r,
                                      cell);
    }
    ZView cells_layer = Fill(z_stack(Z_AXIS_DEPTH, &cells));

    // The bottom bar: a drawer handle normally, a Done bar in rearrange mode.
    ZView bottom;
    if (state->rearrange) {
        bottom = Fill(VStack(
            Spacer(),
            OnTap(exit_rearrange,
                Background(Z_COLOR_PRIMARY,
                    CornerRadius(22.0f,
                        Padding(14.0f,
                            Foreground(Z_COLOR_TEXT_INV,
                                Font(Z_FONT_CALLOUT, Text("Done"))))))),
            .spacing = 0, .padding = 28, .align = Z_ALIGN_CENTER));
    } else {
        bottom = Fill(VStack(
            Spacer(),
            OnTap(open_drawer,
                VStack(
                    Rect(.color = Z_COLOR_TEXT_MUTED,
                         .width = 56, .height = 5, .radius = 3),
                    Foreground(Z_COLOR_TEXT_INV, Font(Z_FONT_CALLOUT, Text("^"))),
                    Foreground(Z_COLOR_TEXT_MUTED,
                        Font(Z_FONT_CAPTION, Text("All apps"))),
                    .spacing = 4, .align = Z_ALIGN_CENTER)),
            .spacing = 0, .padding = 20, .align = Z_ALIGN_CENTER));
    }

    // The lifted ghost: the held item's content, drawn on top, centred on the
    // finger. Non-interactive; placement is static from the ghost centre.
    ZView ghost = NULL;
    if (state->rearrange && state->held) {
        int cw, ch;
        entry_span(&held, &cw, &ch);
        float s = cell_side(state->surface_w);
        ZRect gr;
        gr.w = cw * s + (cw - 1) * GRID_GAP;
        gr.h = ch * s + (ch - 1) * GRID_GAP;
        gr.x = state->ghost_x - gr.w / 2.0f;
        gr.y = state->ghost_y - gr.h / 2.0f;
        ZView gc = (held.kind == HE_WIDGET)
            ? widget_cell_content(app, held.ref)
            : app_cell_content(&g_apps[held.ref]);
        ZView lifted = Frame(gr.w, gr.h,
            ZStack(
                Fill(CornerRadius(ICON_RADIUS + 2.0f,
                    Rect(.color = Z_COLOR_ACCENT_DIM,
                         .radius = ICON_RADIUS + 2.0f))),
                Fill(gc),
                .align = Z_ALIGN_CENTER));
        ghost = Fill(ZStack(
            placed(state->surface_w, state->surface_h, gr, lifted),
            .align = Z_ALIGN_LEADING));
    }

    // Assemble the home surface (back-to-front), the whole thing one pan target.
    ZStackOpts hs = {.align = Z_ALIGN_CENTER};
    int hk = 0;
    hs.children[hk++] = wallpaper(state);
    if (state->rearrange) {
        hs.children[hk++] = raster_layer(state, rows);
    }
    hs.children[hk++] = cells_layer;
    hs.children[hk++] = bottom;
    if (ghost) {
        hs.children[hk++] = ghost;
    }
    ZView home = OnPan(on_home_pan, Fill(z_stack(Z_AXIS_DEPTH, &hs)));

    // The app drawer (unchanged): an opaque panel slid up from below.
    float drawer_v = z_animated_get(state->drawer_anim);
    float slide = (1.0f - drawer_v) * state->surface_h;
    ZView grabber = OnPan(on_drawer_pan,
        VStack(
            Rect(.color = Z_COLOR_TEXT_MUTED, .width = 56, .height = 5,
                 .radius = 3),
            HStack(
                Foreground(Z_COLOR_TEXT_INV, Font(Z_FONT_TITLE, Text("All apps"))),
                Spacer(),
                OnTap(close_drawer,
                    Foreground(Z_COLOR_TEXT_INV, Font(Z_FONT_TITLE, Text("X")))),
                .align = Z_ALIGN_CENTER),
            .spacing = 10, .align = Z_ALIGN_CENTER));

    // Drawer app grid: a plain 4-col icon grid; long-press adds to Home.
    ZStackOpts dgrid = {.spacing = 18, .align = Z_ALIGN_LEADING};
    int gk = 0;
    for (int i = 0; i < g_n_apps && gk < Z_MAX_CHILDREN; i += GRID_COLS) {
        ZStackOpts row = {.spacing = 16, .align = Z_ALIGN_LEADING};
        for (int c = 0; c < GRID_COLS; c++) {
            int j = i + c;
            if (j < g_n_apps) {
                const AppEntry *e = &g_apps[j];
                row.children[c] = Grow(1.0f,
                    OnLongPress(on_drawer_add, (void *)e,
                        OnTapData(launch_app, (void *)e,
                            app_cell_content(e))));
            } else {
                row.children[c] = Spacer();
            }
        }
        dgrid.children[gk++] = z_stack(Z_AXIS_HORIZONTAL, &row);
    }
    ZView drawer = Offset(NULL, slide, Fill(
        Background(Z_COLOR_BG,
            VStack(
                grabber,
                Grow(1.0f, Scroll(app, z_stack(Z_AXIS_VERTICAL, &dgrid),
                                  .axis = Z_AXIS_VERTICAL)),
                .spacing = 12, .padding = 20, .align = Z_ALIGN_LEADING))));

    // Toast.
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
            .spacing = 0, .padding = 100, .align = Z_ALIGN_CENTER));
    }

    // A moving/overlay layer forces a full repaint (partial path under-damages a
    // translated subtree). Rearrange + drag already force it in the handlers.
    if (drawer_v > 0.001f || state->rearrange || toast) {
        z_full_repaint(app);
    }

    ZStackOpts root = {.align = Z_ALIGN_CENTER};
    int k = 0;
    root.children[k++] = home;
    root.children[k++] = drawer;
    if (toast) {
        root.children[k++] = toast;
    }
    return z_stack(Z_AXIS_DEPTH, &root);
}

Z_APP_ID(LauncherState, launcher_body, "os.zelto.launcher")
