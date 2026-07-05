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
// HORIZONTAL PAGING (P29). The bento grid is a carousel of PAGES: each page is a
// GRID_COLS x rows_per_page() grid, and the pack spills overflow onto the next
// page (pack_home_paged). Pages are a PURE FUNCTION of the pack — never stored;
// the one home.layout CSV still owns the whole arrangement. The cells layer is a
// depth ZStack of per-page subtrees, each translated by (page - page_anim) *
// width so the carousel slides RIGIDLY on a flip (the page translation is kept
// OUT of the per-cell springs, which only ease the within-page reflow). A page-
// dots indicator sits above the bottom bar. Off rearrange, a horizontal swipe
// flips pages (paged snap on release); in rearrange, holding a lifted item in an
// edge gutter flips the page mid-drag (edge-dwell timer) so an item can be
// carried across a page boundary — the ghost stays finger-tracked in viewport
// coords the whole time, and the target index folds in the current page.
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
// drawer_anim + page_anim, all unconditional and first); the other items express
// their reflow purely through their packed grid position each rebuild. The home
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
        Weight(Z_WEIGHT_SEMIBOLD,
            Foreground(Z_COLOR_TEXT, Font(Z_FONT_LARGE_TITLE, Text("%s", hhmm)))),
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
        Weight(Z_WEIGHT_SEMIBOLD,
            Foreground(pc, Font(Z_FONT_TITLE, Text("%d%%", pct)))),
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
        Weight(Z_WEIGHT_SEMIBOLD,
            Foreground(n > 0 ? Z_COLOR_ACCENT : Z_COLOR_TEXT_MUTED,
                       Font(Z_FONT_LARGE_TITLE, Text("%d", n)))),
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
// The home is a horizontal carousel of PAGES. Each page is a GRID_COLS-wide,
// rows_per_page()-tall bento grid; the ordered sequence flow-packs across pages
// (overflow spills to the next page). Pages are a PURE FUNCTION of the pack —
// never persisted — so the one home.layout CSV still owns the whole arrangement.
#define GRID_COLS 4
#define GRID_GAP 16.0f
#define GRID_PAD 20.0f
#define GRID_TOP 20.0f
#define MAX_ROWS 20              // per-page occupancy height cap
#define MAX_PAGES 8              // carousel cap
#define BOTTOM_RESERVE 132.0f    // px kept for the page dots + drawer handle / Done
#define ICON_SIZE 104.0f
#define ICON_RADIUS 24.0f
#define ICON_INSET 20.0f

static float cell_side(float surface_w) {
    float w = surface_w > 1.0f ? surface_w : 720.0f;
    float inner = w - 2.0f * GRID_PAD;
    return (inner - (GRID_COLS - 1) * GRID_GAP) / (float)GRID_COLS;
}

// Rows a single page holds: as many whole cells as fit above BOTTOM_RESERVE.
// ZELTO_HOME_ROWS_PER_PAGE forces it (small pages make overflow easy to test).
static int rows_per_page(float sw, float sh) {
    static int override_rows = -2;   // -2 = env unread
    if (override_rows == -2) {
        const char *e = getenv("ZELTO_HOME_ROWS_PER_PAGE");
        override_rows = (e && e[0]) ? atoi(e) : -1;
    }
    int r;
    if (override_rows > 0) {
        r = override_rows;
    } else {
        float s = cell_side(sw);
        float avail = (sh > 1.0f ? sh : 1440.0f) - GRID_TOP - BOTTOM_RESERVE +
                      GRID_GAP;
        r = (int)floorf(avail / (s + GRID_GAP));
    }
    if (r < 2) r = 2;             // must fit the tallest widget (clock is 2 rows)
    if (r > MAX_ROWS) r = MAX_ROWS;
    return r;
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

// A packed cell: which sequence entry, and where — page + (col,row) WITHIN that
// page (row is page-local, 0..rows_per_page-1).
typedef struct Placed {
    int page, col, row, cw, ch;
} Placed;

// First-fit occupancy pack of `seq` across the paged grid: each page is a fresh
// GRID_COLS x rpp occupancy; an entry that doesn't fit the current page spills
// to the next (a widget never straddles a page boundary). Fills `out[i]` per
// entry; returns the PAGE COUNT (>=1).
static int pack_home_paged(const HomeEntry *seq, int n, int rpp, Placed *out) {
    static bool occ[MAX_PAGES][MAX_ROWS][GRID_COLS];
    memset(occ, 0, sizeof(occ));
    if (rpp > MAX_ROWS) rpp = MAX_ROWS;
    int max_page = 0;
    for (int i = 0; i < n; i++) {
        int cw, ch;
        entry_span(&seq[i], &cw, &ch);
        if (ch > rpp) ch = rpp;   // clamp a too-tall widget to one page
        int pg = -1, pr = -1, pc = -1;
        for (int p = 0; p < MAX_PAGES && pg < 0; p++) {
            for (int r = 0; r + ch <= rpp && pg < 0; r++) {
                for (int c = 0; c + cw <= GRID_COLS; c++) {
                    bool ok = true;
                    for (int dr = 0; dr < ch && ok; dr++) {
                        for (int dc = 0; dc < cw && ok; dc++) {
                            if (occ[p][r + dr][c + dc]) {
                                ok = false;
                            }
                        }
                    }
                    if (ok) {
                        pg = p;
                        pr = r;
                        pc = c;
                        break;
                    }
                }
            }
        }
        if (pg < 0) {
            pg = 0;
            pr = 0;
            pc = 0;
        }
        for (int dr = 0; dr < ch; dr++) {
            for (int dc = 0; dc < cw; dc++) {
                if (pr + dr < MAX_ROWS && pc + dc < GRID_COLS) {
                    occ[pg][pr + dr][pc + dc] = true;
                }
            }
        }
        out[i] = (Placed){pg, pc, pr, cw, ch};
        if (pg > max_page) {
            max_page = pg;
        }
    }
    return max_page + 1;
}

// The global (across-page) slot of a packed cell — its ordering key.
static int placed_slot(const Placed *p, int rpp) {
    return (p->page * rpp + p->row) * GRID_COLS + p->col;
}

// Pixel rect of a packed cell within ITS page (top-left page origin). The page's
// horizontal translation is applied at the page-subtree level, not here.
static ZRect cell_rect(float sw, int col, int row, int cw, int ch) {
    float s = cell_side(sw);
    ZRect r;
    r.x = GRID_PAD + col * (s + GRID_GAP);
    r.y = GRID_TOP + row * (s + GRID_GAP);
    r.w = cw * s + (cw - 1) * GRID_GAP;
    r.h = ch * s + (ch - 1) * GRID_GAP;
    return r;
}

// Centre-relative offset that places a cell of rect `r` inside a full-surface
// depth ZStack (mirrors placed()). This is the value an item's keyed x/y anim
// cells spring toward, so a slot change eases instead of teleporting.
static void cell_offset(float sw, float sh, ZRect r, float *ox, float *oy) {
    *ox = r.x - (sw - r.w) / 2.0f;
    *oy = r.y - (sh - r.h) / 2.0f;
}

// The GLOBAL grid slot the ghost centre hovers, folding in the page it is over:
// the ghost is drawn in viewport (current-page) coordinates, so map its local
// (col,row) then offset by `page` worth of rows. This is what lets the target
// index cross a page boundary once the carousel has flipped mid-drag.
static int ghost_slot(float sw, float gx, float gy, int rpp, int page) {
    float s = cell_side(sw);
    int col = (int)floorf((gx - GRID_PAD) / (s + GRID_GAP));
    int row = (int)floorf((gy - GRID_TOP) / (s + GRID_GAP));
    if (col < 0) col = 0;
    if (col > GRID_COLS - 1) col = GRID_COLS - 1;
    if (row < 0) row = 0;
    if (row > rpp - 1) row = rpp - 1;
    return (page * rpp + row) * GRID_COLS + col;
}

// Insertion index for the held item in a reduced (held-removed) sequence whose
// packed cells are `pl`: count the items whose global slot precedes the ghost's.
static int target_index(const Placed *pl, int n, int gslot, int rpp) {
    int idx = 0;
    for (int i = 0; i < n; i++) {
        int s = placed_slot(&pl[i], rpp);
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
    ZAnimated *page_anim;     // carousel scroll position, in page units (cell 2)
    float surface_w, surface_h;

    // Horizontal pager.
    int page;                 // current settled page (integer)
    int npages;               // page count from the last build
    int pan_axis;             // gesture axis lock: 0 undecided, 1 horiz, 2 vert
    float page_base;          // page_anim value at pan begin (for horiz drags)
    int dwell_edge;           // cross-page edge-gutter dwell: -1 none, 0 L, 1 R

    // Wallpaper (P25).
    bool wp_subscribed;
    bool wp_ok;
    char wp_path[ZELTO_WALLPAPER_PATH_MAX];

    // Rearrange mode.
    bool rearrange;
    bool held;                // an item is lifted under the finger
    bool landing;             // released: ghost springing into its committed slot
    HomeKind held_kind;
    int held_ref;
    float ghost_x, ghost_y;   // ghost centre (surface-local)
    int target_index;         // last-computed insertion index (in the reduced seq)

    // Headless animation test hooks (see the test block in launcher_body).
    int test_anim_frames;     // spring steps to advance before the shot (0 = off)
    bool test_landing;        // stage a ghost snap-back rather than a reflow
    bool test_seeded;         // one-shot: springs seeded + stepped
    bool test_page_seeded;    // one-shot: page-flip spring seeded + stepped

    // Transient toast.
    double toast_until;
    char toast[64];
    // Toast swipe-to-dismiss (P33). `toast_drag` is the finger's live DOWNWARD
    // translation of the toast (it sits at the bottom, so down dismisses); driven
    // 1:1 while held and sprung back on a short release. The pointer is cached from
    // the build (the keyed cell is stable) so the pan handler can reach it.
    ZAnimated *toast_drag;
    bool toast_dragging;

    // App-open continuity cue (P32). When an app icon is tapped, `launch_ref` is
    // the launching app's g_apps index (-1 = none) and `launch_anim` (an
    // identity-keyed spring, 0..1) drives a brief hand-off: the tapped tile drifts
    // toward screen centre while the rest of the home fades back, so opening reads
    // as coming FROM that tile — the client-side "coordinated fade" (the toolkit
    // has no scale primitive; the true zoom-from-tile is compositor-owned and
    // deferred). Reset when the launcher returns to the foreground.
    int launch_ref;
    bool launch_inited;
    ZAnimated *launch_anim;

    bool test_applied;        // env test-hook armed once
} LauncherState;

// --- helpers --------------------------------------------------------------
// Begin the app-open cue then fork/exec the target. Recording the launching index
// + waking the spring makes the tile the origin of the transition; the exec still
// happens immediately so the app starts mapping while the cue plays underneath.
static void launch_app(ZApp *app, void *state, void *data) {
    LauncherState *s = state;
    const AppEntry *e = data;
    if (s) {
        s->launch_ref = (int)(e - g_apps);
        if (s->launch_anim) {
            z_animated_set(s->launch_anim, 0.0f);
            z_animated_spring_with(s->launch_anim, 1.0f, Z_SPRING_SNAPPY);
        }
        z_invalidate(app);
    }
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

// Toast swipe-to-dismiss (P33): the toast sits at the bottom, so a DOWNWARD drag
// carries it 1:1 and dismisses it past a threshold/velocity; a short or upward
// release snaps it back. Dismiss just clears the dwell so the toast's exit spring
// (P32) fades it out from the finger's position.
#define TOAST_DISMISS_THRESH 24.0f
#define TOAST_DISMISS_DIST 70.0f
static void on_toast_pan(ZApp *app, void *state, const ZPanEvent *e) {
    LauncherState *s = state;
    if (!s->toast_drag) {
        return;
    }
    if (e->phase == Z_PAN_BEGIN) {
        s->toast_dragging = true;
        z_animated_grab(s->toast_drag);
    } else if (e->phase == Z_PAN_CHANGED) {
        z_animated_set(s->toast_drag, e->translation_y > 0.0f ? e->translation_y
                                                              : 0.0f);
    } else {   // Z_PAN_END
        s->toast_dragging = false;
        float d = z_animated_get(s->toast_drag);
        if (d > TOAST_DISMISS_THRESH || e->velocity_y > 700.0f) {
            s->toast_until = 0.0;                 // end the dwell -> exit spring fades
        } else {
            z_animated_spring_velocity(s->toast_drag, 0.0f, Z_SPRING_STANDARD,
                                       e->velocity_y);
        }
    }
    z_invalidate(app);
}

static void show_toast(LauncherState *s, const char *msg) {
    snprintf(s->toast, sizeof(s->toast), "%s", msg);
    s->toast_dragging = false;
    if (s->toast_drag) {
        z_animated_set(s->toast_drag, 0.0f);   // fresh toast starts un-dragged
    }
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

// Stable identity key for a home entry — the key for its retained keyed anim
// cells (kind in bit 32, ref in the low 32). Because the cell is keyed by item
// IDENTITY, not by call order, its spring survives every reorder even as the
// item's position in the build changes. The x and y axes derive two sub-keys.
static uint64_t entry_key(HomeKind k, int ref) {
    return ((uint64_t)(k == HE_WIDGET ? 1u : 0u) << 32) | (uint32_t)ref;
}
#define KEY_X(base) (base)
#define KEY_Y(base) ((base) | (1ull << 40))

// Only (re)arm a spring when its target actually moved, so a settled cell whose
// packed slot is unchanged does not wake the frame loop on every rebuild.
static void spring_to(ZAnimated *v, float to) {
    if (fabsf(z_animated_target(v) - to) > 0.5f) {
        z_animated_spring(v, to);
    }
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
    s->landing = false;
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
    s->landing = false;
    s->dwell_edge = -1;
    z_after_cancel(app);
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
// The point is in viewport (current-page) coordinates, so only entries packed
// onto the page currently in view are candidates.
static int hit_test_home(LauncherState *s, float x, float y) {
    static Placed pl[MAX_HOME];
    int rpp = rows_per_page(s->surface_w, s->surface_h);
    pack_home_paged(g_home, g_n_home, rpp, pl);
    for (int i = 0; i < g_n_home; i++) {
        if (pl[i].page != s->page) {
            continue;
        }
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
    int rpp = rows_per_page(s->surface_w, s->surface_h);
    pack_home_paged(reduced, nr, rpp, pl);
    int gs = ghost_slot(s->surface_w, s->ghost_x, s->ghost_y, rpp, s->page);
    int ti = target_index(pl, nr, gs, rpp);
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

// Release: the reorder is already committed into g_home; spring the ghost from
// the finger into the item's committed slot (snap-back) and enter landing mode.
// The ghost and the landed item share the SAME keyed cell (entry_key), so once
// the spring settles the ghost is dropped and the item's own cell carries it on
// with no visible jump. Uses the item's base keyed x/y cells, springing them to
// the slot centre; the cells were being tracked to the finger during the drag.
static void ghost_land(ZApp *app, LauncherState *s) {
    int idx = home_find(s->held_kind, s->held_ref);
    if (idx < 0 || s->surface_w < 1.0f) {
        s->held = false;
        s->landing = false;
        return;
    }
    static Placed pl[MAX_HOME];
    int rpp = rows_per_page(s->surface_w, s->surface_h);
    pack_home_paged(g_home, g_n_home, rpp, pl);
    // The ghost snaps back in viewport coords, so make sure the page it landed on
    // is the one in view (a cross-page drop may have parked it elsewhere).
    if (pl[idx].page != s->page && s->page_anim) {
        s->page = pl[idx].page;
        z_animated_spring_with(s->page_anim, (float)pl[idx].page,
                               Z_SPRING_SNAPPY);
    }
    ZRect r = cell_rect(s->surface_w, pl[idx].col, pl[idx].row, pl[idx].cw,
                        pl[idx].ch);
    float ox, oy;
    cell_offset(s->surface_w, s->surface_h, r, &ox, &oy);
    uint64_t base = entry_key(s->held_kind, s->held_ref);
    ZAnimated *cx = z_animated_keyed(app, KEY_X(base), ox);
    ZAnimated *cy = z_animated_keyed(app, KEY_Y(base), oy);
    z_animated_spring(cx, ox);
    z_animated_spring(cy, oy);
    s->held = false;
    s->landing = true;
}

static float clamp01(float a) {
    return a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
}

// Spring the carousel to page `p` (clamped) and record it as the settled page.
static void snap_to_page(ZApp *app, LauncherState *s, int p) {
    int maxp = s->npages > 0 ? s->npages - 1 : 0;
    if (p < 0) p = 0;
    if (p > maxp) p = maxp;
    s->page = p;
    if (s->page_anim) {
        // A page flip is a decisive move — the SNAPPY token, per the motion vocab.
        z_animated_spring_with(s->page_anim, (float)p, Z_SPRING_SNAPPY);
    }
    z_full_repaint(app);
    z_invalidate(app);
}

#define EDGE_GUTTER 52.0f    // px band at each screen edge that arms a page flip
#define EDGE_DWELL_MS 420    // hold there this long to flip to the neighbour page

// Cross-page drag: while an item is held in an edge gutter, this one-shot fires
// after the dwell, flips to the neighbouring page and — if the finger is still
// in the gutter — re-arms itself so a sustained hold keeps paging. Driven by the
// z_after timer (not an arena node) so it can never leak across a rebuild; the
// pan handler cancels it the moment the finger leaves the gutter.
static void on_edge_dwell(ZApp *app, void *ud) {
    LauncherState *s = ud;
    if (!s->rearrange || !s->held || s->dwell_edge < 0) {
        return;
    }
    int target = s->page + (s->dwell_edge == 1 ? 1 : -1);
    // Allow paging one past the last used page (drop onto a fresh page).
    int maxp = s->npages < MAX_PAGES ? s->npages : MAX_PAGES - 1;
    if (target < 0 || target > maxp) {
        return;
    }
    s->page = target;
    if (s->page_anim) {
        z_animated_spring_with(s->page_anim, (float)target, Z_SPRING_SNAPPY);
    }
    z_after(app, EDGE_DWELL_MS, on_edge_dwell, s);   // keep paging if still held
    z_full_repaint(app);
    z_invalidate(app);
}

// --- pan: the single home-surface handler ---------------------------------
// Everything the home surface does is routed through THIS one handler; which
// behaviour runs is resolved from launcher state, never from an arena node (the
// P17 shade bug). In rearrange mode a drag lifts/moves the held item (ghost
// follows) and an edge-gutter dwell flips pages mid-drag. Otherwise the gesture
// axis is locked on first motion: a horizontal drag flips carousel pages (paged
// snap on release), a vertical up-swipe pulls the app drawer up.
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
            s->dwell_edge = -1;
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
            // Edge-gutter dwell → arm/cancel the cross-page flip timer.
            float w = s->surface_w > 1.0f ? s->surface_w : 1.0f;
            int edge = -1;
            if (s->held) {
                if (e->x < EDGE_GUTTER) {
                    edge = 0;
                } else if (e->x > w - EDGE_GUTTER) {
                    edge = 1;
                }
            }
            if (edge != s->dwell_edge) {
                s->dwell_edge = edge;
                z_after_cancel(app);
                if (edge >= 0) {
                    z_after(app, EDGE_DWELL_MS, on_edge_dwell, s);
                }
            }
            z_full_repaint(app);
            z_invalidate(app);
        } else if (e->phase == Z_PAN_END) {
            s->dwell_edge = -1;
            z_after_cancel(app);
            if (s->held) {
                commit_reorder(s);
                ghost_land(app, s);  // spring the ghost into its committed slot
            }
            z_full_repaint(app);
            z_invalidate(app);
        }
        return;
    }

    if (!s->drawer_anim || !s->page_anim) {
        return;
    }
    float h = s->surface_h > 1.0f ? s->surface_h : 1.0f;
    float w = s->surface_w > 1.0f ? s->surface_w : 1.0f;
    if (e->phase == Z_PAN_BEGIN) {
        s->pan_axis = 0;
        s->page_base = z_animated_get(s->page_anim);
    } else if (e->phase == Z_PAN_CHANGED) {
        if (s->pan_axis == 0) {   // lock the axis on the first real motion
            float ax = fabsf(e->translation_x), ay = fabsf(e->translation_y);
            if (ax > 8.0f && ax >= ay) {
                s->pan_axis = 1;
            } else if (ay > 8.0f) {
                s->pan_axis = 2;
            }
        }
        if (s->pan_axis == 1) {
            float p = s->page_base - e->translation_x / w;
            float maxp = s->npages > 0 ? (float)(s->npages - 1) : 0.0f;
            if (p < 0.0f) p = 0.0f;
            if (p > maxp) p = maxp;
            z_animated_set(s->page_anim, p);
            z_full_repaint(app);
            z_invalidate(app);
        } else if (s->pan_axis == 2) {
            z_animated_set(s->drawer_anim, clamp01(-e->translation_y / h));
        }
    } else if (e->phase == Z_PAN_END) {
        if (s->pan_axis == 1) {
            float p = z_animated_get(s->page_anim);
            int target = (int)floorf(p + 0.5f);
            if (e->velocity_x < -600.0f) {
                target = (int)ceilf(p);     // fling left → next page
            } else if (e->velocity_x > 600.0f) {
                target = (int)floorf(p);    // fling right → previous page
            }
            snap_to_page(app, s, target);
        } else if (s->pan_axis == 2) {
            float a = z_animated_get(s->drawer_anim);
            bool open = a > 0.35f || e->velocity_y < -500.0f;
            z_animated_spring(s->drawer_anim, open ? 1.0f : 0.0f);
        }
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
// A clean monogram (the app name's initial) drawn on the tile when an app ships
// no icon of its own — nicer than the shared checkerboard placeholder bitmap,
// and it needs no asset. Uppercased ASCII initial, '?' for an empty name.
static ZView app_monogram(const AppEntry *e) {
    char c = e->name[0];
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 32);
    }
    if (c == '\0') {
        c = '?';
    }
    return Foreground(Z_COLOR_TEXT_INV,
        Font(Z_FONT_LARGE_TITLE, Text("%c", c)));
}

// An app icon over its rounded tile with the app name below. Apps with their own
// icon draw it inset; icon-less apps get a monogram on the coloured tile.
static ZView app_icon_tile(const AppEntry *e) {
    bool own = e->icon_path[0] && z_image_loads(e->icon_path);
    ZView art = own
        ? Frame(ICON_SIZE - 2.0f * ICON_INSET, ICON_SIZE - 2.0f * ICON_INSET,
                Image(e->icon_path))
        : app_monogram(e);
    return Shadow(Z_ELEV_1, Frame(ICON_SIZE, ICON_SIZE,
        Background(e->color,
            CornerRadius(ICON_RADIUS,
                ZStack(art, .align = Z_ALIGN_CENTER)))));
}

static ZView app_cell_content(const AppEntry *e) {
    return VStack(
        app_icon_tile(e),
        TextShadow(Foreground(Z_COLOR_TEXT_INV,
            Font(Z_FONT_CAPTION, Text("%s", e->name)))),
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

    ZView out;
    if (e->kind == HE_WIDGET) {
        out = Frame(r.w, r.h,
            OnLongPress(enter_rearrange, pack_id(e->kind, e->ref), content));
    } else {
        out = Frame(r.w, r.h,
            OnLongPress(enter_rearrange, pack_id(e->kind, e->ref),
                OnTapData(launch_app, (void *)&g_apps[e->ref], content)));
    }

    // App-open continuity cue (P32): while a launch is in flight, the launching
    // tile drifts toward screen centre (the transition's origin) and every other
    // entry fades back, handing the screen off to the opening app.
    float lp = (s->launch_ref >= 0 && s->launch_anim)
                   ? z_animated_get(s->launch_anim)
                   : 0.0f;
    if (lp > 0.001f) {
        if (e->kind == HE_APP && s->launch_ref == e->ref) {
            float tcx = r.x + r.w * 0.5f, tcy = r.y + r.h * 0.5f;
            float dx = (s->surface_w * 0.5f - tcx) * lp * 0.28f;
            float dy = (s->surface_h * 0.5f - tcy) * lp * 0.28f;
            out = OffsetXY(dx, dy, out);
        } else {
            out = Opacity(1.0f - 0.7f * lp, out);
        }
    }
    return out;
}

// Place a fixed-size cell view absolutely at rect `r` inside a full-surface
// depth ZStack (which otherwise centres its children).
static ZView placed(float sw, float sh, ZRect r, ZView cell) {
    return OffsetXY(r.x - (sw - r.w) / 2.0f, r.y - (sh - r.h) / 2.0f, cell);
}

// The faint alignment raster for the CURRENT page: a low-alpha rounded tile at
// every 1x1 slot across the page's rows. Drawn in viewport coords (no page
// translation) so it underlays whichever page is centred.
static ZView raster_layer(LauncherState *s, int rows) {
    ZStackOpts st = {.align = Z_ALIGN_LEADING};
    int k = 0;
    int rr = rows;
    if (rr > MAX_ROWS) {
        rr = MAX_ROWS;
    }
    for (int row = 0; row < rr && k < Z_MAX_CHILDREN; row++) {
        for (int col = 0; col < GRID_COLS && k < Z_MAX_CHILDREN; col++) {
            ZRect r = cell_rect(s->surface_w, col, row, 1, 1);
            // A brighter, accent-tinted slot so the alignment grid actually
            // reads while dragging (the old near-invisible 0x14 white ghost was
            // easy to miss against a busy wallpaper).
            ZView tile = Frame(r.w, r.h,
                CornerRadius(ICON_RADIUS,
                    Rect(.color = z_rgba(0x4a, 0xa3, 0xff, 0x30),
                         .radius = ICON_RADIUS)));
            st.children[k++] = placed(s->surface_w, s->surface_h, r, tile);
        }
    }
    return Fill(z_stack(Z_AXIS_DEPTH, &st));
}

// The carousel page indicator: one dot per page, the current one bright + larger.
// `page_v` is the live (fractional) scroll position; the nearest page reads as
// active mid-flip.
static ZView page_dots(int npages, float page_v) {
    int active = (int)floorf(page_v + 0.5f);
    // Flanking Spacers centre the dots: a bare HStack expands to the full width
    // and would otherwise pack the dots at the leading edge.
    ZStackOpts row = {.spacing = 9, .align = Z_ALIGN_CENTER};
    int k = 0;
    row.children[k++] = Spacer();
    for (int i = 0; i < npages && k < Z_MAX_CHILDREN - 1; i++) {
        bool on = i == active;
        float d = on ? 9.0f : 7.0f;
        row.children[k++] = Frame(d, d,
            CornerRadius(d / 2.0f,
                Rect(.color = on ? Z_COLOR_TEXT_INV : Z_COLOR_TEXT_MUTED,
                     .radius = d / 2.0f)));
    }
    row.children[k++] = Spacer();
    return z_stack(Z_AXIS_HORIZONTAL, &row);
}

// --- body -----------------------------------------------------------------
// Returning to the foreground (the launched app closed / Home pressed): clear the
// app-open cue so the tile and home are back to normal.
static void on_launcher_lifecycle(ZApp *app, void *state, ZLifecycle ev) {
    LauncherState *s = state;
    // Skip the reset when the launch cue is frozen for a screenshot
    // (ZELTO_HOME_LAUNCH) — the launcher activating at boot would otherwise clear
    // the pinned cue before the frame is grabbed.
    if (ev == Z_LC_ACTIVE && !getenv("ZELTO_HOME_LAUNCH")) {
        s->launch_ref = -1;
        if (s->launch_anim) {
            z_animated_set(s->launch_anim, 0.0f);
        }
        z_invalidate(app);
    }
}

static ZView launcher_body(ZApp *app, LauncherState *state) {
    // Retained hooks FIRST + unconditionally so their call-order identity is
    // stable: drawer_anim (cell 0), ghost_anim (cell 1, the only reorder anim),
    // page_anim (cell 2, the carousel scroll position in page units). These
    // three call-order cells sit in a separate namespace from the per-item
    // z_animated_keyed cells (keyed by identity), so adding page_anim here does
    // not disturb any item's spring.
    state->drawer_anim = z_animated_value(app, 0.0f);
    state->ghost_anim = z_animated_value(app, 0.0f);
    state->page_anim = z_animated_value(app, 0.0f);
    // App-open cue spring — IDENTITY-keyed, so it shares no cell with the three
    // call-order anims above (adding it here can't shift their identity).
    state->launch_anim = z_animated_keyed(app, 0x1A0C1DULL /*'launch'*/, 0.0f);
    if (!state->launch_inited) {
        state->launch_inited = true;
        state->launch_ref = -1;
        z_on_lifecycle(app, on_launcher_lifecycle);
    }
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
    //   ZELTO_HOME_ANIM_FRAMES=N step the springs N frames before the shot, so a
    //                            still PNG catches motion (mid-reflow / mid-snap).
    //   ZELTO_HOME_LANDING=1     stage a released ghost mid snap-back (with FRAMES)
    //                            instead of a held mid-reflow.
    //   ZELTO_HOME_PAGE=<n>      settle the carousel on page n (a still shot of a
    //                            non-first page, dots included). Also the page the
    //                            ghost is over for a cross-page-drag shot.
    //   ZELTO_HOME_ROWS_PER_PAGE=<n> force page height (small = easy overflow).
    //   ZELTO_HOME_PAGE_FROM=<f> with ANIM_FRAMES>0 (and NOT rearrange): seed the
    //                            carousel at page f and spring toward ZELTO_HOME_PAGE,
    //                            then freeze — catches a page-flip mid-slide.
    if (!state->test_applied) {
        state->test_applied = true;
        const char *pg = getenv("ZELTO_HOME_PAGE");
        if (pg && pg[0]) {
            int p = atoi(pg);
            if (p < 0) p = 0;
            if (p >= MAX_PAGES) p = MAX_PAGES - 1;
            state->page = p;
            z_animated_set(state->page_anim, (float)p);
        }
        // ZELTO_HOME_DRAWER=1 seeds the app drawer fully open on the first build,
        // so the slid-up all-apps panel is screenshot-verifiable without a swipe.
        const char *dr = getenv("ZELTO_HOME_DRAWER");
        if (dr && dr[0] == '1') {
            z_animated_set(state->drawer_anim, 1.0f);
        }
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
            int rpp = rows_per_page(state->surface_w, state->surface_h);
            pack_home_paged(g_home, g_n_home, rpp, tpl);
            ZRect hr = cell_rect(state->surface_w, tpl[h].col, tpl[h].row,
                                 tpl[h].cw, tpl[h].ch);
            const char *gx = getenv("ZELTO_HOME_GHOST_X");
            const char *gy = getenv("ZELTO_HOME_GHOST_Y");
            state->ghost_x = gx ? (float)atof(gx) : hr.x + hr.w / 2.0f;
            state->ghost_y = gy ? (float)atof(gy) : hr.y + hr.h / 2.0f;
            z_animated_set(state->ghost_anim, state->ghost_x);
            const char *af = getenv("ZELTO_HOME_ANIM_FRAMES");
            state->test_anim_frames = af ? atoi(af) : 0;
            const char *lt = getenv("ZELTO_HOME_LANDING");
            state->test_landing = lt && lt[0] == '1';
            if (state->test_landing) {
                // Commit the drag now so the item sits in its landed slot; the
                // seeding pass below springs the ghost from the finger into it.
                commit_reorder(state);
                state->held = false;
                state->landing = true;
            }
        }
        // ZELTO_HOME_LAUNCH=1 freezes the app-open cue: mark the first placed home
        // APP entry as launching and pin its spring at ZELTO_HOME_LAUNCH_PROG
        // (default 0.6), so the tile-drift + home-fade hand-off is a still shot.
        const char *lc = getenv("ZELTO_HOME_LAUNCH");
        if (lc && lc[0] == '1') {
            for (int i = 0; i < g_n_home; i++) {
                if (g_home[i].kind == HE_APP) {
                    state->launch_ref = g_home[i].ref;
                    break;
                }
            }
            const char *lp = getenv("ZELTO_HOME_LAUNCH_PROG");
            z_animated_pin(state->launch_anim,
                           lp && lp[0] ? (float)atof(lp) : 0.6f);
        }
    }

    // Page-flip test seeding (independent of rearrange): seed page_anim at
    // ZELTO_HOME_PAGE_FROM, spring toward the settled page, step + freeze so a
    // still PNG catches the carousel mid-slide.
    if (!state->test_page_seeded) {
        state->test_page_seeded = true;
        const char *pf = getenv("ZELTO_HOME_PAGE_FROM");
        const char *af = getenv("ZELTO_HOME_ANIM_FRAMES");
        int frames = af ? atoi(af) : 0;
        if (pf && pf[0] && frames > 0 && !state->rearrange) {
            z_animated_set(state->page_anim, (float)atof(pf));
            z_animated_spring_with(state->page_anim, (float)state->page,
                                   Z_SPRING_SNAPPY);   // match the real page flip
            for (int f = 0; f < frames; f++) {
                z_anim_tick(app, 1.0f / 60.0f);
            }
            z_animated_pin(state->page_anim, z_animated_get(state->page_anim));
        }
    }

    // Build the DISPLAY sequence: in rearrange with a held item, splice it out
    // and re-insert at the ghost's target index so the others reflow live.
    HomeEntry disp[MAX_HOME] = {0};
    int nd = 0;
    HomeEntry held = {state->held_kind, state->held_ref};
    // The held OR landing item is "special": drawn as a ghost on top, its home
    // slot a faint placeholder, its own keyed cell driven separately.
    bool special = state->held || state->landing;
    if (state->rearrange && state->held) {
        HomeEntry reduced[MAX_HOME] = {0};
        int nr = 0;
        for (int i = 0; i < g_n_home; i++) {
            if (!entry_eq(&g_home[i], state->held_kind, state->held_ref)) {
                reduced[nr++] = g_home[i];
            }
        }
        static Placed rpl[MAX_HOME];
        int rpp0 = rows_per_page(state->surface_w, state->surface_h);
        pack_home_paged(reduced, nr, rpp0, rpl);
        int gs = ghost_slot(state->surface_w, state->ghost_x, state->ghost_y,
                            rpp0, state->page);
        int ti = target_index(rpl, nr, gs, rpp0);
        if (ti > nr) {
            ti = nr;
        }
        state->target_index = ti;
        for (int i = 0; i < nr; i++) {
            if (i == ti) {
                disp[nd++] = held;
            }
            disp[nd++] = reduced[i];
        }
        if (ti >= nr) {
            disp[nd++] = held;
        }
    } else {
        for (int i = 0; i < g_n_home; i++) {
            disp[nd++] = g_home[i];
        }
    }

    int rpp = rows_per_page(state->surface_w, state->surface_h);
    static Placed pl[MAX_HOME];
    int npages = pack_home_paged(disp, nd, rpp, pl);
    state->npages = npages;
    // Clamp the settled page in case the sequence shrank (removal / commit).
    if (state->page > npages - 1) {
        state->page = npages - 1;
    }
    if (state->page < 0) {
        state->page = 0;
    }

    // In a headless anim-frame test the springs are stepped then FROZEN (pinned)
    // so a still frame holds the motion; in that mode the live springs are not
    // driven and the landing state is held open.
    bool frozen = state->test_anim_frames > 0;

    // Landing settle: once the ghost's snap-back spring reaches the slot, drop the
    // landing state — the item's own keyed cell (same key) already sits at the
    // slot, so its normal cell (built below) takes over with no visible jump.
    if (state->landing && !frozen) {
        uint64_t base = entry_key(state->held_kind, state->held_ref);
        ZAnimated *lx = z_animated_keyed(app, KEY_X(base), 0.0f);
        ZAnimated *ly = z_animated_keyed(app, KEY_Y(base), 0.0f);
        if (!z_animated_active(lx) && !z_animated_active(ly)) {
            state->landing = false;
            special = false;
        }
    }

    // Headless anim seeding (ZELTO_HOME_ANIM_FRAMES): fabricate an in-flight state
    // then step the springs, so a single still frame catches motion. For a reflow
    // shot each non-held item is seeded at its PRE-reflow (g_home order) slot and
    // sprung toward its display slot; for a landing shot the ghost's shared cell
    // is seeded at the finger and sprung into the committed slot.
    if (!state->test_seeded && state->test_anim_frames > 0 &&
        state->surface_w > 1.0f) {
        state->test_seeded = true;
        static Placed hpl[MAX_HOME];
        pack_home_paged(g_home, g_n_home, rpp, hpl);  // pre-reflow home layout
        if (state->landing) {
            uint64_t base = entry_key(state->held_kind, state->held_ref);
            int cw, ch;
            entry_span(&held, &cw, &ch);
            float s = cell_side(state->surface_w);
            ZRect gr = {0};
            gr.w = cw * s + (cw - 1) * GRID_GAP;
            gr.h = ch * s + (ch - 1) * GRID_GAP;
            gr.x = state->ghost_x - gr.w / 2.0f;
            gr.y = state->ghost_y - gr.h / 2.0f;
            float fx, fy;
            cell_offset(state->surface_w, state->surface_h, gr, &fx, &fy);
            ZAnimated *cx = z_animated_keyed(app, KEY_X(base), fx);
            ZAnimated *cy = z_animated_keyed(app, KEY_Y(base), fy);
            z_animated_set(cx, fx);
            z_animated_set(cy, fy);
            int idx = home_find(state->held_kind, state->held_ref);
            if (idx >= 0) {
                ZRect r = cell_rect(state->surface_w, hpl[idx].col, hpl[idx].row,
                                    hpl[idx].cw, hpl[idx].ch);
                float ox, oy;
                cell_offset(state->surface_w, state->surface_h, r, &ox, &oy);
                z_animated_spring(cx, ox);
                z_animated_spring(cy, oy);
            }
        } else {
            for (int i = 0; i < nd; i++) {
                if (special &&
                    entry_eq(&disp[i], state->held_kind, state->held_ref)) {
                    continue;
                }
                uint64_t base = entry_key(disp[i].kind, disp[i].ref);
                ZRect dr = cell_rect(state->surface_w, pl[i].col, pl[i].row,
                                     pl[i].cw, pl[i].ch);
                float ox, oy;
                cell_offset(state->surface_w, state->surface_h, dr, &ox, &oy);
                float hx = ox, hy = oy;
                int hi = home_find(disp[i].kind, disp[i].ref);
                if (hi >= 0) {
                    ZRect hr = cell_rect(state->surface_w, hpl[hi].col,
                                         hpl[hi].row, hpl[hi].cw, hpl[hi].ch);
                    cell_offset(state->surface_w, state->surface_h, hr, &hx, &hy);
                }
                ZAnimated *cx = z_animated_keyed(app, KEY_X(base), hx);
                ZAnimated *cy = z_animated_keyed(app, KEY_Y(base), hy);
                z_animated_set(cx, hx);
                z_animated_set(cy, hy);
                z_animated_spring(cx, ox);
                z_animated_spring(cy, oy);
            }
        }
        for (int f = 0; f < state->test_anim_frames; f++) {
            z_anim_tick(app, 1.0f / 60.0f);
        }
        // Freeze the stepped springs so the still frame holds mid-flight (the
        // real-time frame loop would otherwise settle them before the shot).
        if (state->landing) {
            uint64_t base = entry_key(state->held_kind, state->held_ref);
            ZAnimated *cx = z_animated_keyed(app, KEY_X(base), 0.0f);
            ZAnimated *cy = z_animated_keyed(app, KEY_Y(base), 0.0f);
            z_animated_pin(cx, z_animated_get(cx));
            z_animated_pin(cy, z_animated_get(cy));
        } else {
            for (int i = 0; i < nd; i++) {
                if (special &&
                    entry_eq(&disp[i], state->held_kind, state->held_ref)) {
                    continue;
                }
                uint64_t base = entry_key(disp[i].kind, disp[i].ref);
                ZAnimated *cx = z_animated_keyed(app, KEY_X(base), 0.0f);
                ZAnimated *cy = z_animated_keyed(app, KEY_Y(base), 0.0f);
                z_animated_pin(cx, z_animated_get(cx));
                z_animated_pin(cy, z_animated_get(cy));
            }
        }
    }

    // The bento cells layer: ONE depth ZStack of PAGE subtrees laid side by side.
    // Each page subtree is a full-surface depth stack of its own cells (each at
    // its keyed animated LOCAL offset, springing toward its packed slot so a
    // reflow eases), and the page is translated horizontally by (p - page_v) *
    // surface_w so the whole carousel slides RIGIDLY on a flip — the page
    // translation is deliberately kept OUT of the per-cell springs so a page flip
    // never makes the icons lag behind their page. The special (held/landing)
    // item's slot is a faint placeholder; its content is drawn as the ghost on
    // top (in viewport coords, outside every page, so it stays finger-tracked
    // across a flip). Every item's keyed cell is requested each build regardless
    // of which page it is on, so an off-screen page keeps its springs' identity.
    float page_v = z_animated_get(state->page_anim);
    ZStackOpts pages_root = {.align = Z_ALIGN_LEADING};
    int prk = 0;
    for (int p = 0; p < npages && prk < Z_MAX_CHILDREN; p++) {
        ZStackOpts pcells = {.align = Z_ALIGN_LEADING};
        int ck = 0;
        for (int i = 0; i < nd && ck < Z_MAX_CHILDREN; i++) {
            if (pl[i].page != p) {
                continue;
            }
            ZRect r = cell_rect(state->surface_w, pl[i].col, pl[i].row, pl[i].cw,
                                pl[i].ch);
            if (special &&
                entry_eq(&disp[i], state->held_kind, state->held_ref)) {
                ZView ph = Frame(r.w, r.h,
                    CornerRadius(ICON_RADIUS,
                        Rect(.color = z_rgba(0x4a, 0xa3, 0xff, 0x45),
                             .radius = ICON_RADIUS)));
                pcells.children[ck++] = placed(state->surface_w,
                                               state->surface_h, r, ph);
            } else {
                uint64_t base = entry_key(disp[i].kind, disp[i].ref);
                float ox, oy;
                cell_offset(state->surface_w, state->surface_h, r, &ox, &oy);
                ZAnimated *cx = z_animated_keyed(app, KEY_X(base), ox);
                ZAnimated *cy = z_animated_keyed(app, KEY_Y(base), oy);
                if (!frozen) {
                    spring_to(cx, ox);  // ease toward the packed slot (reflow)
                    spring_to(cy, oy);
                }
                ZView cell = entry_view(app, state, &disp[i], r);
                pcells.children[ck++] = OffsetXYAnimated(cx, cy, cell);
            }
        }
        float px = ((float)p - page_v) * state->surface_w;
        pages_root.children[prk++] =
            OffsetXY(px, 0.0f, Fill(z_stack(Z_AXIS_DEPTH, &pcells)));
    }
    ZView cells_layer = Fill(z_stack(Z_AXIS_DEPTH, &pages_root));

    // The bottom bar: the page dots (when there is more than one page) over a
    // drawer handle normally, or a Done bar in rearrange mode.
    ZView handle_or_done = state->rearrange
        ? OnTap(exit_rearrange,
            Background(Z_COLOR_PRIMARY,
                CornerRadius(22.0f,
                    Padding(14.0f,
                        Foreground(Z_COLOR_TEXT_INV,
                            Font(Z_FONT_CALLOUT, Text("Done")))))))
        : OnTap(open_drawer,
            VStack(
                Rect(.color = Z_COLOR_TEXT_MUTED,
                     .width = 56, .height = 5, .radius = 3),
                TextShadow(Foreground(Z_COLOR_TEXT_INV,
                    Font(Z_FONT_CALLOUT, Text("^")))),
                TextShadow(Foreground(Z_COLOR_TEXT_MUTED,
                    Font(Z_FONT_CAPTION, Text("All apps")))),
                .spacing = 4, .align = Z_ALIGN_CENTER));
    ZStackOpts bstack = {.spacing = 12, .align = Z_ALIGN_CENTER,
                         .padding = state->rearrange ? 28.0f : 20.0f};
    int bk = 0;
    bstack.children[bk++] = Spacer();
    if (npages > 1) {
        bstack.children[bk++] = page_dots(npages, page_v);
    }
    bstack.children[bk++] = handle_or_done;
    ZView bottom = Fill(z_stack(Z_AXIS_VERTICAL, &bstack));

    // The lifted ghost: the held item's content, drawn on top via its shared
    // keyed x/y cell. While HELD the cell is locked to the finger; on release it
    // springs into the committed slot (landing) and, once settled, hands off to
    // the item's normal cell (same key) with no jump. Non-interactive.
    ZView ghost = NULL;
    if (state->rearrange && special) {
        int cw, ch;
        entry_span(&held, &cw, &ch);
        float s = cell_side(state->surface_w);
        ZRect gr = {0};
        gr.w = cw * s + (cw - 1) * GRID_GAP;
        gr.h = ch * s + (ch - 1) * GRID_GAP;
        uint64_t base = entry_key(state->held_kind, state->held_ref);
        ZAnimated *cx = z_animated_keyed(app, KEY_X(base), 0.0f);
        ZAnimated *cy = z_animated_keyed(app, KEY_Y(base), 0.0f);
        if (state->held) {
            // Track the finger: lock the shared cell to the finger-centred offset.
            gr.x = state->ghost_x - gr.w / 2.0f;
            gr.y = state->ghost_y - gr.h / 2.0f;
            float fx, fy;
            cell_offset(state->surface_w, state->surface_h, gr, &fx, &fy);
            z_animated_pin(cx, fx);  // pin to the finger (pan already invalidated)
            z_animated_pin(cy, fy);
        }
        // (landing: cx/cy are springing toward the slot; just read them.)
        ZView gc = (held.kind == HE_WIDGET)
            ? widget_cell_content(app, held.ref)
            : app_cell_content(&g_apps[held.ref]);
        ZView lifted = Frame(gr.w, gr.h,
            Shadow(Z_ELEV_3, ZStack(
                Fill(CornerRadius(ICON_RADIUS + 2.0f,
                    Rect(.color = Z_COLOR_ACCENT_DIM,
                         .radius = ICON_RADIUS + 2.0f))),
                Fill(gc),
                .align = Z_ALIGN_CENTER)));
        ghost = Fill(ZStack(
            OffsetXYAnimated(cx, cy, lifted),
            .align = Z_ALIGN_LEADING));
    }

    // Assemble the home surface (back-to-front), the whole thing one pan target.
    ZStackOpts hs = {.align = Z_ALIGN_CENTER};
    int hk = 0;
    hs.children[hk++] = wallpaper(state);
    if (state->rearrange) {
        hs.children[hk++] = raster_layer(state, rpp);
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
                Weight(Z_WEIGHT_SEMIBOLD, Foreground(Z_COLOR_TEXT_INV,
                    Font(Z_FONT_TITLE, Text("All apps")))),
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

    // Toast (P32): a spring-driven slide-up + fade on the SNAPPY token. The enter
    // spring is IDENTITY-keyed (not call-order) so it never disturbs the launcher's
    // other retained cells; it is driven toward 1 while the toast dwells and back to
    // 0 in its final ~0.3s, and the toast renders while any of it is still visible.
    ZView toast = NULL;
    ZAnimated *toast_enter = z_animated_keyed(app, 0x746F617374ULL /*'toast'*/, 0.0f);
    // Swipe-to-dismiss drag cell (P33), identity-keyed like the entrance so it never
    // disturbs call-order cells; cached on state so the pan handler can reach it.
    ZAnimated *toast_drag = z_animated_keyed(app, 0x746F61737464ULL /*'toastd'*/, 0.0f);
    state->toast_drag = toast_drag;
    // Freeze-frame hook: ZELTO_TOAST_ENTER=<0..1> parks a labelled toast on screen
    // at a pinned entrance progress for a still mid-transition shot.
    const char *tenv = getenv("ZELTO_TOAST_ENTER");
    if (tenv && tenv[0]) {
        if (!state->toast[0]) {
            snprintf(state->toast, sizeof(state->toast), "Added to Home");
        }
        state->toast_until = now_s() + 100.0;
    }
    double t_remaining = state->toast_until - now_s();
    bool toast_dwell = state->toast_until > 0.0 && t_remaining > 0.30;
    float t_target = toast_dwell ? 1.0f : 0.0f;
    if (z_animated_target(toast_enter) != t_target) {
        z_animated_spring_with(toast_enter, t_target, Z_SPRING_SNAPPY);
    }
    if (tenv && tenv[0]) {
        z_animated_pin(toast_enter, (float)atof(tenv));
    }
    // Swipe-dismiss freeze-frame: ZELTO_TOAST_DRAG=<px> pins a held downward drag.
    const char *tdenv = getenv("ZELTO_TOAST_DRAG");
    if (tdenv && tdenv[0]) {
        z_animated_pin(toast_drag, (float)atof(tdenv));
    }
    float te = z_animated_get(toast_enter);
    if ((state->toast_until > 0.0 && now_s() < state->toast_until) ||
        te > 0.01f || z_animated_active(toast_enter)) {
        z_invalidate(app);
        float tslide = (1.0f - te) * 28.0f;   // rises up into place from below
        float td = z_animated_get(toast_drag);   // >=0 dragged down toward dismissal
        float dprog = td / TOAST_DISMISS_DIST;
        if (dprog > 1.0f) {
            dprog = 1.0f;
        }
        // OnPan wraps only the toast box (not the full-screen filler), so a drag on
        // the toast dismisses it while drags elsewhere still reach the home gestures.
        ZView box = OnPan(on_toast_pan,
            Background(Z_COLOR_SURFACE_2,
                CornerRadius(12.0f,
                    Padding(14.0f,
                        Foreground(Z_COLOR_TEXT_INV,
                            Font(Z_FONT_BODY, Text("%s", state->toast)))))));
        toast = Opacity(te * (1.0f - dprog), OffsetXY(0.0f, tslide + td, Fill(VStack(
            Spacer(),
            box,
            .spacing = 0, .padding = 100, .align = Z_ALIGN_CENTER))));
    }

    // A moving/overlay layer forces a full repaint (partial path under-damages a
    // translated subtree). Rearrange + drag already force it in the handlers; a
    // carousel flip in flight (page_anim active, or a fractional page mid-drag)
    // translates every page subtree, so it needs the full repaint too.
    bool paging = z_animated_active(state->page_anim) ||
                  fabsf(page_v - floorf(page_v + 0.5f)) > 0.001f;
    bool launching = state->launch_ref >= 0 &&
                     z_animated_get(state->launch_anim) > 0.001f;
    if (drawer_v > 0.001f || state->rearrange || toast || paging || launching) {
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
