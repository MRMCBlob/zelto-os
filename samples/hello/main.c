// samples/hello - a multi-screen native Zelto app exercising the P5 toolkit:
// a virtualised, scrollable List of rows (wheel + drag scroll, fling momentum)
// inside a Navigator; tapping a row pushes a detail screen with the standard
// slide transition; Escape / Backspace / an edge-swipe pops it back. The detail
// screen also drives an explicit animated value through Offset to show the
// spring engine ticking off the frame callback. libzelto routes the input and
// runs the build -> reconcile -> damaged-repaint loop; zcomp composites it.
// See docs/guides/{animation,gestures,navigation}.md.
#include <stdio.h>
#include <stdlib.h>

#include <zelto/ui.h>

// --- data -----------------------------------------------------------------
#define N_ITEMS 60

typedef struct Item {
    int id;
    char title[32];
    char detail[64];
    ZColor color;
} Item;

static Item g_items[N_ITEMS];
static bool g_ready;

static void ensure_items(void) {
    if (g_ready) {
        return;
    }
    g_ready = true;
    const ZColor palette[4] = {
        Z_COLOR_PRIMARY,
        Z_COLOR_ACCENT,
        z_rgba(0x3d, 0xc7, 0x8c, 0xff),  // green
        z_rgba(0xc8, 0x6b, 0xff, 0xff),  // violet
    };
    for (int i = 0; i < N_ITEMS; i++) {
        g_items[i].id = i;
        snprintf(g_items[i].title, sizeof(g_items[i].title), "Row %d", i);
        snprintf(g_items[i].detail, sizeof(g_items[i].detail),
                 "Detail for row %d — pushed via Navigator.", i);
        g_items[i].color = palette[i % 4];
    }
}

// --- detail screen --------------------------------------------------------
// Toggle the animated box between two positions (explicit animated value +
// Offset + spring). The handler gets the persistent ZAnimated via tap data.
static void toggle_box(ZApp *app, void *state, void *data) {
    (void)app;
    (void)state;
    ZAnimated *x = data;
    z_animated_spring(x, z_animated_get(x) > 60.0f ? 0.0f : 140.0f);
}

static void pop_screen(ZApp *app, void *state) {
    (void)state;
    z_nav_pop(z_navigation(app));
}

static ZView detail_screen(ZApp *app, void *props) {
    Item *it = props;
    ZAnimated *box_x = z_animated_value(app, 0.0f);

    return Background(z_rgba(0x12, 0x16, 0x1c, 0xff),
        VStack(
            // Navbar.
            Background(z_rgba(0x1c, 0x22, 0x2b, 0xff),
                HStack(
                    Foreground(Z_COLOR_TEXT_INV,
                        Font(Z_FONT_TITLE, Text("%s", it->title))),
                    Spacer(),
                    Button(pop_screen, "Back"),
                    .padding = 16, .spacing = 12, .align = Z_ALIGN_CENTER)),

            Foreground(z_rgba(0x9a, 0xa4, 0xad, 0xff), Text("%s", it->detail)),

            // Animated hero: a box translated by an animated value. Tapping it
            // springs it between two x positions; the spring advances on the
            // frame callback and idles when settled.
            OnTapData(toggle_box, box_x,
                Offset(box_x, 0.0f,
                    Frame(120.0f, 120.0f,
                        Rect(.color = it->color, .radius = 20)))),

            Foreground(z_rgba(0x6b, 0x74, 0x7d, 0xff),
                Text("tap the box to spring it · Esc / swipe to go back")),

            Spacer(),
            .padding = 24, .spacing = 22, .align = Z_ALIGN_LEADING));
}

// --- list screen ----------------------------------------------------------
static uint64_t item_key(const void *item, int index) {
    (void)index;
    return (uint64_t)(((const Item *)item)->id + 1);  // stable, non-zero
}

static void open_item(ZApp *app, void *state, void *data) {
    (void)state;
    z_nav_push(z_navigation(app), detail_screen, data);  // data = the Item*
}

// The row's own padding and swatch, named because the List has to be told how
// tall a row is BEFORE one exists (it virtualises on that number), so the height
// and the row have to be computed from the same parts or they drift.
#define ROW_PAD 14.0f
#define ROW_SWATCH 40.0f

// A row is as tall as the taller of its two contents plus its padding — not 64.
// The literal was measured once against 17px type; at the largest text size a
// Body line stands 53 and painted through a row the List had already decided was
// 64 tall, so the glyphs crossed into the neighbouring row while the scroll
// still placed rows 64 apart.
static float item_row_h(ZApp *app) {
    float t = z_line_height(app, Z_FONT_BODY);
    float tall = t > ROW_SWATCH ? t : ROW_SWATCH;
    return tall + 2.0f * ROW_PAD;
}

static ZView item_row(ZApp *app, const void *item, int index) {
    (void)app;
    (void)index;
    const Item *it = item;
    return OnTapData(open_item, (void *)it,
        Background(z_rgba(0x1a, 0x20, 0x28, 0xff),
            HStack(
                Frame(ROW_SWATCH, ROW_SWATCH, Rect(.color = it->color, .radius = 10)),
                Foreground(Z_COLOR_TEXT_INV, Text("%s", it->title)),
                Spacer(),
                Foreground(z_rgba(0x6b, 0x74, 0x7d, 0xff), Text("#%d", it->id)),
                .padding = ROW_PAD, .spacing = 14, .align = Z_ALIGN_CENTER)));
}

// Lifecycle banner: green ACTIVE in the foreground, amber PAUSED when another
// app is in front (driven by the compositor's xdg activated-state broadcast).
static ZView lifecycle_banner(ZApp *app) {
    bool act = z_app_active(app);
    return Background(act ? z_rgba(0x1d, 0x5e, 0x3a, 0xff)
                          : z_rgba(0x5e, 0x49, 0x1d, 0xff),
        Padding(10,
            Foreground(Z_COLOR_TEXT_INV,
                Font(Z_FONT_CALLOUT, Text(act ? "ACTIVE" : "PAUSED")))));
}

static ZView list_screen(ZApp *app, void *props) {
    (void)props;
    ensure_items();
    return Background(z_rgba(0x0e, 0x12, 0x17, 0xff),
        VStack(
            lifecycle_banner(app),
            Background(z_rgba(0x1c, 0x22, 0x2b, 0xff),
                HStack(
                    Foreground(Z_COLOR_TEXT_INV,
                        Font(Z_FONT_TITLE, Text("Zelto · Rows"))),
                    Spacer(),
                    .padding = 16)),

            // The scrollable, virtualised list fills the rest of the screen.
            Grow(1.0f,
                List(app,
                    .data = g_items, .stride = sizeof(Item), .count = N_ITEMS,
                    .row_height = item_row_h(app), .key = item_key,
                    .row = item_row)),
            .spacing = 0));
}

// --- app ------------------------------------------------------------------
typedef struct AppState {
    int unused;
} AppState;

static ZView body(ZApp *app, AppState *state) {
    (void)state;
    ZView v = Navigator(app, .root = list_screen);
    // Freeze-frame hook (P32): ZELTO_NAV_PUSH=<0..1> pushes the detail screen once
    // and pins the slide+cross-fade transition at that progress, so the unified
    // Navigator push motion is screenshot-verifiable mid-flight. Runs after the
    // Navigator has inited this build; the push invalidates, so the next build
    // renders the pinned mid-transition. Reduce Motion would collapse it to 1.
    static bool nav_seeded = false;
    const char *np = getenv("ZELTO_NAV_PUSH");
    if (np && np[0] && !nav_seeded) {
        nav_seeded = true;
        ensure_items();
        ZNav *nav = z_navigation(app);
        z_nav_push(nav, detail_screen, &g_items[3]);
        z_nav_freeze_top(nav, (float)atof(np));
    }
    // Freeze-frame hook (P33): ZELTO_NAV_BACK=<0..1> pushes a detail screen and pins
    // the top screen mid back-swipe (the finger dragging it out to the right, the
    // list sliding under it) — the interruptible edge-swipe frozen at a finger
    // position, verifying the incoming screen tracks under the outgoing one. Same
    // pin as the push (progress is progress); the value is how far the finger is.
    static bool nav_back_seeded = false;
    const char *nb = getenv("ZELTO_NAV_BACK");
    if (nb && nb[0] && !nav_back_seeded) {
        nav_back_seeded = true;
        ensure_items();
        ZNav *nav = z_navigation(app);
        z_nav_push(nav, detail_screen, &g_items[3]);
        z_nav_freeze_top(nav, (float)atof(nb));
    }
    return v;
}

Z_APP(AppState, body)
