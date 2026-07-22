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
} KbdState;

// --- input-method show/hide (driven by the compositor) ---------------------
static void on_show(ZApp *app, void *ud) {
    KbdState *s = ud;
    s->visible = true;
    z_animated_spring(s->anim, 1.0f);
    z_invalidate(app);
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

// --- key views --------------------------------------------------------------
// A key cap. Flanking the mark with Spacers centres it on the key's *main*
// (horizontal) axis — .align only governs the cross (vertical) axis, so without
// them the mark hugs the left edge.
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
    return Grow(grow, act ? OnTap(act, face) : face);
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
// is a grow-weighted empty cap with no fill, not a Spacer inside a Frame.
static ZView half_gutter(void) {
    return Grow(0.5f, Rect(.color = z_rgba(0, 0, 0, 0)));
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
