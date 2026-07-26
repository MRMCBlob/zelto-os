// test_image_region_luma — the measurement the wallpaper's ink is chosen by.
//
// WHAT IS UNDER TEST. P54 made the ink over the wallpaper DERIVED: the status
// bar's clock, the home captions, the home indicator and the lock clock ask what
// is actually behind them instead of reading a palette column, because the
// wallpaper is a photograph and does not change when the appearance does. All of
// that rests on one function, and if it measures the wrong pixels every surface
// above it is confidently wrong — which is exactly what happened twice while it
// was being written:
//
//   1. The first version measured the WHOLE IMAGE. Over the ten shipped
//      wallpapers that picks a different ink from the band's own mean on four of
//      them (spreads to 0.364), so it grew a vertical bound.
//   2. The second measured a full-width BAND, and the status-bar clock came out
//      washed out anyway: the wallpaper's top strip is dark on the right and
//      bright on the left, and the clock is on the left. So it grew a horizontal
//      bound too.
//
// Both were found by looking at a frame. This file is what makes them stay
// found — the bounds are asserted to actually bound, in both axes.
//
// NEGATIVE-TESTED: making the scan ignore c0/c1 (measuring full rows) fails the
// left/right assertions; making it ignore r0/r1 fails the top/bottom ones;
// returning the whole-image mean fails both.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zelto/ui.h>

#include "framework/ztest.h"

// The decoder and the encoder as TUs, the way every C test here takes the piece
// of the SDK it is about (test_contrast_tokens does this with theme.c). Nothing
// links libzelto: a test that needed the whole toolkit would need a compositor.
#include "image.c"
#include "image_write.c"

// A 64x64 PNG in four quadrants, so every bound has something to catch:
//   top-left BLACK      top-right WHITE
//   bottom-left WHITE   bottom-right BLACK
// A quadrant layout rather than a half/half one on purpose — a half/half image
// cannot tell a working horizontal bound from an ignored one.
#define DIM 64

static bool write_quadrants(const char *path) {
    uint32_t *px = malloc((size_t)DIM * DIM * sizeof(uint32_t));
    if (!px) {
        return false;
    }
    for (int y = 0; y < DIM; y++) {
        for (int x = 0; x < DIM; x++) {
            bool left = x < DIM / 2, top = y < DIM / 2;
            bool white = (top && !left) || (!top && left);
            px[y * DIM + x] = white ? 0xffffffffu : 0xff000000u;
        }
    }
    // PREMULTIPLIED: opaque white and opaque black are the same either way, which
    // is why this fixture does not have to care.
    bool ok = z_image_write_png(path, px, DIM, DIM,
                                (size_t)DIM * sizeof(uint32_t), true);
    free(px);
    return ok;
}

static void near(const char *what, float got, float want, float tol) {
    if (got >= want - tol && got <= want + tol) {
        return;
    }
    char g[64], w[64], msg[256];
    snprintf(g, sizeof(g), "%.4f", (double)got);
    snprintf(w, sizeof(w), "%.4f +/- %.3f", (double)want, (double)tol);
    snprintf(msg, sizeof(msg), "%s", what);
    zt_fail_(__FILE__, __LINE__, msg, w, g);
}

int main(void) {
    char path[512];
    const char *tmp = getenv("TMPDIR");
    snprintf(path, sizeof(path), "%s/zelto-region-luma.png",
             tmp && tmp[0] ? tmp : "/tmp");
    if (!write_quadrants(path)) {
        zt_fail_(__FILE__, __LINE__,
                 "could not write the fixture PNG, so nothing below is a test",
                 "a written file", "z_image_write_png failed");
        return zt_result();
    }

    // --- 1. each quadrant reads as itself ---------------------------------
    // Black is 0.0 and white is 1.0 in WCAG relative luminance, so these are
    // exact rather than approximate — a tolerance here would hide a bound that
    // is half working.
    near("the top-LEFT quadrant is black",
         z_image_region_luma(path, 0.0f, 0.0f, 0.5f, 0.5f), 0.0f, 0.001f);
    near("the top-RIGHT quadrant is white",
         z_image_region_luma(path, 0.5f, 0.0f, 1.0f, 0.5f), 1.0f, 0.001f);
    near("the bottom-LEFT quadrant is white",
         z_image_region_luma(path, 0.0f, 0.5f, 0.5f, 1.0f), 1.0f, 0.001f);
    near("the bottom-RIGHT quadrant is black",
         z_image_region_luma(path, 0.5f, 0.5f, 1.0f, 1.0f), 0.0f, 0.001f);

    // --- 2. THE POSITIVE CONTROL for the bounds ---------------------------
    // The whole image is half black and half white by construction, so its mean
    // is 0.5. If either bound were ignored, every quadrant above would return
    // this number instead — and 0.5 is not near 0.0 or 1.0, which is what makes
    // assertion 1 mean something.
    near("the whole image averages the four quadrants",
         z_image_region_luma(path, 0.0f, 0.0f, 1.0f, 1.0f), 0.5f, 0.01f);
    // And each bound ALONE must not be enough: a full-width top band spans one
    // black and one white quadrant, so it is 0.5 too. This is the exact shape of
    // the bug the horizontal bound was added for — the status-bar clock sitting
    // on the bright end of a strip whose mean says dark.
    near("a full-width top BAND is not the same as its left half",
         z_image_region_luma(path, 0.0f, 0.0f, 1.0f, 0.5f), 0.5f, 0.01f);
    near("a full-height left COLUMN is not the same as its top half",
         z_image_region_luma(path, 0.0f, 0.0f, 0.5f, 1.0f), 0.5f, 0.01f);

    // --- 3. degenerate and out-of-range input ------------------------------
    // These arrive from arithmetic on screen geometry (a band computed as
    // ZELTO_BAR_H / z_screen_height), so a zero-height strip or a fraction that
    // has run past 1.0 is a caller doing normal division, not a caller with a
    // bug. Every one of them must still return a usable number.
    near("a zero-height strip still measures the row it names",
         z_image_region_luma(path, 0.0f, 0.0f, 0.5f, 0.0f), 0.0f, 0.001f);
    near("bounds past the edges are clamped, not read past",
         z_image_region_luma(path, -2.0f, -2.0f, 9.0f, 9.0f), 0.5f, 0.01f);
    near("reversed bounds measure the same rectangle",
         z_image_region_luma(path, 0.5f, 0.0f, 0.0f, 0.5f), 0.0f, 0.001f);

    // --- 4. a path that does not decode -----------------------------------
    // -1 rather than 0, and the difference matters: 0 is a legitimate luminance
    // (a black wallpaper) and would send every caller to the light ink for a
    // wallpaper that is not there. The callers test for < 0.
    if (z_image_region_luma("/nonexistent/zelto/not-a-file.png", 0.0f, 0.0f,
                            1.0f, 1.0f) >= 0.0f) {
        zt_fail_(__FILE__, __LINE__,
                 "an image that does not decode must report -1, not a "
                 "luminance - 0 is a legitimate reading for a black wallpaper "
                 "and would be indistinguishable from a missing one",
                 "< 0", ">= 0");
    }

    remove(path);
    return zt_result();
}
