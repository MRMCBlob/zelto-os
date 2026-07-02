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
// up. The "hide" key just parks it locally (the field keeps focus + its text).
//
// LAYERING. TOP layer (like the status/nav bars), created after the nav bar so it
// composites above it (a phone keyboard sits over the nav). The lock screen and
// the shade are OVERLAY, so they always composite ABOVE the keyboard: a locked
// screen shows its own passcode keypad, never the app's keyboard (and text-input
// focus is dropped under the lock anyway). See docs/platform/soft-keyboard.md.
#include <stdint.h>
#include <string.h>

#include <zelto/ui.h>

#define KBD_H 300     // keyboard strip height (px)
#define KEY_H 56      // one key's height

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
// Paste (P22): read the system clipboard and commit it into the focused field. A
// keyboard layer surface never holds keyboard focus, so it can't read the core
// wl_data_device selection; z_clipboard_get goes through wlr-data-control, which
// delivers the selection focus-independently, then z_im_commit_text inserts it at
// the field's caret via input-method — so the copied text crosses process
// boundaries with no cooperation from the target app.
static void kbd_paste_cb(ZApp *app, const char *text, void *ud) {
    (void)ud;
    if (text && text[0]) {
        z_im_commit_text(app, text);
    }
}
static void on_paste(ZApp *app, void *state) {
    (void)app; (void)state;
    z_clipboard_get(kbd_paste_cb, NULL);
}
// The "hide" key parks the keyboard locally (the field keeps focus + text).
static void on_hide_key(ZApp *app, void *state) {
    on_hide(app, state);
}

// --- key views --------------------------------------------------------------
// Flanking the glyph with Spacers centres it on the key's *main* (horizontal)
// axis — .align only governs the cross (vertical) axis, so without them the
// label hugs the left edge.
static ZView cap(ZView inner, float grow) {
    return Grow(grow,
        Background(Z_COLOR_SURFACE_3,
            CornerRadius(8.0f,
                Frame(0.0f, (float)KEY_H,
                    HStack(Spacer(), inner, Spacer(), .align = Z_ALIGN_CENTER)))));
}
static ZView glyph(const char *label) {
    return Foreground(Z_COLOR_TEXT_INV, Font(Z_FONT_CALLOUT, Text("%s", label)));
}

// A single character key (letter or symbol), tap commits it.
static ZView char_key(KbdState *s, char c) {
    char up = (s->shift && c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    char lbl[2] = {up, '\0'};
    return OnTapData(on_char, (void *)(intptr_t)c, cap(glyph(lbl), 1.0f));
}

// A row of character keys from a NUL-terminated string.
static ZView char_row(KbdState *s, const char *chars) {
    ZStackOpts row = {.spacing = 6.0f, .align = Z_ALIGN_CENTER, .grow = 1.0f};
    int k = 0;
    for (const char *p = chars; *p && k < Z_MAX_CHILDREN; p++) {
        row.children[k++] = char_key(s, *p);
    }
    return z_stack(Z_AXIS_HORIZONTAL, &row);
}

// A special (action) key: a labelled tappable cap.
static ZView action_key(const char *label, ZAction act, float grow, ZColor bg) {
    return Grow(grow,
        OnTap(act,
            Background(bg,
                CornerRadius(8.0f,
                    Frame(0.0f, (float)KEY_H,
                        HStack(Spacer(), glyph(label), Spacer(),
                               .align = Z_ALIGN_CENTER))))));
}

static ZView keyboard_grid(KbdState *s) {
    ZColor sp = Z_COLOR_SURFACE_2;                        // special-key fill
    ZColor shift_bg = s->shift ? Z_COLOR_PRIMARY : sp;   // active shift = accent

    ZView row1 = char_row(s, s->symbols ? "1234567890" : "qwertyuiop");
    ZView row2 = char_row(s, s->symbols ? "@#$%&-+()/" : "asdfghjkl");

    // Row 3: a mode key (shift on letters, ABC on symbols), 7 char keys, backspace.
    ZStackOpts r3 = {.spacing = 6.0f, .align = Z_ALIGN_CENTER, .grow = 1.0f};
    int k = 0;
    if (s->symbols) {
        r3.children[k++] = action_key("ABC", on_symbols, 1.6f, sp);
    } else {
        r3.children[k++] = action_key(s->shift ? "SHIFT" : "shift", on_shift,
                                      1.6f, shift_bg);
    }
    const char *r3c = s->symbols ? "*\"':;!?" : "zxcvbnm";
    for (const char *p = r3c; *p; p++) {
        r3.children[k++] = char_key(s, *p);
    }
    r3.children[k++] = action_key("<x", on_backspace, 1.6f, sp);
    ZView row3 = z_stack(Z_AXIS_HORIZONTAL, &r3);

    // Row 4: symbols toggle, space (wide), paste (system clipboard), enter, hide.
    ZView row4 = HStack(
        action_key(s->symbols ? "ABC" : "?123", on_symbols, 1.6f, sp),
        action_key("space", on_space, 4.2f, Z_COLOR_SURFACE_3),
        action_key("paste", on_paste, 1.8f, Z_COLOR_SUCCESS),
        action_key("enter", on_enter, 1.6f, sp),
        action_key("v", on_hide_key, 1.2f, sp),
        .spacing = 6.0f, .align = Z_ALIGN_CENTER, .grow = 1.0f);

    return Fill(
        Background(Z_COLOR_BG,
            VStack(row1, row2, row3, row4,
                   .spacing = 8.0f, .padding = 8.0f, .grow = 1.0f)));
}

// --- body -------------------------------------------------------------------
static ZView kbd_body(ZApp *app, KbdState *s) {
    if (!s->inited) {
        s->inited = true;
        s->anim = z_animated_value(app, 0.0f);
        z_im_bind(app, on_show, on_hide, s);
    }

    // Reserve our height only while shown (app shrinks to keep the field above
    // the keyboard); catch input only while shown (else taps fall through).
    z_layer_set_exclusive_zone(app, s->visible ? KBD_H : 0);
    if (s->visible) {
        z_layer_set_input_region(app, 0, 0, 0, 0);   // whole surface (input on)
    } else {
        z_layer_set_input_none(app);                 // fall through to the app
    }

    // Slide: v animates 0->1; parked slides the whole grid off the bottom edge.
    float v = z_animated_get(s->anim);
    float slide = (1.0f - v) * (float)KBD_H;
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
            .height = KBD_H,
            .keyboard = false)
