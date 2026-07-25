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
// STAGE 0 SCOPE. The grid, the count, and the empty state. The full-screen
// viewer, swiping between photos, delete-with-confirmation, share and
// set-as-wallpaper are the app stage; they change what a tap DOES, not what the
// library IS, so they land without disturbing anything below this line.
#include <stdio.h>
#include <string.h>

#include <zelto/ui.h>

#include "common/app_chrome.h"
#include "common/photos.h"

typedef struct PhotosState {
    bool listed;
    int count;
    char ids[ZELTO_PHOTOS_MAX][ZELTO_PHOTO_ID_MAX];
    // The display path per listed photo, kept in STATE rather than rebuilt into
    // the arena: Image() takes a path by pointer and the tree is rebuilt every
    // frame, so a stack buffer would be a dangling read by the time it paints.
    // (The same reason settings keeps wp_paths in state.)
    char paths[ZELTO_PHOTOS_MAX][ZELTO_PHOTO_PATH_MAX];
} PhotosState;

static void relist(PhotosState *s) {
    s->count = zelto_photos_list(s->ids, ZELTO_PHOTOS_MAX);
    for (int i = 0; i < s->count; i++) {
        zelto_photo_display_path(s->ids[i], s->paths[i], sizeof(s->paths[i]));
    }
    s->listed = true;
}

// One cell: the thumbnail, aspect-FILLED into a square and centre-cropped.
//
// Cover rather than fit, and it is not a style choice. A screenshot is 1:2; a
// letterboxed 1:2 image in a square cell is a narrow strip with two black bars,
// so a grid of them reads as a list of slivers rather than as pictures. Every
// phone gallery centre-crops for exactly this reason, and the full frame is one
// tap away.
static ZView photo_cell(PhotosState *s, int i, float cell) {
    return Frame(cell, cell,
        CornerRadius(Z_RADIUS_CHIP, Cover(Image(s->paths[i]))));
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

static ZView photos_body(ZApp *app, PhotosState *s) {
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

Z_APP_ID(PhotosState, photos_body, ZELTO_PHOTOS_APP_ID)
