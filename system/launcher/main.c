// Zelto System UI — app launcher / home screen.
//
// A normal xdg_toplevel app (app_id "os.zelto.launcher") that the compositor
// keeps at the back of the app stack and sizes to the usable area below the
// status bar (and above the bottom nav bar). It is a phone-style home screen
// split the way a real phone does:
//
//   - a HOME surface: a drawn/photo wallpaper and a directly-manipulable BENTO
//     GRID holding BOTH app icons and widgets in one ordered sequence; and
//   - an APP LIBRARY: the *complete* installed-app list, as the LAST PAGE of the
//     same carousel.
//
// THE APP LIBRARY (P40 stage 2). This used to be a swipe-up DRAWER — an opaque
// panel that slid over the home from below. iOS has no drawer, and deleting one
// is not as simple as removing the surface: the curate menu can REMOVE an app
// from home, and with no second list that app is orphaned with no way back. So
// the drawer's job moves to a page at the END of the carousel, reached by the
// SAME horizontal swipe that flips home pages — one gesture for "move sideways
// through my apps" instead of a page swipe and a separate up-swipe that has to be
// told apart from it (the old on_home_pan locked an axis to do exactly that).
//
// The Library page is SYNTHETIC: it is not in the pack, not in home.layout, and
// cannot be reordered into. Pages are `packed + nlib`, where the packed pages
// come from the layout and the Library pages come from the installed-app list;
// rearrange's cross-page edge-dwell still caps at the last PACKED page, so an
// icon can never be dragged into the Library. Long-press an app there to add it
// back to Home — the inverse of the curate menu's Remove.
//
// SEARCH. The Library's field is a real TextField, so focusing it raises the
// system keyboard (text-input-v3 -> input-method-v2). The compositor sizes the
// home window to the FULL output rather than the usable area (it draws behind the
// bars, see zcomp layer.c), so the keyboard's exclusive zone does NOT shrink it:
// the page insets its own bottom by KBD_H while the field is focused, and hides
// the dots + dock, which the keyboard would otherwise cover. Leaving the page
// blurs the field, or the keyboard would stay up over a page with no field on it.
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
#include <ctype.h>
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
#include "common/exec_cmd.h"
#include "common/safe_areas.h"
#include "common/wallpaper.h"

#define MANIFEST_DIR "/usr/share/zelto/apps"
#define MAX_APPS 32
// The first-run home seed. This used to be EVERY installed app (16), because with
// only the drawer behind it a short seed left the grid looking like an unfinished
// device — one row of icons over a screen of bare wallpaper. The App Library
// changes that calculation: seeding everything makes the Library an exact copy of
// the home pages, so it has nothing to be FOR until you start removing things. A
// small seed gives both surfaces a job from the first boot — home is the set you
// chose, the Library is everything you have — and the widgets keep page 1 full.
// The dock is drawn from the app list, not from this, so the first four apps stay
// reachable from every page regardless.
#define DEFAULT_FAVS 6   // cap: the seed stops here; the rest live in the Library

// One installed app, parsed from a .app manifest. `id` is the manifest basename
// (minus ".app"), e.g. "os.zelto.cards" — the stable key used in the layout.
typedef struct AppEntry {
    char id[96];
    char name[64];
    char subtitle[96];
    char exec_path[256];   // manifest exec= — a command, args and all
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

// The clock/date card: the weekday, then the time as the hero, then the date.
// Ticks once a second. A 2x2 card is a lot of screen, so it carries three lines
// of real hierarchy rather than one number floating in a box.
static ZView w_clock(ZApp *app, void *state) {
    (void)app;
    (void)state;
    char hhmm[8] = "--:--";
    char day[16] = "";
    char date[32] = "";
    time_t t = time(NULL);
    struct tm tmv;
    // LOCAL time, not UTC — the home widget and the status-bar clock have to
    // agree, and both have to agree with the wall clock the user is holding.
    if (localtime_r(&t, &tmv)) {
        snprintf(hhmm, sizeof(hhmm), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        strftime(day, sizeof(day), "%A", &tmv);
        strftime(date, sizeof(date), "%d %B", &tmv);
    }
    // THREE lines inside a ONE-ROW cell (widgets are 2 cols x 1 row, so ~158px
    // tall). That is the tightest type budget on the home screen, and it is the
    // one place the P43 rescale overflowed: at Large Title the time alone stood
    // 95px and pushed the date out through the bottom of the card. A widget is a
    // dense surface — it earns its place by saying three things at a glance, not
    // by saying one of them loudly — so it types a step down from what the same
    // information would take on a full screen.
    return VStack(
        Weight(Z_WEIGHT_SEMIBOLD,
            Foreground(Z_COLOR_TEXT_MUTED,
                Font(Z_FONT_CAPTION, Text("%s", day)))),
        Weight(Z_WEIGHT_BOLD,
            Foreground(Z_COLOR_TEXT, Font(Z_FONT_TITLE2, Text("%s", hhmm)))),
        Foreground(Z_COLOR_TEXT_MUTED, Font(Z_FONT_CAPTION, Text("%s", date))),
        .spacing = 1, .align = Z_ALIGN_LEADING);
}

// A horizontal meter: a filled run and the empty remainder, side by side. The
// battery widget's reason to be 2 cells wide — a number alone would fit in a
// caption.
//
// Built as two Rects in a row rather than a fill stacked over a track: Fill()
// inside a ZStack expands to the PARENT's inner box, so a "track" built that way
// does not size to the meter's Frame — it inflates the whole card's content box
// and shoves the content out through the top. Two siblings need no overlap.
static ZView meter(float frac, ZColor fill, float width) {
    if (frac < 0.04f) {
        frac = 0.04f;   // always show a sliver, so the meter reads as a meter
    }
    if (frac > 1.0f) {
        frac = 1.0f;
    }
    float on = width * frac, off = width - on;
    ZStackOpts row = {.spacing = 3.0f, .align = Z_ALIGN_CENTER};
    int k = 0;
    row.children[k++] = Frame(on, 8.0f, Rect(.color = fill, .radius = 4.0f));
    if (off > 2.0f) {
        row.children[k++] =
            Frame(off, 8.0f, Rect(.color = Z_COLOR_SURFACE_3, .radius = 4.0f));
    }
    return z_stack(Z_AXIS_HORIZONTAL, &row);
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
        Weight(Z_WEIGHT_BOLD,
            Foreground(pc, Font(Z_FONT_TITLE, Text("%d%%", pct)))),
        meter((float)pct / 100.0f, pc, 132.0f),
        Foreground(Z_COLOR_TEXT_MUTED,
            Font(Z_FONT_FOOTNOTE,
                 Text("%s%s", charging ? "Charging \xc2\xb7 " : "", net))),
        .spacing = 6, .align = Z_ALIGN_LEADING);
}

// The notifications glance: the live count of stored notifications.
static ZView w_notifs(ZApp *app, void *state) {
    (void)app;
    (void)state;
    int n = (int)z_setting_get_int("sys.notif_count", 0);
    // Zero is the common case, and "0 notifications" is a worse thing to read than
    // "All clear" — an empty state should say what is true, not print a zero.
    if (n <= 0) {
        static const float tick[] = {0.18f, 0.54f, 0.42f, 0.80f, 0.84f, 0.22f};
        return VStack(
            Frame(30.0f, 30.0f,
                Stroke(.points = tick, .count = 3, .thickness = 3.5f,
                       .color = Z_COLOR_TEXT_MUTED)),
            Weight(Z_WEIGHT_SEMIBOLD,
                Foreground(Z_COLOR_TEXT, Font(Z_FONT_CALLOUT, Text("All clear")))),
            .spacing = 8, .align = Z_ALIGN_LEADING);
    }
    return VStack(
        Weight(Z_WEIGHT_BOLD,
            Foreground(Z_COLOR_TEXT, Font(Z_FONT_LARGE_TITLE, Text("%d", n)))),
        Foreground(Z_COLOR_TEXT_MUTED,
            Font(Z_FONT_FOOTNOTE,
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

// `title` names the widget in the curate menu; it is NOT drawn on the card. A
// clock that says "Clock" over the time, a battery that says "Status" over 99%,
// spends the card's best line telling you what you can already see — and on a 2x2
// it leaves a hole in the middle. The content identifies the widget (this is what
// every widget on a phone does), so the card is content-led.
static const WidgetDef g_widget_defs[] = {
    {"clock",   "Clock",         w_clock,   1000, 2, 1},
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
        // Echo what was READ before parsing it. The count alone cannot tell a
        // loaded arrangement apart from a freshly seeded one of the same size,
        // which is exactly the distinction the reboot-persistence harness
        // (meta/run-qemu.sh HOME_TEST=1) has to make from the serial log.
        fprintf(stderr, "launcher: home.layout read: %s\n", buf);
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
// SAFE AREAS. The home window is the full output (zcomp gives the launcher the
// whole screen, not the usable area) so the wallpaper runs edge to edge under the
// transparent status bar and behind the nav bar — as it does on a phone. Nothing
// the launcher draws may land under either bar, so the grid starts below the status
// bar's safe area and
// the bottom reserve covers the dock AND the nav bar's height.

// A FIXED vertical gap. Note it cannot be Frame(w, h, Spacer()): a Spacer carries
// grow, and Frame only sets a size — the node keeps eating every spare pixel in
// the stack, so a "72px" gap built that way pushes everything above it off-centre.
// An empty (fully transparent) Rect has no grow, so it stays the size it is given.
static ZView vgap(float h) {
    return Frame(1.0f, h, Rect(.color = z_rgba(0, 0, 0, 0)));
}

#define GRID_COLS 4
#define GRID_GAP 16.0f
#define GRID_PAD 20.0f
#define GRID_TOP ((float)ZELTO_BAR_H + 20.0f)
#define MAX_ROWS 20              // per-page occupancy height cap
#define MAX_PAGES 8              // carousel cap

// A Text's height is its font's ascent + descent, which is NOT a compile-time
// constant — it comes from the face. This file estimated it for one phase, as
// `LINE_H(font) = font * 1.31`, and P46 deleted the estimate: the toolkit knows
// the real number and a builder can ask for it (z_line_height, zelto/ui.h), so
// the reserve below is now the same measurement the Text node itself gets rather
// than a ratio fitted to one face at one size. See the note over z_line_height
// for what the ratio actually does across the scale — it is not constant, and
// the direction the estimate promised never to fail in is the one it failed in.

// The bottom bar, DERIVED from what it holds rather than declared.
//
// It was `208.0f + ZELTO_HOMEBAR_H` — a number naming a sum it was not computed
// from, which is the exact shape of the `KBD_H 300` that P44 found hiding 36
// units of slack. Measured against its parts this one carried ~18: the bar is a
// vertical stack of page dots, the dock plate and a home-indicator gap, padded
// and spaced. Now it says so, so a taller dock icon cannot silently overflow the
// space reserved for it.
#define BAR_PAD 14.0f            // the bottom bar's own padding
#define BAR_GAP 12.0f            // between dots, dock and the indicator gap
#define DOTS_H 10.0f             // the tallest page indicator (the Library quad)
#define DOCK_PAD 14.0f           // the dock plate's inset around its icons
#define DOCK_GAP 18.0f           // between dock icons
// The dock's icon is smaller than a home tile (see the dock section below); it
// lives up here because BOTTOM_RESERVE is derived from it and a macro is
// expanded where it is USED, which is above the dock's own code.
#define DOCK_ICON 88.0f
#define DOCK_PLATE_H (DOCK_ICON + 2.0f * DOCK_PAD)
#define BOTTOM_RESERVE (2.0f * BAR_PAD + 3.0f * BAR_GAP + DOTS_H \
                        + DOCK_PLATE_H + (float)ZELTO_HOMEBAR_H)
#define ICON_SIZE 104.0f
// The corner is a FRACTION of the icon (Z_RADIUS_ICON — Apple's icon-grid
// proportion), not a fixed px, so the tile keeps its shape at every size it is
// drawn: the home grid, the drawer, the task switcher, an empty slot's raster.
#define ICON_RADIUS (ICON_SIZE * Z_RADIUS_ICON)

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
    ZAnimated *ghost_anim;    // lifted ghost's x (cell 0) — the ONLY reorder anim
    ZAnimated *page_anim;     // carousel scroll position, in page units (cell 1)
    float surface_w, surface_h;

    // Horizontal pager.
    int page;                 // current settled page (integer)
    int npages;               // PACKED page count from the last build (home only)
    int nlib;                 // App Library pages after them (>= 1)
    int pan_axis;             // gesture axis lock: 0 undecided, 1 horiz, 2 vert
    float page_base;          // page_anim value at pan begin (for horiz drags)
    int dwell_edge;           // cross-page edge-gutter dwell: -1 none, 0 L, 1 R

    // App Library search (P40 stage 2). The query filters the Library grid; the
    // field is a real TextField, so focusing it raises the system keyboard and the
    // page has to inset for it (see the file header).
    ZTextField search;

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
        char cmd[256];
        z_exec_cmd(e->exec_path, cmd, sizeof(cmd));
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

// App Library long-press: add this app to the home sequence. This is the inverse
// of the curate menu's Remove, and the reason the Library exists at all — without
// it, removing an app from home would orphan it with no way back.
static void on_library_add(ZApp *app, void *state, void *data, float x,
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

// The last page you can swipe to: the packed home pages plus the App Library
// pages that follow them.
static int last_page(const LauncherState *s) {
    int total = s->npages + (s->nlib > 0 ? s->nlib : 1);
    return total > 0 ? total - 1 : 0;
}

// Spring the carousel to page `p` (clamped) and record it as the settled page.
static void snap_to_page(ZApp *app, LauncherState *s, int p) {
    int maxp = last_page(s);
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

    if (!s->page_anim) {
        return;
    }
    float w = s->surface_w > 1.0f ? s->surface_w : 1.0f;
    if (e->phase == Z_PAN_BEGIN) {
        s->pan_axis = 0;
        s->page_base = z_animated_get(s->page_anim);
    } else if (e->phase == Z_PAN_CHANGED) {
        // Lock the axis on the first real motion. Only the horizontal one does
        // anything now — the up-swipe that used to pull the drawer up is gone with
        // it, and the bottom edge belongs to the home indicator (a separate layer
        // surface with its own 34px strip), so a vertical drag here is inert. The
        // lock stays because it is what keeps a slightly-diagonal vertical drag
        // from nudging the carousel a page sideways.
        if (s->pan_axis == 0) {
            float ax = fabsf(e->translation_x), ay = fabsf(e->translation_y);
            if (ax > 8.0f && ax >= ay) {
                s->pan_axis = 1;
            } else if (ay > 8.0f) {
                s->pan_axis = 2;
            }
        }
        if (s->pan_axis == 1) {
            float p = s->page_base - e->translation_x / w;
            float maxp = (float)last_page(s);
            if (p < 0.0f) p = 0.0f;
            if (p > maxp) p = maxp;
            z_animated_set(s->page_anim, p);
            z_full_repaint(app);
            z_invalidate(app);
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
        }
    }
}

// --- wallpaper ------------------------------------------------------------
#define WALL_BANDS 10
#define SCRIM_BANDS 8

// A gentle vignette over the BOTTOM of the wallpaper only, so the dock's icons and
// the nav bar's marks survive whatever picture the user chose.
//
// The top scrim is gone. It peaked at alpha 130 and reached a third of the way
// down the screen — and it was buying nothing, because the status bar is its own
// opaque surface and needs no help. What it actually did was darken the wallpaper
// exactly where the widgets sit, so a dark translucent widget card had a dark
// ground behind it and could not read as a card at all. A material needs
// something behind it to be a material AGAINST.
static ZView wallpaper_scrim(void) {
    ZStackOpts col = {0};
    for (int i = 0; i < SCRIM_BANDS; i++) {
        float t = (float)i / (float)(SCRIM_BANDS - 1);
        float e = t < 0.62f ? 0.0f : (t - 0.62f) / 0.38f;   // bottom third only
        uint8_t a = (uint8_t)(e * e * 104.0f);
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

// An app icon: the icon asset IS the tile (P37 — each app ships a full-bleed
// 512x512 squircle with its own gradient and mark), so the launcher draws it
// edge-to-edge under the squircle mask and casts the shadow that lifts it off the
// wallpaper. It no longer paints a coloured square behind the art: doing that on
// top of an icon that already has a tile produced the letter-on-a-swatch look.
//
// An app with NO icon still gets the coloured tile + monogram, which is now what
// that fallback is FOR (an app the system has never seen — a side-loaded package
// mid-install), so it is visibly not a designed icon.
static ZView app_icon_sized(const AppEntry *e, float size) {
    bool own = e->icon_path[0] && z_image_loads(e->icon_path);
    // The corner is a FRACTION of the tile (Apple's icon grid), so the shape holds
    // whether this is a 104px home tile or an 88px dock tile.
    float radius = size * Z_RADIUS_ICON;
    if (own) {
        return Shadow(Z_ELEV_1,
            Frame(size, size, CornerRadius(radius, Image(e->icon_path))));
    }
    return Shadow(Z_ELEV_1, Frame(size, size,
        Background(e->color,
            CornerRadius(radius,
                ZStack(app_monogram(e), .align = Z_ALIGN_CENTER)))));
}

static ZView app_icon_tile(const AppEntry *e) {
    return app_icon_sized(e, ICON_SIZE);
}

static ZView app_cell_content(ZApp *app, const AppEntry *e) {
    // The caption is 11pt Medium — the smallest step in the scale, because an app
    // label is recognised, not read: the icon is what identifies the app and the
    // word only disambiguates it. A shadow keeps it legible over a bright
    // wallpaper without a plate behind it.
    //
    // ELLIPSIZED, as of P50, and the audit is what said so. A grid cell is 158
    // units wide and an app NAME is not this screen's string to choose — it comes
    // out of a manifest. At the largest text size "andemu Demo" measures 206 and
    // a bare Text paints all of it, straight through the cell and across its
    // neighbour, because Text never wraps and never truncates. This is the
    // identifier-in-a-fixed-row case EllipsizeText exists for: the head of the
    // name plus a mark saying there is more, which is what every home screen
    // does. (Not WrapText: a two-line label would shift every icon below it.)
    return VStack(
        app_icon_tile(e),
        TextShadow(Weight(Z_WEIGHT_MEDIUM,
            EllipsizeText(app, e->name, .width = cell_side(z_app_width(app)),
                          .size = Z_FONT_CAPTION2, .weight = Z_WEIGHT_MEDIUM,
                          .color = Z_COLOR_TEXT))),
        .spacing = 7, .align = Z_ALIGN_CENTER);
}

// --- the dock -------------------------------------------------------------
// The four apps that are always one tap away, pinned across every page. This is
// the single strongest piece of phone-shaped muscle memory there is (Jakob's
// Law), and it is also the best real estate on the device — the bottom of the
// screen is where the thumb already is (Fitts). Icons only: a dock is recognised
// by shape and position, and captions here would just add noise at the busiest
// edge of the screen.
//
// The dock rides on its own MATERIAL — a translucent, rounded plate the wallpaper
// shows through — which is what separates it from the grid above without drawing
// a line.
// DOCK_ICON, DOCK_PAD and DOCK_GAP are defined with the grid metrics at the top
// of the file, because BOTTOM_RESERVE is derived from them and is spent above.
#define DOCK_MAX 4

// Which apps are in the dock. Persisted as a CSV of app ids (home.dock); with no
// stored preference the first DOCK_MAX apps (the list is name-sorted) are used, so
// a fresh device still has a populated dock rather than an empty plate.
static int g_dock[DOCK_MAX];
static int g_n_dock;
static bool g_dock_loaded;

static void load_dock(void) {
    if (g_dock_loaded) {
        return;
    }
    g_dock_loaded = true;
    g_n_dock = 0;
    const char *pref = z_prefs_get_str("home.dock", NULL);
    if (pref && pref[0]) {
        char csv[512];
        snprintf(csv, sizeof(csv), "%s", pref);   // strtok_r writes into it
        char *save = NULL;
        for (char *tok = strtok_r(csv, ",", &save);
             tok && g_n_dock < DOCK_MAX; tok = strtok_r(NULL, ",", &save)) {
            int ai = find_app(tok);
            if (ai >= 0) {
                g_dock[g_n_dock++] = ai;
            }
        }
    }
    if (g_n_dock == 0) {
        for (int i = 0; i < g_n_apps && g_n_dock < DOCK_MAX; i++) {
            g_dock[g_n_dock++] = i;
        }
    }
}

static ZView dock_view(void) {
    load_dock();
    if (g_n_dock <= 0) {
        return Spacer();
    }
    // From the same constants BOTTOM_RESERVE is derived from — if these were a
    // second copy, the reserve would go stale the moment the plate was retuned.
    const float pad = DOCK_PAD, gap = DOCK_GAP;
    ZStackOpts row = {.spacing = gap, .align = Z_ALIGN_CENTER, .padding = pad};
    int k = 0;
    for (int i = 0; i < g_n_dock && k < Z_MAX_CHILDREN - 1; i++) {
        const AppEntry *e = &g_apps[g_dock[i]];
        row.children[k++] = OnTapData(launch_app, (void *)e,
                                      app_icon_sized(e, DOCK_ICON));
    }
    // The plate hugs its icons: an HStack fills the width it is given, so without
    // an explicit Frame the material stretches edge to edge and stops reading as a
    // dock. Flanking Spacers then centre the plate in the bottom bar.
    float plate_w = (float)g_n_dock * DOCK_ICON +
                    (float)(g_n_dock - 1) * gap + 2.0f * pad;
    float plate_h = DOCK_ICON + 2.0f * pad;
    // Same material + hairline construction as a widget card (see z_widget), so the
    // dock and the widgets are visibly the same kind of object.
    ZView fill = Background(Z_COLOR_MATERIAL_THIN,
        CornerRadius(Z_RADIUS_SHEET - 1.0f,
            z_stack(Z_AXIS_HORIZONTAL, &row)));
    ZView plate = Shadow(Z_ELEV_2,
        Frame(plate_w, plate_h,
            Background(Z_COLOR_MATERIAL_EDGE,
                CornerRadius(Z_RADIUS_SHEET,
                    ZStack(Fill(fill), .padding = 1.0f)))));
    return HStack(Spacer(), plate, Spacer(), .align = Z_ALIGN_CENTER);
}

static ZView widget_cell_content(ZApp *app, int di) {
    const WidgetDef *d = &g_widget_defs[di];
    return Widget(app, .body = d->build, .refresh_ms = d->refresh_ms);
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
        : app_cell_content(app, &g_apps[e->ref]);

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

// --- the App Library ------------------------------------------------------
// The last page(s) of the carousel: every installed app, name-sorted (g_apps is
// already sorted), over a search field. It reuses the home's cell metrics and
// app_cell_content verbatim — an app icon must be the same object here as it is
// on home, or paging into the Library reads as arriving in a different program.
// The title + search field above the grid — DERIVED, because the literal it
// replaces was measurably wrong.
//
// It was 116, and P43 roughly doubled the type scale (Z_FONT_TITLE 28 -> 51)
// without touching it. Measured off a re-shot frame: the grid's first icon row
// starts at y=278 with GRID_TOP at 101, so the title, the field and the three
// gaps really consume 177 — the reserve was SIXTY-ONE UNITS SHORT. lib_rows()
// spends this on `avail`, so an under-reservation means it can fit one more row
// than there is room for and the last row runs under the dock. It is invisible
// today only because there are twelve apps and the row count is not the binding
// constraint; it becomes visible the moment a device has enough apps.
//
// The same rescale left a second copy of the same mistake below: the non-first
// Library pages hold the grid down with a bare `vgap(52)` meant to match the
// search field's height, and the field now stands ~62. Both now come from the
// parts, so the type scale can move again without dragging either out of true.
// Functions rather than macros, because the parts are no longer all compile-time
// constants: the line heights come off the FACE, which only exists once the app
// does. Same derivation, same parts, one fewer estimate.
#define LIB_FIELD_PAD 12.0f      // z_text_field's own padding (sdk/src/view.c)
static float lib_field_h(ZApp *app) {
    return 2.0f * LIB_FIELD_PAD + z_line_height(app, Z_FONT_BODY);
}
static float lib_top(ZApp *app) {
    return GRID_GAP + z_line_height(app, Z_FONT_TITLE) + GRID_GAP
           + lib_field_h(app) + GRID_GAP;
}

// Case-insensitive substring test. strcasestr is a GNU extension and this file
// is built -Wpedantic, so the scan is written out.
static bool ci_contains(const char *hay, const char *needle) {
    if (!needle || !needle[0]) {
        return true;
    }
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            a++;
            b++;
        }
        if (!*b) {
            return true;
        }
    }
    return false;
}

// Does this app match the live query? Name first, then the id — searching
// "zelto" or "os.zelto.cards" should find things too, and an id is the only
// handle an app with a generic display name has.
static bool lib_matches(const AppEntry *e, const char *q) {
    return ci_contains(e->name, q) || ci_contains(e->id, q);
}

// Rows one Library page holds. While the search field is focused the system
// keyboard covers the bottom KBD_H of the screen and the compositor does NOT
// shrink this window (the home is sized to the full output), so the page reserves
// that height itself instead of the dots + dock it hides.
static int lib_rows(ZApp *app, float sw, float sh, bool searching) {
    float cell = cell_side(sw);
    float reserve = searching ? ((float)ZELTO_KBD_H + 24.0f) : BOTTOM_RESERVE;
    float top = lib_top(app);
    // Say the derived number out loud, once. A reserve is the one kind of metric
    // whose correctness is a comparison — header reserved against header drawn —
    // and until P46 the only way to make that comparison was to measure a
    // screenshot by hand, which is how it stayed 61 units short for two phases.
    // Now the reserve and the frame can be read off the SAME boot log
    // (ZELTO_PROBE_TAPS prints where the grid's first row actually landed).
    static bool said;
    if (!said) {
        said = true;
        fprintf(stderr,
                "[launcher] library header: top=%.1f = gap %.0f + title %.1f + "
                "gap %.0f + field %.1f + gap %.0f\n",
                top, GRID_GAP, z_line_height(app, Z_FONT_TITLE), GRID_GAP,
                lib_field_h(app), GRID_GAP);
        fflush(stderr);
    }
    float avail = sh - GRID_TOP - top - reserve;
    int r = (int)floorf((avail + GRID_GAP) / (cell + GRID_GAP));
    if (r < 1) {
        r = 1;
    }
    if (r > MAX_ROWS) {
        r = MAX_ROWS;
    }
    return r;
}

// Collect the matching app indices into `out`; returns how many.
static int lib_collect(const char *q, int *out) {
    int n = 0;
    for (int i = 0; i < g_n_apps && n < MAX_APPS; i++) {
        if (lib_matches(&g_apps[i], q)) {
            out[n++] = i;
        }
    }
    return n;
}

// The live query, or NULL when the field is empty.
static const char *lib_query(const LauncherState *s) {
    return s->search.len > 0 ? s->search.text : NULL;
}

// How many Library pages the current (possibly filtered) app list needs. Always
// at least one: an empty result set still has a page to say so on.
static int lib_page_count(ZApp *app, const LauncherState *s, bool searching) {
    int idx[MAX_APPS];
    int n = lib_collect(lib_query(s), idx);
    int per = GRID_COLS * lib_rows(app, s->surface_w, s->surface_h, searching);
    int pages = per > 0 ? (n + per - 1) / per : 1;
    return pages < 1 ? 1 : pages;
}

// One Library page. The title sits on every page (so a second page is not a
// mystery grid); the FIELD is built only on the first — two live TextField nodes
// bound to one ZTextField would be two carets on one buffer.
static ZView library_page_view(ZApp *app, LauncherState *s, int lp,
                               bool searching) {
    int idx[MAX_APPS];
    int n = lib_collect(lib_query(s), idx);
    int rows = lib_rows(app, s->surface_w, s->surface_h, searching);
    int per = GRID_COLS * rows;
    int start = lp * per;

    ZStackOpts col = {.spacing = GRID_GAP, .align = Z_ALIGN_LEADING};
    int k = 0;
    col.children[k++] = vgap(GRID_TOP - GRID_PAD);
    col.children[k++] = Weight(Z_WEIGHT_BOLD,
        TextShadow(Foreground(Z_COLOR_TEXT,
            Font(Z_FONT_TITLE, Text("App Library")))));
    if (lp == 0) {
        col.children[k++] = Frame(s->surface_w - 2.0f * GRID_PAD, 0.0f,
            TextField(app, &s->search, "Search"));
    } else {
        // Hold the grid at the same height as page 1, which means matching the
        // SEARCH FIELD's height — so it is the field's height, not a literal that
        // was right when Body was 17pt-as-pixels and is 10 units out now.
        col.children[k++] = vgap(lib_field_h(app));
    }

    if (n == 0) {
        col.children[k++] = vgap(24.0f);
        col.children[k++] = Foreground(Z_COLOR_TEXT_MUTED,
            Font(Z_FONT_CALLOUT, Text("No apps match \"%s\"", s->search.text)));
    }
    for (int r = 0; r < rows && k < Z_MAX_CHILDREN - 3; r++) {
        int base = start + r * GRID_COLS;
        if (base >= n) {
            break;
        }
        // FIXED-width cells, not Grow(1): a grow-weighted row distributes its
        // spare space among however many children it has, so a filtered row of
        // two results would sit at different x's than the four-up rows above it —
        // the icons would visibly shift sideways as you type. A fixed cell (the
        // home grid's own cell_side, so 4 of them plus the gaps come to exactly
        // the padded width) keeps every column on the same axis at every count.
        float cell = cell_side(s->surface_w);
        ZStackOpts row = {.spacing = GRID_GAP, .align = Z_ALIGN_LEADING};
        for (int c = 0; c < GRID_COLS; c++) {
            int j = base + c;
            if (j < n) {
                const AppEntry *e = &g_apps[idx[j]];
                // Long-press ADDS to Home (the inverse of curate's Remove); tap
                // launches. No rearrange here — the Library's order is the app
                // list's, and there is nothing to arrange.
                row.children[c] = Frame(cell, 0.0f,
                    OnLongPress(on_library_add, (void *)e,
                        OnTapData(launch_app, (void *)e,
                            app_cell_content(app, e))));
            } else {
                row.children[c] = Frame(cell, 1.0f,
                    Rect(.color = z_rgba(0, 0, 0, 0)));
            }
        }
        col.children[k++] = z_stack(Z_AXIS_HORIZONTAL, &row);
    }
    col.children[k++] = Spacer();
    // The bottom reserve: the keyboard while searching, else the dots + dock.
    col.children[k++] = vgap(searching ? (float)ZELTO_KBD_H : BOTTOM_RESERVE);

    ZStackOpts opts = col;
    opts.padding = GRID_PAD;
    return Fill(z_stack(Z_AXIS_VERTICAL, &opts));
}

// The carousel page indicator: one dot per HOME page, then a distinct four-square
// mark for the App Library. The Library is not another home page — it is the end
// of the road — so it gets its own mark rather than an n+1'th dot, which is where
// iOS puts it too. `page_v` is the live (fractional) scroll position; the nearest
// page reads as active mid-flip.
static ZView page_dots(int npages, int nlib, float page_v) {
    int active = (int)floorf(page_v + 0.5f);
    // Flanking Spacers centre the dots: a bare HStack expands to the full width
    // and would otherwise pack the dots at the leading edge.
    ZStackOpts row = {.spacing = 9, .align = Z_ALIGN_CENTER};
    int k = 0;
    row.children[k++] = Spacer();
    for (int i = 0; i < npages && k < Z_MAX_CHILDREN - 3; i++) {
        bool on = i == active;
        float d = on ? 9.0f : 7.0f;
        row.children[k++] = Frame(d, d,
            CornerRadius(d / 2.0f,
                Rect(.color = on ? Z_COLOR_TEXT_INV : Z_COLOR_TEXT_MUTED,
                     .radius = d / 2.0f)));
    }
    if (nlib > 0) {
        bool on = active >= npages;
        ZColor c = on ? Z_COLOR_TEXT_INV : Z_COLOR_TEXT_MUTED;
        float q = on ? 4.0f : 3.0f;
        ZView quad = VStack(
            HStack(Frame(q, q, Rect(.color = c, .radius = 1.0f)),
                   Frame(q, q, Rect(.color = c, .radius = 1.0f)),
                   .spacing = 2, .align = Z_ALIGN_CENTER),
            HStack(Frame(q, q, Rect(.color = c, .radius = 1.0f)),
                   Frame(q, q, Rect(.color = c, .radius = 1.0f)),
                   .spacing = 2, .align = Z_ALIGN_CENTER),
            .spacing = 2, .align = Z_ALIGN_CENTER);
        row.children[k++] = quad;
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
    // stable: ghost_anim (cell 0, the only reorder anim), page_anim (cell 1, the
    // carousel scroll position in page units). These call-order cells sit in a
    // separate namespace from the per-item z_animated_keyed cells (keyed by
    // identity), so changing this list cannot disturb any item's spring — the
    // drawer_anim that used to be cell 0 went out with the drawer.
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
        // ZELTO_HOME_SEARCH=<text> seeds the App Library's query (and focuses the
        // field, so the shot also shows the keyboard-inset layout) without driving
        // a tap + keystrokes. Pair it with ZELTO_HOME_PAGE=<last> to land on the
        // Library page.
        const char *sq = getenv("ZELTO_HOME_SEARCH");
        if (sq && sq[0]) {
            snprintf(state->search.text, sizeof(state->search.text), "%s", sq);
            state->search.len = (int)strlen(state->search.text);
            state->search.caret = state->search.len;
            state->search.anchor = state->search.len;
            z_app_focus_field(app, &state->search);
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
    // The App Library pages follow the packed home pages. Its page count depends
    // on whether the search field is focused (the keyboard eats rows), so resolve
    // focus first — and if focus is up but the carousel has left the Library,
    // blur, or the keyboard stays raised over a page with no field on it.
    bool searching = z_app_field_active(app, &state->search);
    if (searching && z_animated_get(state->page_anim) < (float)npages - 0.5f) {
        z_app_focus_field(app, NULL);
        searching = false;
    }
    state->nlib = lib_page_count(app, state, searching);
    // Clamp the settled page in case the sequence (or the filtered list) shrank.
    if (state->page > last_page(state)) {
        state->page = last_page(state);
    }
    if (state->page < 0) {
        state->page = 0;
    }
    // ...and drag the live carousel position back with it, or a page count that
    // shrank under a settled scroll (a removal, a search that filtered the Library
    // down to one page, ZELTO_HOME_PAGE past the end) leaves the surface parked on
    // empty space with every page translated off-screen.
    if (z_animated_get(state->page_anim) > (float)last_page(state)) {
        z_animated_set(state->page_anim, (float)state->page);
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
    // The App Library pages ride the same carousel translation, so paging into
    // them is the same rigid slide as flipping between two home pages — there is
    // no separate surface and no second gesture.
    for (int lp = 0; lp < state->nlib && prk < Z_MAX_CHILDREN - 1; lp++) {
        float px = ((float)(npages + lp) - page_v) * state->surface_w;
        pages_root.children[prk++] = OffsetXY(px, 0.0f,
            library_page_view(app, state, lp, searching));
    }
    ZView cells_layer = Fill(z_stack(Z_AXIS_DEPTH, &pages_root));

    // The bottom bar: page dots over the DOCK — or a Done bar while rearranging.
    // The grab handle above the dock is gone with the drawer it opened: there is
    // no up-swipe to advertise any more, and the App Library is reached by the
    // same sideways swipe as every other page, which needs no handle.
    ZView bottom_content = state->rearrange
        ? OnTap(exit_rearrange,
            Background(Z_COLOR_PRIMARY,
                CornerRadius(Z_RADIUS_PANEL,
                    Padding(14.0f,
                        Foreground(Z_COLOR_ON_PRIMARY,
                            Weight(Z_WEIGHT_SEMIBOLD,
                                Font(Z_FONT_CALLOUT, Text("Done"))))))))
        : dock_view();

    // While searching, the keyboard occupies the bottom KBD_H — the dots and dock
    // would be behind it, so the whole bar stands down and the Library page takes
    // the space back (lib_rows reserves the keyboard instead of BOTTOM_RESERVE).
    ZView bottom = NULL;
    if (!searching) {
        ZStackOpts bstack = {.spacing = BAR_GAP, .align = Z_ALIGN_CENTER,
                             .padding = state->rearrange ? 2.0f * BAR_PAD
                                                         : BAR_PAD};
        int bk = 0;
        bstack.children[bk++] = Spacer();
        // Dots stay up in rearrange too: they are the only readout of which page
        // the edge-dwell flip has carried the held item onto.
        bstack.children[bk++] = page_dots(npages, state->nlib, page_v);
        bstack.children[bk++] = bottom_content;
        // The surface runs under the home indicator, so hold the dock clear of it.
        bstack.children[bk++] = vgap((float)ZELTO_HOMEBAR_H);
        bottom = Fill(z_stack(Z_AXIS_VERTICAL, &bstack));
    }

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
            : app_cell_content(app, &g_apps[held.ref]);
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
    if (bottom) {
        hs.children[hk++] = bottom;
    }
    if (ghost) {
        hs.children[hk++] = ghost;
    }
    ZView home = OnPan(on_home_pan, Fill(z_stack(Z_AXIS_DEPTH, &hs)));

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
    if (state->rearrange || toast || paging || launching) {
        z_full_repaint(app);
    }

    ZStackOpts root = {.align = Z_ALIGN_CENTER};
    int k = 0;
    root.children[k++] = home;
    if (toast) {
        root.children[k++] = toast;
    }
    return z_stack(Z_AXIS_DEPTH, &root);
}

Z_APP_ID(LauncherState, launcher_body, "os.zelto.launcher")
