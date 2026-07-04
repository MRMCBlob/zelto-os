// libzelto text: shaping with HarfBuzz, rasterization with FreeType. One bundled
// face; a glyph atlas / GPU caching is Planned (docs/contributing/sdk-internals.md
// "Text"). If the font fails to load, text degrades gracefully (measured by a
// rough estimate, drawn as nothing) so the rest of the UI still renders.
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_OUTLINE_H
#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ft.h>

#include "internal.h"

struct ZText {
    FT_Library lib;
    FT_Face face;
    hb_font_t *hb_font;
    int cur_px;
    // Variable-font weight axis (Satoshi ships a `wght` axis). When present we
    // pin the design coordinate per ZWeight for a REAL weight (not faux-bold);
    // absent (a static face) we fall back to emboldening the outline in draw.
    bool has_wght;
    int wght_index;            // which var axis is `wght`
    FT_Fixed wght_min, wght_max, wght_def;   // 16.16 design range
    FT_Fixed coords[16];       // current design coords for all axes
    unsigned n_axes;
    ZWeight cur_weight;
};

// Map a ZWeight to a design value on the wght axis (CSS-style 400..700), clamped
// to the face's advertised range.
static FT_Fixed wght_value(const struct ZText *t, ZWeight w) {
    long v = 400;
    switch (w) {
    case Z_WEIGHT_REGULAR:  v = 400; break;
    case Z_WEIGHT_MEDIUM:   v = 500; break;
    case Z_WEIGHT_SEMIBOLD: v = 600; break;
    case Z_WEIGHT_BOLD:     v = 700; break;
    }
    FT_Fixed f = (FT_Fixed)(v << 16);
    if (f < t->wght_min) { f = t->wght_min; }
    if (f > t->wght_max) { f = t->wght_max; }
    return f;
}

// Discover a `wght` variation axis, if the face is a variable font.
static void probe_variations(ZText *t) {
    t->has_wght = false;
    if (!FT_HAS_MULTIPLE_MASTERS(t->face)) {
        return;
    }
    FT_MM_Var *mm = NULL;
    if (FT_Get_MM_Var(t->face, &mm) != 0 || !mm) {
        return;
    }
    t->n_axes = mm->num_axis < 16 ? mm->num_axis : 16;
    for (unsigned i = 0; i < t->n_axes; i++) {
        t->coords[i] = mm->axis[i].def;
        // The wght axis is tagged 'wght' (0x77676874).
        if (mm->axis[i].tag == FT_MAKE_TAG('w', 'g', 'h', 't')) {
            t->has_wght = true;
            t->wght_index = (int)i;
            t->wght_min = mm->axis[i].minimum;
            t->wght_max = mm->axis[i].maximum;
            t->wght_def = mm->axis[i].def;
        }
    }
    FT_Done_MM_Var(t->lib, mm);
}

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
    probe_variations(t);
    t->hb_font = hb_ft_font_create_referenced(t->face);
    t->cur_px = 0;
    t->cur_weight = Z_WEIGHT_REGULAR;
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

// Select the variable-font weight instance (no-op on a static face — draw_glyph
// then emboldens instead). Changing design coords reshapes glyphs + advances, so
// HarfBuzz is told it changed.
static void set_weight(ZText *t, ZWeight w) {
    if (t->cur_weight == w) {
        return;
    }
    t->cur_weight = w;
    if (t->has_wght) {
        t->coords[t->wght_index] = wght_value(t, w);
        FT_Set_Var_Design_Coordinates(t->face, t->n_axes, t->coords);
        hb_ft_font_changed(t->hb_font);
    }
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
static float shape_line(ZText *t, const char *s, int px, ZWeight weight,
                        void (*glyph_cb)(ZText *, unsigned glyph,
                                         float x_off, float y_off, void *ud),
                        void *ud) {
    set_weight(t, weight);
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

float z_text_measure(ZText *t, const char *s, float size, ZWeight weight,
                     float *ascent, float *descent) {
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
    float adv = shape_line(t, s, px, weight, NULL, NULL);
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
    if (x < c->clip_x0 || y < c->clip_y0 || x >= c->clip_x1 ||
        y >= c->clip_y1 || cov == 0) {
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
    // Variable face: the wght instance is already selected, render straight.
    // Static face + a heavier weight: synth the bold by emboldening the outline.
    if (!t->has_wght && t->cur_weight > Z_WEIGHT_REGULAR) {
        if (FT_Load_Glyph(t->face, glyph, FT_LOAD_DEFAULT) != 0) {
            return;
        }
        FT_GlyphSlot gs = t->face->glyph;
        if (gs->format == FT_GLYPH_FORMAT_OUTLINE) {
            float f = t->cur_weight == Z_WEIGHT_MEDIUM     ? 0.5f
                      : t->cur_weight == Z_WEIGHT_SEMIBOLD ? 0.9f
                                                           : 1.4f;
            FT_Outline_Embolden(&gs->outline, (FT_Pos)((float)t->cur_px * f));
        }
        if (FT_Render_Glyph(gs, FT_RENDER_MODE_NORMAL) != 0) {
            return;
        }
    } else if (FT_Load_Glyph(t->face, glyph, FT_LOAD_RENDER) != 0) {
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

void z_text_draw(ZCanvas *canvas, const char *s, float size, ZWeight weight,
                 ZColor color, float pen_x, float pen_y) {
    ZText *t = canvas->text;
    if (!t || !t->face) {
        return;
    }
    int px = (int)(size + 0.5f);
    set_weight(t, weight);
    set_px(t, px);
    DrawCtx ctx = {
        .canvas = canvas,
        .pen_x = pen_x,
        .baseline = pen_y + t->face->size->metrics.ascender / 64.0f,
        .color = color,
    };
    shape_line(t, s, px, weight, draw_glyph, &ctx);
}
