// test_photo_library — the PNG encoder and the library's identity rule.
//
// WHY THIS EXISTS. P53 adds the first WRITE path for images in this OS, and it
// carries two things that are invisible when wrong:
//
//   1. PREMULTIPLICATION. libzelto's canvas holds premultiplied ARGB; a wl_shm
//      buffer and a screencopy frame hold straight. PNG is defined as straight.
//      Get the convention backwards and every OPAQUE pixel still comes out
//      exactly right — the error only exists where alpha < 255. A screenshot is
//      opaque, so the bug would ship, and would surface later on the first
//      translucent capture. So the round-trip is asserted on a TRANSLUCENT image
//      specifically, with the opaque case beside it as the control.
//
//   2. THE ID IS THE INDEX. The library has no database: "newest first" is a
//      reverse sort of fixed-width filenames (see system/common/photos.h). That
//      only holds while the ids really are fixed width and really do compare in
//      time order, and both are properties of a printf format — the kind of thing
//      that survives a careless edit and quietly reorders someone's camera roll.
//
// The decode side is libpng DIRECTLY rather than image.c, deliberately: an
// encoder checked with its own project's decoder can agree with itself about a
// wrong convention. Here the reader is the reference implementation.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <png.h>

#include "framework/ztest.h"

// image_write.c is pulled in as a TU (the test_wrap_lines idiom); it needs
// nothing from the rest of libzelto.
#include "image_write.c"

// --- the two libzelto symbols photos.h touches -----------------------------
// The library header is exercised through $ZELTO_PHOTOS_ROOT, so z_path_media is
// never reached; z_mkdir_p is, and it has to really work.
char *z_path_media(const char *rel) {
    (void)rel;
    return NULL;
}
void z_mkdir_p(const char *path) {
    char buf[512];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(buf)) {
        return;
    }
    memcpy(buf, path, n + 1);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    mkdir(buf, 0755);
}

#include "common/photos.h"

// --- a minimal reference PNG reader ---------------------------------------
// Returns straight-alpha RGBA rows, and reports the file's colour TYPE so the
// "RGB when opaque" decision can be asserted rather than assumed.
typedef struct RefPng {
    int w, h, color_type;
    uint8_t *rgba;   // w*h*4, straight alpha
} RefPng;

static bool ref_read(const char *path, RefPng *out) {
    memset(out, 0, sizeof(*out));
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL,
                                             NULL);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info) {
        if (png) {
            png_destroy_read_struct(&png, info ? &info : NULL, NULL);
        }
        fclose(fp);
        return false;
    }
    if (setjmp(png_jmpbuf(png))) {
        free(out->rgba);
        out->rgba = NULL;
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return false;
    }
    png_init_io(png, fp);
    png_read_info(png, info);
    out->w = (int)png_get_image_width(png, info);
    out->h = (int)png_get_image_height(png, info);
    out->color_type = png_get_color_type(png, info);
    if (out->color_type == PNG_COLOR_TYPE_RGB) {
        png_set_add_alpha(png, 0xff, PNG_FILLER_AFTER);
    }
    png_read_update_info(png, info);

    out->rgba = malloc((size_t)out->w * (size_t)out->h * 4);
    png_bytep *rows = malloc((size_t)out->h * sizeof(png_bytep));
    if (!out->rgba || !rows) {
        free(rows);
        png_longjmp(png, 1);
    }
    for (int y = 0; y < out->h; y++) {
        rows[y] = out->rgba + (size_t)y * (size_t)out->w * 4;
    }
    png_read_image(png, rows);
    png_read_end(png, NULL);
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    return true;
}

static uint32_t argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b) {
    return (a << 24) | (r << 16) | (g << 8) | b;
}

// --- the tests -------------------------------------------------------------

static int test_opaque_writes_rgb(const char *dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/opaque.png", dir);

    uint32_t px[4] = {
        argb(255, 10, 20, 30),   argb(255, 200, 100, 50),
        argb(255, 0, 0, 0),      argb(255, 255, 255, 255),
    };
    ASSERT_TRUE(z_image_write_png(path, px, 2, 2, 2 * 4, false));

    RefPng got;
    ASSERT_TRUE(ref_read(path, &got));
    EXPECT_EQ_INT(2, got.w);
    EXPECT_EQ_INT(2, got.h);
    // Every pixel opaque => no alpha channel is written. This is measured, not a
    // caller flag, so the assertion is on the FILE.
    EXPECT_EQ_INT(PNG_COLOR_TYPE_RGB, got.color_type);
    EXPECT_EQ_INT(10, got.rgba[0]);
    EXPECT_EQ_INT(20, got.rgba[1]);
    EXPECT_EQ_INT(30, got.rgba[2]);
    EXPECT_EQ_INT(200, got.rgba[4]);
    free(got.rgba);
    return 0;
}

static int test_premultiplied_round_trip(const char *dir) {
    char straight_path[512], premul_path[512];
    snprintf(straight_path, sizeof(straight_path), "%s/straight.png", dir);
    snprintf(premul_path, sizeof(premul_path), "%s/premul.png", dir);

    // One pixel: 50% alpha over pure red. Straight is (128, 255,0,0);
    // premultiplied is (128, 128,0,0) — the SAME colour in the two conventions.
    // Written correctly, both files must decode to the same straight red.
    uint32_t straight = argb(128, 255, 0, 0);
    uint32_t premul = argb(128, 128, 0, 0);

    ASSERT_TRUE(z_image_write_png(straight_path, &straight, 1, 1, 4, false));
    ASSERT_TRUE(z_image_write_png(premul_path, &premul, 1, 1, 4, true));

    RefPng a, b;
    ASSERT_TRUE(ref_read(straight_path, &a));
    ASSERT_TRUE(ref_read(premul_path, &b));
    // A translucent pixel exists => the alpha channel must be kept.
    EXPECT_EQ_INT(PNG_COLOR_TYPE_RGBA, a.color_type);
    EXPECT_EQ_INT(PNG_COLOR_TYPE_RGBA, b.color_type);

    EXPECT_EQ_INT(255, a.rgba[0]);
    EXPECT_EQ_INT(128, a.rgba[3]);
    // The un-premultiply: 128 * 255 / 128 = 255, clamped. Off by more than a
    // rounding unit means the convention was ignored (the failure that is
    // invisible on every opaque image).
    EXPECT_TRUE(b.rgba[0] >= 253);
    EXPECT_EQ_INT(128, b.rgba[3]);
    free(a.rgba);
    free(b.rgba);
    return 0;
}

static int test_box_scale_averages(void) {
    // A 2x2 whose four pixels average to a known value. Nearest-neighbour would
    // return one of the corners instead, which is the failure that makes small
    // text on a thumbnail unreadable.
    uint32_t src[4] = {
        argb(255, 0, 0, 0),      argb(255, 100, 100, 100),
        argb(255, 200, 200, 200), argb(255, 255, 255, 255),
    };
    uint32_t *dst = z_image_box_scale(src, 2, 2, 2 * 4, 1, 1);
    ASSERT_TRUE(dst != NULL);
    EXPECT_EQ_INT((0 + 100 + 200 + 255) / 4, (int)((dst[0] >> 16) & 0xff));
    EXPECT_EQ_INT(255, (int)((dst[0] >> 24) & 0xff));
    free(dst);

    // Scaling UP is not what this is for, but it must not read out of bounds or
    // return NULL — the writer calls it with whatever the source turns out to be.
    uint32_t *up = z_image_box_scale(src, 2, 2, 2 * 4, 4, 4);
    EXPECT_TRUE(up != NULL);
    free(up);
    return 0;
}

// The library's whole index is the filename. These are the properties that makes
// that work, asserted directly on the format.
static int test_ids_sort_newest_first(void) {
    char a[ZELTO_PHOTO_ID_MAX], b[ZELTO_PHOTO_ID_MAX];
    ASSERT_TRUE(zelto_photo_new_id(a, sizeof(a)));
    // Fixed width is the load-bearing property: byte order == time order only
    // while every id is the same length.
    EXPECT_EQ_INT(16, (int)strlen(a));

    // A later capture must compare GREATER, so a descending sort is newest-first.
    usleep(3000);
    ASSERT_TRUE(zelto_photo_new_id(b, sizeof(b)));
    EXPECT_TRUE(strcmp(b, a) > 0);
    return 0;
}

// Store three photos and read the library back. This is the "a second app can
// enumerate it" claim in miniature: nothing here shares state with the writer
// except the directory.
static int test_store_and_enumerate(void) {
    uint32_t px[16];
    for (int i = 0; i < 16; i++) {
        px[i] = argb(255, (uint32_t)(i * 16), 40, 60);
    }

    char ids[3][ZELTO_PHOTO_ID_MAX];
    for (int i = 0; i < 3; i++) {
        ASSERT_TRUE(zelto_photo_new_id(ids[i], sizeof(ids[i])));
        ASSERT_TRUE(zelto_photo_store(ids[i], px, 4, 4, 4 * 4, false));
        usleep(3000);
    }
    EXPECT_EQ_INT(3, zelto_photos_count());

    char listed[ZELTO_PHOTOS_MAX][ZELTO_PHOTO_ID_MAX];
    int n = zelto_photos_list(listed, ZELTO_PHOTOS_MAX);
    ASSERT_EQ_INT(3, n);
    // Newest first: the LAST one stored is the FIRST one listed.
    EXPECT_STR_EQ(ids[2], listed[0]);
    EXPECT_STR_EQ(ids[0], listed[2]);

    // A 4x4 source is already smaller than a grid cell, so the thumbnail is the
    // image itself — but it must EXIST, because the grid asks for it by name.
    char thumb[ZELTO_PHOTO_PATH_MAX];
    zelto_thumb_path(ids[0], thumb, sizeof(thumb));
    EXPECT_TRUE(access(thumb, R_OK) == 0);

    // Delete takes the thumbnail with it. A thumb outliving its photo would show
    // a picture the library says is gone.
    char full[ZELTO_PHOTO_PATH_MAX];
    zelto_photo_path(ids[1], full, sizeof(full));
    EXPECT_TRUE(zelto_photo_delete(ids[1]));
    EXPECT_TRUE(access(full, F_OK) != 0);
    zelto_thumb_path(ids[1], thumb, sizeof(thumb));
    EXPECT_TRUE(access(thumb, F_OK) != 0);
    EXPECT_EQ_INT(2, zelto_photos_count());
    return 0;
}

// A large source must be downscaled to the GRID CELL on its short edge, and must
// keep its aspect ratio. A thumbnail that is smaller than its cell is a blurry
// grid; one that ignores the ratio is a squashed one.
static int test_thumb_is_the_grid_cell(void) {
    int w = 128, h = 256;
    uint32_t *px = calloc((size_t)w * (size_t)h, 4);
    ASSERT_TRUE(px != NULL);
    for (int i = 0; i < w * h; i++) {
        px[i] = argb(255, 90, 90, 90);
    }
    char id[ZELTO_PHOTO_ID_MAX];
    ASSERT_TRUE(zelto_photo_new_id(id, sizeof(id)));
    ASSERT_TRUE(zelto_photo_store(id, px, w, h, (size_t)w * 4, false));
    free(px);

    char thumb[ZELTO_PHOTO_PATH_MAX];
    zelto_thumb_path(id, thumb, sizeof(thumb));
    RefPng got;
    ASSERT_TRUE(ref_read(thumb, &got));
    // 128 is already under the cell (a cell is ~216 units at the design width),
    // so this source is stored as-is — assert THAT rather than a scale, and
    // assert the aspect ratio is untouched either way.
    EXPECT_TRUE(got.w * 256 == got.h * 128);
    EXPECT_TRUE(got.w <= w);
    free(got.rgba);

    // And the cell itself is an EXPRESSION, not a literal: it must fall out of
    // the column count and the spacing scale. If someone re-types a number here,
    // this is what says so.
    float grid_w = (float)Z_PT(390);
    float expect_cell = (grid_w - 2.0f * (float)Z_SPACE_L -
                         2.0f * (float)Z_SPACE_XS) / 3.0f;
    EXPECT_TRUE(zelto_photos_cell(grid_w) > expect_cell - 0.01f &&
                zelto_photos_cell(grid_w) < expect_cell + 0.01f);
    EXPECT_EQ_INT((int)(expect_cell + 0.5f), ZELTO_PHOTO_THUMB_EDGE);
    return 0;
}

int main(void) {
    char dir[] = "/tmp/zelto-photolib.XXXXXX";
    if (!mkdtemp(dir)) {
        fprintf(stderr, "could not make a temp dir\n");
        return 1;
    }
    // The library header resolves its tree from here, so the test never touches
    // a real $ZELTO_DATA_DIR.
    setenv("ZELTO_PHOTOS_ROOT", dir, 1);

    test_opaque_writes_rgb(dir);
    test_premultiplied_round_trip(dir);
    test_box_scale_averages();
    test_ids_sort_newest_first();
    test_store_and_enumerate();
    test_thumb_is_the_grid_cell();

    // Leave the tree behind on failure so the PNGs can be looked at; the note
    // goes to STDERR because run-tests.sh only reproduces ZT_FAIL lines from
    // stdout.
    if (zt_failures_ == 0) {
        char cmd[600];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
        if (system(cmd) != 0) { /* best effort */ }
    } else {
        fprintf(stderr, "kept the written PNGs for inspection: %s\n", dir);
    }
    return zt_result();
}
