// libzelto text: shaping with HarfBuzz, rasterization with FreeType. One bundled
// face; a glyph atlas / GPU caching is Planned (docs/contributing/sdk-internals.md
// "Text"). If the font fails to load, text degrades gracefully (measured by a
// rough estimate, drawn as nothing) so the rest of the UI still renders.
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ft.h>

#include "internal.h"

struct ZText {
    FT_Library lib;
    FT_Face face;
    hb_font_t *hb_font;
    int cur_px;
};

ZText *z_text_open(const char *font_path) {
    ZText *t = calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }
    if (FT_Init_FreeType(&t->lib) != 0) {
        free(t);
        return NULL;
    }
    if (FT_New_Face(t->lib, font_path, 0, &t->face) != 0) {
        FT_Done_FreeType(t->lib);
        free(t);
        return NULL;  // caller tolerates NULL
    }
    t->hb_font = hb_ft_font_create_referenced(t->face);
    t->cur_px = 0;
    return t;
}

void z_text_close(ZText *t) {
    if (!t) {
        return;
    }
    if (t->hb_font) {
        hb_font_destroy(t->hb_font);
    }
    if (t->face) {
        FT_Done_Face(t->face);
    }
    if (t->lib) {
        FT_Done_FreeType(t->lib);
    }
    free(t);
}

// Set the working pixel size on the face (and tell HarfBuzz it changed).
static void set_px(ZText *t, int px) {
    if (px < 1) {
        px = 1;
    }
    if (t->cur_px == px) {
        return;
    }
    FT_Set_Pixel_Sizes(t->face, 0, (FT_UInt)px);
    hb_ft_font_changed(t->hb_font);
    t->cur_px = px;
}

// Shape `s` and run `glyph_cb` for each glyph; returns total advance width.
// glyph_cb may be NULL (measure only).
static float shape_line(ZText *t, const char *s, int px,
                        void (*glyph_cb)(ZText *, unsigned glyph,
                                         float x_off, float y_off, void *ud),
                        void *ud) {
    set_px(t, px);
    hb_buffer_t *buf = hb_buffer_create();
    hb_buffer_add_utf8(buf, s, -1, 0, -1);
    hb_buffer_guess_segment_properties(buf);
    hb_shape(t->hb_font, buf, NULL, 0);

    unsigned len = 0;
    hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, &len);
    hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, &len);

    float advance = 0.0f;
    for (unsigned i = 0; i < len; i++) {
        if (glyph_cb) {
            glyph_cb(t, info[i].codepoint, advance + pos[i].x_offset / 64.0f,
                     pos[i].y_offset / 64.0f, ud);
        }
        advance += pos[i].x_advance / 64.0f;
    }
    hb_buffer_destroy(buf);
    return advance;
}

float z_text_measure(ZText *t, const char *s, float size, float *ascent,
                     float *descent) {
    int px = (int)(size + 0.5f);
    if (!t || !t->face) {
        // Rough fallback so layout still allocates space.
        if (ascent) {
            *ascent = size * 0.8f;
        }
        if (descent) {
            *descent = size * 0.2f;
        }
        return (float)strlen(s) * size * 0.5f;
    }
    float adv = shape_line(t, s, px, NULL, NULL);
    if (ascent) {
        *ascent = t->face->size->metrics.ascender / 64.0f;
    }
    if (descent) {
        *descent = -t->face->size->metrics.descender / 64.0f;
    }
    return adv;
}

// --- rasterization --------------------------------------------------------
typedef struct {
    ZCanvas *canvas;
    float pen_x, baseline;
    ZColor color;
} DrawCtx;

static void blend_cover(ZCanvas *c, int x, int y, ZColor col, uint8_t cov) {
    if (x < 0 || y < 0 || x >= c->width || y >= c->height || cov == 0) {
        return;
    }
    uint32_t *dst = &c->pixels[y * c->stride_px + x];
    uint32_t d = *dst;
    uint32_t dr = (d >> 16) & 0xff, dg = (d >> 8) & 0xff, db = d & 0xff;
    // Effective coverage = glyph alpha * source alpha.
    uint32_t a = (uint32_t)cov * col.a / 255u;
    uint32_t r = (col.r * a + dr * (255 - a)) / 255u;
    uint32_t g = (col.g * a + dg * (255 - a)) / 255u;
    uint32_t b = (col.b * a + db * (255 - a)) / 255u;
    *dst = 0xff000000u | (r << 16) | (g << 8) | b;
}

static void draw_glyph(ZText *t, unsigned glyph, float x_off, float y_off,
                       void *ud) {
    DrawCtx *ctx = ud;
    if (FT_Load_Glyph(t->face, glyph, FT_LOAD_RENDER) != 0) {
        return;
    }
    FT_GlyphSlot g = t->face->glyph;
    FT_Bitmap *bm = &g->bitmap;
    int ox = (int)(ctx->pen_x + x_off + 0.5f) + g->bitmap_left;
    int oy = (int)(ctx->baseline - y_off + 0.5f) - g->bitmap_top;
    for (unsigned row = 0; row < bm->rows; row++) {
        for (unsigned col = 0; col < bm->width; col++) {
            uint8_t cov = bm->buffer[row * (unsigned)bm->pitch + col];
            blend_cover(ctx->canvas, ox + (int)col, oy + (int)row, ctx->color,
                        cov);
        }
    }
}

void z_text_draw(ZCanvas *canvas, const char *s, float size, ZColor color,
                 float pen_x, float pen_y) {
    ZText *t = canvas->text;
    if (!t || !t->face) {
        return;
    }
    int px = (int)(size + 0.5f);
    set_px(t, px);
    DrawCtx ctx = {
        .canvas = canvas,
        .pen_x = pen_x,
        .baseline = pen_y + t->face->size->metrics.ascender / 64.0f,
        .color = color,
    };
    shape_line(t, s, px, draw_glyph, &ctx);
}
