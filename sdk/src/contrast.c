// Increase Contrast: the second value of the three design tokens that have one.
//
// See the measured table above z_ink_muted() in <zelto/gfx.h> for where the
// numbers came from and why only three tokens move. This file is the mechanism,
// and it is deliberately the same shape as sdk/src/type.c: one process-global,
// set from the brokered setting at startup and on change, read through a
// function that 121 call sites already go through without knowing it.
//
// NOT gated on z_text_scaling_disable(). The status bar, the keyboard and the
// home indicator opt out of Dynamic Type because their HEIGHTS are contracts
// other processes offset by — nothing about a contract says the clock has to be
// hard to read. A colour swap moves no geometry at all, which is exactly why
// this one is safe everywhere the other is not.

#include <math.h>
#include <stdbool.h>

#include <zelto/gfx.h>

static bool g_increase;

bool z_contrast_increased(void) { return g_increase; }

bool z_contrast_apply(bool increase) {
    bool changed = increase != g_increase;
    g_increase = increase;
    return changed;
}

// The shipped values and their high-contrast partners. Written out here rather
// than as macros in the header because the header's job is now to name the
// TOKEN; two values behind one name is what this file is.
ZColor z_ink_muted(void) {
    return g_increase ? z_rgba(0xc7, 0xc7, 0xcc, 0xff)
                      : z_rgba(0xa1, 0xa1, 0xa8, 0xff);
}

ZColor z_ink_faint(void) {
    return g_increase ? z_rgba(0xa8, 0xa8, 0xb0, 0xff)
                      : z_rgba(0x6c, 0x6c, 0x70, 0xff);
}

ZColor z_hairline(void) {
    return g_increase ? z_rgba(0x6c, 0x6c, 0x70, 0xff)
                      : z_rgba(0x38, 0x38, 0x3a, 0xff);
}

// --- The ink that goes on a fill -------------------------------------------
// WCAG 2.1 relative luminance, then pick whichever of the two inks reaches
// further. See the measured table over Z_COLOR_SUCCESS in <zelto/gfx.h> for why
// this is a computation rather than a token: the header used to prescribe
// TEXT_INV on any semantic fill, and TEXT_INV on the old WARN was 1.99:1.
//
// The sRGB transfer, not a gamma of 2.2 — the same formula test_contrast_tokens
// checks with, so the assertion and the implementation cannot disagree about
// what "contrast" means. (The names are prefixed because that test compiles
// this file into itself and carries its own copy of the formula — two
// independent implementations that must agree.)
static double fill_chan(unsigned v) {
    double c = (double)v / 255.0;
    return c <= 0.03928 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double fill_lum(ZColor c) {
    return 0.2126 * fill_chan(c.r) + 0.7152 * fill_chan(c.g) +
           0.0722 * fill_chan(c.b);
}

static double fill_contrast(ZColor a, ZColor b) {
    double la = fill_lum(a), lb = fill_lum(b);
    double hi = la > lb ? la : lb, lo = la > lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

ZColor z_on_fill(ZColor fill) {
    ZColor light = Z_COLOR_TEXT_INV, dark = Z_COLOR_ON_PRIMARY;
    return fill_contrast(dark, fill) >= fill_contrast(light, fill) ? dark
                                                                  : light;
}
