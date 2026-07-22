// libzelto image decode + cache. An Image() view names a file; this module
// decodes it once, into a premultiplied-ARGB8888 bitmap the renderer can blit
// straight over the surface, and caches it by path — the view tree is rebuilt
// every frame, so decoding per build would be unaffordable. PNGs go through
// libpng; ".svg" goes through a minimal in-house stroked-path rasterizer (the
// project ships stroke-style line icons, and a full SVG engine — nanosvg,
// librsvg — is a heavier dependency than the icon use case needs). See
// docs/contributing/sdk-internals.md.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

// --- premultiplied packing -------------------------------------------------
// The renderer's canvas stores each pixel as (A<<24)|(R<<16)|(G<<8)|B with the
// RGB already multiplied by A (see render.c). Bitmaps match that exactly so a
// blit is a straight source-over.
static uint32_t pack_premul(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    r = (r * a + 127u) / 255u;
    g = (g * a + 127u) / 255u;
    b = (b * a + 127u) / 255u;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

// --- cache -----------------------------------------------------------------
// A small global list, keyed by path. Single-threaded app loop, so no locking.
// Failed decodes are cached (ok=false) so a missing file is not retried each
// frame. Capacity-bounded: past the cap the oldest entry is evicted (icons
// number in the low dozens, so eviction effectively never fires).
#define Z_IMAGE_CACHE_MAX 64

static ZImage *g_cache_head;
static int g_cache_count;

static uint32_t *decode_png(const char *path, int *w, int *h);
static uint32_t *decode_svg(const char *path, int *w, int *h);

static bool has_ext(const char *path, const char *ext) {
    size_t lp = strlen(path), le = strlen(ext);
    if (lp < le) {
        return false;
    }
    return strcasecmp(path + lp - le, ext) == 0;
}

static ZImage *cache_find(const char *path) {
    for (ZImage *e = g_cache_head; e; e = e->next) {
        if (strcmp(e->path, path) == 0) {
            return e;
        }
    }
    return NULL;
}

static void cache_evict_oldest(void) {
    // The list is prepended-to, so the tail is the least recently inserted.
    if (!g_cache_head) {
        return;
    }
    ZImage *prev = NULL, *e = g_cache_head;
    while (e->next) {
        prev = e;
        e = e->next;
    }
    if (prev) {
        prev->next = NULL;
    } else {
        g_cache_head = NULL;
    }
    free(e->path);
    free(e->px);
    free(e);
    g_cache_count--;
}

const ZImage *z_image_get(const char *path) {
    if (!path) {
        return NULL;
    }
    ZImage *e = cache_find(path);
    if (e) {
        return e;
    }

    int w = 0, h = 0;
    uint32_t *px = has_ext(path, ".svg") ? decode_svg(path, &w, &h)
                                         : decode_png(path, &w, &h);

    if (g_cache_count >= Z_IMAGE_CACHE_MAX) {
        cache_evict_oldest();
    }
    e = calloc(1, sizeof(*e));
    if (!e) {
        free(px);
        return NULL;
    }
    e->path = strdup(path);
    if (px && w > 0 && h > 0) {
        e->px = px;
        e->w = w;
        e->h = h;
        e->ok = true;
    } else {
        free(px);
        e->ok = false;
        fprintf(stderr, "libzelto: image decode failed: %s\n", path);
    }
    e->next = g_cache_head;
    g_cache_head = e;
    g_cache_count++;
    return e;
}

bool z_image_adopt(const char *key, int w, int h, uint32_t *px) {
    if (!key || !px || w <= 0 || h <= 0) {
        free(px);
        return false;
    }
    // Replace in place when the key is already cached. A window snapshot is
    // refreshed every time its app is backgrounded, and the key is stable per
    // window, so this is the common path — evicting and re-inserting would let
    // the LRU drop some OTHER card's picture on every app switch.
    ZImage *e = cache_find(key);
    if (e) {
        free(e->px);
        e->px = px;
        e->w = w;
        e->h = h;
        e->ok = true;
        return true;
    }
    if (g_cache_count >= Z_IMAGE_CACHE_MAX) {
        cache_evict_oldest();
    }
    e = calloc(1, sizeof(*e));
    if (!e) {
        free(px);
        return false;
    }
    e->path = strdup(key);
    e->px = px;
    e->w = w;
    e->h = h;
    e->ok = true;
    e->next = g_cache_head;
    g_cache_head = e;
    g_cache_count++;
    return true;
}

bool z_image_loads(const char *path) {
    const ZImage *e = z_image_get(path);
    return e && e->ok;
}

bool z_image_intrinsic(const char *path, int *w, int *h) {
    const ZImage *e = z_image_get(path);
    if (!e || !e->ok) {
        return false;
    }
    if (w) { *w = e->w; }
    if (h) { *h = e->h; }
    return true;
}

// --- PNG (libpng) ----------------------------------------------------------
#include <png.h>

// Decode `path` to a freshly malloc'd premultiplied-ARGB8888 buffer (w*h
// uint32). Any colour type is normalised to 8-bit RGBA first (palette expanded,
// grey promoted, tRNS honoured, 16-bit stripped, opaque alpha added), then each
// pixel is premultiplied + packed. NULL on any failure.
static uint32_t *decode_png(const char *path, int *out_w, int *out_h) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    unsigned char sig[8];
    if (fread(sig, 1, 8, fp) != 8 || png_sig_cmp(sig, 0, 8)) {
        fclose(fp);
        return NULL;
    }
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL,
                                             NULL);
    if (!png) {
        fclose(fp);
        return NULL;
    }
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        fclose(fp);
        return NULL;
    }

    uint32_t *out = NULL;
    png_bytep *rows = NULL;
    if (setjmp(png_jmpbuf(png))) {
        // libpng error handler: clean up and fail.
        free(out);
        free(rows);
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return NULL;
    }

    png_init_io(png, fp);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);

    png_uint_32 w = png_get_image_width(png, info);
    png_uint_32 h = png_get_image_height(png, info);
    int bit_depth = png_get_bit_depth(png, info);
    int color_type = png_get_color_type(png, info);

    // Normalise everything to 8-bit RGBA.
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png);
    }
    if (png_get_valid(png, info, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png);
    }
    if (bit_depth == 16) {
        png_set_strip_16(png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_add_alpha(png, 0xffff, PNG_FILLER_AFTER);
    }
    png_read_update_info(png, info);

    out = malloc((size_t)w * h * sizeof(uint32_t));
    rows = malloc((size_t)h * sizeof(png_bytep));
    png_bytep buf = out ? malloc((size_t)w * h * 4) : NULL;
    if (!out || !rows || !buf) {
        free(buf);
        png_longjmp(png, 1);
    }
    for (png_uint_32 y = 0; y < h; y++) {
        rows[y] = buf + (size_t)y * w * 4;
    }
    png_read_image(png, rows);
    png_read_end(png, NULL);

    for (png_uint_32 y = 0; y < h; y++) {
        const png_bytep row = rows[y];
        for (png_uint_32 x = 0; x < w; x++) {
            const png_bytep p = row + (size_t)x * 4;
            out[(size_t)y * w + x] = pack_premul(p[0], p[1], p[2], p[3]);
        }
    }
    free(buf);
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    *out_w = (int)w;
    *out_h = (int)h;
    return out;
}

// --- SVG (minimal in-house stroked-path rasterizer) ------------------------
// Handles exactly what the bundled icon set uses: absolute M/L/H/V/C/Z path
// commands, stroked (round caps/joins) at a uniform width, drawn in a single
// ink. Fills, gradients, transforms and relative/arc commands are out of scope
// (a full SVG engine is the documented upgrade path). Every stroke is rendered
// in one near-white ink so a two-tone line icon reads as a clean monochrome
// glyph over the launcher's coloured tile.

#define SVG_TARGET 256.0f     // longest side of the rasterized bitmap (px)
#define SVG_MAX_PTS 4096      // flattened points per subpath cap

// Ink: opaque near-white (Z_COLOR_TEXT), premultiplied (a=255 so no change).
static uint32_t svg_ink(float coverage) {
    uint32_t a = (uint32_t)(coverage * 255.0f + 0.5f);
    if (a > 255u) { a = 255u; }
    return pack_premul(0xe6, 0xec, 0xf2, a);
}

// Read the next float out of an SVG path/number string starting at *pp,
// skipping separators (whitespace + commas). Returns false at end of data.
static bool svg_next_num(const char **pp, float *out) {
    const char *p = *pp;
    while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    if (!*p || (!isdigit((unsigned char)*p) && *p != '-' && *p != '+' &&
                *p != '.')) {
        *pp = p;
        return false;
    }
    char *end = NULL;
    *out = strtof(p, &end);
    if (end == p) {
        *pp = p;
        return false;
    }
    *pp = end;
    return true;
}

// A coverage accumulator (max-blended so overlapping capsules at joins don't
// double up) sized w*h, one float per pixel.
typedef struct SvgCanvas {
    float *cov;
    int w, h;
    float scale;   // user-units -> pixels
} SvgCanvas;

static void cov_max(SvgCanvas *sc, int x, int y, float c) {
    if (x < 0 || y < 0 || x >= sc->w || y >= sc->h) {
        return;
    }
    float *slot = &sc->cov[y * sc->w + x];
    if (c > *slot) {
        *slot = c;
    }
}

// Stroke one segment (in pixel space) as a round-capped capsule of radius `r`,
// antialiased over a 1px band via the point-to-segment distance.
static void stroke_segment(SvgCanvas *sc, float ax, float ay, float bx,
                           float by, float r) {
    float minx = (ax < bx ? ax : bx) - r - 1.0f;
    float maxx = (ax > bx ? ax : bx) + r + 1.0f;
    float miny = (ay < by ? ay : by) - r - 1.0f;
    float maxy = (ay > by ? ay : by) + r + 1.0f;
    int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx);
    int y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
    float dx = bx - ax, dy = by - ay;
    float len2 = dx * dx + dy * dy;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float t = 0.0f;
            if (len2 > 1e-6f) {
                t = ((px - ax) * dx + (py - ay) * dy) / len2;
                if (t < 0.0f) { t = 0.0f; }
                if (t > 1.0f) { t = 1.0f; }
            }
            float cx = ax + t * dx, cy = ay + t * dy;
            float d = sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
            float cov = r + 0.5f - d;   // 1px AA ramp at the edge
            if (cov <= 0.0f) {
                continue;
            }
            if (cov > 1.0f) { cov = 1.0f; }
            cov_max(sc, x, y, cov);
        }
    }
}

// Flatten a cubic Bézier into the point buffer (fixed subdivision — icons are
// small, so a modest count is smooth enough).
static void flatten_cubic(float *pts, int *n, float x0, float y0, float x1,
                          float y1, float x2, float y2, float x3, float y3) {
    const int steps = 18;
    for (int i = 1; i <= steps && *n < SVG_MAX_PTS; i++) {
        float t = (float)i / (float)steps;
        float u = 1.0f - t;
        float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
        pts[(*n) * 2] = a * x0 + b * x1 + c * x2 + d * x3;
        pts[(*n) * 2 + 1] = a * y0 + b * y1 + c * y2 + d * y3;
        (*n)++;
    }
}

// Parse a path `d` string into subpaths of flattened points and stroke each.
static void stroke_path(SvgCanvas *sc, const char *d, float r) {
    static float pts[SVG_MAX_PTS * 2];
    const char *p = d;
    float cx = 0.0f, cy = 0.0f;      // current point (user units)
    float sx = 0.0f, sy = 0.0f;      // subpath start
    int n = 0;                        // points in the current subpath
    char cmd = 0;

    // Emit the accumulated subpath's segments (pixel space), then reset it.
    // `close` appends the closing segment back to the subpath start.
    // (Declared as a lambda-free helper via an inline flush below.)
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' ||
               *p == '\r') {
            p++;
        }
        if (!*p) {
            break;
        }
        if (isalpha((unsigned char)*p)) {
            cmd = *p++;
        }
        float a, b, c, e, f, g;
        switch (cmd) {
        case 'M':
            if (!svg_next_num(&p, &a) || !svg_next_num(&p, &b)) {
                p++;
                break;
            }
            // Starting a new subpath: flush the old one (open, no close).
            for (int i = 1; i < n; i++) {
                stroke_segment(sc, pts[(i - 1) * 2] * sc->scale,
                               pts[(i - 1) * 2 + 1] * sc->scale,
                               pts[i * 2] * sc->scale, pts[i * 2 + 1] * sc->scale,
                               r);
            }
            n = 0;
            cx = sx = a;
            cy = sy = b;
            pts[0] = cx;
            pts[1] = cy;
            n = 1;
            cmd = 'L';   // subsequent implicit coords are line-tos
            break;
        case 'L':
            if (!svg_next_num(&p, &a) || !svg_next_num(&p, &b)) {
                p++;
                break;
            }
            cx = a;
            cy = b;
            if (n < SVG_MAX_PTS) {
                pts[n * 2] = cx;
                pts[n * 2 + 1] = cy;
                n++;
            }
            break;
        case 'H':
            if (!svg_next_num(&p, &a)) {
                p++;
                break;
            }
            cx = a;
            if (n < SVG_MAX_PTS) {
                pts[n * 2] = cx;
                pts[n * 2 + 1] = cy;
                n++;
            }
            break;
        case 'V':
            if (!svg_next_num(&p, &a)) {
                p++;
                break;
            }
            cy = a;
            if (n < SVG_MAX_PTS) {
                pts[n * 2] = cx;
                pts[n * 2 + 1] = cy;
                n++;
            }
            break;
        case 'C':
            if (!svg_next_num(&p, &a) || !svg_next_num(&p, &b) ||
                !svg_next_num(&p, &c) || !svg_next_num(&p, &e) ||
                !svg_next_num(&p, &f) || !svg_next_num(&p, &g)) {
                p++;
                break;
            }
            flatten_cubic(pts, &n, cx, cy, a, b, c, e, f, g);
            cx = f;
            cy = g;
            break;
        case 'Z':
        case 'z':
            if (n > 0 && n < SVG_MAX_PTS) {
                pts[n * 2] = sx;
                pts[n * 2 + 1] = sy;
                n++;
            }
            cx = sx;
            cy = sy;
            break;
        default:
            p++;   // unsupported command: skip a char and keep scanning
            break;
        }
    }
    // Flush the final subpath.
    for (int i = 1; i < n; i++) {
        stroke_segment(sc, pts[(i - 1) * 2] * sc->scale,
                       pts[(i - 1) * 2 + 1] * sc->scale,
                       pts[i * 2] * sc->scale, pts[i * 2 + 1] * sc->scale, r);
    }
}

// Pull a float attribute value (e.g. width="24") out of the whole SVG text.
static bool svg_attr_num(const char *svg, const char *key, float *out) {
    const char *k = strstr(svg, key);
    if (!k) {
        return false;
    }
    const char *p = k + strlen(key);
    while (*p == '=' || *p == '"' || *p == '\'' || *p == ' ') {
        p++;
    }
    return svg_next_num(&p, out);
}

// Decode an SVG file: read the viewBox (or width/height) to establish the user
// coordinate space, then stroke every <path d="..."> in it. Returns a premul-
// ARGB buffer of SVG_TARGET on its longest side. NULL on failure.
static uint32_t *decode_svg(const char *path, int *out_w, int *out_h) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz <= 0 || sz > 1 << 20) {   // 1 MiB sanity cap
        fclose(fp);
        return NULL;
    }
    char *svg = malloc((size_t)sz + 1);
    if (!svg) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);   // ftell(SEEK_END) left the cursor at EOF; read from the start
    size_t rd = fread(svg, 1, (size_t)sz, fp);
    fclose(fp);
    svg[rd] = '\0';

    // Coordinate space: prefer viewBox "minx miny w h", else width/height.
    float vbw = 24.0f, vbh = 24.0f;
    const char *vb = strstr(svg, "viewBox");
    if (vb) {
        const char *p = vb + 7;
        while (*p == '=' || *p == '"' || *p == '\'' || *p == ' ') {
            p++;
        }
        float minx, miny, w, h;
        if (svg_next_num(&p, &minx) && svg_next_num(&p, &miny) &&
            svg_next_num(&p, &w) && svg_next_num(&p, &h) && w > 0 && h > 0) {
            vbw = w;
            vbh = h;
        }
    } else {
        float w, h;
        if (svg_attr_num(svg, "width", &w) && svg_attr_num(svg, "height", &h) &&
            w > 0 && h > 0) {
            vbw = w;
            vbh = h;
        }
    }

    float longest = vbw > vbh ? vbw : vbh;
    float scale = SVG_TARGET / longest;
    int ow = (int)(vbw * scale + 0.5f), oh = (int)(vbh * scale + 0.5f);
    if (ow < 1) { ow = 1; }
    if (oh < 1) { oh = 1; }

    SvgCanvas sc = {.cov = calloc((size_t)ow * oh, sizeof(float)),
                    .w = ow, .h = oh, .scale = scale};
    if (!sc.cov) {
        free(svg);
        return NULL;
    }

    // Stroke every path. stroke-width defaults to 2 (the icon set's convention);
    // honour a per-path stroke-width if present.
    const char *p = svg;
    while ((p = strstr(p, "<path")) != NULL) {
        const char *tag_end = strchr(p, '>');
        if (!tag_end) {
            break;
        }
        // Bound the attribute search to this one tag.
        size_t taglen = (size_t)(tag_end - p);
        char *tag = malloc(taglen + 1);
        if (!tag) {
            break;
        }
        memcpy(tag, p, taglen);
        tag[taglen] = '\0';

        float sw = 2.0f;
        svg_attr_num(tag, "stroke-width", &sw);
        float r = sw * scale / 2.0f;
        if (r < 0.5f) { r = 0.5f; }

        const char *dattr = strstr(tag, "d=");
        if (dattr) {
            const char *dp = dattr + 2;
            char q = *dp;
            if (q == '"' || q == '\'') {
                dp++;
                const char *dend = strchr(dp, q);
                if (dend) {
                    size_t dl = (size_t)(dend - dp);
                    char *d = malloc(dl + 1);
                    if (d) {
                        memcpy(d, dp, dl);
                        d[dl] = '\0';
                        stroke_path(&sc, d, r);
                        free(d);
                    }
                }
            }
        }
        free(tag);
        p = tag_end + 1;
    }
    free(svg);

    uint32_t *out = malloc((size_t)ow * oh * sizeof(uint32_t));
    if (!out) {
        free(sc.cov);
        return NULL;
    }
    for (int i = 0; i < ow * oh; i++) {
        out[i] = svg_ink(sc.cov[i]);
    }
    free(sc.cov);
    *out_w = ow;
    *out_h = oh;
    return out;
}
