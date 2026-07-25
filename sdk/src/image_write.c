// libzelto image ENCODE — the other half of image.c, and the piece the OS has
// never had.
//
// WHY THIS EXISTS. Every image in Zelto until now came from the repo: an app
// icon, a wallpaper, an SVG mark. libpng was linked for DECODE only, so the
// system could display a picture and could not make one. That is the whole
// reason the OS has no screenshots, no camera roll and no user content — not a
// missing UI, a missing encoder. P42's per-toplevel capture reads pixels off a
// client buffer and hands them to another process as a sealed memfd, which
// proves the read; nothing has ever put pixels on disk.
//
// WHAT IT WRITES. An 8-bit-per-channel PNG, RGB when every pixel is opaque and
// RGBA when any is not. The opacity scan is not an optimisation flag a caller
// can get wrong: a screenshot is opaque by construction and would otherwise
// carry a whole redundant channel (a 720x1440 frame is 25% smaller as RGB),
// while a captured surface with real translucency must keep it. Deciding by
// measurement rather than by parameter is the same rule the rest of this
// codebase applies to constants.
//
// PREMULTIPLICATION IS THE TRAP HERE. The renderer's canvas and image.c's
// decoded bitmaps store PREMULTIPLIED ARGB (see pack_premul in image.c); a
// wl_shm client buffer and wlr-screencopy's output are STRAIGHT. PNG is defined
// as straight alpha. So the caller must say which it holds, and a wrong answer
// is invisible on an opaque image and wrong only where alpha < 255 — i.e. it
// would ship looking fine. Hence an explicit parameter with no default.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <png.h>

#include "internal.h"

// True when no pixel in the image has alpha < 255. Read on the packed word so
// this costs one pass over the source and no unpacking.
static bool all_opaque(const uint32_t *src, int w, int h, size_t stride) {
    size_t pitch = stride / 4;
    for (int y = 0; y < h; y++) {
        const uint32_t *row = src + (size_t)y * pitch;
        for (int x = 0; x < w; x++) {
            if ((row[x] >> 24) < 0xffu) {
                return false;
            }
        }
    }
    return true;
}

// Un-premultiply one channel: c_straight = c_premul * 255 / a. `a` is never zero
// here (the caller checks), and the result is clamped because a premultiplied
// buffer produced by a lossy path can carry c > a, which would otherwise wrap.
static inline uint8_t unpremul(uint32_t c, uint32_t a) {
    uint32_t v = (c * 255u + a / 2u) / a;
    return (uint8_t)(v > 255u ? 255u : v);
}

bool z_image_write_png(const char *path, const void *pixels, int w, int h,
                       size_t stride, bool premultiplied) {
    if (!path || !pixels || w <= 0 || h <= 0 || stride < (size_t)w * 4) {
        return false;
    }
    const uint32_t *src = pixels;
    // volatile: both live across the setjmp below, and gcc's -Wclobbered (an
    // error here) is right to say a register copy would not survive the longjmp.
    volatile bool opaque = all_opaque(src, w, h, stride);
    volatile int channels = opaque ? 3 : 4;

    // Write to a sibling temp file and rename into place. A photo library is
    // enumerated by another process (the Photos grid rebuilds on every frame),
    // and a half-written PNG that is already listed decodes as a failure and
    // gets CACHED as one by image.c — so a torn write would leave a permanently
    // blank tile that only a reboot clears. rename(2) within a directory is
    // atomic, so a listing sees the file either not at all or complete.
    char tmp[1024];
    int m = snprintf(tmp, sizeof(tmp), "%s.part", path);
    if (m <= 0 || (size_t)m >= sizeof(tmp)) {
        return false;
    }

    FILE *fp = fopen(tmp, "wb");
    if (!fp) {
        return false;
    }
    png_structp png =
        png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        fclose(fp);
        remove(tmp);
        return false;
    }
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, NULL);
        fclose(fp);
        remove(tmp);
        return false;
    }
    uint8_t *row = malloc((size_t)w * (size_t)(int)channels);
    if (!row) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        remove(tmp);
        return false;
    }
    // libpng reports errors by longjmp; everything freed below must already be
    // allocated at this point (it is), or the unwind leaks it.
    if (setjmp(png_jmpbuf(png))) {
        free(row);
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        remove(tmp);
        return false;
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info, (png_uint_32)w, (png_uint_32)h, 8,
                 opaque ? PNG_COLOR_TYPE_RGB : PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    size_t pitch = stride / 4;
    for (int y = 0; y < h; y++) {
        const uint32_t *in = src + (size_t)y * pitch;
        uint8_t *out = row;
        for (int x = 0; x < w; x++) {
            uint32_t p = in[x];
            uint32_t a = (p >> 24) & 0xffu;
            uint32_t r = (p >> 16) & 0xffu;
            uint32_t g = (p >> 8) & 0xffu;
            uint32_t b = p & 0xffu;
            if (premultiplied && a > 0 && a < 255) {
                r = unpremul(r, a);
                g = unpremul(g, a);
                b = unpremul(b, a);
            } else if (premultiplied && a == 0) {
                r = g = b = 0;
            }
            *out++ = (uint8_t)r;
            *out++ = (uint8_t)g;
            *out++ = (uint8_t)b;
            if (!opaque) {
                *out++ = (uint8_t)a;
            }
        }
        png_write_row(png, row);
    }
    png_write_end(png, NULL);

    free(row);
    png_destroy_write_struct(&png, &info);
    // fflush before the rename so the bytes are in the file, not in stdio.
    fflush(fp);
    fclose(fp);

    if (rename(tmp, path) != 0) {
        remove(tmp);
        return false;
    }
    return true;
}

// --- box downscale ---------------------------------------------------------
//
// A thumbnail is a SECOND FILE, not a re-decode of the full image, and the
// reasoning is worth stating because P25 made the opposite call deliberately.
// The wallpaper picker shares ONE cache entry between its thumbnail and the
// full-screen wallpaper: about ten assets, and a downscaled decode would have
// DUPLICATED an already-warm bitmap. A photo library inverts every term of
// that. There is no bound on how many photos exist, the grid shows dozens at
// once, and a 720x1440 frame is ~4 MB decoded — thirty of them is 120 MB
// against image.c's 64-entry cache, which evicts by age and would thrash. So
// the writer, which already holds the pixels, emits a small sibling file; the
// grid loads that, the viewer loads the original, and the two are different
// paths so they are different cache entries by construction.
//
// Box filter (average the source block) rather than nearest: the same choice
// and the same reason as the compositor's halve() — dropping pixels destroys
// small text, and a screenshot is mostly small text.
uint32_t *z_image_box_scale(const void *pixels, int sw, int sh, size_t stride,
                            int dw, int dh) {
    if (!pixels || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 ||
        stride < (size_t)sw * 4) {
        return NULL;
    }
    const uint32_t *src = pixels;
    uint32_t *dst = calloc((size_t)dw * (size_t)dh, 4);
    if (!dst) {
        return NULL;
    }
    size_t pitch = stride / 4;
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((int64_t)y * sh / dh);
        int y1 = (int)((int64_t)(y + 1) * sh / dh);
        if (y1 <= y0) {
            y1 = y0 + 1;
        }
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((int64_t)x * sw / dw);
            int x1 = (int)((int64_t)(x + 1) * sw / dw);
            if (x1 <= x0) {
                x1 = x0 + 1;
            }
            uint32_t a = 0, r = 0, g = 0, b = 0, n = 0;
            for (int sy = y0; sy < y1 && sy < sh; sy++) {
                const uint32_t *in = src + (size_t)sy * pitch;
                for (int sx = x0; sx < x1 && sx < sw; sx++) {
                    uint32_t p = in[sx];
                    a += (p >> 24) & 0xffu;
                    r += (p >> 16) & 0xffu;
                    g += (p >> 8) & 0xffu;
                    b += p & 0xffu;
                    n++;
                }
            }
            if (n == 0) {
                n = 1;
            }
            dst[(size_t)y * dw + x] =
                ((a / n) << 24) | ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    }
    return dst;
}
