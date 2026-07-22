// The adaptive-target classifier. See predict.h for why this is a classifier and
// not a set of resized rectangles.
#include "predict.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// A Gaussian on the offset from a cap's centre, measured in each axis
// separately because a cap is not square (58 x 77 units): one isotropic sigma
// would make the vertical neighbours either unreachable or dominant.
static float touch_p(const ZKbdKey *k, float px, float py) {
    float sx = Z_KBD_SIGMA * k->w;
    float sy = Z_KBD_SIGMA * k->h;
    if (sx <= 0.0f || sy <= 0.0f) {
        return 0.0f;
    }
    float dx = px - (k->x + k->w * 0.5f);
    float dy = py - (k->y + k->h * 0.5f);
    float e = (dx * dx) / (2.0f * sx * sx) + (dy * dy) / (2.0f * sy * sy);
    return expf(-e);
}

// A press this close to a cap's centre IS that cap, whatever the model believes.
static bool in_centre(const ZKbdKey *k, float px, float py) {
    float dx = px - (k->x + k->w * 0.5f);
    float dy = py - (k->y + k->h * 0.5f);
    return fabsf(dx) <= Z_KBD_CENTRE * k->w * 0.5f &&
           fabsf(dy) <= Z_KBD_CENTRE * k->h * 0.5f;
}

// The language model's vote, as a multiplier on the geometric score. Expressed
// as ODDS AGAINST A UNIFORM ALPHABET rather than as a raw probability so the two
// factors are commensurable: 1.0 means "the model has no opinion", and the clamp
// then bounds how far an opinion can move a press in either direction.
//
// THE SPACE BAR IS IN HERE NOW (P48). `ch` may be Z_LM_BOUNDARY, and it goes
// through exactly the same arithmetic as a letter — which is the whole
// implementation of the adaptive space bar. It was excluded for one reason and
// one only: until there was a dictionary there was no model that could answer
// "has a word ended", so the honest factor was 1.0. The uniform baseline is
// Z_LM_SYMBOLS rather than 26 because the boundary now takes real mass out of the
// alphabet's share — after a complete word, most of it.
static float lm_factor(char ch, const char *prefix, bool language) {
    bool scored = (ch >= 'a' && ch <= 'z') || ch == Z_LM_BOUNDARY;
    if (!language || !scored) {
        return 1.0f;   // a modifier, or the model is off: geometry decides
    }
    float p = z_lm_p(prefix, ch);
    float f = p * (float)Z_LM_SYMBOLS;
    if (f > Z_KBD_ODDS) {
        f = Z_KBD_ODDS;
    }
    if (f < 1.0f / Z_KBD_ODDS) {
        f = 1.0f / Z_KBD_ODDS;
    }
    return f;
}

int z_kbd_nearest(const ZKbdKey *keys, int n, float px, float py) {
    int best = -1;
    float best_s = -1.0f;
    for (int i = 0; i < n; i++) {
        float s = touch_p(&keys[i], px, py);
        if (s > best_s) {
            best_s = s;
            best = i;
        }
    }
    return best;
}

int z_kbd_classify(const ZKbdKey *keys, int n, float px, float py,
                   const char *prefix, bool language, char *why,
                   size_t why_n) {
    if (why && why_n) {
        why[0] = '\0';
    }
    if (!keys || n <= 0) {
        return -1;
    }
    int geom = z_kbd_nearest(keys, n, px, py);

    // The inviolable centre, checked BEFORE any scoring so that no combination of
    // model and geometry can produce a different answer. Caps do not overlap, so
    // at most one centre zone can contain a point.
    for (int i = 0; i < n; i++) {
        if (in_centre(&keys[i], px, py)) {
            if (why && why_n) {
                snprintf(why, why_n, "centre");
            }
            return i;
        }
    }

    int best = geom;
    float best_s = -1.0f;
    for (int i = 0; i < n; i++) {
        float s = touch_p(&keys[i], px, py) *
                  lm_factor(keys[i].ch, prefix, language);
        if (s > best_s) {
            best_s = s;
            best = i;
        }
    }
    if (why && why_n) {
        if (best == geom) {
            snprintf(why, why_n, "%s", language ? "model-agrees" : "geometry");
        } else {
            snprintf(why, why_n, "model-over-'%c'",
                     keys[geom].ch ? keys[geom].ch : '?');
        }
    }
    return best;
}
