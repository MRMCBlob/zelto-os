// Zelto System UI — on-screen keyboard (zelto-keyboard, P21).
//
// A bottom-anchored TOP-layer layer-shell app, always running like the bars, but
// only *shown* while a text field is focused. It is the system's single
// input-method-v2 client (via z_im_bind): the compositor raises the "show"
// callback when any app's text field takes focus (the text-input/input-method
// handshake) and "hide" when it blurs. While shown it renders a tap-driven QWERTY
// grid; each key calls z_im_commit_text (insert) or z_im_backspace (delete), and
// the character lands in whatever field owns focus — the app never knows a
// keyboard exists.
//
// SHOW/HIDE. The surface is a fixed KBD_H strip at the bottom. Hidden, it slides
// itself off the bottom edge (a vertical Offset driven by an animated value,
// recomputed each frame — the drawer's trick), reserves NO exclusive zone and
// catches NO input (falls through to the app / nav bar). Shown, it reserves KBD_H
// (the compositor shrinks the app so the focused field stays visible above the
// keyboard — a real exclusive zone, no per-app work), catches input, and slides
// up. There is no "hide" key: the keyboard leaves when the field blurs.
//
// LAYERING. TOP layer (like the status/nav bars), created after the nav bar so it
// composites above it (a phone keyboard sits over the nav). The lock screen and
// the shade are OVERLAY, so they always composite ABOVE the keyboard: a locked
// screen shows its own passcode keypad, never the app's keyboard (and text-input
// focus is dropped under the lock anyway). See docs/platform/soft-keyboard.md.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zelto/ui.h>

#include "common/safe_areas.h"

typedef struct KbdState {
    bool inited;
    bool visible;      // a text field is focused (input method active)
    bool shift;        // one-shot uppercase for the next letter
    bool symbols;      // symbols/numbers layer instead of letters
    ZAnimated *anim;   // 0 = parked off the bottom, 1 = fully up
    // P45 KBD harness: type a string by itself, one character per tick, once a
    // field really is focused. `typed` is how far through ZELTO_KBD_TYPE we are.
    int typed;
    bool typing;
    // P46: the same idea one layer out — press the KEY CAPS instead of calling
    // the commit function. `tapped` is how far through ZELTO_KBD_TAP we are.
    int tapped;
    bool tapping;
    bool audited;
} KbdState;

// Commit the next character of ZELTO_KBD_TYPE, then re-arm until the string is
// done. See the note over the arming code in on_show().
static void type_tick(ZApp *app, void *ud);
// Press the next cap named by ZELTO_KBD_TAP. See the note over tap_tick().
static void tap_tick(ZApp *app, void *ud);

// --- input-method show/hide (driven by the compositor) ---------------------
static void on_show(ZApp *app, void *ud) {
    KbdState *s = ud;
    s->visible = true;
    z_animated_spring(s->anim, 1.0f);
    // THE COORDINATE-FREE TYPING HOOK (P45). ZELTO_KBD_TYPE=<text> commits that
    // text through input-method-v2 exactly as a tapped key cap does, one
    // character at a time.
    //
    // It is armed HERE, off the real show handshake, not at startup: the
    // compositor raises the input method only when an app has actually focused a
    // text field, so committing before that would send the string into nothing.
    // That also makes the hook a stronger test than tapping key caps ever was —
    // it drives the genuine text-input-v3 <-> input-method-v2 relay, end to end,
    // with no key-cap coordinates to go stale. (Hardware keys cannot do this at
    // all: the SDK's kb_key handles Escape/Backspace and Return/space and never
    // inserts text, so a field is only ever typed into through this path.)
    const char *want = getenv("ZELTO_KBD_TYPE");
    if (want && want[0] && !s->typing) {
        s->typing = true;
        z_after(app, 400, type_tick, s);
    }
    // THE KEY-CAP HOOK (P46). ZELTO_KBD_TAP="a,SHIFT,b" presses the caps
    // themselves. Armed off the same handshake and for the same reason; what it
    // adds over ZELTO_KBD_TYPE is everything BETWEEN a finger and the commit —
    // see tap_tick().
    const char *taps = getenv("ZELTO_KBD_TAP");
    if (!taps || !taps[0]) {
        taps = getenv("ZELTO_KBD_CAPS");   // the audit runs without a sequence
    }
    if (taps && taps[0] && !s->tapping) {
        s->tapping = true;
        z_after(app, 400, tap_tick, s);
    }
    z_invalidate(app);
}

static void type_tick(ZApp *app, void *ud) {
    KbdState *s = ud;
    const char *want = getenv("ZELTO_KBD_TYPE");
    if (!want || s->typed >= (int)strlen(want)) {
        return;
    }
    char one[2] = {want[s->typed], '\0'};
    s->typed++;
    z_im_commit_text(app, one);
    fprintf(stderr, "[keyboard] committed '%s' (%d/%d)\n", one, s->typed,
            (int)strlen(want));
    fflush(stderr);
    if (s->typed < (int)strlen(want)) {
        z_after(app, 300, type_tick, s);
    }
}
static void on_hide(ZApp *app, void *ud) {
    KbdState *s = ud;
    s->visible = false;
    s->shift = false;
    s->symbols = false;
    z_animated_spring(s->anim, 0.0f);
    z_invalidate(app);
}

// --- key handlers -----------------------------------------------------------
// A character key: commit the (shift-cased) byte, then clear one-shot shift.
static void on_char(ZApp *app, void *state, void *data) {
    KbdState *s = state;
    int cp = (int)(intptr_t)data;
    char buf[2] = {(char)cp, '\0'};
    if (s->shift && cp >= 'a' && cp <= 'z') {
        buf[0] = (char)(cp - 32);
    }
    z_im_commit_text(app, buf);
    if (s->shift) {
        s->shift = false;
        z_invalidate(app);
    }
}
static void on_shift(ZApp *app, void *state) {
    KbdState *s = state;
    s->shift = !s->shift;
    z_invalidate(app);
}
static void on_symbols(ZApp *app, void *state) {
    KbdState *s = state;
    s->symbols = !s->symbols;
    s->shift = false;
    z_invalidate(app);
}
static void on_space(ZApp *app, void *state) {
    (void)state;
    z_im_commit_text(app, " ");
}
static void on_backspace(ZApp *app, void *state) {
    (void)state;
    z_im_backspace(app);
}
static void on_enter(ZApp *app, void *state) {
    (void)state;
    z_im_commit_text(app, "\n");
}

// --- the key caps, driven as key caps (P46) ---------------------------------
//
// WHAT THIS COVERS THAT ZELTO_KBD_TYPE DOES NOT. The P45 hook calls
// z_im_commit_text() from inside this process, which proves the text-input-v3 <->
// input-method-v2 relay and the persistence behind it, and proves nothing
// whatsoever about the keyboard as a SURFACE. Everything between a finger and
// that call was untested: whether a cap is laid out where the grid says, whether
// pressing it reaches that cap rather than a neighbour or the material behind it,
// whether the mark on a modifier describes what the modifier does, and whether
// the caps are big enough to hit.
//
// WHY THIS IS NOT THE COORDINATE TAPPING THAT ROTTED FIVE HARNESSES. Those
// harnesses carried the numbers: a screenshot was measured, "the 'a' key is at
// (96, 1180)" went into a shell script, the layout moved, and the tap kept
// landing — on something else, silently. Here the test names a CHARACTER and the
// layout answers where it is (z_probe_tap, sdk/src/app.c), so the only way for
// this to drift is for the code that builds the row to change, which is the thing
// under test. There is not a single coordinate in the harness or in this file.
//
// A name is one character ('a', '5', '?') or one of these words.
static struct {
    const char *name;
    ZAction act;
} KEY_WORDS[] = {
    {"SHIFT", on_shift},
    {"BKSP", on_backspace},
    {"SPACE", on_space},
    {"ENTER", on_enter},
    {"SYM", on_symbols},
};
#define N_KEY_WORDS ((int)(sizeof(KEY_WORDS) / sizeof(KEY_WORDS[0])))

// Name -> the handler the cap carries. A character key is an OnTapData node
// keyed by the LOWERCASE codepoint (char_key binds `c`, not the shifted glyph),
// which is exactly why tapping 'a' with shift latched must be asked for as 'a'.
static bool key_handler(const char *name, ZTapAction *on_data, void **data,
                        ZAction *on_plain) {
    *on_data = NULL;
    *data = NULL;
    *on_plain = NULL;
    for (int i = 0; i < N_KEY_WORDS; i++) {
        if (strcmp(name, KEY_WORDS[i].name) == 0) {
            *on_plain = KEY_WORDS[i].act;
            return true;
        }
    }
    if (name[0] && !name[1]) {
        *on_data = on_char;
        *data = (void *)(intptr_t)name[0];
        return true;
    }
    return false;
}

// The reverse, for the audit: what is this cap called?
static void key_name(ZTapAction on_data, void *data, ZAction on_plain, char *buf,
                     size_t n) {
    for (int i = 0; i < N_KEY_WORDS; i++) {
        if (on_plain == KEY_WORDS[i].act) {
            snprintf(buf, n, "%s", KEY_WORDS[i].name);
            return;
        }
    }
    if (on_data == on_char) {
        snprintf(buf, n, "%c", (char)(intptr_t)data);
        return;
    }
    snprintf(buf, n, "?");
}

// --- the touch-target audit -------------------------------------------------
// ZELTO_KBD_CAPS=1 measures every cap on the laid-out grid and prints it. The
// numbers are the assertion; this file does not decide what passes.
#define AUDIT_MAX 64
typedef struct CapAudit {
    int n;
    struct {
        char name[16];
        float x, y, w, h;
    } cap[AUDIT_MAX];
} CapAudit;

static float in_pt(float units) {
    return units * (float)Z_TYPE_DEN / (float)Z_TYPE_NUM;
}

static void audit_cap(void *ud, ZTapAction on_data, void *data, ZAction on_plain,
                      float x, float y, float w, float h) {
    CapAudit *a = ud;
    if (a->n >= AUDIT_MAX) {
        return;
    }
    key_name(on_data, data, on_plain, a->cap[a->n].name,
             sizeof(a->cap[a->n].name));
    a->cap[a->n].x = x;
    a->cap[a->n].y = y;
    a->cap[a->n].w = w;
    a->cap[a->n].h = h;
    a->n++;
}

static void audit_caps(ZApp *app) {
    CapAudit a = {0};
    z_probe_taps(app, audit_cap, &a);

    float min_w = 0.0f, min_h = 0.0f;
    for (int i = 0; i < a.n; i++) {
        // Reported in POINTS as well as units, because 44pt is the number anybody
        // arguing about a touch target reaches for, and converting it in a shell
        // script is how a metric ends up wrong in two places.
        fprintf(stderr,
                "[keyboard] cap '%s' x=%.0f y=%.0f w=%.0f h=%.0f "
                "(%.1fpt x %.1fpt)\n",
                a.cap[i].name, a.cap[i].x, a.cap[i].y, a.cap[i].w, a.cap[i].h,
                in_pt(a.cap[i].w), in_pt(a.cap[i].h));
        if (i == 0 || a.cap[i].w < min_w) {
            min_w = a.cap[i].w;
        }
        if (i == 0 || a.cap[i].h < min_h) {
            min_h = a.cap[i].h;
        }
    }
    fprintf(stderr,
            "[keyboard] caps: %d total, smallest %.0fx%.0f units "
            "(%.1fpt x %.1fpt)\n",
            a.n, min_w, min_h, in_pt(min_w), in_pt(min_h));

    // Per row: are the caps uniform, and how much of the row hits NOTHING?
    //
    // The second number is the one nobody had. A cap is a rounded face with a
    // KEY_GAP between it and the next, and that gap belongs to no key — a press
    // that lands in it reaches the material behind and does nothing. iOS gives
    // its caps hit regions that meet in the gutter, so its keyboard has no dead
    // strips at all; this one does, and their width is printed here so the claim
    // is a measurement rather than an impression.
    for (int i = 0; i < a.n; i++) {
        if (i > 0 && a.cap[i].y == a.cap[i - 1].y) {
            continue;             // same row, already summarised
        }
        float y = a.cap[i].y;
        float lo = a.cap[i].x, hi = a.cap[i].x + a.cap[i].w;
        float covered = 0.0f, cmin = 0.0f, cmax = 0.0f;
        int cells = 0, chars = 0;
        for (int j = 0; j < a.n; j++) {
            if (a.cap[j].y != y) {
                continue;
            }
            covered += a.cap[j].w;
            if (a.cap[j].x < lo) { lo = a.cap[j].x; }
            if (a.cap[j].x + a.cap[j].w > hi) { hi = a.cap[j].x + a.cap[j].w; }
            cells++;
            // Uniformity is a claim about the CHARACTER caps: a modifier is
            // deliberately 1.6x or 5.6x a letter, so folding them in would make
            // every row look ragged and hide the thing being watched for.
            if (!a.cap[j].name[1]) {
                if (chars == 0 || a.cap[j].w < cmin) { cmin = a.cap[j].w; }
                if (chars == 0 || a.cap[j].w > cmax) { cmax = a.cap[j].w; }
                chars++;
            }
        }
        float span = hi - lo;
        fprintf(stderr,
                "[keyboard] row y=%.0f: %d caps, span %.0f, covered %.0f, "
                "dead %.0f (%.1f%%), char widths %.0f..%.0f\n",
                y, cells, span, covered, span - covered,
                span > 0.0f ? 100.0f * (span - covered) / span : 0.0f, cmin,
                cmax);
    }
    fflush(stderr);
}

// Press the next cap named by ZELTO_KBD_TAP, then re-arm.
//
// The wait for the slide is not politeness. The grid rides an Offset driven by
// the show spring, and an Offset bakes into the layout — so while the keyboard is
// sliding up, every cap's frame is genuinely somewhere else. Pressing then would
// resolve a frame that is about to move and, worse, might still be off the bottom
// of the surface where the hit walk's own surface clip refuses it.
//
// It is a GUARD, not a tested behaviour, and the difference is worth writing
// down: removing this check did not make test_keyboard_caps_sim fail, because
// the first press is armed 400ms after the show handshake and the spring has
// already arrived by then. It earns its place on the runs where that is not
// true — reduce-motion, a loaded machine, a longer sequence — not on this one.
static void tap_tick(ZApp *app, void *ud) {
    KbdState *s = ud;
    if (z_animated_get(s->anim) < 0.999f) {
        z_after(app, 100, tap_tick, s);   // still sliding; the caps are moving
        return;
    }
    // The audit runs off the same settled grid, and on its own: measuring the
    // caps is a claim about the LAYOUT, so it must not need a tap sequence.
    if (!s->audited) {
        s->audited = true;
        if (getenv("ZELTO_KBD_CAPS")) {
            audit_caps(app);
        }
    }
    const char *spec = getenv("ZELTO_KBD_TAP");
    if (!spec || !spec[0]) {
        return;
    }

    // Walk to token number s->tapped.
    const char *p = spec;
    for (int i = 0; i < s->tapped && p; i++) {
        p = strchr(p, ',');
        if (p) {
            p++;
        }
    }
    if (!p || !*p) {
        fprintf(stderr, "[keyboard] taps done (%d)\n", s->tapped);
        fflush(stderr);
        return;
    }
    char name[16];
    const char *end = strchr(p, ',');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= sizeof(name)) {
        len = sizeof(name) - 1;
    }
    memcpy(name, p, len);
    name[len] = '\0';
    s->tapped++;

    ZTapAction on_data;
    void *data;
    ZAction on_plain;
    if (!key_handler(name, &on_data, &data, &on_plain)) {
        fprintf(stderr, "[keyboard] tap '%s': no such key name\n", name);
        fflush(stderr);
        z_after(app, 250, tap_tick, s);
        return;
    }
    ZProbeTap t = z_probe_tap(app, on_data, data, on_plain);
    if (!t.found) {
        // Not a crash: on the symbols layer there is no 'q' and no SHIFT, and a
        // test that asks for one should read a specific line saying so.
        fprintf(stderr, "[keyboard] tap '%s': NOT ON THIS LAYER\n", name);
    } else {
        fprintf(stderr,
                "[keyboard] tap '%s' cap x=%.0f y=%.0f w=%.0f h=%.0f hit=%s "
                "ran=%s\n",
                name, t.x, t.y, t.w, t.h, t.hit_same ? "same" : "DIFFERENT",
                t.ran ? "yes" : "no");
    }
    fflush(stderr);
    // A gap wide enough for the rebuild a modifier triggers: shift and the layer
    // key both change what the NEXT cap is, and the next resolve reads the tree
    // the last build laid out.
    z_after(app, 250, tap_tick, s);
}

// --- key views --------------------------------------------------------------
// A key cap. Flanking the mark with Spacers centres it on the key's *main*
// (horizontal) axis — .align only governs the cross (vertical) axis, so without
// them the mark hugs the left edge.
//
// SHARE, NOT GROW. This was Grow for twenty-five phases, which means every cap
// was as wide as the letter printed on it: measured at 720x1440, 'w' came out 69
// units and 'i' 49 in the same row, and latching shift re-measured the uppercase
// glyphs and moved every key in the row sideways under the finger. Grow divides
// the SLACK left after each child is measured, so the content leaks through;
// Share drops the intrinsic and lets the weights divide the row (zelto/ui.h).
// A key cap is a grid cell that happens to have a letter in it, so the letter
// must not have a vote. Found by the P46 cap audit, which is the first thing that
// ever measured a key.
//
// Character caps are the LIGHTEST surface in the system (SURFACE_4): on a heavy
// dark material, a field of forty SURFACE_3 caps reads as one grey slab with
// hairlines in it. The things you aim at have to be the light ones, and the
// modifiers a step below them.
static ZView key_cap(ZView inner, ZAction act, float grow, ZColor bg) {
    ZView face = Shadow(Z_ELEV_1,
        Background(bg,
            CornerRadius(Z_RADIUS_CHIP,
                Frame(0.0f, (float)ZELTO_KEY_H,
                    HStack(Spacer(), inner, Spacer(),
                           .align = Z_ALIGN_CENTER)))));
    return Share(grow, act ? OnTap(act, face) : face);
}
static ZView glyph(const char *label) {
    return Weight(Z_WEIGHT_MEDIUM,
        Foreground(Z_COLOR_TEXT, Font(Z_FONT_CALLOUT, Text("%s", label))));
}
// A key whose fill is LIGHT (the active shift) needs dark ink on it.
static ZView glyph_on(const char *label, ZColor ink) {
    return Weight(Z_WEIGHT_SEMIBOLD,
        Foreground(ink, Font(Z_FONT_SUBHEAD, Text("%s", label))));
}

// The modifier marks. These were the words "shift", "del" and "enter", which is
// what a keyboard looks like when nobody has drawn it: three English words in the
// middle of a grid of single letters, each one wider than the key it sits on and
// each one needing to be READ. They are marks now — the same three every phone
// keyboard has drawn for fifteen years — built from the toolkit's round-capped
// polyline in the unit box, so they scale with the cap and ship no bitmaps.
// The marks are drawn in the unit box, so only the frame around them carries a
// size — and that size used to be 22, chosen against a 56-unit key cap. A cap is
// ZELTO_KEY_H (Apple's 42pt) now, so the frame is a point size too: ~20pt is the
// keyboard glyph on the phone this copies. The two non-square marks keep the
// aspect they were drawn at.
#define MARK_H ((float)Z_PT(20))                 // 37
#define MARK_W_DELETE (MARK_H * 26.0f / 22.0f)   // the drawn glyph's aspect
#define MARK_W_RETURN (MARK_H * 24.0f / 22.0f)
#define MARK_STROKE ((float)Z_PT(2))             // 3 — the ink, at the glyph's scale

static ZView mark_shift(ZColor ink) {
    static const float arrow[] = {0.50f, 0.12f, 0.88f, 0.50f, 0.68f, 0.50f,
                                  0.68f, 0.82f, 0.32f, 0.82f, 0.32f, 0.50f,
                                  0.12f, 0.50f};
    return Frame(MARK_H, MARK_H,
        Stroke(.points = arrow, .count = 7, .thickness = MARK_STROKE, .color = ink,
               .closed = true));
}
static ZView mark_delete(ZColor ink) {
    static const float body[] = {0.36f, 0.20f, 0.94f, 0.20f, 0.94f, 0.80f,
                                 0.36f, 0.80f, 0.06f, 0.50f};
    static const float x1[] = {0.54f, 0.37f, 0.80f, 0.63f};
    static const float x2[] = {0.80f, 0.37f, 0.54f, 0.63f};
    return Frame(MARK_W_DELETE, MARK_H,
        ZStack(
            Frame(MARK_W_DELETE, MARK_H,
                Stroke(.points = body, .count = 5, .thickness = MARK_STROKE,
                       .color = ink, .closed = true)),
            Frame(MARK_W_DELETE, MARK_H,
                Stroke(.points = x1, .count = 2, .thickness = MARK_STROKE, .color = ink)),
            Frame(MARK_W_DELETE, MARK_H,
                Stroke(.points = x2, .count = 2, .thickness = MARK_STROKE, .color = ink)),
            .align = Z_ALIGN_CENTER));
}
static ZView mark_return(ZColor ink) {
    static const float hook[] = {0.86f, 0.22f, 0.86f, 0.60f, 0.22f, 0.60f};
    static const float head[] = {0.42f, 0.42f, 0.22f, 0.60f, 0.42f, 0.78f};
    return Frame(MARK_W_RETURN, MARK_H,
        ZStack(
            Frame(MARK_W_RETURN, MARK_H,
                Stroke(.points = hook, .count = 3, .thickness = MARK_STROKE,
                       .color = ink)),
            Frame(MARK_W_RETURN, MARK_H,
                Stroke(.points = head, .count = 3, .thickness = MARK_STROKE,
                       .color = ink)),
            .align = Z_ALIGN_CENTER));
}

// A single character key (letter or symbol), tap commits it.
static ZView char_key(KbdState *s, char c) {
    char up = (s->shift && c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    char lbl[2] = {up, '\0'};
    return OnTapData(on_char, (void *)(intptr_t)c,
        key_cap(glyph(lbl), NULL, 1.0f, Z_COLOR_SURFACE_4));
}

// A half-key gutter at the end of a row (the a-s-d-f row is inset by half a key
// on both sides, so its nine keys sit UNDER the gaps of the ten above them). It
// is a share-weighted empty cap with no fill, not a Spacer inside a Frame. Share
// for the same reason the caps use it: half a key means half of what a key gets,
// which is only true if a key's width is its share of the row.
static ZView half_gutter(void) {
    return Share(0.5f, Rect(.color = z_rgba(0, 0, 0, 0)));
}

// A row of character keys from a NUL-terminated string, optionally inset by half
// a key at both ends (the home row).
static ZView char_row(KbdState *s, const char *chars, bool inset) {
    ZStackOpts row = {.spacing = (float)ZELTO_KEY_GAP, .align = Z_ALIGN_CENTER,
                      .grow = 1.0f};
    int k = 0;
    if (inset) {
        row.children[k++] = half_gutter();
    }
    for (const char *p = chars; *p && k < Z_MAX_CHILDREN - 1; p++) {
        row.children[k++] = char_key(s, *p);
    }
    if (inset) {
        row.children[k++] = half_gutter();
    }
    return z_stack(Z_AXIS_HORIZONTAL, &row);
}

// A modifier key carrying a MARK. Its fill is a step below a character cap, so
// the letters — the things you are actually aiming at — stay the light ones.
static ZView mark_key(ZView mark, ZAction act, float grow, ZColor bg) {
    return key_cap(mark, act, grow, bg);
}

// A modifier key carrying a WORD (only "123" / "ABC" and "space" survive; the
// rest are marks now). Words take the smaller type step — a 20px "space" next to
// a 20px "q" makes the word look like it is shouting.
static ZView word_key(const char *label, ZAction act, float grow, ZColor bg,
                      ZColor ink) {
    return key_cap(glyph_on(label, ink), act, grow, bg);
}

static ZView keyboard_grid(KbdState *s) {
    ZColor sp = Z_COLOR_SURFACE_3;      // a modifier cap
    ZColor spi = Z_COLOR_TEXT;          // its ink
    // Shift latched: a LIGHT key with dark ink, the way a phone shows it (this is
    // the same "lit" treatment as an active Control Center toggle).
    ZColor shift_bg = s->shift ? Z_COLOR_PRIMARY : sp;
    ZColor shift_ink = s->shift ? Z_COLOR_ON_PRIMARY : spi;

    ZView row1 = char_row(s, s->symbols ? "1234567890" : "qwertyuiop", false);
    ZView row2 = char_row(s, s->symbols ? "@#$%&-+()/" : "asdfghjkl",
                          !s->symbols);

    // Row 3: shift (letters only), the last char keys, delete.
    ZStackOpts r3 = {.spacing = (float)ZELTO_KEY_GAP, .align = Z_ALIGN_CENTER,
                     .grow = 1.0f};
    int k = 0;
    if (!s->symbols) {
        r3.children[k++] = mark_key(mark_shift(shift_ink), on_shift, 1.6f,
                                    shift_bg);
    }
    const char *r3c = s->symbols ? ".,?!'\";:" : "zxcvbnm";
    for (const char *p = r3c; *p; p++) {
        r3.children[k++] = char_key(s, *p);
    }
    r3.children[k++] = mark_key(mark_delete(spi), on_backspace, 1.6f, sp);
    ZView row3 = z_stack(Z_AXIS_HORIZONTAL, &r3);

    // Row 4: the layer key, space, return. The "paste" and "hide" keys are gone.
    // Paste was a keyboard key doing a TEXT FIELD's job — the field's own
    // selection menu already pastes (sdk/src/app.c field_paste_cb), which is where
    // you are looking when you want it, and where iOS puts it. "hide" was a button
    // whose whole purpose was to undo the thing you did to get the keyboard up:
    // the keyboard hides when the field blurs, and there is a home gesture for
    // leaving. Space keeps the LIGHTER character-key fill, because it IS a
    // character key.
    ZView row4 = HStack(
        word_key(s->symbols ? "ABC" : "123", on_symbols, 1.7f, sp, spi),
        word_key("space", on_space, 5.6f, Z_COLOR_SURFACE_4, Z_COLOR_TEXT),
        mark_key(mark_return(spi), on_enter, 2.0f, sp),
        .spacing = (float)ZELTO_KEY_GAP, .align = Z_ALIGN_CENTER, .grow = 1.0f);

    // A heavy material: the app behind shows only as a hint. See kbd_body for why
    // this one is a tint rather than a compositor blur.
    return Fill(
        Background(Z_COLOR_MATERIAL_THICK,
            VStack(row1, row2, row3, row4,
                   .spacing = (float)ZELTO_KEY_GAP,
                   .padding = (float)ZELTO_KEY_PAD, .grow = 1.0f)));
}

// --- body -------------------------------------------------------------------
static ZView kbd_body(ZApp *app, KbdState *s) {
    if (!s->inited) {
        s->inited = true;
        s->anim = z_animated_value(app, 0.0f);
        z_im_bind(app, on_show, on_hide, s);
        // Headless test hook: ZELTO_KBD_SHOW=1 raises the keyboard on the first
        // build (without a real text-input focus handshake) so the QWERTY layout is
        // screenshot-verifiable. Keys go nowhere with no focused field — this is a
        // layout capture only. ZELTO_KBD_SYMBOLS=1 shows the symbols layer instead.
        const char *ks = getenv("ZELTO_KBD_SHOW");
        if (ks && ks[0] == '1') {
            s->visible = true;
            s->symbols = getenv("ZELTO_KBD_SYMBOLS") &&
                         getenv("ZELTO_KBD_SYMBOLS")[0] == '1';
            z_animated_set(s->anim, 1.0f);
        }
    }

    // Reserve our height only while shown (app shrinks to keep the field above
    // the keyboard); catch input only while shown (else taps fall through).
    z_layer_set_exclusive_zone(app, s->visible ? ZELTO_KBD_H : 0);
    if (s->visible) {
        z_layer_set_input_region(app, 0, 0, 0, 0);   // whole surface (input on)
    } else {
        z_layer_set_input_none(app);                 // fall through to the app
    }
    // NO z_backdrop here, deliberately. The keyboard is the one system surface a
    // blur does nothing for — it is a dense field of opaque keys, so almost none of
    // the backdrop survives to be seen — and asking for one adds a surface commit
    // per build, which perturbs the order the compositor hands out exclusive zones
    // between the two bottom-anchored bars: the keyboard would take the bottom edge
    // and shove the nav bar up into its own key rows. It gets a heavy tint instead.

    // Slide: v animates 0->1; parked slides the whole grid off the bottom edge.
    float v = z_animated_get(s->anim);
    float slide = (1.0f - v) * (float)ZELTO_KBD_H;
    z_full_repaint(app);   // a big translated subtree wants a full repaint
    return Offset(NULL, slide, keyboard_grid(s));
}

// TOP layer, bottom-anchored, full width, fixed KBD_H height. Exclusive zone is
// toggled at runtime (0 hidden / KBD_H shown). keyboard=false: the on-screen
// keyboard takes NO wl_keyboard focus — it drives text via input-method-v2.
Z_LAYER_APP(KbdState, kbd_body,
            .layer = Z_LAYER_TOP,
            .anchor = Z_ANCHOR_BOTTOM | Z_ANCHOR_LEFT | Z_ANCHOR_RIGHT,
            .exclusive_zone = 0,
            .height = ZELTO_KBD_H,
            .keyboard = false)
