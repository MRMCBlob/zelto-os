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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <zelto/ui.h>

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

// Launcher persistent state. drawer_anim (0 hidden .. 1 covering) drives the
// drawer's vertical Offset; surface_h is the launcher's pixel height, both
// refreshed every build. drawer_anim is a retained hook: it MUST be allocated at
// the same call-order position every rebuild (the first z_animated_value in this
// body) or its identity shifts and the animation state corrupts.
typedef struct LauncherState {
    ZAnimated *drawer_anim;   // 0 hidden below the fold .. 1 covering home
    float surface_h;

    // Long-press context menu. menu_idx is a g_apps index; menu_open gates the
    // overlay. A short-lived toast (cap-reached feedback) shows until toast_until.
    bool menu_open;
    int menu_idx;
    double toast_until;       // monotonic seconds; 0 = no toast
    char toast[64];
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
        OnLongPress(on_icon_longpress, (void *)e,
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

// --- body -----------------------------------------------------------------
static ZView launcher_body(ZApp *app, LauncherState *state) {
    // Retained hooks, allocated FIRST + unconditionally every rebuild so their
    // identity is stable: drawer_anim is the body's single z_animated_value (anim
    // cell 0). The drawer is always built (never conditionally), so its Scroll
    // cell stays stable too; it is merely translated off-screen when at rest.
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

    // (3) CURATE CONTEXT MENU (long-press): a dimmed modal over everything with
    // an Add/Remove-from-home action for the long-pressed app. Built only while
    // open; carries no retained hooks, so the cell order above stays fixed.
    ZView menu = NULL;
    if (state->menu_open && state->menu_idx >= 0 &&
        state->menu_idx < g_n_apps) {
        const AppEntry *e = &g_apps[state->menu_idx];
        bool fav = fav_pos(state->menu_idx) >= 0;
        menu = Fill(ZStack(
            OnTap(close_menu, Fill(Background(z_rgba(0x00, 0x00, 0x00, 0x99),
                                              Fill(Spacer())))),
            Background(z_rgba(0x1b, 0x22, 0x2c, 0xff),
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
            Background(z_rgba(0x24, 0x2a, 0x34, 0xf0),
                CornerRadius(12.0f,
                    Padding(14.0f,
                        Foreground(Z_COLOR_TEXT_INV,
                            Font(Z_FONT_BODY, Text("%s", state->toast)))))),
            .spacing = 0, .padding = 48, .align = Z_ALIGN_CENTER));
    }

    // A layer in motion (the sliding drawer) or a translucent overlay (the menu
    // scrim / toast) must paint as a full repaint, or the partial-repaint path
    // re-blends over already-correct pixels and darkens them cumulatively. Force
    // it whenever the drawer is off its rest position or an overlay is up.
    if (drawer_v > 0.001f || menu || toast) {
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
    if (toast) {
        root.children[k++] = toast;
    }
    return z_stack(Z_AXIS_DEPTH, &root);
}

Z_APP_ID(LauncherState, launcher_body, "os.zelto.launcher")
