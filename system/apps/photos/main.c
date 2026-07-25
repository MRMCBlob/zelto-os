// Photos — the library browser.
//
// This is the READER half of the pipeline P53 builds: something captures
// (zelto-shot today, the camera later), the shared library in
// system/common/photos.h stores, and this app browses. It owns nothing. Its
// entire model is a directory listing, re-read when the app becomes visible, and
// that is the point of the library design rather than a shortcut — a second
// process can enumerate the same photos with the same header and get the same
// answer in the same order.
//
// The grid, a full-screen viewer, and the four things you can do with a photo
// once you are looking at it: swipe to the next one, delete it, share it, or
// make it the wallpaper. None of those own any state the library does not
// already hold — the model is still a directory listing.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zelto/ui.h>

#include "common/app_chrome.h"
#include "common/photos.h"
#include "common/wallpaper.h"

typedef struct PhotosState {
    bool listed;
    int count;
    char ids[ZELTO_PHOTOS_MAX][ZELTO_PHOTO_ID_MAX];
    // The display path per listed photo, kept in STATE rather than rebuilt into
    // the arena: Image() takes a path by pointer and the tree is rebuilt every
    // frame, so a stack buffer would be a dangling read by the time it paints.
    // (The same reason settings keeps wp_paths in state.)
    char paths[ZELTO_PHOTOS_MAX][ZELTO_PHOTO_PATH_MAX];
    // A stable per-cell pointer for OnTapData. The tap needs to say WHICH photo,
    // and the only thing a build may hand a handler is a pointer that outlives
    // the arena — so the indices live here rather than being cooked up per frame.
    int idx[ZELTO_PHOTOS_MAX];

    bool registered;                      // one-time lifecycle hookup
    bool nav_seeded;                      // ZELTO_PHOTOS_VIEW applied (once)
    int viewing;                          // index the viewer is showing
    char full[ZELTO_PHOTO_PATH_MAX];      // the ORIGINAL, not the thumbnail
    bool confirm_delete;
} PhotosState;

// The root screen gets no props (navigation.c passes NULL), so the app state
// reaches it the way Settings' does: one file-scope pointer, set in body().
static PhotosState *g_state;

static void relist(PhotosState *s) {
    s->count = zelto_photos_list(s->ids, ZELTO_PHOTOS_MAX);
    for (int i = 0; i < s->count; i++) {
        zelto_photo_display_path(s->ids[i], s->paths[i], sizeof(s->paths[i]));
        s->idx[i] = i;
    }
    if (s->viewing >= s->count) {
        s->viewing = s->count - 1;
    }
    if (s->viewing < 0) {
        s->viewing = 0;
    }
    s->listed = true;
}

// Re-read the library when we come back to the front. A screenshot taken while
// this app sits in the background is a photo the grid would otherwise not know
// about until the app was killed — which is the one way a library browser can be
// wrong about the library.
static void on_lifecycle(ZApp *app, void *state, ZLifecycle ev) {
    if (ev == Z_LC_ACTIVE) {
        relist((PhotosState *)state);
        z_invalidate(app);
    }
}

// --- the viewer -------------------------------------------------------------

static ZView viewer_screen(ZApp *app, void *props);

// The photo being viewed, at FULL size — never the thumbnail. The grid draws the
// small sibling because it is drawing dozens of them; the viewer is the one place
// the original is worth decoding, and showing the thumb scaled up here is the
// easiest way for a gallery to look quietly broken.
static void sync_full(PhotosState *s) {
    if (s->count > 0) {
        zelto_photo_path(s->ids[s->viewing], s->full, sizeof(s->full));
    } else {
        s->full[0] = '\0';
    }
}

// Step to the photo `d` along, clamped. Clamped rather than wrapping: at the end
// of a roll the picture staying put is the honest answer, where wrapping around
// to the newest reads as "the swipe did something else".
static void step(PhotosState *s, int d) {
    int n = s->viewing + d;
    if (n >= 0 && n < s->count) {
        s->viewing = n;
        sync_full(s);
    }
}

// Swipe left/right between photos. The step is committed on RELEASE against a
// quarter-screen threshold.
// ponytail: the photo does not track the finger mid-drag — add an OffsetXY bound
// to translation_x if the gesture needs to feel connected rather than merely work.
static void viewer_pan(ZApp *app, void *state, const ZPanEvent *e) {
    PhotosState *s = state;
    if (e->phase != Z_PAN_END || s->confirm_delete) {
        return;
    }
    float threshold = (float)z_app_width(app) * 0.25f;
    if (e->translation_x <= -threshold) {
        step(s, +1);            // dragged left: the NEXT (older) photo
    } else if (e->translation_x >= threshold) {
        step(s, -1);
    }
    z_invalidate(app);
}

static void open_viewer(ZApp *app, void *state, void *data) {
    PhotosState *s = state;
    s->viewing = *(int *)data;
    s->confirm_delete = false;
    sync_full(s);
    z_nav_push(z_navigation(app), viewer_screen, s);
}

// SHARE. The payload is the photo's PATH, not its bytes: z_share carries text
// (binary items are Planned), and a path is a working handle here precisely
// because the media root is shared rather than app-scoped — the receiver can
// open it. The MIME is still image/png, so the sheet resolves image handlers
// rather than text ones.
static void share_photo(ZApp *app, void *state) {
    PhotosState *s = state;
    (void)app;
    if (s->count == 0) {
        return;
    }
    ZShareItem item = {.mime = "image/png", .text = s->full};
    z_share(&item, 1);
}

// SET AS WALLPAPER. The whole feature is writing the brokered key P25 already
// defined: the launcher and the lock screen observe it and repaint themselves.
// This is the test of whether the library is a library — a second app consuming
// it — and it passes by being one line.
static void set_wallpaper(ZApp *app, void *state) {
    PhotosState *s = state;
    (void)app;
    if (s->count > 0) {
        z_setting_set_str(ZELTO_WALLPAPER_KEY, s->full);
        fprintf(stderr, "zelto-photos: wallpaper set to %s\n", s->full);
    }
}

static void ask_delete(ZApp *app, void *state) {
    ((PhotosState *)state)->confirm_delete = true;
    z_invalidate(app);
}

static void cancel_delete(ZApp *app, void *state) {
    ((PhotosState *)state)->confirm_delete = false;
    z_invalidate(app);
}

// DELETE, confirmed. Deleting the last photo leaves nothing to view, so the
// viewer pops rather than sitting on an empty frame.
static void do_delete(ZApp *app, void *state) {
    PhotosState *s = state;
    s->confirm_delete = false;
    if (s->count > 0) {
        fprintf(stderr, "zelto-photos: deleting %s\n", s->ids[s->viewing]);
        zelto_photo_delete(s->ids[s->viewing]);
        relist(s);
        sync_full(s);
    }
    if (s->count == 0) {
        z_nav_pop(z_navigation(app));
    }
    z_invalidate(app);
}

// The confirmation. A delete is the one irreversible thing this app can do —
// there is no trash — so it asks, and the destructive answer is the one that
// carries the colour.
static ZView confirm_card(ZApp *app, float w) {
    float card_w = w - 2.0f * Z_SPACE_2XL;
    return Background(Z_COLOR_SCRIM,
        Fill(ZStack(
            Background(Z_COLOR_SURFACE,
                CornerRadius(Z_RADIUS_PANEL,
                    Frame(card_w, 0.0f,
                        VStack(
                            Weight(Z_WEIGHT_SEMIBOLD, Foreground(Z_COLOR_TEXT,
                                Font(Z_FONT_HEADLINE, Text("Delete photo?")))),
                            Foreground(Z_COLOR_TEXT_MUTED,
                                WrapText(app, "This photo will be removed from "
                                              "your library. It cannot be undone.",
                                         .width = card_w - 2.0f * Z_SPACE_L,
                                         .size = Z_FONT_SUBHEAD)),
                            HStack(
                                Button(cancel_delete, "Cancel"),
                                Foreground(z_on_fill(Z_COLOR_DANGER),
                                    Background(Z_COLOR_DANGER,
                                        CornerRadius(Z_RADIUS_CHIP,
                                            // "Delete Photo", not "Delete": the
                                            // toolbar button behind this card
                                            // already says "Delete", and two
                                            // controls with one name on screen at
                                            // once is ambiguous to a reader and
                                            // to anything resolving a control BY
                                            // that name. Naming the object is
                                            // also the better confirm copy.
                                            Button(do_delete, "Delete Photo")))),
                                .spacing = Z_SPACE_S, .align = Z_ALIGN_CENTER),
                            .spacing = Z_SPACE_M, .padding = Z_SPACE_L,
                            .align = Z_ALIGN_LEADING)))),
            .align = Z_ALIGN_CENTER)));
}

static ZView viewer_screen(ZApp *app, void *props) {
    PhotosState *s = props;
    if (s->count == 0) {
        return Background(Z_COLOR_BG, Fill(Spacer()));
    }

    char pos[32];
    snprintf(pos, sizeof(pos), "%d of %d", s->viewing + 1, s->count);

    // THE IMAGE NEEDS A BOUNDED FRAME, AND Grow WILL NOT GIVE IT ONE. Grow
    // divides the SLACK, and an Image with no frame measures to its intrinsic
    // pixels — a 946x2048 photo is already taller than the screen, so there is no
    // slack to divide and Grow(1) adds nothing. The first cut did exactly that
    // and pushed the toolbar off the bottom of the surface: the picture looked
    // perfect and the three buttons did not exist.
    //
    // So the height is an EXPRESSION OVER ITS PARTS, and each part is ASKED for
    // rather than estimated — z_line_height goes to the face and z_row_h is the
    // touch-target floor, so this tracks Dynamic Type instead of drifting from it
    // at the sizes nobody photographs.
    // The toolbar is one row, or three stacked rows past the reflow break — and
    // the image's height has to know which, or the reflow simply moves the
    // clipping from the right edge to the bottom one.
    float toolbar_h = z_text_size_reflows()
                          ? 3.0f * z_row_h(app) + 2.0f * (float)Z_SPACE_S
                          : z_row_h(app);
    float img_h = (float)z_app_height(app) - 2.0f * ZELTO_PHOTOS_MARGIN
                  - z_line_height(app, Z_FONT_FOOTNOTE) - toolbar_h
                  - 2.0f * (float)Z_SPACE_M;
    if (img_h < 1.0f) {
        img_h = 1.0f;
    }

    ZView body = VStack(
        Foreground(Z_COLOR_TEXT_MUTED, Font(Z_FONT_FOOTNOTE, Text("%s", pos))),
        // Aspect-FIT (the grid crops, the viewer shows the whole picture).
        Frame((float)z_app_width(app) - 2.0f * ZELTO_PHOTOS_MARGIN, img_h,
              Image(s->full)),
        // THE TOOLBAR REFLOWS, and the audit is what said so. Three buttons side
        // by side fit comfortably at the shipped sizes and walk clean off the
        // right edge at the top of the accessibility range — "Wallpaper" was
        // laid out at x=325 w=480 on a 720 screen, i.e. a control nothing on
        // screen says is there and no gesture can reach. Past the measured
        // break the row becomes a column, which is the same answer Settings
        // gives for the same reason; asking the predicate is what keeps the two
        // from drifting apart.
        z_stack(z_text_size_reflows() ? Z_AXIS_VERTICAL : Z_AXIS_HORIZONTAL,
            &(ZStackOpts){
                .children = {
                    Button(share_photo, "Share"),
                    Button(set_wallpaper, "Wallpaper"),
                    Button(ask_delete, "Delete"),
                },
                .spacing = Z_SPACE_S, .align = Z_ALIGN_CENTER}),
        .spacing = Z_SPACE_M, .padding = ZELTO_PHOTOS_MARGIN,
        .align = Z_ALIGN_CENTER);

    ZView screen = Background(Z_COLOR_BG, Fill(OnPan(viewer_pan, body)));
    if (s->confirm_delete) {
        screen = ZStack(screen, confirm_card(app, (float)z_app_width(app)),
                        .align = Z_ALIGN_CENTER);
    }
    return screen;
}

// One cell: the thumbnail, aspect-FILLED into a square and centre-cropped.
//
// Cover rather than fit, and it is not a style choice. A screenshot is 1:2; a
// letterboxed 1:2 image in a square cell is a narrow strip with two black bars,
// so a grid of them reads as a list of slivers rather than as pictures. Every
// phone gallery centre-crops for exactly this reason, and the full frame is one
// tap away.
static ZView photo_cell(PhotosState *s, int i, float cell) {
    return OnTapData(open_viewer, &s->idx[i],
        Frame(cell, cell,
            CornerRadius(Z_RADIUS_CHIP, Cover(Image(s->paths[i])))));
}

// The grid. Its cell is an EXPRESSION over the column count and the spacing
// scale (zelto_photos_cell), never a literal — the wallpaper picker's cell was a
// number that happened to fit until P52 derived it, and copying that number here
// would have re-introduced precisely what that fixed.
//
// A short last row is padded with a FIXED-WIDTH transparent cell, never a
// Spacer: a grow-weighted filler redistributes by child count, so a row of two
// would sit at different x's than a row of three and the columns would visibly
// step sideways.
static ZView photo_grid(PhotosState *s, float grid_w) {
    float cell = zelto_photos_cell(grid_w);
    ZStackOpts grid = {.spacing = ZELTO_PHOTOS_GAP, .align = Z_ALIGN_LEADING};
    int k = 0;
    for (int i = 0; i < s->count && k < Z_MAX_CHILDREN; i += ZELTO_PHOTOS_COLS) {
        ZStackOpts row = {.spacing = ZELTO_PHOTOS_GAP, .align = Z_ALIGN_CENTER};
        for (int c = 0; c < ZELTO_PHOTOS_COLS; c++) {
            int j = i + c;
            row.children[c] = j < s->count
                ? photo_cell(s, j, cell)
                : Frame(cell, cell, Rect(.color = z_rgba(0, 0, 0, 0)));
        }
        grid.children[k++] = z_stack(Z_AXIS_HORIZONTAL, &row);
    }
    return z_stack(Z_AXIS_VERTICAL, &grid);
}

// Nothing captured yet. PROSE, so WrapText — a Text node measures to one line
// however long it is and paints straight off the right edge, and this sentence
// is exactly the length that survives at the default text size and leaves the
// screen at AX5.
static ZView empty_state(ZApp *app, float w) {
    return VStack(
        Weight(Z_WEIGHT_SEMIBOLD,
            Foreground(Z_COLOR_TEXT, Font(Z_FONT_HEADLINE, Text("No photos")))),
        Foreground(Z_COLOR_TEXT_MUTED,
            WrapText(app,
                     "Screenshots and pictures you take will appear here.",
                     .width = w, .size = Z_FONT_SUBHEAD)),
        .spacing = Z_SPACE_XS, .align = Z_ALIGN_LEADING);
}

// The grid screen: the Navigator's root, which is handed no props (see g_state).
static ZView grid_screen(ZApp *app, void *props) {
    (void)props;
    PhotosState *s = g_state;
    if (!s->listed) {
        relist(s);
    }

    float w = (float)z_app_width(app);
    float column = w - 2.0f * ZELTO_PHOTOS_MARGIN;

    char sub[64];
    snprintf(sub, sizeof(sub), s->count == 1 ? "%d photo" : "%d photos",
             s->count);

    ZStackOpts col = {.spacing = Z_SPACE_M, .align = Z_ALIGN_LEADING};
    int k = 0;
    // The title WRAPS. A Large Title is 74 units at the default size and 141 at
    // AX5; a one-word title survives, but the rule is the rule and the next
    // screen this app grows will not be one word.
    col.children[k++] = Weight(Z_WEIGHT_BOLD,
        Foreground(Z_COLOR_TEXT,
            WrapText(app, "Photos", .width = column, .size = Z_FONT_LARGE_TITLE,
                     .weight = Z_WEIGHT_BOLD)));
    col.children[k++] = Foreground(Z_COLOR_TEXT_MUTED,
        Font(Z_FONT_SUBHEAD, Text("%s", sub)));
    col.children[k++] = s->count > 0 ? photo_grid(s, w)
                                     : empty_state(app, column);

    return Background(Z_COLOR_BG,
        Fill(Scroll(app,
            Padding(ZELTO_PHOTOS_MARGIN, z_stack(Z_AXIS_VERTICAL, &col)),
            .axis = Z_AXIS_VERTICAL)));
}

// A Navigator, so BACK out of the viewer is the system edge-swipe and the
// Escape key — the same gesture as everywhere else in the OS — rather than an
// on-screen affordance this app would have had to invent.
static ZView photos_body(ZApp *app, PhotosState *s) {
    g_state = s;
    if (!s->registered) {
        s->registered = true;
        z_on_lifecycle(app, on_lifecycle);
    }
    ZView nav = Navigator(app, .root = grid_screen);

    // ZELTO_PHOTOS_VIEW=<index> opens the viewer on that photo at boot — the
    // ZELTO_SETTINGS_SCREEN idiom, and for the same reason: a harness must be
    // able to reach a screen without tapping a coordinate that will rot. Applied
    // after the first build, so the Navigator's root exists to be pushed onto.
    if (!s->nav_seeded) {
        s->nav_seeded = true;
        const char *want = getenv("ZELTO_PHOTOS_VIEW");
        if (want && want[0] && s->count > 0) {
            int i = atoi(want);
            s->viewing = (i >= 0 && i < s->count) ? i : 0;
            sync_full(s);
            z_nav_push(z_navigation(app), viewer_screen, s);
        }
    }
    return nav;
}

Z_APP_ID(PhotosState, photos_body, ZELTO_PHOTOS_APP_ID)
