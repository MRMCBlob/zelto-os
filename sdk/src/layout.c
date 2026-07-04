// libzelto layout: a single-pass, constraint-light stack/flex layout. Children
// report a desired size (measure), then each parent distributes free main-axis
// space to flexible children and aligns the cross axis (arrange).
// See docs/guides/layout.md and docs/contributing/sdk-internals.md.
#include "internal.h"

static float maxf(float a, float b) { return a > b ? a : b; }

// Compute each node's desired (content + padding) size into node->w / node->h.
static void measure(ZView n, ZText *text) {
    for (int i = 0; i < n->n_children; i++) {
        measure(n->children[i], text);
    }

    float pad2 = 2.0f * n->padding;
    float dw = 0.0f, dh = 0.0f;

    switch (n->kind) {
    case Z_K_RECT:
    case Z_K_SPACER:
    case Z_K_STROKE:
        // A vector mark has no intrinsic size; it fills whatever Frame() gives it.
        dw = n->fixed_w;
        dh = n->fixed_h;
        break;
    case Z_K_SCROLL:
        // A viewport sizes to its parent (fills); content size is the child's.
        dw = 0.0f;
        dh = 0.0f;
        break;
    case Z_K_TEXT: {
        float ascent = 0.0f, descent = 0.0f;
        dw = z_text_measure(text, n->text, n->font_size, n->weight, &ascent,
                            &descent);
        dh = ascent + descent;
        break;
    }
    case Z_K_IMAGE: {
        // Intrinsic pixel size when no fixed frame is given; a Frame() around
        // the Image (the usual case for an app icon) overrides both below.
        int iw = 0, ih = 0;
        if (n->img_path && z_image_intrinsic(n->img_path, &iw, &ih)) {
            dw = (float)iw;
            dh = (float)ih;
        }
        break;
    }
    case Z_K_STACK: {
        if (n->abs_children) {
            // Virtualised list content: full virtual height, fills width.
            dw = 0.0f;
            dh = n->content_h;
            break;
        }
        if (n->axis == Z_AXIS_DEPTH) {
            for (int i = 0; i < n->n_children; i++) {
                dw = maxf(dw, n->children[i]->w);
                dh = maxf(dh, n->children[i]->h);
            }
        } else {
            bool horiz = (n->axis == Z_AXIS_HORIZONTAL);
            float main = 0.0f, cross = 0.0f;
            for (int i = 0; i < n->n_children; i++) {
                ZView c = n->children[i];
                main += horiz ? c->w : c->h;
                cross = maxf(cross, horiz ? c->h : c->w);
            }
            if (n->n_children > 1) {
                main += n->spacing * (float)(n->n_children - 1);
            }
            dw = horiz ? main : cross;
            dh = horiz ? cross : main;
        }
        break;
    }
    }

    if (n->fixed_w > 0.0f) {
        dw = n->fixed_w;
    }
    if (n->fixed_h > 0.0f) {
        dh = n->fixed_h;
    }
    n->w = dw + pad2;
    n->h = dh + pad2;
}

static float align_offset(ZAlign align, float free) {
    switch (align) {
    case Z_ALIGN_CENTER:
        return free / 2.0f;
    case Z_ALIGN_TRAILING:
        return free;
    case Z_ALIGN_LEADING:
    default:
        return 0.0f;
    }
}

// Assign final frames top-down. Reads children's desired sizes (from measure)
// before overwriting them.
static void arrange(ZView n, float x, float y, float w, float h) {
    // Offset (drag / screen transition) shifts this node and its whole subtree.
    x += n->off_x;
    y += n->off_y;
    n->x = x;
    n->y = y;
    n->w = w;
    n->h = h;

    float pad = n->padding;
    float ix = x + pad, iy = y + pad;
    float iw = w - 2.0f * pad, ih = h - 2.0f * pad;

    // Scroll viewport: lay the content out at its natural height, translated up
    // by the offset, and record the metrics the fling/clamp logic reads back.
    if (n->kind == Z_K_SCROLL) {
        if (n->n_children == 0) {
            return;
        }
        ZView c = n->children[0];
        float content_h = c->content_h > 0.0f ? c->content_h : c->h;
        if (n->scroll) {
            n->scroll->viewport_h = h;
            n->scroll->content_h = content_h;
            float max = content_h - h;
            if (max < 0.0f) {
                max = 0.0f;
            }
            if (n->scroll->offset > max) {
                n->scroll->offset = max;
            }
            if (n->scroll->offset < 0.0f) {
                n->scroll->offset = 0.0f;
            }
        }
        float off = n->scroll ? n->scroll->offset : 0.0f;
        arrange(c, x, y - off, w, content_h);
        return;
    }

    if (n->kind != Z_K_STACK || n->n_children == 0) {
        return;
    }

    // Absolutely positioned children (list rows): place each at its layout_y.
    if (n->abs_children) {
        for (int i = 0; i < n->n_children; i++) {
            ZView c = n->children[i];
            float cw = c->fixed_w > 0.0f ? c->fixed_w : iw;
            float ch = c->fixed_h > 0.0f ? c->fixed_h : c->h;
            arrange(c, ix, y + c->layout_y, cw, ch);
        }
        return;
    }

    if (n->axis == Z_AXIS_DEPTH) {
        for (int i = 0; i < n->n_children; i++) {
            ZView c = n->children[i];
            float cw = c->w > 0.0f ? c->w : iw;
            float ch = c->h > 0.0f ? c->h : ih;
            if (c->fill) {
                cw = iw;
                ch = ih;
            }
            arrange(c, ix + (iw - cw) / 2.0f, iy + (ih - ch) / 2.0f, cw, ch);
        }
        return;
    }

    bool horiz = (n->axis == Z_AXIS_HORIZONTAL);
    float inner_main = horiz ? iw : ih;
    float inner_cross = horiz ? ih : iw;

    float total_main = 0.0f, total_grow = 0.0f;
    for (int i = 0; i < n->n_children; i++) {
        ZView c = n->children[i];
        total_main += horiz ? c->w : c->h;
        total_grow += c->grow;
    }
    if (n->n_children > 1) {
        total_main += n->spacing * (float)(n->n_children - 1);
    }
    float freedom = inner_main - total_main;
    if (freedom < 0.0f) {
        freedom = 0.0f;
    }

    float pos = horiz ? ix : iy;
    for (int i = 0; i < n->n_children; i++) {
        ZView c = n->children[i];
        float c_main = (horiz ? c->w : c->h);
        if (total_grow > 0.0f) {
            c_main += freedom * c->grow / total_grow;
        }
        // Cross axis: size to content, or fill the parent. Nested stacks fill
        // the cross axis (unless given a fixed cross size); leaves that left
        // their cross dimension unset (0) fill too.
        float c_cross = horiz ? c->h : c->w;
        bool fixed_cross = horiz ? (c->fixed_h > 0.0f) : (c->fixed_w > 0.0f);
        if ((c->kind == Z_K_STACK && !fixed_cross) || c->kind == Z_K_SCROLL ||
            c_cross <= 0.0f || c_cross > inner_cross) {
            c_cross = inner_cross;
        }
        if (c->fill) {
            c_main = inner_main;
            c_cross = inner_cross;
        }
        float cross_off = align_offset(n->align, inner_cross - c_cross);

        if (horiz) {
            arrange(c, pos, iy + cross_off, c_main, c_cross);
        } else {
            arrange(c, ix + cross_off, pos, c_cross, c_main);
        }
        pos += c_main + n->spacing;
    }
}

void z_layout(ZView root, float w, float h, ZText *text) {
    measure(root, text);
    arrange(root, 0.0f, 0.0f, w, h);
}
